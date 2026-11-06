/* loading.c -- the loading screen, as the Android build's Java side
 * (GTAActivityBase) draws it over the game, from the APK's res/raw/ PNGs in
 * game/:
 *   - OS_ShowLoadingScreen: a random one of loading_screen_<1-4>.png,
 *     stretched over the screen, no bar.
 *   - OS_UpdateLoadingBar(progress, 0) (Java ShowLoadingBar): the bar, hud_loadbar.png with
 *     hud_loadbarprogress.png over it cut to progress (0..1) of its width,
 *     centred, at 88% of the screen height; the _small pair when
 *     hud_loadbarprogress.png is wider than the screen. Java also switches
 *     the picture to loading_screen_3.png there, which only made the
 *     random one flash: it stays.
 *   - OS_UpdateLoadingBar(_, 1) (Java ShowLoadingBarProgressHack): a thread
 *     moves the bar every 250 ms by 2.5% of what is left, up to 95%, until
 *     HideLoadingScreen. Here that progress follows the time since then.
 * A load runs inside one viewOnDrawFrame, so each of these draws into the
 * game's GL context and swaps at once; frames between them show it too. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "util.h"

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_ARRAY_BUFFER 0x8892
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_VIEWPORT 0x0BA2
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_STENCIL_TEST 0x0B90
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_SRC_ALPHA 0x80CB
#define GL_BLEND_DST_ALPHA 0x80CA
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED 0x8622

static struct {
	void (*GenTextures)(int, unsigned *);
	void (*DeleteTextures)(int, const unsigned *);
	void (*BindTexture)(unsigned, unsigned);
	void (*ActiveTexture)(unsigned);
	void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
	void (*TexParameteri)(unsigned, unsigned, int);
	void (*PixelStorei)(unsigned, int);
	unsigned (*CreateShader)(unsigned);
	void (*ShaderSource)(unsigned, int, const char *const *, const int *);
	void (*CompileShader)(unsigned);
	unsigned (*CreateProgram)(void);
	void (*AttachShader)(unsigned, unsigned);
	void (*BindAttribLocation)(unsigned, unsigned, const char *);
	void (*LinkProgram)(unsigned);
	void (*UseProgram)(unsigned);
	int (*GetUniformLocation)(unsigned, const char *);
	void (*Uniform1i)(int, int);
	void (*Uniform4f)(int, float, float, float, float);
	void (*Uniform2f)(int, float, float);
	void (*BindBuffer)(unsigned, unsigned);
	void (*BindFramebuffer)(unsigned, unsigned);
	void (*VertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
	void (*EnableVertexAttribArray)(unsigned);
	void (*DisableVertexAttribArray)(unsigned);
	void (*GetVertexAttribiv)(unsigned, unsigned, int *);
	void (*DrawArrays)(unsigned, int, int);
	void (*Viewport)(int, int, int, int);
	void (*Enable)(unsigned);
	void (*Disable)(unsigned);
	unsigned char (*IsEnabled)(unsigned);
	void (*GetIntegerv)(unsigned, int *);
	void (*GetBooleanv)(unsigned, unsigned char *);
	void (*ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
	void (*BlendFuncSeparate)(unsigned, unsigned, unsigned, unsigned);
	void (*ClearColor)(float, float, float, float);
	void (*Clear)(unsigned);
	void *(*GetCurrentContext)(void);
} gl;

typedef struct {
	unsigned tex;
	int w, h;
} Image;

static unsigned program;
static int u_rect, u_tc, u_s;
static Image picture, bar_back, bar_front;
static int picture_number; /* which loading_screen_<n> picture holds */
static int showing, bar_shown, hack;
static float bar_progress;
static double hack_start;

/* ---- 8-bit PNGs: RGB, RGBA or palette (with tRNS), not interlaced, to RGBA ---- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c)
{
	int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static uint8_t *load_png(const char *path, int *w, int *h)
{
	*w = *h = 0;
	FILE *f = fopen(path, "rb");
	if (!f) {
		log_printf("loading screen: %s missing\n", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc(n);
	size_t got = fread(d, 1, n, f);
	fclose(f);
	uint8_t *z = malloc(n), *out = NULL, *raw = NULL, *pix = NULL;
	uint8_t palette[256][4];
	memset(palette, 255, sizeof(palette));
	size_t zn = 0;
	int type = -1, bpp, stride;
	uLongf rn;
	for (long p = 8; got == (size_t)n && p + 12 <= n; ) {
		uint32_t len = be32(d + p);
		const uint8_t *tag = d + p + 4, *body = d + p + 8;
		if (p + 12 + (long)len > n)
			break;
		if (!memcmp(tag, "IHDR", 4)) {
			*w = be32(body);
			*h = be32(body + 4);
			type = body[9];
			if (body[8] != 8 || body[12] != 0 || (type != 2 && type != 3 && type != 6))
				goto out;
		} else if (!memcmp(tag, "PLTE", 4)) {
			for (uint32_t i = 0; i < len / 3 && i < 256; i++)
				memcpy(palette[i], body + 3 * i, 3);
		} else if (!memcmp(tag, "tRNS", 4)) {
			for (uint32_t i = 0; i < len && i < 256; i++)
				palette[i][3] = body[i];
		} else if (!memcmp(tag, "IDAT", 4)) {
			memcpy(z + zn, body, len);
			zn += len;
		}
		p += 12 + len;
	}
	if (*w <= 0 || *h <= 0 || type < 0)
		goto out;
	bpp = type == 2 ? 3 : type == 6 ? 4 : 1;
	stride = *w * bpp;
	rn = (uLongf)(stride + 1) * *h;
	raw = malloc(rn);
	if (uncompress(raw, &rn, z, zn) != Z_OK || rn != (uLongf)(stride + 1) * *h)
		goto out;
	pix = malloc((size_t)stride * *h);
	for (int y = 0; y < *h; y++) {
		const uint8_t *in = raw + (size_t)y * (stride + 1) + 1;
		uint8_t *row = pix + (size_t)y * stride, *up = y ? row - stride : NULL;
		int filter = in[-1];
		for (int x = 0; x < stride; x++) {
			int a = x >= bpp ? row[x - bpp] : 0, b = up ? up[x] : 0, c = x >= bpp && up ? up[x - bpp] : 0;
			int v = in[x];
			switch (filter) {
			case 1: v += a; break;
			case 2: v += b; break;
			case 3: v += (a + b) / 2; break;
			case 4: v += paeth(a, b, c); break;
			}
			row[x] = v;
		}
	}
	out = malloc((size_t)*w * *h * 4);
	for (size_t i = 0; i < (size_t)*w * *h; i++) {
		if (type == 3) {
			memcpy(out + 4 * i, palette[pix[i]], 4);
		} else {
			memcpy(out + 4 * i, pix + bpp * i, 3);
			out[4 * i + 3] = type == 6 ? pix[4 * i + 3] : 255;
		}
	}
out:
	free(pix);
	free(raw);
	free(z);
	free(d);
	return out;
}

static void load_image(Image *img, const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", fs_root, name);
	uint8_t *rgba = load_png(path, &img->w, &img->h);
	if (!rgba)
		return;
	if (!img->tex)
		gl.GenTextures(1, &img->tex);
	gl.BindTexture(GL_TEXTURE_2D, img->tex);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img->w, img->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	free(rgba);
}

/* ---- drawing ---- */

static int gl_thread(void)
{
	return gl.GetCurrentContext && gl.GetCurrentContext();
}

static int setup(void)
{
	if (program)
		return 1;
	if (!gl.Clear) {
#define L(f) if (!(*(void **)&gl.f = mali_sym("gl" #f))) return 0;
		L(GenTextures) L(DeleteTextures) L(BindTexture) L(ActiveTexture) L(TexImage2D) L(TexParameteri)
		L(PixelStorei) L(CreateShader) L(ShaderSource) L(CompileShader) L(CreateProgram) L(AttachShader)
		L(BindAttribLocation) L(LinkProgram) L(UseProgram) L(GetUniformLocation) L(Uniform1i) L(Uniform4f)
		L(Uniform2f) L(BindBuffer) L(BindFramebuffer) L(VertexAttribPointer) L(EnableVertexAttribArray)
		L(DisableVertexAttribArray) L(GetVertexAttribiv) L(DrawArrays) L(Viewport) L(Enable) L(Disable)
		L(IsEnabled) L(GetIntegerv) L(GetBooleanv) L(ColorMask) L(BlendFuncSeparate) L(ClearColor) L(Clear)
#undef L
		*(void **)&gl.GetCurrentContext = mali_sym("eglGetCurrentContext");
	}
	if (!gl_thread())
		return 0;
	/* p: the unit square from the top left; r: x, y, width, height as
	 * fractions of the screen; tc: how much of the texture */
	static const char *vs =
		"attribute vec2 p; uniform vec4 r; uniform vec2 tc; varying vec2 t;"
		"void main() { t = p * tc; vec2 q = r.xy + p * r.zw;"
		" gl_Position = vec4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0); }";
	static const char *fs =
		"precision mediump float; varying vec2 t; uniform sampler2D s;"
		"void main() { gl_FragColor = texture2D(s, t); }";
	unsigned v = gl.CreateShader(GL_VERTEX_SHADER), f = gl.CreateShader(GL_FRAGMENT_SHADER);
	gl.ShaderSource(v, 1, &vs, NULL);
	gl.ShaderSource(f, 1, &fs, NULL);
	gl.CompileShader(v);
	gl.CompileShader(f);
	program = gl.CreateProgram();
	gl.AttachShader(program, v);
	gl.AttachShader(program, f);
	gl.BindAttribLocation(program, 0, "p");
	gl.LinkProgram(program);
	u_rect = gl.GetUniformLocation(program, "r");
	u_tc = gl.GetUniformLocation(program, "tc");
	u_s = gl.GetUniformLocation(program, "s");
	return 1;
}

/* img at x, y (pixels from the top left), w x h, showing its left part u (0..1) */
static void blit(const Image *img, float x, float y, float w, float h, float u)
{
	if (!img->tex || w <= 0.0f)
		return;
	float sw = egl_width(), sh = egl_height();
	gl.BindTexture(GL_TEXTURE_2D, img->tex);
	gl.Uniform4f(u_rect, x / sw, y / sh, w / sw, h / sh);
	gl.Uniform2f(u_tc, u, 1.0f);
	gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw(void)
{
	int fb, prog, buf, active, tex, view[4], attr0, blend[4];
	unsigned char mask[4], on[5];
	static const unsigned caps[5] = { GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST };
	gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	gl.GetIntegerv(GL_CURRENT_PROGRAM, &prog);
	gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &buf);
	gl.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
	gl.ActiveTexture(GL_TEXTURE0);
	gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	gl.GetIntegerv(GL_VIEWPORT, view);
	gl.GetIntegerv(GL_BLEND_SRC_RGB, &blend[0]);
	gl.GetIntegerv(GL_BLEND_DST_RGB, &blend[1]);
	gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &blend[2]);
	gl.GetIntegerv(GL_BLEND_DST_ALPHA, &blend[3]);
	gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr0);
	gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
	for (int i = 0; i < 5; i++) {
		on[i] = gl.IsEnabled(caps[i]);
		gl.Disable(caps[i]);
	}
	gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.ColorMask(1, 1, 1, 1);
	int sw = egl_width(), sh = egl_height();
	gl.Viewport(0, 0, sw, sh);
	gl.ClearColor(0, 0, 0, 1);
	gl.Clear(GL_COLOR_BUFFER_BIT);

	static const float quad[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	gl.UseProgram(program);
	gl.Uniform1i(u_s, 0);
	gl.BindBuffer(GL_ARRAY_BUFFER, 0);
	gl.EnableVertexAttribArray(0);
	gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, 0, quad);
	blit(&picture, 0, 0, sw, sh, 1.0f); /* ImageView FIT_XY */
	if (bar_shown) {
		gl.Enable(GL_BLEND);
		gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		float x = (sw - bar_back.w) / 2, y = floorf(sh * 0.88f);
		blit(&bar_back, x, y, bar_back.w, bar_back.h, 1.0f);
		int cut = (int)(bar_front.w * bar_progress); /* Bitmap.createBitmap's width */
		if (bar_front.w)
			blit(&bar_front, x, y, cut, bar_front.h, (float)cut / bar_front.w);
	}
	egl_swap();

	for (int i = 0; i < 5; i++)
		(on[i] ? gl.Enable : gl.Disable)(caps[i]);
	gl.BlendFuncSeparate(blend[0], blend[1], blend[2], blend[3]);
	gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
	gl.Viewport(view[0], view[1], view[2], view[3]);
	if (!attr0)
		gl.DisableVertexAttribArray(0);
	gl.BindBuffer(GL_ARRAY_BUFFER, buf);
	gl.BindTexture(GL_TEXTURE_2D, tex);
	gl.ActiveTexture(active);
	gl.UseProgram(prog);
	gl.BindFramebuffer(GL_FRAMEBUFFER, fb);
}

static void set_picture(int n)
{
	if (picture_number == n)
		return;
	char name[64];
	snprintf(name, sizeof(name), "loading_screen_%d.png", n);
	load_image(&picture, name);
	picture_number = n;
}

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

void loading_show(void)
{
	if (!setup())
		return; /* not the GL thread (the game's tick thread) */
	/* Java's Random is seeded from the clock; rand() started the same on
	 * every launch (always the same picture). Our own, not the game's. */
	static unsigned seed;
	if (!seed)
		seed = (unsigned)(now_s() * 1000.0);
	set_picture(1 + rand_r(&seed) % 4);
	bar_shown = 0;
	showing = 1;
	draw();
}

void loading_progress(float progress, int fake)
{
	if (!showing || !setup())
		return;
	if (fake) {
		if (!hack) {
			hack = 1;
			hack_start = now_s();
		}
		/* the Java thread's steps so far: p += (1 - p) * 0.025, at most 0.95 */
		int steps = (int)((now_s() - hack_start) / 0.25);
		progress = fminf(0.95f, 1.0f - powf(0.975f, steps));
	}
	if (!bar_back.tex) {
		/* the big pair unless its progress image is wider than the screen */
		load_image(&bar_front, "hud_loadbarprogress.png");
		int small = bar_front.w > egl_width();
		if (small)
			load_image(&bar_front, "hud_loadbarprogress_small.png");
		load_image(&bar_back, small ? "hud_loadbar_small.png" : "hud_loadbar.png");
	}
	bar_progress = progress < 0.0f ? 0.0f : progress > 1.0f ? 1.0f : progress;
	bar_shown = 1;
	draw();
}

/* Java's views stay over the game until HideLoadingScreen: frames that run
 * while it is up show it instead of the game's. 1 when it was drawn. */
int loading_frame(void)
{
	if (!showing || !setup())
		return 0;
	if (hack)
		loading_progress(0.0f, 1);
	else
		draw();
	return 1;
}

void loading_hide(void)
{
	showing = bar_shown = hack = 0;
}

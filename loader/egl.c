/* egl.c -- the game's window: a GLES 2.0 context on ARM's Mali driver's EGL,
 * drawing to /dev/fb0, with tear-free flips and an FPS counter. The game's
 * own GLSL ES shaders run as shipped. Carried over from glOSports-maxpayne. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <linux/fb.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "so_util.h"
#include "util.h"

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int EGLint;
typedef unsigned EGLBoolean;

#define EGL_NONE 0x3038
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

/* ARM's fbdev native window type */
static struct { unsigned short width, height; } native_window;

static struct {
	EGLDisplay (*GetDisplay)(void *);
	EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
	EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
	EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
	EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
	EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
	EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
	EGLBoolean (*SwapInterval)(EGLDisplay, EGLint);
	EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
	EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
	EGLBoolean (*Terminate)(EGLDisplay);
	EGLint (*GetError)(void);
	EGLContext (*GetCurrentContext)(void);
} egl;

static EGLDisplay display;
static EGLSurface surface;
static EGLContext context, movie_context;
static EGLConfig config;
static int wait_for_pan; /* GAME_VSYNC */

/* GLES calls of the FPS counter */
static struct {
	void (*Clear)(unsigned);
	void (*ClearColor)(float, float, float, float);
	void (*Scissor)(int, int, int, int);
	void (*Enable)(unsigned);
	void (*Disable)(unsigned);
	unsigned char (*IsEnabled)(unsigned);
	void (*GetIntegerv)(unsigned, int *);
	void (*GetFloatv)(unsigned, float *);
	void (*GetBooleanv)(unsigned, unsigned char *);
	void (*ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
	void (*BindFramebuffer)(unsigned, unsigned);
} gl;

void egl_init(void)
{
	if (display)
		return;
#define LOAD(f) if (!(*(void **)&egl.f = mali_sym("egl" #f))) fatal_error("libMali.so has no egl" #f);
	LOAD(GetDisplay) LOAD(Initialize) LOAD(ChooseConfig) LOAD(CreateWindowSurface)
	LOAD(CreateContext) LOAD(MakeCurrent) LOAD(SwapBuffers) LOAD(SwapInterval)
	LOAD(DestroySurface) LOAD(DestroyContext) LOAD(Terminate) LOAD(GetError)
	LOAD(GetCurrentContext)
#undef LOAD

	struct fb_var_screeninfo vi = { 0 };
	int fd = open("/dev/fb0", O_RDONLY);
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0)
		fatal_error("can't read /dev/fb0");
	close(fd);
	native_window.width = vi.xres;
	native_window.height = vi.yres;

	display = egl.GetDisplay(NULL);
	EGLint major, minor;
	if (!display || !egl.Initialize(display, &major, &minor))
		fatal_error("eglInitialize failed 0x%x", egl.GetError());
	int rgb565 = vi.bits_per_pixel == 16;
	const EGLint attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, rgb565 ? 5 : 8, EGL_GREEN_SIZE, rgb565 ? 6 : 8, EGL_BLUE_SIZE, rgb565 ? 5 : 8,
		EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};
	EGLint n = 0;
	if (!egl.ChooseConfig(display, attribs, &config, 1, &n) || n < 1)
		fatal_error("no EGL config 0x%x", egl.GetError());
	surface = egl.CreateWindowSurface(display, config, &native_window, NULL);
	const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	context = egl.CreateContext(display, config, NULL, ctx_attribs);
	if (!surface || !context || !egl.MakeCurrent(display, surface, surface, context))
		fatal_error("EGL surface/context failed 0x%x", egl.GetError());
	wait_for_pan = env_int("GAME_VSYNC", 1);
	egl.SwapInterval(display, wait_for_pan);
#define LOADGL(f) if (!(*(void **)&gl.f = mali_sym("gl" #f))) fatal_error("libMali.so has no gl" #f);
	LOADGL(Clear) LOADGL(ClearColor) LOADGL(Scissor) LOADGL(Enable) LOADGL(Disable)
	LOADGL(IsEnabled) LOADGL(GetIntegerv) LOADGL(GetFloatv) LOADGL(GetBooleanv)
	LOADGL(ColorMask) LOADGL(BindFramebuffer)
#undef LOADGL
	log_printf("glos: EGL %d.%d, GLES 2.0, %dx%d, %d bpp\n", major, minor,
		native_window.width, native_window.height, vi.bits_per_pixel);
}

int egl_width(void) { return native_window.width ? native_window.width : 640; }
int egl_height(void) { return native_window.height ? native_window.height : 480; }

/* the game moves the context between its threads (loading screen, render
 * queue): log the first moves and every failure with the thread's name */
static void log_current(const char *what, int ok)
{
	static int logged;
	if (ok && logged >= 40)
		return;
	logged++;
	char name[16] = "?";
	pthread_getname_np(pthread_self(), name, sizeof(name));
	log_printf("egl: %s on %s (%ld): %s 0x%x\n", what, name, (long)gettid(),
		ok ? "ok" : "FAILED", ok ? 0 : egl.GetError());
}

/* this thread has the game's context current (it draws the game) */
int egl_is_render_thread(void)
{
	return display && egl.GetCurrentContext() == context;
}

/* The video player draws with a context of its own on the game's surface,
 * so none of the game's GL state is touched: 1 switches this thread to it
 * (made on the first call), 0 puts back what the thread had current before
 * (the game's context, or none). Fails (0) when another thread has the
 * surface current. Carried over from glOSports-gtactw. */
int egl_movie_context(int on)
{
	static EGLContext previous;
	if (on) {
		if (!display)
			return 0;
		previous = egl.GetCurrentContext();
		if (!movie_context) {
			const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
			movie_context = egl.CreateContext(display, config, NULL, ctx_attribs);
			if (!movie_context)
				return 0;
		}
		if (egl.MakeCurrent(display, surface, surface, movie_context))
			return 1;
		log_printf("glos: movie context refused 0x%x (game context %s here)\n", egl.GetError(),
			previous == context ? "current" : previous ? "other" : "not current");
		return 0;
	}
	return previous ? egl.MakeCurrent(display, surface, surface, previous)
		: egl.MakeCurrent(display, NULL, NULL, NULL);
}

int egl_make_current(void)
{
	int ok = egl.MakeCurrent(display, surface, surface, context);
	log_current("make current", ok);
	return ok;
}

int egl_unmake_current(void)
{
	int ok = egl.MakeCurrent(display, NULL, NULL, NULL);
	log_current("release", ok);
	return ok;
}

/* ---- FPS counter: a 5x7 pixel font drawn with scissored clears, so no GL
 * state of the game's but the scissor, clear colour and colour mask is
 * touched (all put back). ---- */

#define GL_SCISSOR_TEST 0x0C11
#define GL_SCISSOR_BOX 0x0C10
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_BUFFER_BIT 0x4000


static int show_fps, log_fps;
static int fps_digits[3] = { -1, -1, -1 };

/* 5x9 glyphs (rows 7-8 for descenders), bit 4 is the left column */
static const unsigned char glyphs[13][9] = {
	{ 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e }, /* 0 */
	{ 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e }, /* 1 */
	{ 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f }, /* 2 */
	{ 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e }, /* 3 */
	{ 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 }, /* 4 */
	{ 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e }, /* 5 */
	{ 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e }, /* 6 */
	{ 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
	{ 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e }, /* 8 */
	{ 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c }, /* 9 */
	{ 0x06, 0x09, 0x08, 0x1c, 0x08, 0x08, 0x08 }, /* f */
	{ 0, 0, 0x1e, 0x11, 0x11, 0x11, 0x1e, 0x10, 0x10 }, /* p */
	{ 0, 0, 0x0f, 0x10, 0x0e, 0x01, 0x1e }, /* s */
};
#define FONT_DOT 2
#define FONT_ADVANCE (6 * FONT_DOT)

static void fill(int x, int y, int w, int h)
{
	/* x, y from the top left; GL's origin is the bottom left */
	gl.Scissor(x, native_window.height - y - h, w, h);
	gl.Clear(GL_COLOR_BUFFER_BIT);
}

/* one glyph, each row's runs of dots as one clear */
static void draw_glyph(int x, int y, int g)
{
	for (int r = 0; r < 9; r++) {
		unsigned bits = glyphs[g][r];
		for (int c = 0; c < 5; c++) {
			if (!(bits & (0x10 >> c)))
				continue;
			int n = 1;
			while (c + n < 5 && (bits & (0x10 >> (c + n))))
				n++;
			fill(x + c * FONT_DOT, y + r * FONT_DOT, n * FONT_DOT, FONT_DOT);
			c += n;
		}
	}
}

/* "28 fps", right-aligned on the right edge; a dark shadow 2 px down right */
static void draw_label(int right, int y)
{
	int g[6], n = 0;
	for (int i = 0; i < 3; i++)
		if (fps_digits[i] >= 0)
			g[n++] = fps_digits[i];
	int digits = n;
	g[n++] = 10;
	g[n++] = 11;
	g[n++] = 12;
	int x0 = right - (n * FONT_ADVANCE - FONT_DOT) - FONT_ADVANCE / 2;
	for (int pass = 0; pass < 3; pass++) {
		int off = 2 - pass;
		for (int i = 0, x = x0; i < n; i++, x += FONT_ADVANCE) {
			if (i == digits)
				x += FONT_ADVANCE / 2; /* the space */
			if (off)
				gl.ClearColor(0.05f, 0.05f, 0.05f, 1);
			else
				gl.ClearColor(0.94f, 0.94f, 0.9f, 1);
			draw_glyph(x + off, y + off, g[i]);
		}
	}
}

static void draw_fps(void)
{
	int fb, scissor[4];
	float clear[4];
	unsigned char mask[4];
	unsigned char scissor_on = gl.IsEnabled(GL_SCISSOR_TEST);
	gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	gl.GetIntegerv(GL_SCISSOR_BOX, scissor);
	gl.GetFloatv(GL_COLOR_CLEAR_VALUE, clear);
	gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
	if (fb)
		gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.Enable(GL_SCISSOR_TEST);
	gl.ColorMask(1, 1, 1, 1);

	/* top right, from the first count on */
	if (fps_digits[2] >= 0)
		draw_label(native_window.width - 6, 6);

	gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
	gl.ClearColor(clear[0], clear[1], clear[2], clear[3]);
	gl.Scissor(scissor[0], scissor[1], scissor[2], scissor[3]);
	if (!scissor_on)
		gl.Disable(GL_SCISSOR_TEST);
	if (fb)
		gl.BindFramebuffer(GL_FRAMEBUFFER, fb);
}

static unsigned pans_done, swaps_done;
static double swap_ms; /* time spent in eglSwapBuffers since the last report */

static void count_frame(void)
{
	static struct timespec last;
	static int frames;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	frames++;
	double dt = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
	if (dt < 0.5)
		return;
	if (last.tv_sec) {
		int fps = (int)(frames / dt + 0.5);
		if (fps > 999)
			fps = 999;
		fps_digits[0] = fps >= 100 ? fps / 100 : -1;
		fps_digits[1] = fps >= 10 ? fps / 10 % 10 : -1;
		fps_digits[2] = fps % 10;
		if (log_fps)
			log_printf("fps %.1f, swap %.1f ms/frame (swaps %u, pans %u)\n", frames / dt, swap_ms / frames, swaps_done, pans_done);
	}
	last = now;
	frames = 0;
	swap_ms = 0;
}

/* ---- tear-free flips ----
 * ARM's fbdev EGL returns from eglSwapBuffers at once and pans the
 * framebuffer (FBIOPAN_DISPLAY, which waits for the vblank) on a thread of
 * its own. With two buffers the GPU would then draw the next frame into the
 * one still on screen. So before a frame is flushed, wait until the previous
 * frame's pan has landed; the game prepares the frame meanwhile. The pans
 * are seen here because the driver's ioctl() resolves to this one. */

static pthread_mutex_t pan_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pan_cond = PTHREAD_COND_INITIALIZER;

/* GAME_PAN_TRACE=n: log n events with times, once, GAME_PAN_TRACE_DELAY seconds
 * after the start: W wait for the pan, S/s swap, P/p pan to yoffset / done */
static struct { double t; char kind; int val; } trace[4096];
static int trace_n, trace_max;
static double trace_t0;

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void trace_event(char kind, int val)
{
	if (!trace_max)
		return;
	double t = now_ms() - trace_t0;
	if (t < 0)
		return; /* GAME_PAN_TRACE_DELAY not over yet */
	pthread_mutex_lock(&pan_lock);
	if (trace_n < trace_max) {
		trace[trace_n].t = t;
		trace[trace_n].kind = kind;
		trace[trace_n].val = val;
		if (++trace_n == trace_max) {
			for (int i = 0; i < trace_n; i++)
				log_printf("trace %9.2f %c %d\n", trace[i].t, trace[i].kind, trace[i].val);
			trace_max = 0;
		}
	}
	pthread_mutex_unlock(&pan_lock);
}

static int (*real_ioctl)(int, unsigned long, void *);

int ioctl(int fd, unsigned long request, ...)
{
	if (!real_ioctl)
		real_ioctl = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "ioctl");
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (request == FBIOPAN_DISPLAY && arg)
		trace_event('P', ((struct fb_var_screeninfo *)arg)->yoffset);
	int r = real_ioctl(fd, request, arg);
	if (request == FBIOPAN_DISPLAY) {
		int e = errno;
		trace_event('p', r);
		pthread_mutex_lock(&pan_lock);
		/* never ahead of the swaps: a pan that lands after its wait gave
		 * up (and counting started afresh) would otherwise switch the
		 * wait off for good */
		if ((int)(pans_done - swaps_done) < 0)
			pans_done++;
		pthread_cond_broadcast(&pan_cond);
		pthread_mutex_unlock(&pan_lock);
		errno = e;
	}
	return r;
}

static void wait_previous_pan(void)
{
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	t.tv_nsec += 100 * 1000000; /* never hang on a pan that doesn't come */
	if (t.tv_nsec >= 1000000000) {
		t.tv_sec++;
		t.tv_nsec -= 1000000000;
	}
	pthread_mutex_lock(&pan_lock);
	/* a slow frame can leave its pan late: wait for it next time, never
	 * count it as done (that would let every later frame tear). But a pan
	 * that is 100 ms late never comes: the driver merges the pans of frames
	 * swapped while it was behind (around a loading screen), and waiting
	 * for those cost every later frame the full 100 ms (10 fps for good).
	 * Then start counting afresh. */
	while ((int)(pans_done - swaps_done) < 0)
		if (pthread_cond_timedwait(&pan_cond, &pan_lock, &t) == ETIMEDOUT) {
			pans_done = swaps_done;
			break;
		}
	pthread_mutex_unlock(&pan_lock);
}

/* a pointer for games that need touch (egl_cursor), a small cross */
static int cursor_x = -1, cursor_y = -1, cursor_down;

void egl_cursor(int x, int y, int down)
{
	cursor_x = x;
	cursor_y = y;
	cursor_down = down;
}

static void draw_cursor(void)
{
	int fb, scissor[4];
	float clear[4];
	unsigned char mask[4];
	unsigned char scissor_on = gl.IsEnabled(GL_SCISSOR_TEST);
	gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	gl.GetIntegerv(GL_SCISSOR_BOX, scissor);
	gl.GetFloatv(GL_COLOR_CLEAR_VALUE, clear);
	gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
	if (fb)
		gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.Enable(GL_SCISSOR_TEST);
	gl.ColorMask(1, 1, 1, 1);
	gl.ClearColor(0, 0, 0, 1);
	fill(cursor_x - 9, cursor_y - 2, 19, 5);
	fill(cursor_x - 2, cursor_y - 9, 5, 19);
	if (cursor_down)
		gl.ClearColor(1, 0.3f, 0.2f, 1);
	else
		gl.ClearColor(1, 1, 1, 1);
	fill(cursor_x - 8, cursor_y - 1, 17, 3);
	fill(cursor_x - 1, cursor_y - 8, 3, 17);
	gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
	gl.ClearColor(clear[0], clear[1], clear[2], clear[3]);
	gl.Scissor(scissor[0], scissor[1], scissor[2], scissor[3]);
	if (!scissor_on)
		gl.Disable(GL_SCISSOR_TEST);
	if (fb)
		gl.BindFramebuffer(GL_FRAMEBUFFER, fb);
}

/* a frame cap for a game whose own limiter does nothing here: each swap
 * waits for its slot on a fixed 1/fps grid (a late frame starts the grid
 * again, no catching up); 0 leaves the pace to the flips */
static int frame_limit;

void egl_frame_limit(int fps)
{
	frame_limit = fps;
}

static void wait_frame_slot(void)
{
	static int64_t next;
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	int64_t now = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec, period = 1000000000 / frame_limit;
	if (next > now) {
		t.tv_sec = next / 1000000000;
		t.tv_nsec = next % 1000000000;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
	} else if (now - next > period) {
		next = now;
	}
	next += period;
}

void egl_swap(void)
{
	int movie = movie_frame(); /* a video on: its picture instead of the game's */
	egl_present();
	if (movie)
		movie_frame_done();
}

/* the swap itself (FPS counter, the wait for the previous flip), in
 * whatever context is current: egl_swap's, and the video player's own */
void egl_present(void)
{
	if (show_fps || log_fps)
		count_frame();
	if (show_fps)
		draw_fps();
	if (cursor_x >= 0)
		draw_cursor();
	struct timespec a, b;
	if (log_fps)
		clock_gettime(CLOCK_MONOTONIC, &a);
	if (frame_limit)
		wait_frame_slot();
	trace_event('W', 0);
	static int first = 1;
	if (first) {
		/* the driver pans once when the surface is made: count from here */
		pthread_mutex_lock(&pan_lock);
		pans_done = swaps_done = 0;
		pthread_mutex_unlock(&pan_lock);
		first = 0;
	} else if (wait_for_pan) {
		wait_previous_pan();
	}
	/* counted before the swap: its pan (on the driver's thread) can land
	 * before eglSwapBuffers returns */
	pthread_mutex_lock(&pan_lock);
	swaps_done++;
	pthread_mutex_unlock(&pan_lock);
	trace_event('S', 0);
	egl.SwapBuffers(display, surface);
	trace_event('s', 0);
	if (log_fps) {
		clock_gettime(CLOCK_MONOTONIC, &b);
		swap_ms += (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
	}
}

void egl_shutdown(void)
{
	if (!display)
		return;
	egl.MakeCurrent(display, NULL, NULL, NULL);
	egl.DestroyContext(display, context);
	egl.DestroySurface(display, surface);
	egl.Terminate(display);
	display = NULL;
}

/* settings from start.sh, read before the first frame */
__attribute__((constructor)) static void egl_settings(void)
{
	show_fps = env_int("GAME_FPS_COUNTER", 0);
	log_fps = env_int("GAME_LOG_FPS", 0);
	trace_max = env_int("GAME_PAN_TRACE", 0);
	if (trace_max > (int)(sizeof(trace) / sizeof(*trace)))
		trace_max = sizeof(trace) / sizeof(*trace);
	trace_t0 = now_ms() + env_int("GAME_PAN_TRACE_DELAY", 0) * 1000.0;
}

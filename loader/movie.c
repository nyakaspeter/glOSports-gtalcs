/* movie.c -- the games' videos, which on Android a Java MediaPlayer shows over
 * the game: here the game's file (H.264 + AAC in MP4, as the games ship
 * them, or converted where a port's README says so) is decoded with the static FFmpeg from
 * tools/build-ffmpeg.sh, drawn with a GL context of the loader's own on the
 * game's surface (egl_movie_context: none of the game's GL state is touched)
 * and heard through the folder's OpenAL. movie_play() blocks until the video
 * ends or A / Start is pressed, as the game then finds the movie over. It
 * returns 0, and the game goes on as before, when there is no such file or the
 * call comes from a thread without the game's context.
 *
 * Shared by the ports' loaders; improvements are carried over by hand. */
#include <AL/al.h>
#include <AL/alc.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_LUMINANCE 0x1909
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FLOAT 0x1406
#define GL_TRIANGLE_STRIP 0x0005
#define GL_COLOR_BUFFER_BIT 0x4000

static struct {
	unsigned (*CreateShader)(unsigned);
	void (*ShaderSource)(unsigned, int, const char *const *, const int *);
	void (*CompileShader)(unsigned);
	void (*GetShaderiv)(unsigned, unsigned, int *);
	void (*GetShaderInfoLog)(unsigned, int, int *, char *);
	unsigned (*CreateProgram)(void);
	void (*AttachShader)(unsigned, unsigned);
	void (*BindAttribLocation)(unsigned, unsigned, const char *);
	void (*LinkProgram)(unsigned);
	void (*GetProgramiv)(unsigned, unsigned, int *);
	void (*UseProgram)(unsigned);
	int (*GetUniformLocation)(unsigned, const char *);
	void (*Uniform1i)(int, int);
	void (*GenTextures)(int, unsigned *);
	void (*BindTexture)(unsigned, unsigned);
	void (*ActiveTexture)(unsigned);
	void (*TexParameteri)(unsigned, unsigned, int);
	void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
	void (*PixelStorei)(unsigned, int);
	void (*VertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
	void (*EnableVertexAttribArray)(unsigned);
	void (*DrawArrays)(unsigned, int, int);
	void (*Viewport)(int, int, int, int);
	void (*ClearColor)(float, float, float, float);
	void (*Clear)(unsigned);
} gl;

static unsigned program, tex[3];
static AVRational stream_sar; /* the video stream's, when its frames carry none */

static int load_gl(void)
{
	static int loaded;
	if (loaded)
		return loaded > 0;
#define LOAD(f) if (!(*(void **)&gl.f = mali_sym("gl" #f))) { loaded = -1; return 0; }
	LOAD(CreateShader) LOAD(ShaderSource) LOAD(CompileShader) LOAD(GetShaderiv)
	LOAD(GetShaderInfoLog) LOAD(CreateProgram) LOAD(AttachShader) LOAD(BindAttribLocation)
	LOAD(LinkProgram) LOAD(GetProgramiv) LOAD(UseProgram) LOAD(GetUniformLocation)
	LOAD(Uniform1i) LOAD(GenTextures) LOAD(BindTexture) LOAD(ActiveTexture)
	LOAD(TexParameteri) LOAD(TexImage2D) LOAD(PixelStorei) LOAD(VertexAttribPointer)
	LOAD(EnableVertexAttribArray) LOAD(DrawArrays) LOAD(Viewport) LOAD(ClearColor) LOAD(Clear)
#undef LOAD
	loaded = 1;
	return 1;
}

static unsigned shader(unsigned type, const char *src)
{
	unsigned s = gl.CreateShader(type);
	gl.ShaderSource(s, 1, &src, NULL);
	gl.CompileShader(s);
	int ok = 0;
	gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char msg[512];
		gl.GetShaderInfoLog(s, sizeof(msg), NULL, msg);
		log_printf("movie: shader: %s\n", msg);
	}
	return s;
}

/* once, in the movie context: a quad, three luminance planes, BT.601
 * limited-range YUV to RGB (what x264 writes) */
static void setup_gl(void)
{
	if (program)
		return;
	static const char *vs =
		"attribute vec2 pos; varying vec2 uv;\n"
		"void main() { uv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5); gl_Position = vec4(pos, 0.0, 1.0); }\n";
	static const char *fs =
		"precision mediump float; varying vec2 uv; uniform sampler2D ty, tu, tv;\n"
		"void main() {\n"
		"  float y = 1.164 * (texture2D(ty, uv).r - 0.0625);\n"
		"  float u = texture2D(tu, uv).r - 0.5, v = texture2D(tv, uv).r - 0.5;\n"
		"  gl_FragColor = vec4(y + 1.596 * v, y - 0.391 * u - 0.813 * v, y + 2.018 * u, 1.0);\n"
		"}\n";
	program = gl.CreateProgram();
	gl.AttachShader(program, shader(GL_VERTEX_SHADER, vs));
	gl.AttachShader(program, shader(GL_FRAGMENT_SHADER, fs));
	gl.BindAttribLocation(program, 0, "pos");
	gl.LinkProgram(program);
	gl.UseProgram(program);
	gl.Uniform1i(gl.GetUniformLocation(program, "ty"), 0);
	gl.Uniform1i(gl.GetUniformLocation(program, "tu"), 1);
	gl.Uniform1i(gl.GetUniformLocation(program, "tv"), 2);
	gl.GenTextures(3, tex);
	for (int i = 0; i < 3; i++) {
		gl.ActiveTexture(GL_TEXTURE0 + i);
		gl.BindTexture(GL_TEXTURE_2D, tex[i]);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, 0, quad);
	gl.EnableVertexAttribArray(0);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

/* a plane into its texture; rows copied when the decoder pads them */
static void upload(int i, const uint8_t *p, int stride, int w, int h, uint8_t *tmp)
{
	if (stride != w) {
		for (int y = 0; y < h; y++)
			memcpy(tmp + y * w, p + y * stride, w);
		p = tmp;
	}
	gl.ActiveTexture(GL_TEXTURE0 + i);
	gl.BindTexture(GL_TEXTURE_2D, tex[i]);
	gl.TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, p);
}

static void draw(const AVFrame *f, uint8_t *tmp, int fresh)
{
	int sw = egl_width(), sh = egl_height();
	gl.Viewport(0, 0, sw, sh);
	gl.ClearColor(0, 0, 0, 1);
	gl.Clear(GL_COLOR_BUFFER_BIT);
	if (!f || !f->width)
		return; /* black until the first picture */
	/* fit the screen, keeping the video's shape (letterbox); the width as
	 * shown, from the sample aspect ratio: GTA III's 860x480 are anamorphic
	 * 4:3 (32:43) */
	AVRational sar = f->sample_aspect_ratio.num > 0 ? f->sample_aspect_ratio : stream_sar;
	int dw = sar.num > 0 && sar.den > 0 ? (int)((int64_t)f->width * sar.num / sar.den) : f->width;
	int vw = sw, vh = sw * f->height / dw;
	if (vh > sh) {
		vh = sh;
		vw = sh * dw / f->height;
	}
	gl.Viewport((sw - vw) / 2, (sh - vh) / 2, vw, vh);
	if (fresh) {
		upload(0, f->data[0], f->linesize[0], f->width, f->height, tmp);
		upload(1, f->data[1], f->linesize[1], f->width / 2, f->height / 2, tmp);
		upload(2, f->data[2], f->linesize[2], f->width / 2, f->height / 2, tmp);
	}
	gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* ---- sound: decoded AAC (planar float) as 16-bit stereo in a queue of
 * OpenAL buffers on one source ---- */

#define AUDIO_BUFFERS 24
#define AUDIO_CHUNK 4096 /* frames a buffer, ~93 ms */

typedef struct {
	ALCdevice *device;
	ALCcontext *context, *previous;
	ALuint source, buffers[AUDIO_BUFFERS];
	int free_buffers[AUDIO_BUFFERS], nfree;
	int16_t *pending;
	int npending, cap; /* frames */
	int rate, started;
} Audio;

static int audio_open(Audio *a, int rate)
{
	memset(a, 0, sizeof(*a));
	a->rate = rate;
	a->previous = alcGetCurrentContext();
	if (!a->previous) {
		/* the game has no sound yet: a device of our own for the video */
		a->device = alcOpenDevice(NULL);
		if (!a->device)
			return 0;
		a->context = alcCreateContext(a->device, NULL);
		if (!a->context || !alcMakeContextCurrent(a->context))
			return 0;
	}
	alGetError(); /* an error the game's own calls left behind isn't ours */
	alGenSources(1, &a->source);
	alGenBuffers(AUDIO_BUFFERS, a->buffers);
	alSourcef(a->source, AL_GAIN, 1.0f);
	alSourcei(a->source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f(a->source, AL_POSITION, 0, 0, 0);
	for (int i = 0; i < AUDIO_BUFFERS; i++)
		a->free_buffers[a->nfree++] = a->buffers[i];
	return alGetError() == AL_NO_ERROR;
}

static void audio_add(Audio *a, const AVFrame *f)
{
	int n = f->nb_samples, ch = f->ch_layout.nb_channels;
	if (a->npending + n > a->cap) {
		a->cap = (a->npending + n) * 2;
		a->pending = realloc(a->pending, a->cap * 2 * sizeof(int16_t));
	}
	int16_t *out = a->pending + a->npending * 2;
	for (int i = 0; i < n; i++)
		for (int c = 0; c < 2; c++) {
			float v = f->format == AV_SAMPLE_FMT_FLTP ? ((const float *)f->data[c < ch ? c : 0])[i]
				: f->format == AV_SAMPLE_FMT_FLT ? ((const float *)f->data[0])[i * ch + (c < ch ? c : 0)]
				: 0.0f;
			int s = (int)(v * 32767.0f);
			out[i * 2 + c] = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
		}
	a->npending += n;
}

/* processed buffers back to the free list, pending sound into free buffers */
static void audio_pump(Audio *a, int flush)
{
	ALint done = 0;
	alGetSourcei(a->source, AL_BUFFERS_PROCESSED, &done);
	while (done-- > 0) {
		ALuint b;
		alSourceUnqueueBuffers(a->source, 1, &b);
		a->free_buffers[a->nfree++] = b;
	}
	int off = 0;
	while (a->nfree && (a->npending - off >= AUDIO_CHUNK || (flush && a->npending > off))) {
		int n = a->npending - off < AUDIO_CHUNK ? a->npending - off : AUDIO_CHUNK;
		ALuint b = a->free_buffers[--a->nfree];
		alBufferData(b, AL_FORMAT_STEREO16, a->pending + off * 2, n * 4, a->rate);
		alSourceQueueBuffers(a->source, 1, &b);
		off += n;
	}
	if (off) {
		memmove(a->pending, a->pending + off * 2, (a->npending - off) * 4);
		a->npending -= off;
	}
	if (a->started) {
		ALint state;
		alGetSourcei(a->source, AL_SOURCE_STATE, &state);
		if (state != AL_PLAYING) /* ran dry: go on */
			alSourcePlay(a->source);
	}
}

static void audio_close(Audio *a)
{
	if (!a->source)
		goto out;
	alSourceStop(a->source);
	alSourcei(a->source, AL_BUFFER, 0);
	alDeleteSources(1, &a->source);
	alDeleteBuffers(AUDIO_BUFFERS, a->buffers);
	if (a->context) {
		alcMakeContextCurrent(NULL);
		alcDestroyContext(a->context);
	}
	if (a->device)
		alcCloseDevice(a->device);
out:
	free(a->pending);
}

/* ---- playback: a thread decodes and keeps the picture due now in
 * `shown`; the game's render thread draws it at each swap (movie_frame) in
 * place of the game's frame, while the game waits on OS_MovieIsPlaying ---- */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static AVFrame *shown;           /* the picture due now (guarded by lock) */
static int shown_new;            /* not uploaded yet */
static volatile int playing;     /* a video is on */
static volatile int stop_asked;
static char movie_path[512], movie_name[256];
static uint8_t *upload_tmp;
static size_t upload_size;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* the game's name ("BullyOrig/Movies/AttractMode.mp4", "CTW_iPhone_intro",
 * "movies\Logo.mpg.m4v": backslashes read as slashes), relative to game/,
 * with .mp4 added when it has no extension */
static int find_file(const char *name, char *path, size_t size)
{
	char fixed[256];
	snprintf(fixed, sizeof(fixed), "%s", name);
	for (char *c = fixed; *c; c++)
		if (*c == '\\')
			*c = '/';
	name = fixed;
	while (*name == '/')
		name++;
	snprintf(path, size, "%s/%s", fs_root, name);
	if (access(path, R_OK) == 0)
		return 1;
	const char *dot = strrchr(name, '.');
	if (!dot || strchr(dot, '/')) {
		snprintf(path, size, "%s/%s.mp4", fs_root, name);
		return access(path, R_OK) == 0;
	}
	return 0;
}

static AVCodecContext *open_decoder(AVFormatContext *fmt, int idx, int threads)
{
	const AVCodec *codec = avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
	if (!codec)
		return NULL;
	AVCodecContext *c = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(c, fmt->streams[idx]->codecpar);
	c->thread_count = threads;
	if (avcodec_open2(c, codec, NULL) < 0) {
		avcodec_free_context(&c);
		return NULL;
	}
	return c;
}

static void *decode_thread(void *arg)
{
	AVFormatContext *fmt = NULL;
	AVCodecContext *vdec = NULL, *adec = NULL;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	Audio audio = { 0 };
	int frames = 0, dropped = 0;
	const char *name = movie_name;
	if (avformat_open_input(&fmt, movie_path, NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
		log_printf("movie %s: can't open\n", movie_path);
		goto out;
	}
	int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (vi < 0 || !(vdec = open_decoder(fmt, vi, 3))) {
		log_printf("movie %s: no video stream\n", movie_path);
		goto out;
	}
	if (ai >= 0 && (adec = open_decoder(fmt, ai, 1)) && !audio_open(&audio, adec->sample_rate)) {
		log_printf("movie %s: no sound (OpenAL)\n", movie_path);
		audio_close(&audio);
		memset(&audio, 0, sizeof(audio));
		avcodec_free_context(&adec);
	}
	AVRational vtb = fmt->streams[vi]->time_base;
	stream_sar = fmt->streams[vi]->sample_aspect_ratio.num > 0 ? fmt->streams[vi]->sample_aspect_ratio
		: fmt->streams[vi]->codecpar->sample_aspect_ratio;
	log_printf("movie %s: %dx%d, %s\n", movie_path, vdec->width, vdec->height, adec ? "with sound" : "no sound");

	int held = pad_buttons(); /* a button still down from the menu doesn't skip */
	double t0 = 0;
	double behind_since = 0;
	while (!stop_asked && av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == ai && adec) {
			if (avcodec_send_packet(adec, pkt) >= 0)
				while (avcodec_receive_frame(adec, frame) >= 0)
					audio_add(&audio, frame);
			audio_pump(&audio, 0);
		} else if (pkt->stream_index == vi && avcodec_send_packet(vdec, pkt) >= 0) {
			while (!stop_asked && avcodec_receive_frame(vdec, frame) >= 0) {
				double pts = frame->best_effort_timestamp == AV_NOPTS_VALUE ? 0
					: frame->best_effort_timestamp * av_q2d(vtb);
				if (!t0) {
					/* the clock and the sound start with the first picture */
					t0 = now() - pts;
					if (adec) {
						audio_pump(&audio, 1);
						alSourcePlay(audio.source);
						audio.started = 1;
					}
				}
				double late = now() - t0 - pts;
				if (late > 0.1) {
					dropped++; /* behind: decode on, hand over the next one in time */
					/* still a second behind after two seconds of it: too heavy
					 * for this decoder, stopped instead of a frozen picture */
					if (late > 1.0 && behind_since == 0)
						behind_since = now();
					else if (late <= 1.0)
						behind_since = 0;
					if (behind_since && now() - behind_since > 2.0) {
						log_printf("movie %s: can't keep up (%.1f s behind), stopped: convert it to the screen's width\n", name, late);
						stop_asked = 1;
					}
				} else {
					behind_since = 0;
					while (late < -0.002 && !stop_asked) {
						if (adec)
							audio_pump(&audio, 0);
						usleep(late < -0.012 ? 10000 : 1000);
						late = now() - t0 - pts;
					}
					pthread_mutex_lock(&lock);
					av_frame_unref(shown);
					av_frame_ref(shown, frame);
					shown_new = 1;
					pthread_mutex_unlock(&lock);
					frames++;
				}
				int b = pad_buttons();
				held &= b;
				if ((b & ~held) & (0x1 | 0x10)) { /* A or Start */
					log_printf("movie %s: skipped at %.1f s\n", name, pts);
					stop_asked = 1;
				}
			}
		}
		av_packet_unref(pkt);
	}
	/* the sound's tail */
	if (adec && !stop_asked) {
		audio_pump(&audio, 1);
		for (;;) {
			ALint state, queued;
			alGetSourcei(audio.source, AL_SOURCE_STATE, &state);
			alGetSourcei(audio.source, AL_BUFFERS_QUEUED, &queued);
			if (state != AL_PLAYING || queued == 0 || stop_asked)
				break;
			usleep(10000);
		}
	}
	log_printf("movie %s: %d frames shown, %d dropped\n", name, frames, dropped);
out:
	if (audio.source || audio.context)
		audio_close(&audio);
	if (audio.previous)
		alcMakeContextCurrent(audio.previous);
	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&vdec);
	avcodec_free_context(&adec);
	avformat_close_input(&fmt);
	pthread_mutex_lock(&lock);
	av_frame_unref(shown);
	playing = 0;
	pthread_mutex_unlock(&lock);
	return NULL;
}

static int start(const char *name);
static void show_newest(void);

/* the video runs on its own; the game waits on movie_playing (Bully) or
 * goes on drawing (its swaps show the video) */
int movie_play(const char *name)
{
	return start(name);
}

/* the video plays before this returns, as a cutscene: the calling thread
 * (the game's, holding the screen) draws it and reads the pad meanwhile, so
 * the game doesn't run on under it. If that thread can't take the screen,
 * as movie_play. */
int movie_play_wait(const char *name)
{
	if (!start(name))
		return 0;
	if (!egl_movie_context(1)) {
		log_printf("movie %s: the calling thread can't take the screen, the game runs on\n", name);
		return 1;
	}
	setup_gl();
	while (playing) {
		pad_update();
		if (pad_quit_requested())
			exit_game(0);
		if (shown_new)
			show_newest();
		usleep(2000);
	}
	egl_movie_context(0);
	return 1;
}

static int start(const char *name)
{
	if (!name || !find_file(name, movie_path, sizeof(movie_path))) {
		log_printf("movie %s: no converted file, skipped\n", name ? name : "(null)");
		return 0;
	}
	if (playing || !load_gl())
		return 0;
	if (!shown)
		shown = av_frame_alloc();
	snprintf(movie_name, sizeof(movie_name), "%s", name);
	stop_asked = 0;
	shown_new = 0;
	playing = 1;
	pthread_t t;
	if (pthread_create(&t, NULL, decode_thread, NULL) != 0) {
		playing = 0;
		return 0;
	}
	pthread_detach(t);
	return 1;
}

int movie_playing(void) { return playing; }

/* GAME_MOVIE_TEST=<file>@<seconds> (test runs only): the game's own call for
 * a video that is hard to reach in play (end credits) is made that many
 * seconds after the first frame; the port's per-frame hook asks here and
 * calls its OS_MoviePlay hook with the name when it is due, once */
const char *movie_test_due(void)
{
	static int state; /* 0 not read, 1 waiting, 2 done or off */
	static double due;
	static char name[256];
	if (state == 2)
		return NULL;
	if (!state) {
		const char *e = getenv("GAME_MOVIE_TEST");
		const char *at = e ? strrchr(e, '@') : NULL;
		if (!e || !*e) {
			state = 2;
			return NULL;
		}
		snprintf(name, sizeof(name), "%.*s", at ? (int)(at - e) : (int)strlen(e), e);
		due = now() + (at ? atof(at + 1) : 20.0);
		state = 1;
		log_printf("movie: test: %s in %.0f s\n", name, due - now());
	}
	if (now() < due)
		return NULL;
	state = 2;
	return name;
}

/* the newest picture, in the movie context (current on this thread) */
static void show_newest(void)
{
	setup_gl();
	pthread_mutex_lock(&lock);
	size_t need = (size_t)shown->width * shown->height;
	if (need > upload_size) {
		free(upload_tmp);
		upload_tmp = malloc(need);
		upload_size = need;
	}
	draw(shown->width ? shown : NULL, upload_tmp, shown_new);
	shown_new = 0;
	pthread_mutex_unlock(&lock);
	egl_present();
}

/* from the game's render thread while it idles (it gets no frames while the
 * game thread waits for the video): takes the screen with the movie context
 * and shows the newest picture */
void movie_present(void)
{
	static int logged;
	if (!playing || !shown_new)
		return;
	if (!egl_movie_context(1)) {
		if (!logged++)
			log_printf("movie: render thread can't take the screen\n");
		return;
	}
	if (!logged++)
		log_printf("movie: drawn by the render thread\n");
	show_newest();
	egl_movie_context(0);
}

void movie_stop(void)
{
	stop_asked = 1;
}

/* from egl_swap, on the render thread: while a video is on, its picture is
 * drawn (in the movie context) instead of the game's frame; returns 1 when
 * it switched contexts, for movie_frame_done after the swap */
int movie_frame(void)
{
	if (!playing || !egl_movie_context(1))
		return 0;
	setup_gl();
	pthread_mutex_lock(&lock);
	size_t need = (size_t)shown->width * shown->height;
	if (need > upload_size) {
		free(upload_tmp);
		upload_tmp = malloc(need);
		upload_size = need;
	}
	draw(shown->width ? shown : NULL, upload_tmp, shown_new);
	shown_new = 0;
	pthread_mutex_unlock(&lock);
	return 1;
}

void movie_frame_done(void)
{
	egl_movie_context(0);
}

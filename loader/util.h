#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

/* The game's .so is armeabi-v7a softfp: floats travel in core registers.
 * Every function of ours that it calls with or for a float or double is
 * declared with this; the loader itself is armhf. */
#define SOFTFP __attribute__((pcs("aapcs")))

/* the game folder's game/ (the Android app's storage root) */
extern char fs_root[256];

int log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fatal_error(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
float env_float(const char *name, float def);
int env_int(const char *name, int def);

int ret0(void);
int ret1(void);
int retm1(void);

/* imports.c: the .so's imports (bionic, libm, GLES, EGL, zlib) */
int imports_load_gl(void);
int resolve_imports(void);
uintptr_t import_lookup(const char *name);
void *mali_sym(const char *name);

/* egl.c: GLES 2.0 on EGL on /dev/fb0, tear-free flips, FPS counter */
void egl_init(void);
int egl_make_current(void);
int egl_unmake_current(void);
void egl_present(void);
int egl_movie_context(int on);
int egl_is_render_thread(void);

/* movie.c */
int movie_play(const char *name);    /* starts it; 0: no such file */
int movie_play_wait(const char *name); /* as a cutscene: returns when it ends */
int movie_playing(void);
void movie_stop(void);
int movie_frame(void);               /* egl_swap: draw the video's picture */
void movie_frame_done(void);
void movie_present(void);              /* from the thread that holds the screen */
const char *movie_test_due(void);   /* GAME_MOVIE_TEST: a video to start now, or NULL */
void egl_swap(void);
void egl_frame_limit(int fps);
/* show a pointer at x, y (screen pixels from the top left; x < 0 hides it) */
void egl_cursor(int x, int y, int down);
void egl_shutdown(void);
int egl_width(void);
int egl_height(void);

/* loading.c: the loading screen Android's Java side draws */
void loading_show(void);
void loading_progress(float progress, int fake);
void loading_hide(void);
int loading_frame(void);

/* openal.c: the OpenAL Soft inside the .so redirected to the folder's */
void patch_openal(void);
/* an al* / alc* import of the game: the folder's OpenAL, softfp wrapped */
uintptr_t al_import(const char *name);

/* jni.c: a fake Java VM and JNIEnv for the game's JNI_OnLoad and Java calls */
void *jni_env(void);
void jni_load(void);

/* pad.c */
void pad_init(void);
void pad_update(void);
int pad_quit_requested(void);
int pad_buttons(void);
float pad_axis(int axis);

/* main.c */
void exit_game(int code) __attribute__((noreturn));

/* game.c: what differs per game */
extern const char *game_so_name;
extern const char *const game_required_files[];
void patch_game(void);
/* after JNI_OnLoad: start the game's threads and return */
void game_start(void);

/* Java methods the game calls through JNI, answered by game.c. The return
 * value is 32 bits (int, boolean, object as pointer); float methods return
 * through *f. args holds the call's arguments, one word each. */
typedef struct {
	const char *name;
	int (*call)(const uintptr_t *args, float *f);
} JavaMethod;
extern const JavaMethod game_java_methods[];

#endif

/* android.c -- what the three War Drum games (GTA SA, Chinatown Wars, Bully)
 * share above the loader: threads, the screen, and the Java methods they all
 * call for the window, storage, device facts and the pad. game.c hooks and
 * lists these. After the Vita loaders (Andy Nguyen, MIT). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "android.h"
#include "so_util.h"
#include "util.h"

typedef struct {
	int (*func)(void *);
	void *arg;
	char *handle;
} thread_start;

static void *thread_stub(void *p)
{
	thread_start s = *(thread_start *)p;
	free(p);
	s.handle[0x41] = 1; /* running */
	s.func(s.arg);
	return NULL;
}

/* The handle is War Drum's 0x48-byte thread record: +0x24 the thread, +0x41
 * the running flag. Threads are joinable for OS_ThreadWait. */
void *OS_ThreadLaunch(int (*func)(void *), void *arg, int cpu, const char *name, int unused, int priority)
{
	char *handle = calloc(1, 0x48);
	thread_start *s = malloc(sizeof(*s));
	s->func = func;
	s->arg = arg;
	s->handle = handle;
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, 1024 * 1024);
	pthread_t t;
	int r = pthread_create(&t, &a, thread_stub, s);
	pthread_attr_destroy(&a);
	if (r != 0) {
		free(s);
		free(handle);
		return NULL;
	}
	if (name)
		pthread_setname_np(t, name);
	*(pthread_t *)(handle + 0x24) = t;
	return handle;
}

void OS_ThreadWait(void *handle)
{
	if (handle)
		pthread_join(*(pthread_t *)((char *)handle + 0x24), NULL);
}

int OS_ScreenGetWidth(void) { return egl_width(); }
int OS_ScreenGetHeight(void) { return egl_height(); }

/* ---- Java methods (jni.c calls these by name) ---- */

int java_InitEGLAndGLES2(const uintptr_t *a, float *f) { egl_init(); return 1; }
int java_swapBuffers(const uintptr_t *a, float *f) { egl_swap(); return 1; }
int java_makeCurrent(const uintptr_t *a, float *f) { return egl_make_current(); }
int java_unMakeCurrent(const uintptr_t *a, float *f) { return egl_unmake_current(); }

int java_getAppLocalValue(const uintptr_t *a, float *f)
{
	const char *key = (const char *)a[0];
	if (key && strcmp(key, "STORAGE_ROOT") == 0)
		return (int)(uintptr_t)fs_root;
	return 0;
}

/* main (1) and patch (2) OBB, relative to the storage root */
int java_FileGetArchiveName(const uintptr_t *a, float *f)
{
	switch ((int)a[0]) {
	case 1: return (int)(uintptr_t)"/Android/main.obb";
	case 2: return (int)(uintptr_t)"/Android/patch.obb";
	}
	return 0;
}

int java_DeleteFile(const uintptr_t *a, float *f)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", fs_root, (const char *)a[0]);
	return unlink(path) == 0;
}

int java_GetDeviceInfo(const uintptr_t *a, float *f) { return 0; }

/* bits 6+: memory in MB, 0x1: phone; not a Tegra */
int java_GetDeviceType(const uintptr_t *a, float *f) { return (256 << 6) | (3 << 2) | 0x1; }

/* 0 English, 1 French, 2 German, 3 Italian, 4 Spanish, 5 Japanese */
int java_GetDeviceLocale(const uintptr_t *a, float *f) { return env_int("GAME_LANGUAGE", 0); }

/* 8: a PS3 pad (0, 5, 6: Xbox 360) */
int java_GetGamepadType(const uintptr_t *a, float *f)
{
	if (a[0] != 0)
		return -1;
	pad_update();
	return 8;
}

int java_GetGamepadButtons(const uintptr_t *a, float *f) { return a[0] == 0 ? pad_buttons() : 0; }
int java_GetGamepadAxis(const uintptr_t *a, float *f) { *f = a[0] == 0 ? pad_axis(a[1]) : 0.0f; return 0; }

int java_ignore(const uintptr_t *a, float *f) { return 0; }

int ProcessEvents(void)
{
	return pad_quit_requested(); /* 1 leaves the game */
}

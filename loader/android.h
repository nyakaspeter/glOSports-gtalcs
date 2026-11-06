#ifndef ANDROID_H
#define ANDROID_H

#include <stdint.h>

void *OS_ThreadLaunch(int (*func)(void *), void *arg, int cpu, const char *name, int unused, int priority);
void OS_ThreadWait(void *handle);
int OS_ScreenGetWidth(void);
int OS_ScreenGetHeight(void);
int ProcessEvents(void);

int java_InitEGLAndGLES2(const uintptr_t *a, float *f);
int java_swapBuffers(const uintptr_t *a, float *f);
int java_makeCurrent(const uintptr_t *a, float *f);
int java_unMakeCurrent(const uintptr_t *a, float *f);
int java_getAppLocalValue(const uintptr_t *a, float *f);
int java_FileGetArchiveName(const uintptr_t *a, float *f);
int java_DeleteFile(const uintptr_t *a, float *f);
int java_GetDeviceInfo(const uintptr_t *a, float *f);
int java_GetDeviceType(const uintptr_t *a, float *f);
int java_GetDeviceLocale(const uintptr_t *a, float *f);
int java_GetGamepadType(const uintptr_t *a, float *f);
int java_GetGamepadButtons(const uintptr_t *a, float *f);
int java_GetGamepadAxis(const uintptr_t *a, float *f);
int java_ignore(const uintptr_t *a, float *f);

/* the Java methods all three games call, for game_java_methods[] */
#define COMMON_JAVA_METHODS \
	{ "InitEGLAndGLES2", java_InitEGLAndGLES2 }, \
	{ "swapBuffers", java_swapBuffers }, \
	{ "makeCurrent", java_makeCurrent }, \
	{ "unMakeCurrent", java_unMakeCurrent }, \
	{ "getAppLocalValue", java_getAppLocalValue }, \
	{ "FileGetArchiveName", java_FileGetArchiveName }, \
	{ "DeleteFile", java_DeleteFile }, \
	{ "GetDeviceInfo", java_GetDeviceInfo }, \
	{ "GetDeviceType", java_GetDeviceType }, \
	{ "GetDeviceLocale", java_GetDeviceLocale }, \
	{ "GetGamepadType", java_GetGamepadType }, \
	{ "GetGamepadButtons", java_GetGamepadButtons }, \
	{ "GetGamepadAxis", java_GetGamepadAxis }

#endif

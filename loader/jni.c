/* jni.c -- a fake Java VM and JNIEnv. War Drum's Android layer reaches Java
 * for the window (InitEGLAndGLES2, swapBuffers), storage paths, the pad and
 * device facts; game.c answers those calls by method name. Every other JNI
 * function is a stub that names itself in the log the first time it is
 * called. After the Vita loaders' jni_patch.c (Andy Nguyen, MIT). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "so_util.h"
#include "util.h"

static const void *env_table[233];
static const void *const *env = env_table; /* a JNIEnv * is &env */
static const void *vm_table[8];
static const void *const *vm = vm_table;   /* a JavaVM * is &vm */

void *jni_env(void)
{
	return (void *)&env;
}

/* ---- stubs for the slots not implemented below ---- */

#define STUB(i, name) \
	static int stub_##name(void) { \
		static int seen; \
		if (!seen++) log_printf("jni: %s (slot %d) called, not implemented\n", #name, i); \
		return 0; \
	}
#include "jni_names.inc"
#undef STUB

static const void *const stubs[233] = {
#define STUB(i, name) [i] = (const void *)stub_##name,
#include "jni_names.inc"
#undef STUB
};

/* ---- methods: an ID is the index into game_java_methods, plus one ---- */

static int method_count(void)
{
	int n = 0;
	while (game_java_methods[n].name)
		n++;
	return n;
}

static int GetMethodID(void *e, void *cls, const char *name, const char *sig)
{
	for (int i = 0; game_java_methods[i].name; i++)
		if (strcmp(game_java_methods[i].name, name) == 0)
			return i + 1;
	log_printf("jni: method %s%s not provided\n", name, sig ? sig : "");
	return 0;
}

static int call(int id, va_list ap, float *f)
{
	uintptr_t args[8];
	for (int i = 0; i < 8; i++)
		args[i] = va_arg(ap, uintptr_t);
	if (id <= 0 || id > method_count())
		return 0;
	return game_java_methods[id - 1].call(args, f);
}

static int CallIntMethodV(void *e, void *obj, int id, va_list ap)
{
	float f;
	return call(id, ap, &f);
}

static int CallIntMethod(void *e, void *obj, int id, ...)
{
	va_list ap;
	va_start(ap, id);
	int r = CallIntMethodV(e, obj, id, ap);
	va_end(ap);
	return r;
}

static SOFTFP float CallFloatMethodV(void *e, void *obj, int id, va_list ap)
{
	float f = 0.0f;
	call(id, ap, &f);
	return f;
}

static SOFTFP float CallFloatMethod(void *e, void *obj, int id, ...)
{
	va_list ap;
	va_start(ap, id);
	float r = CallFloatMethodV(e, obj, id, ap);
	va_end(ap);
	return r;
}

static void CallVoidMethodV(void *e, void *obj, int id, va_list ap)
{
	float f;
	call(id, ap, &f);
}

static void CallVoidMethod(void *e, void *obj, int id, ...)
{
	va_list ap;
	va_start(ap, id);
	CallVoidMethodV(e, obj, id, ap);
	va_end(ap);
}

/* ---- objects and strings: a string is its UTF-8 bytes ---- */

static void *FindClass(void *e, const char *name) { return (void *)0x43434343; }
static void *NewGlobalRef(void *e, void *obj) { return obj ? obj : (void *)0x42424242; }
static char *NewStringUTF(void *e, const char *bytes) { return bytes ? strdup(bytes) : NULL; }
static const char *GetStringUTFChars(void *e, const char *s, unsigned char *copy) { if (copy) *copy = 0; return s; }
static int GetStringUTFLength(void *e, const char *s) { return s ? strlen(s) : 0; }
static int GetJavaVM(void *e, void **out) { *out = (void *)&vm; return 0; }

static int natives_count;

typedef struct { const char *name, *sig; void *fn; } JNINativeMethod;
static const JNINativeMethod *natives;

static int RegisterNatives(void *e, void *cls, const JNINativeMethod *methods, int n)
{
	natives = methods;
	natives_count = n;
	for (int i = 0; i < n; i++)
		log_printf("jni: native %s%s\n", methods[i].name, methods[i].sig);
	return 0;
}

/* ---- the VM ---- */

static int GetEnv(void *v, void **out, int version) { *out = (void *)&env; return 0; }
static int AttachCurrentThread(void *v, void **out, void *args) { *out = (void *)&env; return 0; }

static void setup(void)
{
	for (int i = 0; i < 233; i++)
		env_table[i] = stubs[i];
#define SET(i, f) env_table[i] = (const void *)(f)
	SET(6, FindClass);            /* classes are never looked at, but must not be NULL */
	SET(15, ret0);                /* ExceptionOccurred */
	SET(17, ret0);                /* ExceptionClear */
	SET(21, NewGlobalRef);
	SET(22, ret0);                /* DeleteGlobalRef */
	SET(23, ret0);                /* DeleteLocalRef */
	SET(31, ret0);                /* GetObjectClass */
	SET(33, GetMethodID);
	SET(113, GetMethodID);        /* GetStaticMethodID */
	/* Call<type>Method / V for the instance (34-63) and static (114-143)
	 * methods: object, boolean, byte, char, short, int, long, float,
	 * double, void, three slots each (plain, V, A) */
	for (int base = 34; base <= 114; base += 80) {
		for (int t = 0; t < 10; t++) {
			int slot = base + t * 3;
			if (t == 7) {          /* float */
				SET(slot, CallFloatMethod);
				SET(slot + 1, CallFloatMethodV);
			} else if (t == 9) {   /* void */
				SET(slot, CallVoidMethod);
				SET(slot + 1, CallVoidMethodV);
			} else if (t != 6 && t != 8) { /* long and double stay stubs */
				SET(slot, CallIntMethod);
				SET(slot + 1, CallIntMethodV);
			}
		}
	}
	SET(94, ret0);                /* GetFieldID */
	SET(144, ret0);               /* GetStaticFieldID */
	SET(167, NewStringUTF);
	SET(168, GetStringUTFLength);
	SET(169, GetStringUTFChars);
	SET(170, ret0);               /* ReleaseStringUTFChars */
	SET(215, RegisterNatives);
	SET(219, GetJavaVM);
	SET(228, ret0);               /* ExceptionCheck */
#undef SET
	for (int i = 0; i < 8; i++)
		vm_table[i] = (const void *)ret0;
	vm_table[4] = (const void *)AttachCurrentThread;
	vm_table[5] = (const void *)ret0; /* DetachCurrentThread */
	vm_table[6] = (const void *)GetEnv;
	vm_table[7] = (const void *)AttachCurrentThread; /* ...AsDaemon */
}

/* JNI_OnLoad finds the game's Java classes and methods; game.c then makes
 * the calls the Java side would (paths, display size, frames). */
void jni_load(void)
{
	setup();
	int (*JNI_OnLoad)(void *, void *) = (void *)so_find_addr("JNI_OnLoad");
	JNI_OnLoad((void *)&vm, NULL);
	game_start();
}

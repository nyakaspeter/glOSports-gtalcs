/* openal.c -- the OpenAL Soft built into the game's .so (with an Android
 * OpenSL ES backend) is bypassed: each of its entry points jumps to the same
 * function of the OpenAL Soft in the folder's lib/ (ALSA, settings in
 * alsoft.conf). The entry points are found by walking the .so's symbols;
 * those that pass or return floats go through SOFTFP wrappers, and one of
 * those without a wrapper is left alone and reported. Carried over from
 * glOSports-maxpayne's list version. */
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>
#include <dlfcn.h>
#include <string.h>

#include "so_util.h"
#include "util.h"

/* The game stops a source and deletes its buffers right away, without
 * unqueueing them first; do that for it (from the Vita loaders). A stream
 * may already hold the first buffer of its next track by then (the intro's
 * first cutscene after the menu music): the whole queue comes off and the
 * buffers that stay are queued again in order, or the next track would be
 * played from an empty queue and stay silent. */
static ALuint last_stopped_src;

static void unqueue_deleted(ALuint src, ALsizei n, const ALuint *bufs)
{
	ALuint q[64];
	ALint queued = 0;
	alGetSourcei(src, AL_BUFFERS_QUEUED, &queued);
	if (queued <= 0 || queued > 64)
		return;
	alGetError();
	alSourceUnqueueBuffers(src, queued, q);
	if (alGetError() != AL_NO_ERROR)
		return; /* not all processed (a rewound source): leave it as it is */
	for (ALint i = 0; i < queued; i++) {
		ALsizei j = 0;
		while (j < n && bufs[j] != q[i])
			j++;
		if (j == n)
			alSourceQueueBuffers(src, 1, &q[i]);
	}
}

static void alSourceStopHook(ALuint src)
{
	last_stopped_src = src;
	alSourceStop(src);
}

static void alDeleteBuffersHook(ALsizei n, const ALuint *bufs)
{
	if (last_stopped_src) {
		ALint type = 0;
		alGetSourcei(last_stopped_src, AL_SOURCE_TYPE, &type);
		if (type == AL_STREAMING)
			unqueue_deleted(last_stopped_src, n, bufs);
		else
			alSourcei(last_stopped_src, AL_BUFFER, 0);
		last_stopped_src = 0;
	}
	alDeleteBuffers(n, bufs);
}

/* The game asks for a context at its own rate (22050 Hz); mix at GAME_AUDIO_RATE. */
static ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *attrs)
{
	ALCint a[32];
	int n = 0;
	int rate = env_int("GAME_AUDIO_RATE", 44100);
	for (int i = 0; attrs && attrs[i] && n < 28; i += 2) {
		if (attrs[i] == ALC_FREQUENCY && rate > 0)
			continue;
		a[n++] = attrs[i];
		a[n++] = attrs[i + 1];
	}
	if (rate > 0) {
		a[n++] = ALC_FREQUENCY;
		a[n++] = rate;
	}
	a[n] = 0;
	return alcCreateContext(dev, a);
}

#define W3(n, T) static SOFTFP void w_##n(ALuint o, ALenum p, T v) { n(o, p, v); }
#define W5(n, T) static SOFTFP void w_##n(ALuint o, ALenum p, T x, T y, T z) { n(o, p, x, y, z); }
W3(alAuxiliaryEffectSlotf, ALfloat) W3(alBufferf, ALfloat) W3(alEffectf, ALfloat)
W3(alFilterf, ALfloat) W3(alSourcef, ALfloat) W3(alSourcedSOFT, ALdouble)
W5(alBuffer3f, ALfloat) W5(alSource3f, ALfloat) W5(alSource3dSOFT, ALdouble)
static SOFTFP void w_alListenerf(ALenum p, ALfloat v) { alListenerf(p, v); }
static SOFTFP void w_alListener3f(ALenum p, ALfloat x, ALfloat y, ALfloat z) { alListener3f(p, x, y, z); }
static SOFTFP void w_alDopplerFactor(ALfloat v) { alDopplerFactor(v); }
static SOFTFP void w_alDopplerVelocity(ALfloat v) { alDopplerVelocity(v); }
static SOFTFP void w_alSpeedOfSound(ALfloat v) { alSpeedOfSound(v); }
static SOFTFP ALfloat w_alGetFloat(ALenum p) { return alGetFloat(p); }
static SOFTFP ALdouble w_alGetDouble(ALenum p) { return alGetDouble(p); }
#undef W3
#undef W5

static void *alGetProcAddressHook(const ALchar *name);
static void *alcGetProcAddressHook(ALCdevice *dev, const ALCchar *name);

static const DynLibFunction al_special[] = {
#define S(n) { #n, (uintptr_t)w_##n }
	S(alAuxiliaryEffectSlotf), S(alBufferf), S(alEffectf), S(alFilterf), S(alSourcef),
	S(alSourcedSOFT), S(alBuffer3f), S(alSource3f), S(alSource3dSOFT), S(alListenerf),
	S(alListener3f), S(alDopplerFactor), S(alDopplerVelocity), S(alSpeedOfSound),
	S(alGetFloat), S(alGetDouble),
#undef S
	{ "alSourceStop", (uintptr_t)alSourceStopHook },
	{ "alDeleteBuffers", (uintptr_t)alDeleteBuffersHook },
	{ "alcCreateContext", (uintptr_t)alcCreateContextHook },
	{ "alGetProcAddress", (uintptr_t)alGetProcAddressHook },
	{ "alcGetProcAddress", (uintptr_t)alcGetProcAddressHook },
};

/* AL functions that pass a float or double by value: the object setters
 * al<Object>f / 3f / d / 3d (SOFT), plus the global setters and getters.
 * Getters (alGetSourcef ...) return theirs through a pointer, and names
 * like alSourceRewind or alIsEnabled only end in a 'd': those go straight
 * through (leaving them to the game's own OpenAL, which never gets a
 * context, made the first cutscene's stream stay silent) */
static int takes_float(const char *name)
{
	static const char *objects[] = { "Source", "Listener", "Buffer", "Effect", "Filter", "AuxiliaryEffectSlot" };
	static const char *suffixes[] = { "f", "3f", "dSOFT", "3dSOFT" };
	if (!strcmp(name, "alDopplerFactor") || !strcmp(name, "alDopplerVelocity") ||
	    !strcmp(name, "alSpeedOfSound") || !strcmp(name, "alGetFloat") || !strcmp(name, "alGetDouble"))
		return 1;
	if (strncmp(name, "al", 2))
		return 0;
	for (size_t i = 0; i < sizeof(objects) / sizeof(*objects); i++) {
		size_t n = strlen(objects[i]);
		if (strncmp(name + 2, objects[i], n))
			continue;
		for (size_t j = 0; j < sizeof(suffixes) / sizeof(*suffixes); j++)
			if (!strcmp(name + 2 + n, suffixes[j]))
				return 1;
	}
	return 0;
}

static void *openal;

static uintptr_t al_target(const char *name)
{
	for (size_t i = 0; i < sizeof(al_special) / sizeof(*al_special); i++)
		if (strcmp(al_special[i].symbol, name) == 0)
			return al_special[i].func;
	if (takes_float(name))
		return 0;
	return (uintptr_t)dlsym(openal, name);
}

/* names the game looks up get our entry points (wrappers included) */
static void *alGetProcAddressHook(const ALchar *name)
{
	uintptr_t a = al_target(name);
	if (!a)
		log_printf("alGetProcAddress(%s): not provided\n", name);
	return (void *)a;
}

static void *alcGetProcAddressHook(ALCdevice *dev, const ALCchar *name)
{
	return alGetProcAddressHook(name);
}

static int is_al_name(const char *n)
{
	return (n[0] == 'a' && n[1] == 'l' && ((n[2] >= 'A' && n[2] <= 'Z') ||
		(n[2] == 'c' && n[3] >= 'A' && n[3] <= 'Z')));
}

static void load_openal(void)
{
	if (openal)
		return;
	openal = dlopen("libopenal.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!openal)
		fatal_error("can't load lib/libopenal.so.1: %s", dlerror());
}

/* LCS imports OpenAL instead of carrying its own: its al* imports resolve
 * here (the float setters to the softfp wrappers above) */
uintptr_t al_import(const char *name)
{
	load_openal();
	return al_target(name);
}

void patch_openal(void)
{
	load_openal();
	uintptr_t s = so_symbol("InitializeCriticalSection");
	if (s)
		hook_addr(s, (uintptr_t)ret0);
	int hooked = 0, skipped = 0;
	for (int i = 0; i < so_symbol_count(); i++) {
		uintptr_t addr;
		const char *name = so_function_name(i, &addr);
		if (!name || !is_al_name(name))
			continue;
		if (!dlsym(openal, name))
			continue; /* an internal helper of the game's OpenAL */
		uintptr_t target = al_target(name);
		if (target) {
			hook_addr(addr, target);
			hooked++;
		} else {
			log_printf("openal: %s left to the game's own OpenAL (floats, no wrapper)\n", name);
			skipped++;
		}
	}
	log_printf("openal: %d entry points redirected, %d left\n", hooked, skipped);
}

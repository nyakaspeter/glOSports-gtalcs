/* game.c -- Grand Theft Auto: Liberty City Stories, Rockstar's Android build
 * (libGTALcs.so of 2016-07-15, md5 9c1fd961, with OBBs main.17 and patch.15).
 * Unlike the older War Drum games it has no NVEvent main loop: Java's
 * GLSurfaceView renderer drives it (GTAJNIlib viewOnInit, setDisplaySize,
 * viewOnDrawFrame every frame, GL context current), and the Java side
 * hands it paths and device facts through GTAJNIlib's setters. The loader
 * makes those calls itself. The OBBs are the game's own encrypted WAD
 * archives, which it mounts and reads itself (LogicalFS). */
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#include <zlib.h>

#include "android.h"
#include "so_util.h"
#include "util.h"

const char *game_so_name = "libGTALcs.so";
/* and the game's files: files/ (unpacked) or main.obb and patch.obb */
const char *const game_required_files[] = {
	"libGTALcs.so", "assets/xml/audio_data.xml", NULL,
};

/* CommonAPI, the Java methods AndroidUtil_JNIInit looks up. Strings are
 * their UTF-8 bytes (jni.c). No videos, music playlist or Social Club. */
static int java_GetDeviceLanguage(const uintptr_t *a, float *f) { return (int)(uintptr_t)"en"; }
static int java_GetVersionName(const uintptr_t *a, float *f) { return (int)(uintptr_t)"1.0"; }
static int java_QuitApp(const uintptr_t *a, float *f) { exit_game(0); }

/* OS_ApplicationTick's startup waits in state 2 for gbSocialClubStarted,
 * which hal::Main::main sets once Java has brought up Social Club's views:
 * no Social Club, the flag right away */
static int java_StartSocialClub(const uintptr_t *a, float *f)
{
	*(uint8_t *)so_find_addr("gbSocialClubStarted") = 1;
	return 0;
}

static int java_HideLoadingScreen(const uintptr_t *a, float *f) { loading_hide(); return 0; }

/* the loading screen (loading.c): the game asks Java to show it and passes
 * the progress */
static void OS_ShowLoadingScreen(void)
{
	/* not during the startup (OS_ApplicationTick's states 0-7: on Android
	 * the intro movie covers it, here the picture only flashed up before
	 * the menu) */
	static const int *game_state;
	if (!game_state)
		game_state = (const int *)so_find_addr("gGameState");
	if (*game_state >= 8)
		loading_show();
}
static SOFTFP void OS_UpdateLoadingBar(float progress, int fake) { loading_progress(progress, fake); }

const JavaMethod game_java_methods[] = {
	{ "GetDeviceLanguage", java_GetDeviceLanguage },
	{ "GetVersionName", java_GetVersionName },
	{ "QuitApp", java_QuitApp },
	{ "PlayVideo", java_ignore },
	{ "isVideoPlaying", java_ignore },
	{ "PlaylistInit", java_ignore },
	{ "PlaylistPlay", java_ignore },
	{ "PlaylistPause", java_ignore },
	{ "PlaylistPlaying", java_ignore },
	{ "PlaylistHasSongs", java_ignore },
	{ "GooglePlaylistAvailable", java_ignore },
	{ "GoToUrl", java_ignore },
	{ "ShowLoadingScreen", java_ignore },
	{ "HideLoadingScreen", java_HideLoadingScreen },
	{ "ShowLoadingBar", java_ignore },
	{ "ShowLoadingBarProgressHack", java_ignore },
	{ "StartSocialClub", java_StartSocialClub },
	{ "CallSocialClubUpdate", java_ignore },
	{ "CallSocialClubSignOut", java_ignore },
	{ "RemoveFrameRateLock", java_ignore },
	{ "setKeepScreenOn", java_ignore },
	{ "setInUI", java_ignore },
	{ NULL, NULL },
};

/* The pad. Buttons go in the way the Java side sends them: GTAJNIlib's
 * onJoyButtonDown / onJoyButtonUp with War Drum's button index (pad.c's bit
 * order: A, B, X, Y, Start, Back, LB, RB, up, down, left, right, L3, R3),
 * which set the joypadButtons[] that WarGamepad_GetGamepadButtons(button)
 * reads and queue the button events the menus act on. The sticks and
 * triggers go straight to WarGamepad_GetGamepadAxis; type 0 is an Xbox 360
 * pad, the GA36-MB's layout. */
static int WarGamepad_GetGamepadType(void)
{
	return 0;
}

static SOFTFP float WarGamepad_GetGamepadAxis(int axis)
{
	return pad_axis(axis);
}

/* The game switches to the pad (prompts, menu navigation) when
 * Touchscreen::Update sees pad input: GTouchscreen->+8. That runs on the
 * title screen, which the legal screen skip leaves out, so it is set here;
 * there is no touch screen to switch back to. */
static void use_pad(void)
{
	static uint8_t **touchscreen;
	if (!touchscreen)
		touchscreen = (uint8_t **)so_find_addr("GTouchscreen");
	if (touchscreen && *touchscreen)
		(*touchscreen)[8] = 1;
}

static void send_buttons(void *env)
{
	static void (*down)(void *, void *, int, int), (*up)(void *, void *, int, int);
	static int held;
	if (!down) {
		down = (void *)so_find_addr("Java_com_rockstargames_gtalcs_GTAJNIlib_onJoyButtonDown");
		up = (void *)so_find_addr("Java_com_rockstargames_gtalcs_GTAJNIlib_onJoyButtonUp");
	}
	pad_update();
	int now = pad_buttons(), changed = now ^ held;
	for (int i = 0; i < 16; i++)
		if (changed & (1 << i))
			((now & (1 << i)) ? down : up)(env, NULL, 0, i);
	held = now;
}

#define JAVA(name) "Java_com_rockstargames_gtalcs_GTAJNIlib_" #name

static void *java(const char *name)
{
	void *f = (void *)so_find_addr(name);
	if (!f)
		fatal_error("%s missing", name);
	return f;
}

/* The stats block (clock, money, health, weapon, wanted level) and the
 * help box are drawn in the touch layout's summary and tut buttons
 * (GTouchscreen +0x78, +0x7c: x, y, width, height from +4, in layout units,
 * times +680 / +684 for screen pixels). Placed every frame as in
 * glOSports-gtavc: the stats block's button 30 pixels in from the right and
 * 36 down, the help box at 32, 36. */
static void place_hud(void)
{
	static uint8_t **touchscreen;
	if (!touchscreen)
		touchscreen = (uint8_t **)so_find_addr("GTouchscreen");
	uint8_t *ts = *touchscreen;
	if (!ts)
		return;
	float sx = *(float *)(ts + 680), sy = *(float *)(ts + 684);
	float *summary = *(float **)(ts + 0x78), *tut = *(float **)(ts + 0x7c);
	if (sx <= 0.0f || sy <= 0.0f)
		return;
	if (summary) {
		summary[1] = (OS_ScreenGetWidth() - 30.0f) / sx - summary[3];
		summary[2] = 36.0f / sy;
	}
	if (tut) {
		tut[1] = 32.0f / sx;
		tut[2] = 36.0f / sy;
	}
}

/* Game detail (m_PrefsGameDetail) picks a row of three tables in
 * gDeviceDetailSettings (C_DeviceDetailSettings), 0 Low to 2 High:
 *   - world and model cull/fade (+0x00 and +0x34, 16 bytes a row): within
 *     the first distance everything is drawn; beyond it an object has to
 *     cover a part of the screen growing from the fourth value to the third
 *     over the second distance (CalcScreenSize, Lucid::CalcModelScreenSize);
 *   - LOD (+0x68, 76 bytes a row, as the game's dvLODSettings<level>_*):
 *     vehicle LOD 0 and 1 and fade distance, hidden component distance, big
 *     vehicle LOD 0 and 1, ped LOD and fade distance (floats); max peds,
 *     max peds inside, max random cars, max parked cars (ints); population
 *     cull and spawn radii, driver and boat rider detail ranges (floats).
 * High stays as it is; Low and Medium draw and populate less than the
 * game's own rows: distances and counts times DETAIL_LOW / DETAIL_MEDIUM,
 * screen parts divided by it. The fade distances stay at least the spawn
 * radius (cars and peds would appear in view), the radii stay the game's.
 * Set every frame from the game's values, taken once
 * C_DeviceDetailSettings::Initialise has filled the tables. */
#define DETAIL_LOW 0.5f
#define DETAIL_MEDIUM 0.75f

static void shorten_detail(void)
{
	static uint8_t *dds;
	static float world[2][2][4];
	static uint32_t lod[2][19];
	static int taken;
	if (!dds)
		dds = (uint8_t *)so_find_addr("gDeviceDetailSettings");
	if (!dds)
		return;
	if (!taken) {
		if (*(float *)(dds + 0x20 + 4) <= 0.0f) /* High's world distance: not filled yet */
			return;
		for (int r = 0; r < 2; r++) {
			memcpy(world[r][0], dds + r * 16, 16);
			memcpy(world[r][1], dds + 0x34 + r * 16, 16);
			memcpy(lod[r], dds + 0x68 + r * 76, 76);
		}
		taken = 1;
	}
	for (int r = 0; r < 2; r++) {
		float k = r ? DETAIL_MEDIUM : DETAIL_LOW;
		for (int t = 0; t < 2; t++) {
			float *row = (float *)(dds + (t ? 0x34 : 0) + r * 16);
			row[0] = world[r][t][0] * k;
			row[1] = world[r][t][1] * k;
			row[2] = world[r][t][2] / k;
			row[3] = world[r][t][3] / k;
		}
		uint32_t *row = (uint32_t *)(dds + 0x68 + r * 76);
		float spawn, f;
		memcpy(&spawn, &lod[r][16], 4);
		for (int i = 0; i < 8; i++) {
			memcpy(&f, &lod[r][i], 4);
			f *= k;
			if ((i == 2 || i == 7) && f < spawn)
				f = spawn;
			memcpy(&row[i], &f, 4);
		}
		for (int i = 8; i < 12; i++) {
			uint32_t n = (uint32_t)(lod[r][i] * k + 0.5f);
			row[i] = n ? n : 1;
		}
	}
}

static void *render_thread(void *arg)
{
	void *env = jni_env();
	egl_init();
	/* GAME_FPS_CAP (start.sh): the game's own Frame Limiter is only stored,
	 * and the mobile menu has no entry for it */
	egl_frame_limit(env_int("GAME_FPS_CAP", 0));
	void (*viewOnInit)(void *, void *) = java(JAVA(viewOnInit));
	void (*setDisplaySize)(void *, void *, int, int) = java(JAVA(setDisplaySize));
	SOFTFP void (*viewOnDrawFrame)(void *, void *, float) = java(JAVA(viewOnDrawFrame));
	void (*viewOnResume)(void *, void *) = java(JAVA(viewOnResume));
	viewOnInit(env, NULL);
	setDisplaySize(env, NULL, egl_width(), egl_height()); /* InitGame */
	viewOnResume(env, NULL); /* as Android's onResume after the start */
	log_printf("game: initialised, %dx%d\n", egl_width(), egl_height());

	struct timespec last, now;
	clock_gettime(CLOCK_MONOTONIC, &last);
	for (;;) {
		if (pad_quit_requested())
			exit_game(0);
		clock_gettime(CLOCK_MONOTONIC, &now);
		float ms = (now.tv_sec - last.tv_sec) * 1e3f + (now.tv_nsec - last.tv_nsec) / 1e6f;
		last = now;
		send_buttons(env);
		use_pad();
		place_hud();
		shorten_detail();
		viewOnDrawFrame(env, NULL, ms);
		if (!loading_frame())
			egl_swap();
	}
	return NULL;
}

void game_start(void)
{
	void *env = jni_env();
	char path[512];
	void (*set_str)(void *, void *, const char *);

	((void (*)(void *, void *, void *))java(JAVA(setAssetManager)))(env, NULL, (void *)1);
	/* the main OBB first, then the patch: _LogicalFS_Init mounts them in this order */
	set_str = java(JAVA(setGameFilesDir));
	snprintf(path, sizeof(path), "%s/main.obb", fs_root);
	set_str(env, NULL, strdup(path));
	snprintf(path, sizeof(path), "%s/patch.obb", fs_root);
	set_str(env, NULL, strdup(path));
	/* saves and settings; the game appends file names directly */
	set_str = java(JAVA(setPrivateFilesDir));
	snprintf(path, sizeof(path), "%s/", fs_root);
	set_str(env, NULL, strdup(path));
	/* CPU kHz (the GA36-MB's A33 at 1.2 GHz picks the lowest detail index),
	 * then model, board and GPU strings */
	((void (*)(void *, void *, int, const char *, const char *, const char *))java(JAVA(setDeviceInfo)))
		(env, NULL, 1200000, "GA36-MB", "GA36-MB", "Mali-400");
	((void (*)(void *, void *, int))java(JAVA(setOSVersion)))(env, NULL, 19);
	((void (*)(void *, void *, int))java(JAVA(setHasVibrator)))(env, NULL, 0);
	((void (*)(void *, void *, int))java(JAVA(setIsTVDevice)))(env, NULL, 0);
	((void (*)(void *, void *, int))java(JAVA(setNoJoysticks)))(env, NULL, 1);

	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, 4 * 1024 * 1024);
	pthread_t t;
	if (pthread_create(&t, &a, render_thread, NULL) != 0)
		fatal_error("can't start the render thread");
	pthread_attr_destroy(&a);
}

/* The radar where glOSports-gtavc / glOSports-gta3 show it (measured
 * there: ring centred 76 in and 93 up, about 100 across). The game places
 * it from the touch layout's radar button (top left) through these four
 * CHud getters, left, top, width and height in screen pixels, and draws it
 * 25 pixels below the centre they give. */
#define RADAR_SIZE 96.0f
static SOFTFP float CHud_GetRadarPosX(void) { return 76.0f - RADAR_SIZE / 2; }
static SOFTFP float CHud_GetRadarPosY(void) { return OS_ScreenGetHeight() - 93.0f - RADAR_SIZE / 2 - 25.0f; }
static SOFTFP float CHud_GetRadarSize(void) { return RADAR_SIZE; }

/* Lock-on follows GTA SA's rules, as in glOSports-gtavc
 * (CPlayerPed::FindWeaponLockOnTarget, EvaluateTarget,
 * FindNextWeaponLockOnTarget, EvaluateNeighbouringTarget). This game, from
 * the PSP code, took only peds in front of the player within 40 m
 * (CPed::OurPedCanSeeThisOne, in line of sight) and scored them by
 * distance plus 5 x the angle off the player's facing. SA scores a
 * candidate in weapon range twice: off the player's facing, and when it is
 * on screen off the camera's heading (here from the camera to the player);
 * the better counts. A score is 1 - 0.785 x the angle off (none past about
 * 73 degrees), over the fourth root of the distance, times 0.8 for the
 * game's aiming priorities (IsThisPedAnAimingPriority, and the player's
 * threats) and 0.1 for the rest, and counts only in line of sight
 * (buildings and objects, the game's own test from OurPedCanSeeThisOne).
 * The next target was already SA's (the nearest angle from the camera on
 * the asked side, within 50 degrees); it now also needs line of sight. The
 * two searches' OurPedCanSeeThisOne calls answer yes. Entities: CMatrix at
 * +16 (forward +32, position +64); the ped's weapon slot at +1736, its
 * weapons at +1444, 28 bytes each; TheCamera's position at +48. */
typedef struct { float x, y, z, w; } CVector4;
static int (*CPlayerPed__DoesTargetHaveToBeBroken)(char *ped, const CVector4 *pos, void *weapon);
static int (*CEntity__GetIsOnScreen)(char *entity);
static int (*CWorld__ProcessLineOfSight)(const CVector4 *a, const CVector4 *b, void *colpoint, void **hit,
	int buildings, int vehicles, int peds, int objects, int dummies, int see_through, int camera_ignore,
	int shoot_through, int b9);
static float *camera_pos;

static float angle_of(float dx, float dy)
{
	float a = atan2f(dy, dx);
	return a < 0.0f ? a + 6.28318531f : a;
}

static float limit_angle(float a)
{
	while (a > 3.14159265f)
		a -= 6.28318531f;
	while (a < -3.14159265f)
		a += 6.28318531f;
	return a;
}

/* in range (on the ground, as the game measures) and holdable by the weapon */
static int can_lock(char *ped, char *target, float max_dist, float *dist)
{
	float *p = (float *)(ped + 64), *t = (float *)(target + 64);
	float dx = t[0] - p[0], dy = t[1] - p[1];
	*dist = sqrtf(dx * dx + dy * dy);
	if (*dist > max_dist)
		return 0;
	int slot = *(int8_t *)(ped + 1736);
	return !CPlayerPed__DoesTargetHaveToBeBroken(ped, (const CVector4 *)t, ped + 1444 + slot * 28);
}

/* the game's line of sight test: 1 m above each, buildings and objects */
static int in_sight(char *ped, char *target)
{
	float *p = (float *)(ped + 64), *t = (float *)(target + 64);
	CVector4 from = { p[0], p[1], p[2] + 1.0f, p[3] }, to = { t[0], t[1], t[2] + 1.0f, t[3] };
	uint8_t colpoint[128] __attribute__((aligned(16)));
	void *hit = NULL;
	return !CWorld__ProcessLineOfSight(&from, &to, colpoint, &hit, 1, 0, 0, 1, 0, 0, 0, 1, 0);
}

static float sa_score(float angle, float dist)
{
	float s = 1.0f - 0.785398f * fabsf(angle);
	if (s <= 0.0f)
		return 0.0f;
	return dist > 1.0f ? s / sqrtf(sqrtf(dist)) : s;
}

/* heading: the player's facing, as atan2 of its forward vector */
static SOFTFP void evaluate_target(char *ped, char *target, char **best, float *best_score,
	float max_dist, float heading, int priority)
{
	float dist;
	if (!can_lock(ped, target, max_dist, &dist))
		return;
	float *p = (float *)(ped + 64), *t = (float *)(target + 64);
	float to_target = angle_of(t[0] - p[0], t[1] - p[1]);
	float score = sa_score(limit_angle(to_target - heading), dist);
	if (CEntity__GetIsOnScreen(target)) {
		float camera = angle_of(p[0] - camera_pos[0], p[1] - camera_pos[1]);
		float s = sa_score(limit_angle(to_target - camera), dist);
		if (s > score)
			score = s;
	}
	score *= priority ? 0.8f : 0.1f;
	if (score > 0.0f && score > *best_score && in_sight(ped, target)) {
		*best = target;
		*best_score = score;
	}
}

/* the game's own, plus line of sight: heading is from the camera to the
 * current target */
static SOFTFP void evaluate_neighbouring_target(char *ped, char *target, char **best, float *best_score,
	float max_dist, float heading, int left, int unused)
{
	float dist;
	if (!can_lock(ped, target, max_dist, &dist))
		return;
	float *t = (float *)(target + 64);
	float a = limit_angle(angle_of(t[0] - camera_pos[0], t[1] - camera_pos[1]) - heading);
	if (fabsf(a) >= 0.872665f || (left ? a <= 0.0f : a >= 0.0f))
		return;
	if (-fabsf(a) > *best_score && in_sight(ped, target)) {
		*best = target;
		*best_score = -fabsf(a);
	}
}

/* While locked on, CPlayerPed::ProcessPlayerWeapon moves the lock with
 * CPad::ShiftTargetLeft/RightJustDown: the PSP's D-pad left / right while
 * the target button is held. A flick of the right stick past half (from
 * inside half) does it too, as in GTA SA. */
static int (*CPad__GetTarget)(void *pad);
static int (*CPad__DPadLeftJustDown)(void *pad), (*CPad__DPadRightJustDown)(void *pad);

static int flicked(float *last, int left)
{
	float x = pad_axis(2), was = *last;
	*last = x;
	return left ? (x <= -0.5f && was > -0.5f) : (x >= 0.5f && was < 0.5f);
}

static int shift_target_left(void *pad)
{
	static float last;
	int flick = flicked(&last, 1);
	if (*(uint16_t *)((char *)pad + 162) || !CPad__GetTarget(pad))
		return 0;
	return CPad__DPadLeftJustDown(pad) || flick;
}

static int shift_target_right(void *pad)
{
	static float last;
	int flick = flicked(&last, 0);
	if (*(uint16_t *)((char *)pad + 162) || !CPad__GetTarget(pad))
		return 0;
	return CPad__DPadRightJustDown(pad) || flick;
}

static void patch_lock_on(void)
{
	CPlayerPed__DoesTargetHaveToBeBroken = (void *)so_find_addr("_ZN10CPlayerPed24DoesTargetHaveToBeBrokenE7CVectorP7CWeapon");
	CEntity__GetIsOnScreen = (void *)so_find_addr("_ZN7CEntity13GetIsOnScreenEv");
	CWorld__ProcessLineOfSight = (void *)so_find_addr("_ZN6CWorld18ProcessLineOfSightERK7CVectorS2_R9CColPointRP7CEntitybbbbbbbbb");
	camera_pos = (float *)((char *)so_find_addr("TheCamera") + 48);
	hook_sym("_ZN10CPlayerPed14EvaluateTargetEP7CEntityPS1_Pfffb", evaluate_target);
	hook_sym("_ZN10CPlayerPed26EvaluateNeighbouringTargetEP7CEntityPS1_Pfffbb", evaluate_neighbouring_target);
	/* the two searches' "bl CPed::OurPedCanSeeThisOne" become "mov r0, #1" */
	uintptr_t sees = so_find_addr("_ZN4CPed19OurPedCanSeeThisOneEP7CEntityb");
	static const uintptr_t calls[] = { 0x427f6c, 0x42832c };
	for (size_t i = 0; i < sizeof(calls) / sizeof(*calls); i++) {
		uint32_t *ins = (uint32_t *)(so_base + calls[i]);
		uintptr_t to = so_base + calls[i] + 8 + ((int32_t)(*ins << 8) >> 6);
		if ((*ins >> 24) == 0xeb && to == sees)
			*ins = 0xe3a00001;
		else
			log_printf("game: lock-on pre-check at %#x not patched\n", (unsigned)calls[i]);
	}
	CPad__GetTarget = (void *)so_find_addr("_ZN4CPad9GetTargetEv");
	CPad__DPadLeftJustDown = (void *)so_find_addr("_ZN4CPad16DPadLeftJustDownEv");
	CPad__DPadRightJustDown = (void *)so_find_addr("_ZN4CPad17DPadRightJustDownEv");
	hook_sym("_ZN4CPad23ShiftTargetLeftJustDownEv", shift_target_left);
	hook_sym("_ZN4CPad24ShiftTargetRightJustDownEv", shift_target_right);
}

/* A function's start, run elsewhere: its first two instructions (which
 * must not depend on the pc) and a jump back, for calling the original of
 * a function hooked with hook_arm. NULL when they aren't the ones given. */
static void *trampoline(const char *name, uint32_t first, uint32_t second)
{
	uint32_t *f = (uint32_t *)so_find_addr(name);
	if (f[0] != first || f[1] != second)
		return NULL;
	uint32_t *t = mmap(NULL, 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (t == MAP_FAILED)
		return NULL;
	t[0] = first;
	t[1] = second;
	t[2] = 0xe51ff004;	/* ldr pc, [pc, #-4] */
	t[3] = (uint32_t)(uintptr_t)(f + 2);
	__builtin___clear_cache((char *)t, (char *)(t + 4));
	return t;
}

/* The game's files, unpacked: game/files/ holds what main.obb and
 * patch.obb would (the patch's over the main's; tools/unpack.py), so a
 * change is a file copied, not an OBB rebuilt and sent. Every file the
 * game opens by name goes through LogicalFS_OpenBundleFile, which looks
 * in each mounted WAD; here game/files/<name> comes first, opened as the
 * game opens a WAD's own file (Platform::FileOpenOSFilePath; HOME is vfat,
 * so names match in any case). With game/files/ there, the OBBs aren't
 * mounted (or needed). */
static char files_root[512];
static void *(*open_bundle_file)(const char *, int);
static void *(*open_os_file)(const char *, int, int);
static int (*mount_wad)(const char *, const char *);
static int file_log;	/* GTALCS_FILELOG=1 (testing): every name opened */
static void *open_unpacked(const char *name, int arg)
{
	char path[1024];
	void *f = NULL;
	if (files_root[0]) {
		snprintf(path, sizeof(path), "%s%s%s", files_root, name[0] == '/' ? "" : "/", name);
		for (char *p = path; *p; p++)
			if (*p == '\\')
				*p = '/';
		f = open_os_file(path, 0, arg);
		if (!f) {
			/* the OBBs' files whose names tools/unpack.py couldn't recover:
			 * files/_byhash/<the WAD's hash of the name: ~crc32 of it in lower
			 * case> */
			char low[512];
			size_t n = 0;
			for (; name[n] && n < sizeof(low) - 1; n++)
				low[n] = name[n] == '\\' ? '/' : tolower((unsigned char)name[n]);
			snprintf(path, sizeof(path), "%s/_byhash/%08x", files_root,
				(unsigned)~crc32(0, (const unsigned char *)low, n));
			f = open_os_file(path, 0, arg);
		}
	}
	if (!f)
		f = open_bundle_file(name, arg);
	if (file_log)
		log_printf("file: %s%s\n", name, f ? "" : " (none)");
	return f;
}

static int mount_wad_unless_unpacked(const char *path, const char *mount_point)
{
	return files_root[0] ? 1 : mount_wad(path, mount_point);
}

static void patch_unpacked_files(void)
{
	char path[512];
	struct stat st;
	snprintf(path, sizeof(path), "%s/files", fs_root);
	int unpacked = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
	file_log = env_int("GTALCS_FILELOG", 0);
	if (!unpacked) {
		snprintf(path, sizeof(path), "%s/main.obb", fs_root);
		if (stat(path, &st) < 0)
			fatal_error("%s/files is missing: see README.md for the game files", fs_root);
		if (!file_log)
			return;
	}
	open_os_file = (void *)so_find_addr("_ZN8Platform18FileOpenOSFilePathEPKc8FileModei");
	open_bundle_file = trampoline("_Z24LogicalFS_OpenBundleFilePKci", 0xe92d4ff0, 0xe28db020);
	mount_wad = trampoline("_Z38LogicalFS_MountWadFromPhysicalFilePathPKcS0_", 0xe92d4830, 0xe1a05001);
	if (!open_bundle_file || !mount_wad) {
		log_printf("game: the file system isn't as expected, files/ not used\n");
		return;
	}
	hook_sym("_Z24LogicalFS_OpenBundleFilePKci", open_unpacked);
	if (!unpacked)
		return;
	strcpy(files_root, path);
	hook_sym("_Z38LogicalFS_MountWadFromPhysicalFilePathPKcS0_", mount_wad_unless_unpacked);
	log_printf("game: the game's files from %s, no OBBs\n", path);
}

/* the intro (Movies/LCS_INTRO_1080.m4v, from the APK's res/raw, converted to 640x480:
 * tools/movies.sh): on Android a Java player shows it while
 * OS_ApplicationTick's startup polls OS_MovieIsPlaying. movie.c plays it; the
 * poll shows the newest picture when its thread holds the screen, a render
 * thread's sleeps do otherwise (w_usleep in imports.c). A or Start skips. */
static SOFTFP int OS_MoviePlay(const char *file, int a2, int a3, float a4)
{
	movie_play(file);
	return 0;
}

static int OS_MovieIsPlaying(int *a)
{
	if (egl_is_render_thread())
		movie_present();
	return movie_playing();
}

void patch_game(void)
{
	hook_sym("_Z12OS_MoviePlayPKcbbf", OS_MoviePlay);
	hook_sym("_Z17OS_MovieIsPlayingPi", OS_MovieIsPlaying);
	patch_lock_on();
	/* Social Club's own screens (the main menu's Rockstar button, sign
	 * out) need the SocialClub object hal::Main::main makes for Java's
	 * views, which never runs here: they do nothing */
	hook_sym("_ZN14SocialServices17InvokedFromIngameEbPKc", ret0);
	hook_sym("_ZN14SocialServices7SignOutEv", ret0);
	hook_sym("_Z20OS_ShowLoadingScreenv", OS_ShowLoadingScreen);
	hook_sym("_Z19OS_UpdateLoadingBarfb", OS_UpdateLoadingBar);
	patch_unpacked_files();
	/* the legal screen counts as shown, as in glOSports-gta3 */
	*(uint8_t *)so_find_addr("shownLegalScreen") = 1;
	/* never ask for a rating (CheckForAppRate sets the request every third
	 * time it is asked) */
	hook_sym("_Z15CheckForAppRatev", ret0);
	hook_sym("_ZN4CHud12GetRadarPosXEv", CHud_GetRadarPosX);
	hook_sym("_ZN4CHud12GetRadarPosYEv", CHud_GetRadarPosY);
	hook_sym("_ZN4CHud13GetRadarSizeXEv", CHud_GetRadarSize);
	hook_sym("_ZN4CHud13GetRadarSizeYEv", CHud_GetRadarSize);
	/* The graphics detail setting renders the 3D view at 50% (low), 75%
	 * (medium) or 100% (high) of the screen, but GetOffscreenResScale
	 * keeps it at 720 lines or more (its literal 720.0f): on the GA36-MB's
	 * 480 lines every setting was full size. No minimum: the setting
	 * applies here as on bigger screens. */
	uint32_t *min_lines = (uint32_t *)(so_base + 0x265274);
	if (*min_lines == 0x44340000)
		*min_lines = 0;
	else
		log_printf("game: GetOffscreenResScale's 720 lines not where expected, left alone\n");
	hook_sym("_Z25WarGamepad_GetGamepadAxisi", WarGamepad_GetGamepadAxis);
	hook_sym("_Z25WarGamepad_GetGamepadTypev", WarGamepad_GetGamepadType);
}

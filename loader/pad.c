/* pad.c -- the GA36-MB's pad through SDL's game controller API (layout from
 * SDL_GAMECONTROLLERCONFIG in start.sh), presented to the game as the PS3
 * pad of War Drum's GetGamepad* calls. Carried over from glOSports-maxpayne.
 *
 * Stick shaping, from start.sh:
 *   GAME_STICK_SCALE     multiplies stick values (the GA36-MB's sticks reach ~2/3)
 *   GAME_STICK_DEADZONE  radial dead zone after scaling (0..1), rescaled to 0..1 */
#include <SDL.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>

#include "util.h"

static SDL_GameController *pad;
static float stick_scale = 1.0f, dead_zone = 0.12f;
static int quit;
static float axes[6];
static int buttons;

/* appd stops a game with SIGTERM. SDL would turn it into SDL_QUIT for the
 * game to act on, but GTA SA's own shutdown stops its threads and never ends
 * the process (the GPU driver then stays in use): end it here. The kernel
 * frees the GPU and glos-gpu puts the driver back. */
static void on_term(int sig)
{
	(void)sig;
	_exit(0);
}

void pad_init(void)
{
	stick_scale = env_float("GAME_STICK_SCALE", 1.0f);
	dead_zone = env_float("GAME_STICK_DEADZONE", 0.12f);
	if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
		log_printf("pad: SDL_Init failed: %s\n", SDL_GetError());
		return;
	}
	signal(SIGTERM, on_term); /* after SDL_Init, which installs its own */
	signal(SIGINT, on_term);
	for (int i = 0; i < SDL_NumJoysticks(); i++)
		if (SDL_IsGameController(i) && (pad = SDL_GameControllerOpen(i)))
			break;
	if (pad)
		log_printf("pad: %s (scale %.2f, dead zone %.2f)\n", SDL_GameControllerName(pad), stick_scale, dead_zone);
	else
		log_printf("pad: no game controller\n");
}

static void stick(SDL_GameControllerAxis ax, SDL_GameControllerAxis ay, float *out)
{
	float x = SDL_GameControllerGetAxis(pad, ax) / 32767.0f * stick_scale;
	float y = SDL_GameControllerGetAxis(pad, ay) / 32767.0f * stick_scale;
	float len = sqrtf(x * x + y * y);
	if (len <= dead_zone) {
		out[0] = out[1] = 0.0f;
		return;
	}
	float scaled = (len - dead_zone) / (1.0f - dead_zone);
	if (scaled > 1.0f)
		scaled = 1.0f;
	out[0] = x / len * scaled;
	out[1] = y / len * scaled;
}

void pad_update(void)
{
	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		if (ev.type == SDL_QUIT)
			quit = 1;
		else if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad)
			pad = SDL_GameControllerOpen(ev.cdevice.which);
	}
	if (!pad) {
		buttons = 0;
		memset(axes, 0, sizeof(axes));
		return;
	}
	static const struct { SDL_GameControllerButton b; int mask; } map[] = {
		{ SDL_CONTROLLER_BUTTON_A, 0x1 },     /* cross */
		{ SDL_CONTROLLER_BUTTON_B, 0x2 },     /* circle */
		{ SDL_CONTROLLER_BUTTON_X, 0x4 },     /* square */
		{ SDL_CONTROLLER_BUTTON_Y, 0x8 },     /* triangle */
		{ SDL_CONTROLLER_BUTTON_START, 0x10 },
		{ SDL_CONTROLLER_BUTTON_BACK, 0x20 },
		{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x40 },
		{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x80 },
		{ SDL_CONTROLLER_BUTTON_DPAD_UP, 0x100 },
		{ SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x200 },
		{ SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x400 },
		{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x800 },
		{ SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x1000 },
		{ SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x2000 },
	};
	int b = 0;
	for (size_t i = 0; i < sizeof(map) / sizeof(*map); i++)
		if (SDL_GameControllerGetButton(pad, map[i].b))
			b |= map[i].mask;
	buttons = b;
	stick(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &axes[0]);
	stick(SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, &axes[2]);
	axes[4] = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000 ? 1.0f : 0.0f;
	axes[5] = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000 ? 1.0f : 0.0f;
}

int pad_quit_requested(void) { return quit; }
int pad_buttons(void) { return buttons; }
float pad_axis(int axis) { return axis >= 0 && axis < 6 ? axes[axis] : 0.0f; }

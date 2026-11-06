/* main.c -- load a War Drum Android game's .so (armeabi-v7a) into this
 * process, point its imports at goodluckOS, patch its Android layer and run
 * it. Carried over from glOSports-maxpayne; after the Vita loaders' main.c
 * (Andy Nguyen, MIT). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>
#include <SDL.h>

#include "so_util.h"
#include "util.h"

void exit_game(int code)
{
	egl_shutdown();
	SDL_Quit();
	fflush(stdout);
	fflush(stderr);
	_exit(code);
}

/* "lib+offset" for an address: the game's .so, or any library through dladdr */
static void where(char *buf, size_t len, uintptr_t a)
{
	Dl_info info;
	if (a >= so_base && a < so_base + so_span)
		snprintf(buf, len, "%s+0x%x", game_so_name, a - so_base);
	else if (dladdr((void *)a, &info) && info.dli_fname)
		snprintf(buf, len, "%s+0x%x %s", basename((char *)info.dli_fname),
			a - (uintptr_t)info.dli_fbase, info.dli_sname ? info.dli_sname : "");
	else
		snprintf(buf, len, "?");
}

static void crash_handler(int sig, siginfo_t *si, void *uc_)
{
	ucontext_t *uc = uc_;
	uintptr_t pc = uc->uc_mcontext.arm_pc, lr = uc->uc_mcontext.arm_lr;
	char wpc[128], wlr[128];
	where(wpc, sizeof(wpc), pc);
	where(wlr, sizeof(wlr), lr);
	fprintf(stderr, "loader: signal %d at %p (fault address %p)\n"
		"  pc 0x%08x (%s)  lr 0x%08x (%s)  sp 0x%08lx\n",
		sig, (void *)pc, si->si_addr, pc, wpc, lr, wlr, uc->uc_mcontext.arm_sp);
	fprintf(stderr, "  r0 %08lx r1 %08lx r2 %08lx r3 %08lx r4 %08lx r5 %08lx r6 %08lx\n"
		"  r7 %08lx r8 %08lx r9 %08lx r10 %08lx fp %08lx ip %08lx\n",
		uc->uc_mcontext.arm_r0, uc->uc_mcontext.arm_r1, uc->uc_mcontext.arm_r2,
		uc->uc_mcontext.arm_r3, uc->uc_mcontext.arm_r4, uc->uc_mcontext.arm_r5,
		uc->uc_mcontext.arm_r6, uc->uc_mcontext.arm_r7, uc->uc_mcontext.arm_r8,
		uc->uc_mcontext.arm_r9, uc->uc_mcontext.arm_r10, uc->uc_mcontext.arm_fp,
		uc->uc_mcontext.arm_ip);
	/* return addresses into the game's code on the stack: a rough backtrace */
	const uintptr_t *sp = (const uintptr_t *)uc->uc_mcontext.arm_sp;
	int n = 0;
	for (int i = 0; i < 1024 && n < 24; i++) {
		uintptr_t a = sp[i];
		/* Thumb return addresses are odd, ARM ones (LCS is ARM code) even */
		if (a >= so_base + 0x1000 && a < so_text_end) {
			fprintf(stderr, "  stack: %s+0x%x\n", game_so_name, a - so_base);
			n++;
		}
	}
	fflush(stderr);
	_exit(128 + sig);
}

static void check_data(void)
{
	char path[512];
	struct stat st;
	for (int i = 0; game_required_files[i]; i++) {
		snprintf(path, sizeof(path), "%s/%s", fs_root, game_required_files[i]);
		if (stat(path, &st) < 0)
			fatal_error("%s is missing: see README.md for the game files", path);
	}
}

int main(int argc, char **argv)
{
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n <= 0)
		fatal_error("can't find the game folder");
	exe[n] = '\0';
	snprintf(fs_root, sizeof(fs_root), "%s/game", dirname(exe));
	if (chdir(fs_root) < 0)
		fatal_error("%s is missing: see README.md for the game files", fs_root);
	check_data();

	struct sigaction sa = { 0 };
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);

	char path[512];
	snprintf(path, sizeof(path), "%s/%s", fs_root, game_so_name);
	int r = so_load(path);
	if (r < 0)
		fatal_error("can't load %s (%d)", path, r);
	log_printf("%s at %p\n", game_so_name, (void *)so_base);

	if (imports_load_gl() < 0)
		fatal_error("can't load ARM's GLES driver");
	int missing = resolve_imports();
	if (missing)
		log_printf("%d imports not provided\n", missing);

	so_make_text_writable(1);
	patch_openal();
	patch_game();
	so_make_text_writable(0);
	so_flush_caches();

	pad_init();
	so_execute_init_array();

	/* starts the game's own main thread and returns. This thread stays:
	 * appctl kill-application (cgroup.kill) signals each process's first
	 * thread, and one that has exited drops the SIGKILL, leaving the game
	 * running with the GPU driver loaded. */
	jni_load();
	for (;;)
		pause();
}

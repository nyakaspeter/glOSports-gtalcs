/* imports.c -- the imports of the game's .so, resolved to goodluckOS's glibc,
 * libm, zlib and ARM's Mali driver. Carried over from glOSports-maxpayne and
 * extended for the War Drum GTA / Bully builds.
 *
 * Bionic (Android's libc) and glibc share the kernel's numbers (errno,
 * open flags, syscalls, ioctls) and the base calling convention, so most
 * imports go straight through. What differs:
 *   - floats: the .so is softfp, so every import that takes or returns a
 *     float or double gets a SOFTFP wrapper;
 *   - object sizes: bionic's pthread mutex, cond and sem are one word, so the
 *     word holds a pointer to the glibc object (made on first use, as the
 *     game also uses bionic's static initialisers); pthread_attr_t, stat,
 *     FILE, sigset_t and mbstate_t are emulated or translated;
 *   - data: __sF (stdin/out/err), _ctype_, __stack_chk_guard.
 * Imports not in the table: every gl / egl name resolves to ARM's driver
 * (those taking floats through wrappers); anything else is reported at load.
 * After the Vita loaders' import tables (fgsfds, Andy Nguyen, MIT). */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <fenv.h>
#include <locale.h>
#include <malloc.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>

#include "so_util.h"
#include "util.h"

/* ---- stdio: bionic's __sF[3] (84-byte FILEs) and stderr ---- */

static uint8_t fake_sF[3][0x54];
static FILE *fake_stderr = (FILE *)fake_sF[2];

static FILE *F(FILE *f)
{
	if ((uint8_t *)f == fake_sF[0]) return stdin;
	if ((uint8_t *)f == fake_sF[1]) return stdout;
	if ((uint8_t *)f == fake_sF[2]) return stderr;
	return f;
}

static int w_fclose(FILE *f) { return fclose(F(f)); }
static size_t w_fread(void *p, size_t s, size_t n, FILE *f) { return fread(p, s, n, F(f)); }
static size_t w_fwrite(const void *p, size_t s, size_t n, FILE *f) { return fwrite(p, s, n, F(f)); }
static int w_fseek(FILE *f, long o, int w) { return fseek(F(f), o, w); }
static long w_ftell(FILE *f) { return ftell(F(f)); }
static int w_feof(FILE *f) { return feof(F(f)); }
static int w_ferror(FILE *f) { return ferror(F(f)); }
static int w_fflush(FILE *f) { return fflush(f ? F(f) : NULL); }
static int w_fgetc(FILE *f) { return fgetc(F(f)); }
static char *w_fgets(char *s, int n, FILE *f) { return fgets(s, n, F(f)); }
static int w_fputc(int c, FILE *f) { return fputc(c, F(f)); }
static int w_fputs(const char *s, FILE *f) { return fputs(s, F(f)); }
static int w_ungetc(int c, FILE *f) { return ungetc(c, F(f)); }
static int w_setvbuf(FILE *f, char *b, int m, size_t s) { return setvbuf(F(f), b, m, s); }
static void w_rewind(FILE *f) { rewind(F(f)); }
static int w_fileno(FILE *f) { return fileno(F(f)); }
static int w_vfprintf(FILE *f, const char *fmt, va_list ap) { return vfprintf(F(f), fmt, ap); }
static int w_fscanf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vfscanf(F(f), fmt, ap);
	va_end(ap);
	return n;
}
static wint_t w_getwc(FILE *f) { return getwc(F(f)); }
static wint_t w_putwc(wchar_t c, FILE *f) { return putwc(c, F(f)); }
static wint_t w_ungetwc(wint_t c, FILE *f) { return ungetwc(c, F(f)); }

static int w_fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vfprintf(F(f), fmt, ap);
	va_end(ap);
	return n;
}

static int w_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vfprintf(stdout, fmt, ap);
	va_end(ap);
	return n;
}

static int w_android_log_print(int prio, const char *tag, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	size_t len = strlen(buf);
	return fprintf(stderr, "%s: %s%s", tag, buf, len && buf[len - 1] == '\n' ? "" : "\n");
}

/* ---- files ---- */

/* bionic's struct stat on ARM (the kernel's stat64) */
struct bionic_stat {
	uint64_t st_dev;
	uint8_t pad0[4];
	uint32_t st_ino32;
	uint32_t st_mode, st_nlink, st_uid, st_gid;
	uint64_t st_rdev;
	uint8_t pad3[4];
	int64_t st_size;
	uint32_t st_blksize;
	uint64_t st_blocks;
	uint32_t st_atime_, st_atime_nsec, st_mtime_, st_mtime_nsec, st_ctime_, st_ctime_nsec;
	uint64_t st_ino;
};
_Static_assert(offsetof(struct bionic_stat, st_mtime_) == 0x50, "bionic stat layout");
_Static_assert(sizeof(struct bionic_stat) == 104, "bionic stat size");

static void to_bionic_stat(const struct stat64 *s, struct bionic_stat *b)
{
	memset(b, 0, sizeof(*b));
	b->st_dev = s->st_dev;
	b->st_ino32 = (uint32_t)s->st_ino;
	b->st_ino = s->st_ino;
	b->st_mode = s->st_mode;
	b->st_nlink = s->st_nlink;
	b->st_uid = s->st_uid;
	b->st_gid = s->st_gid;
	b->st_rdev = s->st_rdev;
	b->st_size = s->st_size;
	b->st_blksize = s->st_blksize;
	b->st_blocks = s->st_blocks;
	b->st_atime_ = s->st_atim.tv_sec;
	b->st_atime_nsec = s->st_atim.tv_nsec;
	b->st_mtime_ = s->st_mtim.tv_sec;
	b->st_mtime_nsec = s->st_mtim.tv_nsec;
	b->st_ctime_ = s->st_ctim.tv_sec;
	b->st_ctime_nsec = s->st_ctim.tv_nsec;
}

static int w_stat(const char *path, struct bionic_stat *b)
{
	struct stat64 s;
	int r = stat64(path, &s);
	if (r == 0)
		to_bionic_stat(&s, b);
	return r;
}

static int w_fstat(int fd, struct bionic_stat *b)
{
	struct stat64 s;
	int r = fstat64(fd, &s);
	if (r == 0)
		to_bionic_stat(&s, b);
	return r;
}

static int w_open(const char *path, int flags, int mode) { return open(path, flags, mode); }
/* glibc's struct dirent64 has bionic's struct dirent layout */
static struct dirent64 *w_readdir(DIR *d) { return readdir64(d); }

/* ---- pthread ---- */

/* bionic's static initialisers */
#define BIONIC_MUTEX_RECURSIVE_INIT 0x4000
#define BIONIC_MUTEX_ERRORCHECK_INIT 0x8000
#define IS_HEAP(v) ((v) > 0x10000)

static pthread_mutex_t *new_mutex(int type)
{
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, type);
	pthread_mutex_t *m = malloc(sizeof(*m));
	pthread_mutex_init(m, &a);
	pthread_mutexattr_destroy(&a);
	return m;
}

static pthread_mutex_t *mutex_get(volatile uintptr_t *slot)
{
	uintptr_t v = *slot;
	if (IS_HEAP(v))
		return (pthread_mutex_t *)v;
	int type = v == BIONIC_MUTEX_RECURSIVE_INIT ? PTHREAD_MUTEX_RECURSIVE :
		v == BIONIC_MUTEX_ERRORCHECK_INIT ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL;
	pthread_mutex_t *m = new_mutex(type);
	if (__sync_bool_compare_and_swap(slot, v, (uintptr_t)m))
		return m;
	pthread_mutex_destroy(m);
	free(m);
	return (pthread_mutex_t *)*slot;
}

/* bionic's pthread_mutexattr_t is the type number itself (same numbers) */
static int w_mutexattr_init(int *a) { *a = PTHREAD_MUTEX_NORMAL; return 0; }
static int w_mutexattr_settype(int *a, int type) { *a = type; return 0; }

static int w_mutex_init(uintptr_t *slot, const int *attr)
{
	*slot = (uintptr_t)new_mutex(attr ? *attr : PTHREAD_MUTEX_NORMAL);
	return 0;
}

static int w_mutex_destroy(uintptr_t *slot)
{
	if (IS_HEAP(*slot)) {
		pthread_mutex_destroy((pthread_mutex_t *)*slot);
		free((void *)*slot);
	}
	*slot = 0;
	return 0;
}

static int w_mutex_lock(uintptr_t *slot) { return pthread_mutex_lock(mutex_get(slot)); }
static int w_mutex_trylock(uintptr_t *slot) { return pthread_mutex_trylock(mutex_get(slot)); }
static int w_mutex_unlock(uintptr_t *slot) { return pthread_mutex_unlock(mutex_get(slot)); }

static pthread_cond_t *cond_get(volatile uintptr_t *slot)
{
	uintptr_t v = *slot;
	if (IS_HEAP(v))
		return (pthread_cond_t *)v;
	pthread_cond_t *c = malloc(sizeof(*c));
	pthread_cond_init(c, NULL);
	if (__sync_bool_compare_and_swap(slot, v, (uintptr_t)c))
		return c;
	pthread_cond_destroy(c);
	free(c);
	return (pthread_cond_t *)*slot;
}

static int w_cond_init(uintptr_t *slot, const void *attr)
{
	*slot = 0;
	cond_get(slot);
	return 0;
}

static int w_cond_destroy(uintptr_t *slot)
{
	if (IS_HEAP(*slot)) {
		pthread_cond_destroy((pthread_cond_t *)*slot);
		free((void *)*slot);
	}
	*slot = 0;
	return 0;
}

static int w_cond_broadcast(uintptr_t *c) { return pthread_cond_broadcast(cond_get(c)); }
static int w_cond_signal(uintptr_t *c) { return pthread_cond_signal(cond_get(c)); }
static int w_cond_wait(uintptr_t *c, uintptr_t *m) { return pthread_cond_wait(cond_get(c), mutex_get(m)); }
static int w_cond_timedwait(uintptr_t *c, uintptr_t *m, const struct timespec *t)
{
	return pthread_cond_timedwait(cond_get(c), mutex_get(m), t);
}

/* bionic's relative-timeout wait, in milliseconds */
static int w_cond_timeout_np(uintptr_t *c, uintptr_t *m, unsigned ms)
{
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	t.tv_sec += ms / 1000;
	t.tv_nsec += (ms % 1000) * 1000000L;
	if (t.tv_nsec >= 1000000000L) {
		t.tv_sec++;
		t.tv_nsec -= 1000000000L;
	}
	return pthread_cond_timedwait(cond_get(c), mutex_get(m), &t);
}

/* bionic's pthread_attr_t */
typedef struct {
	uint32_t flags;
	void *stack_base;
	size_t stack_size;
	size_t guard_size;
	int32_t sched_policy;
	int32_t sched_priority;
} bionic_attr;

static int w_attr_init(bionic_attr *a)
{
	memset(a, 0, sizeof(*a));
	a->stack_size = 1024 * 1024;
	return 0;
}
static int w_attr_getstacksize(const bionic_attr *a, size_t *s) { *s = a->stack_size; return 0; }
static int w_attr_setstacksize(bionic_attr *a, size_t s) { a->stack_size = s; return 0; }
static int w_attr_setdetachstate(bionic_attr *a, int d) { if (d) a->flags |= 1; else a->flags &= ~1u; return 0; }
static int w_attr_getschedparam(const bionic_attr *a, struct sched_param *p) { p->sched_priority = a->sched_priority; return 0; }
static int w_attr_setschedparam(bionic_attr *a, const struct sched_param *p) { a->sched_priority = p->sched_priority; return 0; }

static int w_pthread_create(pthread_t *t, const bionic_attr *ba, void *(*fn)(void *), void *arg)
{
	pthread_attr_t a;
	pthread_attr_init(&a);
	size_t stack = ba && ba->stack_size ? ba->stack_size : 1024 * 1024;
	if (stack < 256 * 1024)
		stack = 256 * 1024;
	pthread_attr_setstacksize(&a, stack);
	if (ba && (ba->flags & 1)) /* PTHREAD_ATTR_FLAG_DETACHED */
		pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	int r = pthread_create(t, &a, fn, arg);
	pthread_attr_destroy(&a);
	return r;
}

/* bionic's sem_t is one word too */
static int w_sem_init(sem_t **slot, int pshared, unsigned value)
{
	*slot = malloc(sizeof(sem_t));
	return sem_init(*slot, 0, value);
}
static int w_sem_destroy(sem_t **slot)
{
	int r = sem_destroy(*slot);
	free(*slot);
	*slot = NULL;
	return r;
}
static int w_sem_post(sem_t **s) { return sem_post(*s); }
static int w_sem_wait(sem_t **s) { return sem_wait(*s); }
static int w_sem_trywait(sem_t **s) { return sem_trywait(*s); }
static int w_sem_getvalue(sem_t **s, int *v) { return sem_getvalue(*s, v); }

/* ---- misc libc ---- */

static int stack_chk_guard = 0x42424242;
static void w_stack_chk_fail(void) { fatal_error("stack smashing detected in libMaxPayne.so"); }
static int *w_errno(void) { return __errno_location(); }
static pid_t w_gettid(void) { return syscall(SYS_gettid); }
static long w_syscall(long n, long a, long b, long c, long d, long e, long f) { return syscall(n, a, b, c, d, e, f); }
static int w_ioctl(int fd, unsigned long req, void *arg) { return ioctl(fd, req, arg); }
static void w_exit(int code) { exit_game(code); }
static int w_sigemptyset(uint32_t *set) { *set = 0; return 0; } /* bionic's sigset_t is 32 bits */
static char *w_setlocale(int cat, const char *loc) { static char c[] = "C"; return c; }
static void *w_dlopen(const char *f, int m) { return NULL; }

static long w_sysconf(int name)
{
	switch (name) { /* bionic's numbers */
	case 0x06: return sysconf(_SC_CLK_TCK);
	case 0x27: case 0x28: return sysconf(_SC_PAGESIZE);
	case 0x60: return sysconf(_SC_NPROCESSORS_CONF);
	case 0x61: return sysconf(_SC_NPROCESSORS_ONLN);
	case 0x62: return sysconf(_SC_PHYS_PAGES);
	case 0x63: return sysconf(_SC_AVPHYS_PAGES);
	}
	log_printf("sysconf: unknown bionic name 0x%x\n", name);
	errno = EINVAL;
	return -1;
}

/* bionic's mbstate_t is 4 bytes, glibc's 8: keep our own */
static size_t w_mbrtowc(wchar_t *wc, const char *s, size_t n, void *ps)
{
	static __thread mbstate_t st;
	return mbrtowc(wc, s, n, &st);
}
static size_t w_wcrtomb(char *s, wchar_t wc, void *ps)
{
	static __thread mbstate_t st;
	return wcrtomb(s, wc, &st);
}

static void *w_aeabi_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *w_aeabi_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static void *w_aeabi_memset(void *d, size_t n, int c) { return memset(d, c, n); }
static void *w_aeabi_memclr(void *d, size_t n) { return memset(d, 0, n); }

/* the C++ runtime inside the .so unwinds through its own exception index */
static uintptr_t w_gnu_Unwind_Find_exidx(uintptr_t pc, int *count)
{
	if (pc >= so_base && pc < so_base + so_span) {
		*count = so_exidx_count;
		return so_exidx;
	}
	*count = 0;
	return 0;
}

/* bionic's _ctype_: a pointer to 257 flag bytes, indexed by c + 1 */
static char ctype_table[257];
/* bionic's _tolower_tab_ / _toupper_tab_: pointers to 257 shorts, c + 1 */
static short tolower_table[257], toupper_table[257];
static const short *tolower_ptr = tolower_table, *toupper_ptr = toupper_table;
static const char *ctype_ptr = ctype_table;

static void init_ctype(void)
{
	tolower_table[0] = toupper_table[0] = -1;
	for (int c = 0; c < 256; c++) {
		tolower_table[c + 1] = c < 128 ? tolower(c) : c;
		toupper_table[c + 1] = c < 128 ? toupper(c) : c;
	}
	for (int c = 0; c < 128; c++) {
		char f = 0;
		if (isupper(c)) f |= 0x01;
		if (islower(c)) f |= 0x02;
		if (isdigit(c)) f |= 0x04;
		if (isspace(c)) f |= 0x08;
		if (ispunct(c)) f |= 0x10;
		if (iscntrl(c)) f |= 0x20;
		if (isxdigit(c) && !isdigit(c)) f |= 0x40;
		if (c == ' ') f |= 0x80;
		ctype_table[c + 1] = f;
	}
}

/* glibc 2.36 (the build's) has no strlcpy / strlcat */
static size_t w_strlcpy(char *d, const char *s, size_t n)
{
	size_t len = strlen(s);
	if (n) {
		size_t c = len < n - 1 ? len : n - 1;
		memcpy(d, s, c);
		d[c] = '\0';
	}
	return len;
}

static size_t w_strlcat(char *d, const char *s, size_t n)
{
	size_t dl = strnlen(d, n);
	return dl == n ? n + strlen(s) : dl + w_strlcpy(d + dl, s, n - dl);
}

/* ---- C++ runtime and compiler helpers ---- */

static void *w_new(size_t n) { void *p = malloc(n ? n : 1); if (!p) fatal_error("out of memory (%zu bytes)", n); return p; }
static void w_delete(void *p) { free(p); }

/* the run-time ABI helpers use the base calling convention in both ABIs */
extern void __aeabi_idiv(void), __aeabi_idivmod(void), __aeabi_uidiv(void), __aeabi_uidivmod(void);
/* glibc's runtime raises SIGFPE on a division by zero. GTA SA's
 * MobileSettings::Initialize divides by (screen width - 640) and caps the
 * result at 100, so at 640 pixels the saturated quotient is what it means. */
static int w_idiv(int a, int b)
{
	if (b == 0)
		return a > 0 ? INT_MAX : a < 0 ? INT_MIN : 0;
	return a / b;
}

static unsigned w_uidiv(unsigned a, unsigned b)
{
	return b ? a / b : a ? UINT_MAX : 0;
}
extern void __aeabi_ldivmod(void), __aeabi_uldivmod(void), __aeabi_l2d(void), __aeabi_ul2d(void);
extern void __aeabi_l2f(void), __aeabi_ul2f(void), __aeabi_d2lz(void), __aeabi_d2ulz(void);
extern void __aeabi_f2lz(void), __aeabi_f2ulz(void);

/* bionic's jmp_buf is 64 words; glibc's setjmp writes past that (the signal
 * mask), so the .so gets its own pair: r4-r11, sp, lr, d8-d15 (104 bytes) */
int w_setjmp(void *env);
void w_longjmp(void *env, int val);
__asm__(
	".text\n.arm\n.align 2\n"
	".global w_setjmp\n.type w_setjmp, %function\n"
	"w_setjmp:\n"
	"	mov ip, sp\n"
	"	stmia r0!, {r4-r11, ip, lr}\n"
	"	vstmia r0, {d8-d15}\n"
	"	mov r0, #0\n"
	"	bx lr\n"
	".global w_longjmp\n.type w_longjmp, %function\n"
	"w_longjmp:\n"
	"	ldmia r0!, {r4-r11, ip, lr}\n"
	"	vldmia r0, {d8-d15}\n"
	"	mov sp, ip\n"
	"	movs r0, r1\n"
	"	moveq r0, #1\n"
	"	bx lr\n"
);

static int w_android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	size_t len = strlen(buf);
	return fprintf(stderr, "%s: %s%s", tag, buf, len && buf[len - 1] == '\n' ? "" : "\n");
}
static int w_android_log_write(int prio, const char *tag, const char *text) { return fprintf(stderr, "%s: %s\n", tag, text); }
static void w_android_log_assert(const char *cond, const char *tag, const char *fmt, ...) { fatal_error("assertion %s (%s)", cond ? cond : "", tag ? tag : ""); }
static void w_assert2(const char *file, int line, const char *func, const char *expr) { fatal_error("assertion %s failed at %s:%d (%s)", expr, file, line, func); }

/* ---- softfp maths and number parsing ---- */

#define F1(n) static SOFTFP float w_##n(float x) { return n(x); }
#define D1(n) static SOFTFP double w_##n(double x) { return n(x); }
#define F2(n) static SOFTFP float w_##n(float x, float y) { return n(x, y); }
#define D2(n) static SOFTFP double w_##n(double x, double y) { return n(x, y); }
D1(acos) F1(acosf) D1(asin) F1(asinf) D1(atan) D2(atan2) F2(atan2f) F1(atanf)
D1(ceil) F1(ceilf) D1(cos) F1(cosf) D1(cosh) F1(coshf) D1(exp) F1(expf) D1(exp2) F1(exp2f)
D1(fabs) F1(fabsf) D1(floor) F1(floorf) D2(fmod) F2(fmodf) D1(log) F1(logf) D1(log10)
F1(log10f) D1(log2) F1(log2f) D2(pow) F2(powf) D1(round) F1(roundf) D1(sin) F1(sinf)
D1(sinh) F1(sinhf) D1(sqrt) F1(sqrtf) D1(tan) F1(tanf) D1(tanh) F1(tanhf) D1(trunc) F1(truncf)
D2(fmax) F2(fmaxf) D2(fmin) F2(fminf) D2(hypot) F2(hypotf)
static SOFTFP double w_ldexp(double x, int e) { return ldexp(x, e); }
static SOFTFP float w_ldexpf(float x, int e) { return ldexpf(x, e); }
static SOFTFP double w_frexp(double x, int *e) { return frexp(x, e); }
static SOFTFP float w_frexpf(float x, int *e) { return frexpf(x, e); }
static SOFTFP double w_modf(double x, double *i) { return modf(x, i); }
static SOFTFP float w_modff(float x, float *i) { return modff(x, i); }
static SOFTFP long w_lrint(double x) { return lrint(x); }
static SOFTFP long w_lrintf(float x) { return lrintf(x); }
static SOFTFP long w_lround(double x) { return lround(x); }
static SOFTFP long w_lroundf(float x) { return lroundf(x); }
static SOFTFP void w_sincosf(float x, float *s, float *c) { sincosf(x, s, c); }
static SOFTFP int w_isfinite(double d) { return isfinite(d); }
static SOFTFP int w_isfinitef(float d) { return isfinite(d); }
static SOFTFP int w_isinf(double d) { return isinf(d); }
static SOFTFP int w_isnan(double d) { return isnan(d); }
static SOFTFP int w_signbit(double d) { return signbit(d); }
static SOFTFP double w_strtold(const char *s, char **e) { return strtod(s, e); }
static SOFTFP double w_atof(const char *s) { return atof(s); }
static SOFTFP double w_strtod(const char *s, char **e) { return strtod(s, e); }
static SOFTFP float w_strtof(const char *s, char **e) { return strtof(s, e); }

/* ---- GLES 2.0 and EGL, from ARM's libMali.so (in glos/lib) ----
 * Every gl / egl import resolves to the driver; those with float arguments
 * by value go through these wrappers. */

static void *mali;
#define GLF(name, args, call, ...) \
	static void (*p_##name)(__VA_ARGS__); \
	static SOFTFP void w_##name args { p_##name call; }
GLF(glBlendColor, (float r, float g, float b, float a), (r, g, b, a), float, float, float, float)
GLF(glClearColor, (float r, float g, float b, float a), (r, g, b, a), float, float, float, float)
GLF(glClearDepthf, (float d), (d), float)
GLF(glDepthRangef, (float n, float f), (n, f), float, float)
GLF(glLineWidth, (float w), (w), float)
GLF(glPolygonOffset, (float f, float u), (f, u), float, float)
GLF(glSampleCoverage, (float v, unsigned char i), (v, i), float, unsigned char)
GLF(glTexParameterf, (unsigned t, unsigned p, float v), (t, p, v), unsigned, unsigned, float)
GLF(glUniform1f, (int l, float x), (l, x), int, float)
GLF(glUniform2f, (int l, float x, float y), (l, x, y), int, float, float)
GLF(glUniform3f, (int l, float x, float y, float z), (l, x, y, z), int, float, float, float)
GLF(glUniform4f, (int l, float x, float y, float z, float w), (l, x, y, z, w), int, float, float, float, float)
GLF(glVertexAttrib1f, (unsigned i, float x), (i, x), unsigned, float)
GLF(glVertexAttrib2f, (unsigned i, float x, float y), (i, x, y), unsigned, float, float)
GLF(glVertexAttrib3f, (unsigned i, float x, float y, float z), (i, x, y, z), unsigned, float, float, float)
GLF(glVertexAttrib4f, (unsigned i, float x, float y, float z, float w), (i, x, y, z, w), unsigned, float, float, float, float)
#undef GLF

static const struct { const char *name; void **real; void *wrapper; } gl_float_funcs[] = {
#define E(n) { #n, (void **)&p_##n, (void *)w_##n }
	E(glBlendColor), E(glClearColor), E(glClearDepthf), E(glDepthRangef), E(glLineWidth),
	E(glPolygonOffset), E(glSampleCoverage), E(glTexParameterf), E(glUniform1f), E(glUniform2f),
	E(glUniform3f), E(glUniform4f), E(glVertexAttrib1f), E(glVertexAttrib2f), E(glVertexAttrib3f),
	E(glVertexAttrib4f),
#undef E
};

static uintptr_t gl_lookup(const char *name)
{
	for (size_t i = 0; i < sizeof(gl_float_funcs) / sizeof(*gl_float_funcs); i++)
		if (strcmp(gl_float_funcs[i].name, name) == 0)
			return (uintptr_t)gl_float_funcs[i].wrapper;
	if (strcmp(name, "eglGetProcAddress") == 0)
		return 0; /* in the table below */
	return (uintptr_t)dlsym(mali, name);
}

static void *w_eglGetProcAddress(const char *name)
{
	uintptr_t a = import_lookup(name);
	if (!a)
		log_printf("eglGetProcAddress(%s): not provided\n", name);
	return (void *)a;
}

/* ---- the APK's assets: files under game/assets/ (and LCS's OBBs, by
 * absolute path). Platform::File asks for its position as length minus
 * remaining length on every read, so the length is kept: seeking to the end
 * each time dropped stdio's buffer and made each line a disk read. ---- */

typedef struct {
	FILE *f;
	long len;
} Asset;

static void *w_AAssetManager_fromJava(void *env, void *obj) { return (void *)1; }

static Asset *w_AAssetManager_open(void *mgr, const char *name, int mode)
{
	char path[512];
	if (name[0] == '/')
		snprintf(path, sizeof(path), "%s", name);
	else
		snprintf(path, sizeof(path), "%s/assets/%s", fs_root, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		log_printf("asset %s: not found\n", name);
		return NULL;
	}
	setvbuf(f, NULL, _IOFBF, 64 * 1024);
	Asset *a = malloc(sizeof(*a));
	a->f = f;
	fseek(f, 0, SEEK_END);
	a->len = ftell(f);
	fseek(f, 0, SEEK_SET);
	return a;
}

static void w_AAsset_close(Asset *a) { fclose(a->f); free(a); }
static long w_AAsset_getLength(Asset *a) { return a->len; }
static long w_AAsset_getRemainingLength(Asset *a) { return a->len - ftell(a->f); }
static int w_AAsset_read(Asset *a, void *buf, size_t n) { return fread(buf, 1, n, a->f); }
static long w_AAsset_seek(Asset *a, long off, int whence) { return fseek(a->f, off, whence) < 0 ? -1 : ftell(a->f); }

static const int page_size = 4096;

/* LCS runs strtok over the extension string, which writes into it; ARM's is
 * read-only: hand out writable copies */
static const unsigned char *w_glGetString(unsigned name)
{
	static char *copies[8];
	static const unsigned char *(*real)(unsigned);
	if (!real)
		*(void **)&real = mali_sym("glGetString");
	const unsigned char *r = real(name);
	unsigned i = name & 7;
	if (!r)
		return r;
	if (!copies[i] || strcmp(copies[i], (const char *)r) != 0) {
		free(copies[i]);
		copies[i] = strdup((const char *)r);
	}
	return (const unsigned char *)copies[i];
}

/* ---- the table ---- */

/* the game's sleeps: while a video plays, a thread that holds the game's
 * context and sleeps (a render thread idling) shows the newest picture first
 * (movie.c) */
static int w_usleep(useconds_t us)
{
	if (movie_playing() && egl_is_render_thread())
		movie_present();
	return usleep(us);
}

#define FN(name, f) { #name, (uintptr_t)(f) }
#define SAME(name) { #name, (uintptr_t)&name }

static DynLibFunction imports[] = {
	/* Android-only libraries: assets from game/assets/, no Immersion vibration */
	FN(AAssetManager_fromJava, w_AAssetManager_fromJava), FN(AAssetManager_open, w_AAssetManager_open),
	FN(AAsset_close, w_AAsset_close), FN(AAsset_getLength, w_AAsset_getLength),
	FN(AAsset_getRemainingLength, w_AAsset_getRemainingLength), FN(AAsset_read, w_AAsset_read),
	FN(AAsset_seek, w_AAsset_seek),
	FN(ImmVibeCloseDevice, retm1), FN(ImmVibeGetEffectState, retm1),
	FN(ImmVibeGetIVTEffectIndexFromName, retm1), FN(ImmVibeInitialize2, retm1),
	FN(ImmVibeOpenDevice, retm1), FN(ImmVibePlayUHLEffect, retm1),
	FN(ImmVibeStopPlayingEffect, retm1), FN(ImmVibeTerminate, retm1),
	FN(__android_log_print, w_android_log_print),

	FN(__aeabi_memclr, w_aeabi_memclr), FN(__aeabi_memclr8, w_aeabi_memclr),
	FN(__aeabi_memcpy, w_aeabi_memcpy), FN(__aeabi_memcpy4, w_aeabi_memcpy),
	FN(__aeabi_memmove, w_aeabi_memmove), FN(__aeabi_memmove4, w_aeabi_memmove),
	FN(__aeabi_memset, w_aeabi_memset),
	FN(__cxa_atexit, ret0), /* the game leaves through exit_game() */
	FN(__errno, w_errno),
	FN(__gnu_Unwind_Find_exidx, w_gnu_Unwind_Find_exidx),
	FN(__google_potentially_blocking_region_begin, ret0),
	FN(__google_potentially_blocking_region_end, ret0),
	FN(__sF, fake_sF), FN(stderr, &fake_stderr),
	FN(__stack_chk_fail, w_stack_chk_fail), FN(__stack_chk_guard, &stack_chk_guard),
	FN(_ctype_, &ctype_ptr),
	FN(abort, abort), FN(exit, w_exit), FN(_exit, w_exit), FN(raise, raise),
	FN(__page_size, &page_size), FN(fcntl, fcntl), FN(uname, uname),
	FN(sigaction, ret0), FN(sigemptyset, w_sigemptyset),
	FN(setjmp, w_setjmp), FN(longjmp, w_longjmp), FN(_setjmp, w_setjmp), FN(_longjmp, w_longjmp),
	FN(dlopen, w_dlopen), FN(dlsym, ret0), FN(dlclose, ret0), FN(dlerror, ret0),
	FN(_Znwj, w_new), FN(_Znaj, w_new), FN(_ZdlPv, w_delete), FN(_ZdaPv, w_delete),
	FN(__aeabi_idiv, w_idiv), FN(__aeabi_idivmod, __aeabi_idivmod),
	FN(__aeabi_uidiv, w_uidiv), FN(__aeabi_uidivmod, __aeabi_uidivmod),
	FN(__aeabi_ldivmod, __aeabi_ldivmod), FN(__aeabi_uldivmod, __aeabi_uldivmod),
	FN(__aeabi_l2d, __aeabi_l2d), FN(__aeabi_ul2d, __aeabi_ul2d), FN(__aeabi_l2f, __aeabi_l2f),
	FN(__aeabi_ul2f, __aeabi_ul2f), FN(__aeabi_d2lz, __aeabi_d2lz), FN(__aeabi_d2ulz, __aeabi_d2ulz),
	FN(__aeabi_f2lz, __aeabi_f2lz), FN(__aeabi_f2ulz, __aeabi_f2ulz),
	FN(__aeabi_memclr4, w_aeabi_memclr), FN(__aeabi_memcpy8, w_aeabi_memcpy),
	FN(__aeabi_memmove8, w_aeabi_memmove), FN(__aeabi_memset4, w_aeabi_memset), FN(__aeabi_memset8, w_aeabi_memset),
	FN(__cxa_finalize, ret0), FN(__cxa_pure_virtual, abort),
	FN(__android_log_vprint, w_android_log_vprint), FN(__android_log_write, w_android_log_write),
	FN(__android_log_assert, w_android_log_assert), FN(__assert2, w_assert2),
	FN(_tolower_tab_, &tolower_ptr), FN(_toupper_tab_, &toupper_ptr),
	FN(__isfinite, w_isfinite), FN(__isfinitef, w_isfinitef), FN(isfinite, w_isfinite),
	FN(__isinf, w_isinf), FN(isinf, w_isinf), FN(__isnan, w_isnan), FN(isnan, w_isnan),
	FN(__signbit, w_signbit),

#define M(n) FN(n, w_##n)
	M(acos), M(acosf), M(asin), M(asinf), M(atan), M(atan2), M(atan2f), M(atanf), M(ceil),
	M(ceilf), M(cos), M(cosf), M(cosh), M(coshf), M(exp), M(expf), M(exp2), M(exp2f), M(fabs),
	M(fabsf), M(floor), M(floorf), M(fmod), M(fmodf), M(log), M(logf), M(log10), M(log10f),
	M(log2), M(log2f), M(pow), M(powf), M(round), M(roundf), M(sin), M(sinf), M(sinh), M(sinhf),
	M(sqrt), M(sqrtf), M(tan), M(tanf), M(tanh), M(tanhf), M(trunc), M(truncf), M(fmax),
	M(fmaxf), M(fmin), M(fminf), M(hypot), M(hypotf), M(ldexp), M(ldexpf), M(frexp), M(frexpf),
	M(modf), M(modff), M(lrint), M(lrintf), M(lround), M(lroundf), M(sincosf),
	M(atof), M(strtod), M(strtof), M(strtold),
#undef M
	SAME(fegetround), SAME(fesetround),

	SAME(atoi), SAME(atol), SAME(atoll), SAME(strtol), SAME(strtoul), SAME(strtoll), SAME(strtoull),
	SAME(bsearch), SAME(qsort), SAME(rand), SAME(srand), SAME(lrand48), SAME(srand48), SAME(abs), SAME(labs),
	SAME(isalnum), SAME(isalpha), SAME(isdigit), SAME(islower), SAME(isupper), SAME(isprint),
	SAME(ispunct), SAME(isxdigit), SAME(iscntrl), SAME(isgraph), SAME(memalign), SAME(posix_memalign),
	SAME(strdup), SAME(strndup), SAME(strnlen), SAME(strspn), SAME(strcspn), SAME(strsep), SAME(strtok_r),
	FN(strlcpy, w_strlcpy), FN(strlcat, w_strlcat), SAME(vasprintf), SAME(asprintf), SAME(puts), SAME(putchar),
	SAME(vprintf), SAME(vfprintf), SAME(vsscanf), SAME(ctime), SAME(gmtime), SAME(gmtime_r), SAME(mktime),
	SAME(difftime), SAME(clock), SAME(access), SAME(unlink), SAME(rename), SAME(rmdir), SAME(getcwd),
	SAME(chdir), SAME(fsync), SAME(ftruncate), SAME(pread), SAME(pwrite), SAME(sleep), SAME(getpid),
	SAME(mmap), SAME(munmap), SAME(mprotect), SAME(madvise), SAME(wcscpy), SAME(wcsncpy), SAME(wcscmp),
	SAME(wcsncmp), SAME(wcschr), SAME(wcsrchr), SAME(wcstol), SAME(swprintf), SAME(vswprintf),
	SAME(mbstowcs), SAME(wcstombs), SAME(iswspace), SAME(iswalpha), SAME(iswdigit), SAME(iswupper),
	SAME(iswlower), SAME(towlower), SAME(towupper),
	SAME(calloc), SAME(free), SAME(malloc), SAME(realloc),
	SAME(isspace), SAME(tolower), SAME(toupper),
	SAME(btowc), SAME(wctob), SAME(wctype), SAME(iswctype),
	FN(mbrtowc, w_mbrtowc), FN(wcrtomb, w_wcrtomb),
	SAME(wcscoll), SAME(wcsftime), SAME(wcslen), SAME(wcsxfrm),
	SAME(wmemchr), SAME(wmemcmp), SAME(wmemcpy), SAME(wmemmove), SAME(wmemset),
	SAME(memchr), SAME(memcmp), SAME(memcpy), SAME(memmove), SAME(memset),
	SAME(strcasecmp), SAME(strcat), SAME(strchr), SAME(strcmp), SAME(strcoll),
	SAME(strcpy), SAME(stpcpy), SAME(strerror), SAME(strlen), SAME(strncasecmp),
	SAME(strncat), SAME(strncmp), SAME(strncpy), SAME(strpbrk), SAME(strrchr),
	SAME(strstr), SAME(strtok), SAME(strxfrm),
	SAME(snprintf), SAME(sprintf), SAME(vsnprintf), SAME(vsprintf), SAME(sscanf),
	FN(printf, w_printf), FN(fprintf, w_fprintf),
	SAME(getenv), FN(setlocale, w_setlocale), FN(sysconf, w_sysconf),
	FN(syscall, w_syscall), FN(gettid, w_gettid), FN(ioctl, w_ioctl),

	SAME(clock_gettime), SAME(gettimeofday), SAME(time), SAME(asctime),
	SAME(localtime), SAME(localtime_r), SAME(strftime), SAME(nanosleep), FN(usleep, w_usleep),

	SAME(fopen), SAME(fdopen), SAME(freopen), FN(fclose, w_fclose), FN(rewind, w_rewind), FN(fileno, w_fileno),
	FN(fputwc, w_putwc), FN(fgetwc, w_getwc), FN(fscanf, w_fscanf), FN(vfprintf, w_vfprintf), FN(fread, w_fread),
	FN(fwrite, w_fwrite), FN(fseek, w_fseek), FN(ftell, w_ftell), FN(feof, w_feof),
	FN(ferror, w_ferror), FN(fflush, w_fflush), FN(fgetc, w_fgetc), FN(getc, w_fgetc),
	FN(fgets, w_fgets), FN(fputc, w_fputc), FN(putc, w_fputc), FN(fputs, w_fputs),
	FN(ungetc, w_ungetc), FN(setvbuf, w_setvbuf),
	FN(getwc, w_getwc), FN(putwc, w_putwc), FN(ungetwc, w_ungetwc),
	SAME(close), SAME(lseek), SAME(mkdir), FN(open, w_open), SAME(read), SAME(write),
	SAME(writev), SAME(poll), SAME(remove), FN(stat, w_stat), FN(fstat, w_fstat),
	SAME(opendir), SAME(closedir), FN(readdir, w_readdir),
	SAME(gzopen), SAME(gzgets), SAME(gzclose),

	FN(pthread_attr_init, w_attr_init), FN(pthread_attr_destroy, ret0),
	FN(pthread_attr_getstacksize, w_attr_getstacksize),
	FN(pthread_attr_setstacksize, w_attr_setstacksize),
	FN(pthread_attr_getschedparam, w_attr_getschedparam),
	FN(pthread_attr_setschedparam, w_attr_setschedparam),
	FN(pthread_create, w_pthread_create), SAME(pthread_join), SAME(pthread_self),
	SAME(pthread_setname_np), FN(pthread_setschedparam, ret0),
	SAME(pthread_key_create), SAME(pthread_key_delete),
	SAME(pthread_getspecific), SAME(pthread_setspecific), SAME(pthread_once),
	FN(pthread_mutexattr_init, w_mutexattr_init), FN(pthread_mutexattr_destroy, ret0),
	FN(pthread_mutexattr_settype, w_mutexattr_settype),
	FN(pthread_mutex_init, w_mutex_init), FN(pthread_mutex_destroy, w_mutex_destroy),
	FN(pthread_mutex_lock, w_mutex_lock), FN(pthread_mutex_unlock, w_mutex_unlock),
	FN(pthread_cond_init, w_cond_init), FN(pthread_cond_destroy, w_cond_destroy),
	FN(pthread_cond_broadcast, w_cond_broadcast), FN(pthread_cond_signal, w_cond_signal),
	FN(pthread_cond_wait, w_cond_wait), FN(pthread_cond_timedwait, w_cond_timedwait),
	SAME(sched_get_priority_max), SAME(sched_get_priority_min), SAME(sched_yield),
	FN(sem_init, w_sem_init), FN(sem_destroy, w_sem_destroy), FN(sem_post, w_sem_post),
	FN(sem_wait, w_sem_wait), FN(sem_trywait, w_sem_trywait), FN(sem_getvalue, w_sem_getvalue),
	FN(pthread_attr_setdetachstate, w_attr_setdetachstate), FN(pthread_attr_setschedpolicy, ret0),
	FN(pthread_detach, pthread_detach), FN(pthread_equal, pthread_equal), FN(pthread_kill, ret0),
	FN(pthread_exit, pthread_exit), FN(pthread_mutexattr_setpshared, ret0),
	FN(pthread_condattr_init, ret0), FN(pthread_condattr_destroy, ret0),
	FN(pthread_mutex_trylock, w_mutex_trylock), FN(pthread_cond_timeout_np, w_cond_timeout_np),

	FN(eglGetProcAddress, w_eglGetProcAddress),
	FN(glGetString, w_glGetString),
};

#define NUM_IMPORTS (sizeof(imports) / sizeof(*imports))

uintptr_t import_lookup(const char *name)
{
	for (size_t i = 0; i < NUM_IMPORTS; i++)
		if (strcmp(imports[i].symbol, name) == 0)
			return imports[i].func;
	if (mali && (strncmp(name, "gl", 2) == 0 || strncmp(name, "egl", 3) == 0))
		return gl_lookup(name);
	if (strncmp(name, "al", 2) == 0)
		return al_import(name);
	/* C++ exceptions: the ARM EHABI unwinder of libgcc_s, as bionic's */
	if (strncmp(name, "_Unwind_", 8) == 0 || strcmp(name, "__gnu_unwind_frame") == 0) {
		static void *gcc_s;
		if (!gcc_s)
			gcc_s = dlopen("libgcc_s.so.1", RTLD_NOW | RTLD_GLOBAL);
		return gcc_s ? (uintptr_t)dlsym(gcc_s, name) : 0;
	}
	return 0;
}

void *mali_sym(const char *name)
{
	return mali ? dlsym(mali, name) : NULL;
}

int imports_load_gl(void)
{
	mali = dlopen("libMali.so", RTLD_NOW | RTLD_GLOBAL);
	if (!mali) {
		log_printf("can't load libMali.so: %s (start through ./glos/glos-gpu run mali)\n", dlerror());
		return -1;
	}
	for (size_t i = 0; i < sizeof(gl_float_funcs) / sizeof(*gl_float_funcs); i++)
		if (!(*gl_float_funcs[i].real = dlsym(mali, gl_float_funcs[i].name)))
			log_printf("libMali.so has no %s\n", gl_float_funcs[i].name);
	return 0;
}

int resolve_imports(void)
{
	init_ctype();
	return so_resolve(import_lookup);
}

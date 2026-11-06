#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "util.h"

char fs_root[256];

int log_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vfprintf(stderr, fmt, ap);
	va_end(ap);
	return n;
}

void fatal_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("loader: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	fflush(stderr);
	_exit(1);
}

float env_float(const char *name, float def)
{
	const char *v = getenv(name);
	return v && *v ? (float)atof(v) : def;
}

int env_int(const char *name, int def)
{
	const char *v = getenv(name);
	return v && *v ? atoi(v) : def;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }

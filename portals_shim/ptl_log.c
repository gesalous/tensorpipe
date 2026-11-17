#include "ptl_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int ptl_log_level(void)
{
	static int level = -1;
	if (level < 0) {
		const char *e = getenv("TP_PTL_LOG");
		level = (e != NULL && e[0] != '\0') ? atoi(e) : 1;
	}
	return level;
}

void ptl_log(int level, const char *fmt, ...)
{
	va_list ap;
	if (level > ptl_log_level())
		return;
	fprintf(stderr, "[ptl-shim] %s: ", level == 1 ? "WARN" : "DEBUG");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void ptl_fatal(const char *file, int line, const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "[ptl-shim] FATAL (%s:%d): ", file, line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	abort();
}

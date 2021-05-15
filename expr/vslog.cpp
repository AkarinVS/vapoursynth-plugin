#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "vslog.h"

void vsLog(const char *file, long line, VSMessageType type, const char *msg, ...) {
	const char *typs = nullptr;
	switch (type) {
	case mtDebug: typs = "DBG"; break;
	case mtWarning: typs = "WARN"; break;
	case mtCritical: typs = "CRIT"; break;
	case mtFatal: typs = "FATAL"; break;
	default: typs = "???"; break;
	}

	va_list va;
	va_start(va, msg);
	fprintf(stderr, "[%s] %s:%ld: ", typs, file, line);
	vfprintf(stderr, msg, va);
	va_end(va);

    if (type == mtFatal)
        abort();
}

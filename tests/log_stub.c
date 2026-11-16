/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host-test log_line: print to stderr. The daemon supplies its own. */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

void log_line(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/**
 * error_lite.c
 * Minimal logging implementation for ym7129_test standalone build.
 */
#include "error.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static log_mode_t s_log_mode = LOG_TO_CONSOLE;

void set_log_mode(log_mode_t mode) {
    s_log_mode = mode;
}

void logging(enum loglevel_t level, const char *fmt, ...) {
    if ((int)level < LOG_LEVEL) return;
    const char *prefix = "";
    switch (level) {
        case LOG_LEVEL_DEBUG: prefix = "[DBG] "; break;
        case LOG_LEVEL_INFO:  prefix = "[INF] "; break;
        case LOG_LEVEL_WARN:  prefix = "[WRN] "; break;
        case LOG_LEVEL_ERROR: prefix = "[ERR] "; break;
        case LOG_LEVEL_FATAL: prefix = "[FAT] "; break;
    }
    fprintf(stderr, "%s", prefix);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

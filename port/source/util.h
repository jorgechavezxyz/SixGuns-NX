#pragma once
#include <stdio.h>

void log_init(void);
void log_close(void);
void log_flush(void);
void debugPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Set by main once EGL owns the screen, so fatal_error can release it. */
extern void (*fatal_cleanup)(void);
void fatal_error(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* Rewrites Android paths (/sdcard/..., /data/data/<pkg>/...) to SD card paths.
 * Returns either `path` itself or `buf`. */
const char *remap_path(const char *path, char *buf, size_t bufsize);

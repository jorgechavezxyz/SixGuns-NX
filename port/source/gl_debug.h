#pragma once
/* GL call instrumentation (see gl_debug.c). */
void *gl_debug_wrap(const char *name, void *real);
void gl_debug_init(void);
void gl_debug_frame(unsigned long frame, unsigned every);

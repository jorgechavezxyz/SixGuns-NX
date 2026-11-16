#pragma once

/* JNIEnv* and JavaVM* to hand to the engine. */
extern void *jni_env;
extern void *jni_vm;

void jni_init(void);
void *jni_new_string(const char *s);
void *jni_find_class(const char *name);

#include <stddef.h>
/* Pending GameRenderer.setKeyboard request: copies the current text and the
 * int arguments, returns 1 once per request. */
int jni_keyboard_take_request(char *initial, size_t n, int args[6]);
void jni_keyboard_set_result(const char *text);

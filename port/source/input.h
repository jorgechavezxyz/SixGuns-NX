#pragma once
#include <switch.h>

#define INPUT_MAX_TOUCH 8

typedef struct {
  unsigned long frame;
  u64 held, down, up;          /* HidNpadButton_* masks */
  HidAnalogStickState ls, rs;  /* raw stick positions (+-32767) */
  int ntouch;
  u32 touch_started;           /* new finger IDs this frame, before any UI consumes touches */
  struct { int x, y, id; } t[INPUT_MAX_TOUCH];
} InputFrame;

void input_init(void);
void input_poll(PadState *pad, unsigned long frame, InputFrame *out);
void input_close(void);
int input_debug_capture_active(void);
/* Game event for replay alignment, e.g. "menu:15Menu_MainMenu3D", "tutorial:0". */
void input_sync(const char *event);

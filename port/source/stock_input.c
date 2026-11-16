/* Stock input (the OLD RELEASE variant, built with -DSTOCK_INPUT).
 *
 * Feeds the Switch pad to the game's OWN Android controller support exactly
 * the way the Android Java layer would, with no port-side input logic:
 *   - "controller connected": NativeBridgeHIDControllers.NativeControllerConnected
 *     and Game.nativeConnectedController (sets AndroidController::GameController,
 *     which switches the UI to controller mode);
 *   - buttons: Game.nativeButton{A,B,X,Y,LeftTrigger,RightTrigger,Start,Select}
 *     Pressed/Released and Game.nativeDpad{Up,Down,Left,Right}Pressed/Released;
 *   - left stick: Game.nativeLeftStick<dir>Pressed/Released, 8 directions
 *     (digital: the game has no analog channel in use; HidController's
 *     NativeHandleInputEvents(int, double) exists but nothing registers for it);
 *   - right stick: Game.nativeRightStick<dir>Pressed, 8 directions, no
 *     release (DefaultButtonListener::rotate turns the camera 5 degrees per
 *     call), sent every frame at full deflection, every other frame when light.
 * All of it lands in ButtonManager -> DefaultButtonListener (gameplay actions
 * through ActionManager, menus through ControllableMenuSystemList).
 * L and R have no Android mapping. + and - together still quit the port. */
#include <math.h>
#include <switch.h>
#include "input.h"
#include "so_util.h"
#include "util.h"

typedef void (*jfn)(void *env, void *cls);
#define JN(name) "Java_com_gameloft_android_ANMP_GloftSXHM_Game_" name

extern void *jni_env;
extern void *jni_new_string(const char *s);
extern void *jni_find_class(const char *name);

static so_module *mod;
static void *game_cls;

static jfn sym(const char *name) {
  jfn f = (jfn)so_symbol(mod, name);
  if (!f) debugPrintf("[stock] missing %s\n", name);
  return f;
}
static void call(const char *name) {
  char full[160];
  snprintf(full, sizeof(full), JN("%s"), name);
  jfn f = sym(full);
  if (f) f(jni_env, game_cls);
}

void stock_input_init(so_module *m) {
  mod = m;
  game_cls = jni_find_class("com/gameloft/android/ANMP/GloftSXHM/Game");
  void (*connected)(void *, void *, void *) = (void *)so_symbol(mod,
      "Java_com_gameloft_android_ANMP_GloftSXHM_GLUtils_controller_NativeBridgeHIDControllers_NativeControllerConnected");
  if (connected)
    connected(jni_env, jni_find_class("com/gameloft/android/ANMP/GloftSXHM/GLUtils/controller/NativeBridgeHIDControllers"),
              jni_new_string("Nintendo Switch Controller"));
  /* main.c already calls Game.nativeConnectedController before this. */
  debugPrintf("[stock] game's own Android controller support connected (stock input, no port input logic)\n");
}

static const struct { u64 key; const char *name; } buttons[] = {
  { HidNpadButton_A, "ButtonA" }, { HidNpadButton_B, "ButtonB" }, { HidNpadButton_X, "ButtonX" },
  { HidNpadButton_Y, "ButtonY" }, { HidNpadButton_ZL, "ButtonLeftTrigger" },
  { HidNpadButton_ZR, "ButtonRightTrigger" }, { HidNpadButton_Plus, "ButtonStart" },
  { HidNpadButton_Minus, "ButtonSelect" }, { HidNpadButton_Up, "DpadUp" }, { HidNpadButton_Down, "DpadDown" },
  { HidNpadButton_Left, "DpadLeft" }, { HidNpadButton_Right, "DpadRight" },
};

/* 8 directions, index 0 = right, counter-clockwise; -1 = centred. */
static const char *const dirs[8] = { "Right", "UpRight", "Up", "UpLeft", "Left", "DownLeft", "Down", "DownRight" };
static int stick_dir(HidAnalogStickState s, int cur, float *mag) {
  float x = s.x / 32767.0f, y = s.y / 32767.0f;
  *mag = sqrtf(x * x + y * y);
  float on = cur < 0 ? 0.5f : 0.35f; /* hysteresis */
  if (*mag < on) return -1;
  float a = atan2f(y, x);
  int d = (int)lrintf(a / (3.14159265f / 4));
  return (d + 8) % 8;
}

void stock_input_frame(const InputFrame *in, unsigned long frame) {
  char name[64];
  for (size_t i = 0; i < sizeof(buttons) / sizeof(*buttons); i++) {
    if (in->down & buttons[i].key) {
      snprintf(name, sizeof(name), "native%sPressed", buttons[i].name);
      debugPrintf("[stock] %s\n", name);
      call(name);
    }
    if (in->up & buttons[i].key) {
      snprintf(name, sizeof(name), "native%sReleased", buttons[i].name);
      call(name);
    }
  }

  static int ldir = -1;
  float lmag, rmag;
  int nd = stick_dir(in->ls, ldir, &lmag);
  if (nd != ldir) {
    if (ldir >= 0) { snprintf(name, sizeof(name), "nativeLeftStick%sReleased", dirs[ldir]); call(name); }
    if (nd >= 0) { snprintf(name, sizeof(name), "nativeLeftStick%sPressed", dirs[nd]); call(name); }
    ldir = nd;
  }

  static int rdir = -1;
  rdir = stick_dir(in->rs, rdir, &rmag);
  if (rdir >= 0 && (rmag > 0.8f || (frame & 1) == 0)) {
    snprintf(name, sizeof(name), "nativeRightStick%sPressed", dirs[rdir]);
    call(name);
  }
}

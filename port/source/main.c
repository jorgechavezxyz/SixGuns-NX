/* Six Guns NX: loads the Android engine (libAndroid.so) and drives it the way
 * the Java side (Game / GameRenderer / GameGLSurfaceView) would on Android. */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>
#include <switch.h>
#include "config.h"
#include "imports.h"
#include "imports_libc.h"
#include "gl_debug.h"
#include "input.h"

int controller_init(so_module *mod);
void controller_update(const InputFrame *in);
#include "jni.h"
#include "so_util.h"
#include "util.h"

static so_module game;
so_module *port_engine_module(void) { return &game; } /* for symbolizing (gl_debug.c) */
extern char __start__[];

const char *thread_entry_name(void *fn) {
  static __thread char buf[256];
  uintptr_t off = 0;
  uintptr_t a = (uintptr_t)fn;
  if (a < game.base || a >= game.base + game.image_size) {
    snprintf(buf, sizeof(buf), "(loader +0x%lx)", (unsigned long)(a - (uintptr_t)&__start__));
    return buf;
  }
  const char *n = so_symbol_name(&game, a, &off);
  snprintf(buf, sizeof(buf), "%s+0x%lx (engine +0x%lx)", n ? n : "?", (unsigned long)off, (unsigned long)(a - game.base));
  return buf;
}
static EGLDisplay display;
static EGLSurface surface;
static EGLContext context;

/* JNI natives: (JNIEnv*, jclass, args...) */
#define JNAME(cls, fn) "Java_com_gameloft_android_ANMP_GloftSXHM_" cls "_" fn
typedef void (*jfn_v)(void *, void *);
typedef void (*jfn_iii)(void *, void *, int, int, int);
typedef void (*jfn_iiii)(void *, void *, int, int, int, int);

/* GameGLSurfaceView.nativeOnTouch(action, x, y, pointerId) -> appOnTouch.
 * NOT Android MotionEvent order: appOnTouch (engine +0x9a5c2c) sends actions
 * 1/4 to Game::OnPointerPressed (vtable +0x98), 0/3 to Game::OnPointerReleased
 * (+0xa0) and the rest (2/5) to OnPointerDragged (+0x90). With 0 = down every
 * touch reached the engine as release-then-press: the release was dropped (no
 * slot yet) and the touch stayed pressed until the next one, so a button fired
 * on the second tap (TouchManager slots seen with the [ptrace] trace). */
enum { TOUCH_DOWN = 1, TOUCH_UP = 0, TOUCH_MOVE = 2 };
static jfn_iiii on_touch;
/* A touch from port code (ui_menus.c presses the hidden buttons of the menus it draws itself):
 * down = 1 press, 0 release; screen pixels. */
void port_touch(int down, int x, int y, int id) { if (on_touch) on_touch(jni_env, NULL, down ? TOUCH_DOWN : TOUCH_UP, x, y, id); }
typedef void (*jfn_ii)(void *, void *, int, int);
typedef void (*jfn_renderer_init)(void *, void *, int, int, int, void *);

static void *sym(const char *name) {
  void *p = (void *)so_symbol(&game, name);
  if (!p) debugPrintf("[main] missing export %s\n", name);
  return p;
}

/* Calls a no-argument Game.native* entry point if present. */
static void call_game_v(const char *fn) {
  char name[256];
  snprintf(name, sizeof(name), JNAME("Game", "%s"), fn);
  jfn_v f = (jfn_v)so_symbol(&game, name);
  if (f) f(jni_env, jni_find_class("com/gameloft/android/ANMP/GloftSXHM/Game"));
}
void port_call_game_v(const char *fn) { call_game_v(fn); }

/* ---------------------------------------------------------------- graphics */

static int init_egl(void) {
  debugPrintf("[egl] eglGetDisplay\n");
  display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  debugPrintf("[egl] eglInitialize\n");
  if (!display || !eglInitialize(display, NULL, NULL)) return -1;
  eglBindAPI(EGL_OPENGL_ES_API);

  static const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  EGLConfig cfg;
  EGLint n;
  debugPrintf("[egl] eglChooseConfig\n");
  if (!eglChooseConfig(display, cfg_attr, &cfg, 1, &n) || n == 0) return -2;

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, SCREEN_W, SCREEN_H);
  debugPrintf("[egl] eglCreateWindowSurface\n");
  surface = eglCreateWindowSurface(display, cfg, win, NULL);
  if (!surface) return -3;

  static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  debugPrintf("[egl] eglCreateContext\n");
  context = eglCreateContext(display, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (!context) return -4;
  eglMakeCurrent(display, surface, surface, context);
  eglSwapInterval(display, 1);
  return 0;
}

static void deinit_egl(void) {
  if (!display) return;
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (context) eglDestroyContext(display, context);
  if (surface) eglDestroySurface(display, surface);
  eglTerminate(display);
  display = NULL;
  context = NULL;
  surface = NULL;
}

/* Saves the current back buffer as a TGA on the SD card (debug aid: the
 * emulator window can't be captured from outside). */
static void save_screenshot(const char *path) {
  static uint8_t px[SCREEN_W * SCREEN_H * 4];
  static void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
  static void (*finish)(void);
  if (!read_pixels) read_pixels = (void *)eglGetProcAddress("glReadPixels");
  if (!finish) finish = (void *)eglGetProcAddress("glFinish");
  finish();
  /* Read in 320-pixel strips: Ryujinx only copies back the first quarter of a
   * full-width readback. */
  static uint8_t strip[320 * SCREEN_H * 4];
  for (int sx = 0; sx < SCREEN_W; sx += 320) {
    read_pixels(sx, 0, 320, SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, strip);
    for (int y = 0; y < SCREEN_H; y++)
      memcpy(px + (y * SCREEN_W + sx) * 4, strip + y * 320 * 4, 320 * 4);
  }
  FILE *f = fopen(path, "wb");
  if (!f) return;
  uint8_t hdr[18] = {0};
  hdr[2] = 2;                       /* uncompressed true-color */
  hdr[12] = SCREEN_W & 255; hdr[13] = SCREEN_W >> 8;
  hdr[14] = SCREEN_H & 255; hdr[15] = SCREEN_H >> 8;
  hdr[16] = 32; hdr[17] = 8;        /* 32 bpp, bottom-left origin like GL */
  fwrite(hdr, 1, sizeof(hdr), f);
  for (size_t i = 0; i < sizeof(px); i += 4) { uint8_t t = px[i]; px[i] = px[i + 2]; px[i + 2] = t; }
  fwrite(px, 1, sizeof(px), f);
  fclose(f);
  debugPrintf("[main] screenshot -> %s\n", path);
}

/* ---------------------------------------------------------------- text input */

typedef void (*jfn_kb_finish)(void *, void *, void *, int);

/* Shows the Switch software keyboard for a pending setKeyboard request and
 * hands the text to GameRenderer.onKeyboardFinish. */
static void handle_keyboard(jfn_kb_finish kb_finish, void *renderer_cls) {
  char initial[512], out[512];
  int args[6];
  if (!jni_keyboard_take_request(initial, sizeof(initial), args)) return;

  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_FAILED(rc)) { debugPrintf("[kbd] swkbdCreate failed 0x%x\n", rc); return; }
  /* setKeyboard(maxLength, text, inputType, x, y, w, h); inputType follows
   * Android's InputType class: 1 = text, 2 = number. */
  int max_len = args[0] > 0 && args[0] < 500 ? args[0] : 32;
  swkbdConfigMakePresetDefault(&kbd);
  if ((args[1] & 0xf) == 2) swkbdConfigSetType(&kbd, SwkbdType_NumPad);
  swkbdConfigSetInitialText(&kbd, initial);
  swkbdConfigSetStringLenMax(&kbd, max_len);
  u64 t0 = armGetSystemTick();
  rc = swkbdShow(&kbd, out, sizeof(out));
  /* On hardware the first attempt can come back at once with
   * LibnxError_LibAppletBadExit (0x5d59) without the keyboard ever showing:
   * try once more. */
  if (rc == MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit) && armTicksToNs(armGetSystemTick() - t0) < 1500000000ull) {
    debugPrintf("[kbd] keyboard applet exited at once (0x%x): retrying\n", rc);
    rc = swkbdShow(&kbd, out, sizeof(out));
  }
  swkbdClose(&kbd);

  if (R_FAILED(rc)) {
    debugPrintf("[kbd] keyboard cancelled (0x%x), keeping \"%s\"\n", rc, initial);
    snprintf(out, sizeof(out), "%s", initial);
  } else {
    debugPrintf("[kbd] entered \"%s\"\n", out);
  }
  jni_keyboard_set_result(out);
  /* GameRenderer.onKeyboardFinish calls the text field's callback, a global
   * the field sets (GOT slot +0x199af48); when the screen that asked for the
   * keyboard is gone it is NULL and the game jumped to 0 (crash on hardware:
   * keyboard opened from the MP lobby, menu left, keyboard closed). */
  {
    extern so_module *port_engine_module(void);
    so_module *mod = port_engine_module();
    void **slot = mod ? *(void ***)(mod->base + 0x199af48) : NULL;
    if (!slot || !*slot) {
      debugPrintf("[kbd] the screen that asked for the keyboard is gone: result dropped\n");
      return;
    }
  }
  if (kb_finish) kb_finish(jni_env, renderer_cls, jni_new_string(out), 0);
}

/* ---------------------------------------------------------------- input */

typedef struct { u64 key; const char *press, *release; } button_map;

static const button_map buttons[] = {
  { HidNpadButton_A,     "nativeButtonAPressed",            "nativeButtonAReleased" },
  { HidNpadButton_B,     "nativeButtonBPressed",            "nativeButtonBReleased" },
  { HidNpadButton_X,     "nativeButtonXPressed",            "nativeButtonXReleased" },
  { HidNpadButton_Y,     "nativeButtonYPressed",            "nativeButtonYReleased" },
  { HidNpadButton_ZL,    "nativeButtonLeftTriggerPressed",  "nativeButtonLeftTriggerReleased" },
  { HidNpadButton_ZR,    "nativeButtonRightTriggerPressed", "nativeButtonRightTriggerReleased" },
  { HidNpadButton_Plus,  "nativeButtonStartPressed",        "nativeButtonStartReleased" },
  { HidNpadButton_Minus, "nativeButtonSelectPressed",       "nativeButtonSelectReleased" },
  { HidNpadButton_Up,    "nativeDpadUpPressed",             "nativeDpadUpReleased" },
  { HidNpadButton_Down,  "nativeDpadDownPressed",           "nativeDpadDownReleased" },
  { HidNpadButton_Left,  "nativeDpadLeftPressed",           "nativeDpadLeftReleased" },
  { HidNpadButton_Right, "nativeDpadRightPressed",          "nativeDpadRightReleased" },
};

/* In menus, the left stick acts as a D-pad: one step when pushed, then repeats
 * after a short hold (like holding the arrow keys). Synthesized presses go
 * through the same paths as the real D-pad. */
static void menu_stick_to_dpad(InputFrame *in, unsigned long frame) {
  extern int game_menu_active(void);
  static u64 dir_prev, synth_prev;
  static unsigned long next_repeat;
  u64 dir = 0;
  if (game_menu_active()) {
    const int th = 16000; /* about half deflection */
    int x = in->ls.x, y = in->ls.y;
    if (abs(x) > abs(y)) { if (x > th) dir = HidNpadButton_Right; else if (x < -th) dir = HidNpadButton_Left; }
    else { if (y > th) dir = HidNpadButton_Up; else if (y < -th) dir = HidNpadButton_Down; }
  }
  u64 synth = dir;
  if (dir != dir_prev) {
    next_repeat = frame + 24;            /* 0.4 s before the first repeat */
  } else if (dir) {
    if (frame == next_repeat) synth = 0; /* release for one frame... */
    else if (frame > next_repeat) next_repeat = frame + 9; /* ...then press again; every 0.15 s */
  }
  dir_prev = dir;
  /* Turn the synthetic held state into press/release events, skipping
   * directions the real D-pad is already holding. */
  u64 down = synth & ~synth_prev & ~in->held, up = synth_prev & ~synth & ~in->held;
  in->down |= down;
  in->up |= up;
  in->held |= synth;
  synth_prev = synth;
}

/* A Back press requested by port code (screenshot.c: CLOSE on a popup). */
static volatile int back_requested;
void port_request_back(void) { back_requested = 1; }
/* Close the generic Popup only (CLOSE on a purchase popup, screenshot.c). */
static volatile int popup_close_requested;
static int a_release_kept; /* set when A closed a popup: that A's release is not sent */
void port_request_popup_close(void) { popup_close_requested = 1; }

unsigned long port_frame_no;

static void handle_input(const InputFrame *in) {
  /* After a popup over the shop closes, the shop's controller focus is on the
   * tab bar (D-pad Left/Right change tabs, Down does nothing) until A is
   * pressed; a fresh shop starts on the items. So the port taps A once a few
   * frames after the popup closed (A on the current tab re-selects it). */
  static int a_tap;
  if (popup_close_requested) {
    popup_close_requested = 0;
    a_release_kept = 1; /* the A that pressed CLOSE: its release must not reach a popup underneath */
    static void *(*pinst)(int);
    static void (*phide)(void *);
    static int (*pen)(void *);
    if (!pinst) {
      pinst = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI5PopupE11GetInstanceEb");
      phide = (void *)so_symbol(port_engine_module(), "_ZN5Popup9HidePopupEv");
      pen = (void *)so_symbol(port_engine_module(), "_ZN5Popup9IsEnabledEv");
    }
    void *p = pinst ? pinst(0) : NULL;
    if (p && phide && pen && (pen(p) & 1)) { debugPrintf("[input] popup closed (CLOSE)\n"); phide(p); }
    extern int shop_menu_active(void), shop_ui_owning(void);
    if (shop_menu_active() && !shop_ui_owning()) a_tap = 6; /* old grid: focus back on the items */
  }
  if (back_requested) {
    back_requested = 0;
    debugPrintf("[input] Back (requested)\n");
    call_game_v("nativeBackPressed");
    extern int shop_menu_active(void);
    extern int shop_ui_owning(void);
    if (shop_menu_active() && !shop_ui_owning()) a_tap = 6;
  }
  if (a_tap > 0) {
    a_tap--;
    if (a_tap == 2) { debugPrintf("[input] A tap (shop focus back on the items)\n"); call_game_v("nativeButtonAPressed"); }
    if (a_tap == 0) call_game_v("nativeButtonAReleased");
  }
  static int touch_active[16], touch_x[16], touch_y[16];
  static int no_android_buttons = -1;
  if (no_android_buttons < 0) {
    struct stat st;
    no_android_buttons = stat(PORT_ROOT "/debug_no_android_buttons", &st) == 0;
    if (no_android_buttons) debugPrintf("[input] DEBUG: Android button path disabled\n");
  }

  /* B: in menus it's Android's Back key (press only); in gameplay it belongs to
   * the controller scheme (turn around, controller.c). Sending Back in
   * gameplay opened the pause menu, and the B release then closed it again. */
  extern int game_menu_active(void);
  int in_menu = game_menu_active();
#ifndef STOCK_INPUT
  static u64 android_sent; /* presses whose release we owe the engine */
  for (size_t i = 0; i < sizeof(buttons) / sizeof(*buttons) && !no_android_buttons; i++) {
    /* B: handled below. +: the controller scheme's pause event opens the menu
     * (controller.c); the Android Start release would close it again. */
    if (buttons[i].key == HidNpadButton_B || buttons[i].key == HidNpadButton_Plus) continue;
    /* Comics: A is the universal hold-to-skip (controller.c), not a menu press */
    {
      extern int cutscene_active, cutscene_comic;
      if (buttons[i].key == HidNpadButton_A && cutscene_active && !(android_sent & HidNpadButton_A)) continue;
      /* ...and a comic takes no other buttons either: Select (-) there reached
       * the gameplay underneath and opened the map as soon as the comic ended
       * ("complete the tutorial first" over a black screen). */
      if (cutscene_comic && !(android_sent & buttons[i].key)) continue;
    }
    /* − on the map closes it again (− opens it, controller.c): Back, not Select */
    {
      extern const char *current_menu_class;
      if (buttons[i].key == HidNpadButton_Minus && strstr(current_menu_class, "Menu_IGM_Map")) {
        if (in->down & HidNpadButton_Minus) { debugPrintf("[input] Back (- on the map)\n"); call_game_v("nativeBackPressed"); }
        continue;
      }
    }
    /* One scheme per press (input_route.c); the release follows the press. */
    extern int input_route_android(u64 key);
    /* Tutorial prompts confirm on the A press *and* on the release
     * (DefaultButtonListener::aPressed / aReleased both call the tutorial
     * menu's confirm). The press already advances the prompt, so a release
     * that lands on the next prompt confirmed that one unseen ("Help the lady"
     * dialogue skipped pages), and a quick tap confirmed the same prompt
     * twice. So an A press that confirmed a tutorial prompt sends no release:
     * the release only carries that second confirm and an ActionManager
     * "released" edge (an event bit, nothing held). */
    extern int input_route_menu_step(void);
    static int a_press_step = -1;
    if ((in->down & buttons[i].key) && input_route_android(buttons[i].key)) {
      debugPrintf("[input] %s\n", buttons[i].press);
      if (buttons[i].key == HidNpadButton_A) a_press_step = input_route_menu_step();
      call_game_v(buttons[i].press);
      android_sent |= buttons[i].key;
    }
    if ((in->up & buttons[i].key) && (android_sent & buttons[i].key)) {
      extern int shop_ui_list_active(void);
      if (buttons[i].key == HidNpadButton_A && a_release_kept) {
        /* This A pressed CLOSE on a purchase popup (closed by the port). An achievement popup opened
         * by the same purchase sat under it, and the release confirmed its focused SCREENSHOT button:
         * an album screenshot nobody asked for (owner, hardware 2026-11-14). */
        a_release_kept = 0;
        debugPrintf("[input] A release kept (its press closed a popup)\n");
      } else if (buttons[i].key == HidNpadButton_A && shop_ui_list_active()) {
        /* An A pressed elsewhere (SHOP in the pause menu, CLOSE on a purchase popup) released over
         * the shop list: the shop's own controller scheme, focused on the first tab, took the release
         * as a click on it and jumped to the Legendary tab (UpdateUI, text id 0x265). The list has
         * the controls: the release stays with the port. */
        debugPrintf("[input] A release kept from the shop (list has the controls)\n");
      } else if (buttons[i].key == HidNpadButton_A && ({ extern int cutscene_active; cutscene_active; })) {
        /* A pressed before a comic/cinematic started (loading screen closes on the press): the
         * native release (DefaultButtonListener::aReleased) pops a comic or skips a cinematic at
         * once, without the 5 s hold. */
        debugPrintf("[input] A release kept (a cutscene started under it)\n");
      } else if (buttons[i].key == HidNpadButton_A && a_press_step != -1) {
        debugPrintf("[input] A release not sent (the press confirmed tutorial prompt %d; now %d)\n", a_press_step,
                    input_route_menu_step());
      } else {
        call_game_v(buttons[i].release);
      }
      if (buttons[i].key == HidNpadButton_A) a_press_step = -1;
      android_sent &= ~buttons[i].key;
    }
  }
  if (in->up & HidNpadButton_A) a_release_kept = 0; /* only ever the one release */
  /* L/R in the shop switch tabs, like ZL/ZR (ControllableMenuSystem::left/
   * rightTrigger move between the tab widgets). Sent to JoystickHW, L (elixirs)
   * closed the shop instead; controller.c skips L/R there. */
  {
    extern int shop_menu_active(void);
    static u64 shoulder_sent;
    static const struct { u64 key; const char *press, *release; } sh[] = {
      { HidNpadButton_L, "nativeButtonLeftTriggerPressed", "nativeButtonLeftTriggerReleased" },
      { HidNpadButton_R, "nativeButtonRightTriggerPressed", "nativeButtonRightTriggerReleased" },
    };
    for (int k = 0; k < 2; k++) {
      if ((in->down & sh[k].key) && shop_menu_active()) {
        debugPrintf("[input] %s (shoulder button in the shop)\n", sh[k].press);
        call_game_v(sh[k].press);
        shoulder_sent |= sh[k].key;
      }
      if ((in->up & sh[k].key) && (shoulder_sent & sh[k].key)) {
        call_game_v(sh[k].release);
        shoulder_sent &= ~sh[k].key;
      }
    }
  }
  extern int cutscene_comic;
  if ((in->down & HidNpadButton_B) && in_menu && !cutscene_comic) { debugPrintf("[input] Back (menu)\n"); call_game_v("nativeBackPressed"); }
  /* + opens the pause menu (controller.c) and, with a menu up, backs out of
   * it: + again closes the pause menu. */
  extern int plus_was_back;
  plus_was_back = (in->down & HidNpadButton_Plus) && in_menu && !cutscene_comic;
  if (plus_was_back) { debugPrintf("[input] Back (+ in menu)\n"); call_game_v("nativeBackPressed"); }

#endif
  /* Sticks are analog: controller.c writes them into the engine's JoystickHW
   * state every frame. Touch coordinates already match SCREEN_W x SCREEN_H. */
  int seen[16] = {0}, blocked_seen[16] = {0};
  static int touch_blocked[16]; /* fingers that started on the top bar stats */
  for (int i = 0; i < in->ntouch; i++) {
    int id = in->t[i].id & 15, x = in->t[i].x, y = in->t[i].y;
    {
      /* a finger that lands on the top bar's stat items never reaches the game (pointer.c) */
      extern int port_top_bar_stats(float, float);
      if (!touch_active[id] && !touch_blocked[id] && port_top_bar_stats((float)x, (float)y)) {
        touch_blocked[id] = 1;
        debugPrintf("[input] touch on the top bar stats ignored (%d,%d)\n", x, y);
      }
      if (touch_blocked[id]) { blocked_seen[id] = 1; continue; } /* no down / move for it */
    }
    seen[id] = 1;
    if (!touch_active[id]) {
      debugPrintf("[input] touch down %d,%d (finger %d)\n", x, y, id);
      { extern void pointer_trace_start(void); pointer_trace_start(); }
      if (on_touch) on_touch(jni_env, NULL, TOUCH_DOWN, x, y, id);
      touch_active[id] = 1;
    } else if (x != touch_x[id] || y != touch_y[id]) {
      if (on_touch) on_touch(jni_env, NULL, TOUCH_MOVE, x, y, id);
    }
    touch_x[id] = x;
    touch_y[id] = y;
  }
  for (int id = 0; id < 16; id++) if (!blocked_seen[id]) touch_blocked[id] = 0; /* lifted */
  for (int id = 0; id < 16; id++) {
    if (touch_active[id] && !seen[id]) {
      if (on_touch) on_touch(jni_env, NULL, TOUCH_UP, touch_x[id], touch_y[id], id);
      touch_active[id] = 0;
    }
  }
}

/* ---------------------------------------------------------------- main */

static void ensure_dirs(void) {
  mkdir(PORT_ROOT, 0777);
  mkdir(PORT_ROOT "/sdcard", 0777);
  mkdir(PORT_ROOT "/internal", 0777);
  mkdir(DATA_ROOT, 0777);
  mkdir(SAVE_ROOT, 0777);
  mkdir(CACHE_ROOT, 0777);
}

volatile int port_quit_requested;

int main(int argc, char *argv[]) {
  ensure_dirs();
  log_init();
  { /* keep the Ryujinx thread-name TLS slots (crash.c) from being garbage-collected */
    extern __thread const char *ryujinx_thread_name_slots[80];
    __asm__ volatile("" :: "r"(ryujinx_thread_name_slots[0]));
  }
  { extern void backup_saves(void); backup_saves(); }
  { /* Explicit marker, emulator only: exercise persistence failure paths without loading the game. */
    FILE *f=fopen(PORT_ROOT "/selftest_memfile","r"), *em=f ? fopen(PORT_ROOT "/emulator_mode","r"):NULL;
    if(f) fclose(f);
    if(em) { fclose(em); extern int memfile_selftest(void),backup_selftest(void); int failed=memfile_selftest();
      failed+=backup_selftest();
      {extern int prefs_selftest(void);failed+=prefs_selftest();}
      debugPrintf("[main] persistence self-test ended: %s\n",failed ? "FAIL":"PASS");
      log_close(); svcExitProcess(); }
  }
  debugPrintf("Six Guns NX loader starting\n");
  {extern void memfile_stress_fixture(void);memfile_stress_fixture();}
  { extern void net_init(void); net_init(); } /* marker `lan`: real sockets (net.c) */
  {
    FILE *f=fopen(PORT_ROOT "/selftest_network","r"),*em=f ? fopen(PORT_ROOT "/emulator_mode","r"):NULL;
    if(f) fclose(f);
    if(em) {fclose(em);extern int net_selftest(void);int failed=net_selftest();
      debugPrintf("[main] network self-test ended: %s\n",failed ? "FAIL":"PASS");log_close();svcExitProcess();}
  }
  { extern void heap_report(void); heap_report(); }

  {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[main] memory: total %llu MB, used %llu MB, heap %llu MB\n",
                (unsigned long long)(total >> 20), (unsigned long long)(used >> 20),
                (unsigned long long)((fake_heap_end - fake_heap_start) >> 20));
    extern void *sbrk(ptrdiff_t);
    debugPrintf("[main] heap %p..%p, sbrk now %p\n", fake_heap_start, fake_heap_end, sbrk(0));
  }
  {
    MemoryInfo mi; u32 pi;
    u64 sp = (u64)__builtin_frame_address(0);
    svcQueryMemory(&mi, &pi, sp);
    debugPrintf("[main] main thread stack: %llu KB (sp %p, region %p+0x%llx)\n",
                (unsigned long long)(mi.size >> 10), (void *)sp, (void *)mi.addr, (unsigned long long)mi.size);
  }
  AppletType at = appletGetAppletType();
  debugPrintf("[main] applet type %d\n", (int)at);
  if (at != AppletType_Application && at != AppletType_SystemApplication)
    fatal_error("Started in applet mode (type %d), probably from the Album.\n\n"
                "  Close this, then hold R while starting any game to open the\n"
                "  homebrew menu, and launch Six Guns from there.", (int)at);

  struct stat st;
  if (stat(SO_PATH, &st) != 0) fatal_error("Missing %s\nSee README for setup.", SO_PATH);
  if (stat(OBB_ROOT "/" MAIN_OBB, &st) != 0) fatal_error("Missing %s\nRun prepare_sd.sh again (see README).", OBB_ROOT "/" MAIN_OBB);
  if (stat(APK_PATH, &st) != 0) fatal_error("Missing %s\nRun prepare_sd.sh again (see README).", APK_PATH);

  appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
  jni_init();

  /* Order matters in Ryujinx: EGL context first, then map the engine, then
   * start SDL (its audio thread). Mapping before eglCreateContext hung the
   * emulated GPU; mapping while SDL's thread ran hung intermittently. */
  struct stat dbg;
  if (stat(PORT_ROOT "/debug_ipc_test", &dbg) == 0) {
    debugPrintf("[main] DEBUG: IPC handle trick only, no mapping\n");
    Handle h = get_own_process_handle();
    for (int i = 0; i < 30; i++) { debugPrintf("[main] ipc-test heartbeat %d (handle 0x%x)\n", i, h); svcSleepThread(500000000ULL); }
    fatal_error("debug_ipc_test finished");
  }
  if (stat(PORT_ROOT "/debug_small_map", &dbg) == 0) {
    debugPrintf("[main] DEBUG: mapping a 1 MB dummy region as code\n");
    Handle h = get_own_process_handle();
    void *buf = memalign(0x1000, 0x100000);
    memset(buf, 0, 0x100000);
    virtmemLock();
    void *dst = virtmemFindCodeMemory(0x100000, 0x1000);
    Result rc = svcMapProcessCodeMemory(h, (u64)dst, (u64)buf, 0x100000);
    if (R_SUCCEEDED(rc)) virtmemAddReservation(dst, 0x100000);
    virtmemUnlock();
    debugPrintf("[main] map rc 0x%x\n", rc);
    rc = svcSetProcessMemoryPermission(h, (u64)dst, 0x100000, Perm_Rx);
    debugPrintf("[main] perm rc 0x%x\n", rc);
    for (int i = 0; i < 30; i++) { debugPrintf("[main] small-map heartbeat %d\n", i); svcSleepThread(500000000ULL); }
    fatal_error("debug_small_map finished");
  }

  /* Startup order (each step chosen to dodge a Ryujinx hang; harmless on HW):
   * 1. allocate + relocate the engine image before GPU/audio touch the heap,
   * 2. bring up audio and GPU,
   * 3. map the image as code (skipped in emulator mode). */
  so_emulator_mode = stat(PORT_ROOT "/emulator_mode", &dbg) == 0;
  if (so_emulator_mode) debugPrintf("[main] emulator_mode marker found\n");
  if (so_load(&game, SO_PATH) < 0) fatal_error("Could not load libAndroid.so (see log.txt)");
  imports_set_module(&game);
  if (so_relocate(&game, resolve_import) < 0) fatal_error("Relocation failed (see log.txt)");
  { extern void telemetry_disable(so_module *); telemetry_disable(&game); }
  { extern void social_init(so_module *); social_init(&game); }
  { extern void tweaks_apply(so_module *); tweaks_apply(&game); }
  { extern void net_patch(so_module *); net_patch(&game); } /* `lan`: real host address (net.c) */

  debugPrintf("[main] SDL_Init\n");
  int sr = SDL_Init(SDL_INIT_AUDIO);
  debugPrintf("[main] SDL_Init returned %d\n", sr);

  debugPrintf("[main] init_egl\n");
  int r = init_egl();
  fatal_cleanup = deinit_egl;
  if (r) fatal_error("EGL init failed (%d)", r);
  debugPrintf("[main] EGL ready: %s\n", (const char *)eglQueryString(display, EGL_VERSION));
  {
    /* What the GL driver offers (Mesa nouveau; identical on hardware and in
     * Ryubing, since Mesa runs inside the game). ETC1 matters: nearly every
     * game texture is ETC1, and an unsupported format is decoded on the CPU. */
    const GLubyte *(*get_string)(GLenum) = (void *)eglGetProcAddress("glGetString");
    void (*get_iv)(GLenum, GLint *) = (void *)eglGetProcAddress("glGetIntegerv");
    if (get_string && get_iv) {
      debugPrintf("[gl] renderer %s / %s\n", get_string(GL_RENDERER), get_string(GL_VERSION));
      const char *ext = (const char *)get_string(GL_EXTENSIONS);
      debugPrintf("[gl] OES_compressed_ETC1_RGB8_texture: %s\n", ext && strstr(ext, "GL_OES_compressed_ETC1_RGB8_texture") ? "yes" : "no");
      GLint n = 0, fm[64];
      get_iv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
      if (n > 64) n = 64;
      get_iv(GL_COMPRESSED_TEXTURE_FORMATS, fm);
      char line[512] = ""; size_t o = 0;
      for (int i = 0; i < n && o < sizeof(line) - 12; i++) o += snprintf(line + o, sizeof(line) - o, " 0x%x", fm[i]);
      debugPrintf("[gl] %d compressed formats:%s\n", n, line);
    }
  }

  /* custom multiplayer maps from mods (reads custom_map.txt in the mod zips; patches engine code, so before finalize) */
  { extern void custom_maps_install(so_module *); custom_maps_install(&game); }
  { extern void gfx_settings_install(so_module *); gfx_settings_install(&game); } /* graphics menu hooks */
  if (so_finalize(&game) < 0) fatal_error("Could not map engine code (see log.txt)");

  bionic_tls_init_thread(); /* main thread runs engine code from here on */
  so_run_init(&game);
  debugPrintf("[main] engine static init done\n");

  /* Java side startup order on Android: JNI_OnLoad -> Game.nativeInit ->
   * GameRenderer.nativeInit (on GL thread) -> nativeResize -> nativeRender loop. */
  int (*JNI_OnLoad)(void *, void *) = sym("JNI_OnLoad");
  if (JNI_OnLoad) debugPrintf("[main] JNI_OnLoad -> 0x%x\n", JNI_OnLoad(jni_vm, NULL));

  void *game_cls = jni_find_class("com/gameloft/android/ANMP/GloftSXHM/Game");
  void *renderer_cls = jni_find_class("com/gameloft/android/ANMP/GloftSXHM/GameRenderer");

  /* PackageUtils (acp_utils) caches its Java methods here: APK path, save
   * folder, native lib dir. Android calls it from the Activity's onCreate. */
  jfn_v pu_init = (jfn_v)so_symbol(&game, JNAME("PackageUtils_JNIBridge", "NativeInit"));
  if (pu_init) {
    debugPrintf("[main] PackageUtils.JNIBridge.NativeInit\n");
    pu_init(jni_env, jni_find_class("com/gameloft/android/ANMP/GloftSXHM/PackageUtils/JNIBridge"));
  }
  /* Activity.onResume: refreshes software identifiers (APK path, locale, ...). */
  jfn_v pu_resume = (jfn_v)so_symbol(&game, JNAME("PackageUtils_JNIBridge", "NativeOnResume"));
  if (pu_resume) {
    debugPrintf("[main] PackageUtils.JNIBridge.NativeOnResume\n");
    pu_resume(jni_env, jni_find_class("com/gameloft/android/ANMP/GloftSXHM/PackageUtils/JNIBridge"));
  }

  { extern void mods_register(so_module *); mods_register(&game); } /* switch/sixguns/mods overrides OBB files */
  debugPrintf("[main] Game.nativeInit\n");
  call_game_v("nativeInit");

  /* GameRenderer.nativeInit(int manufacturer, int width, int height, String path):
   * confirmed from the engine (stores into g_manufacture, g_windowWidth, g_windowHeight). */
  jfn_renderer_init renderer_init = sym(JNAME("GameRenderer", "nativeInit"));
  jfn_ii renderer_resize = sym(JNAME("GameRenderer", "nativeResize"));
  jfn_v renderer_render = sym(JNAME("GameRenderer", "nativeRender"));
  jfn_v surface_pause = sym(JNAME("GameGLSurfaceView", "nativePause"));
  jfn_v surface_resume = sym(JNAME("GameGLSurfaceView", "nativeResume"));
  jfn_v renderer_destroy = sym(JNAME("GameRenderer", "nativeDestroy"));
  on_touch = sym(JNAME("GameGLSurfaceView", "nativeOnTouch"));
  jfn_kb_finish kb_finish = sym(JNAME("GameRenderer", "onKeyboardFinish"));
  if (!renderer_render) fatal_error("Engine has no nativeRender export");

  debugPrintf("[main] GameRenderer.nativeInit\n");
  if (renderer_init) renderer_init(jni_env, renderer_cls, 0, SCREEN_W, SCREEN_H, jni_new_string(DATA_ROOT "/"));
  if (renderer_resize) renderer_resize(jni_env, renderer_cls, SCREEN_W, SCREEN_H);
  {
    typedef void (*jfn_surface)(void *, void *, void *, int, int);
    jfn_surface sc = (jfn_surface)so_symbol(&game, JNAME("PackageUtils_JNIBridge", "NativeSurfaceChanged"));
    if (sc) sc(jni_env, jni_find_class("com/gameloft/android/ANMP/GloftSXHM/PackageUtils/JNIBridge"),
               jni_new_string("surface"), SCREEN_W, SCREEN_H);
  }
  call_game_v("nativeConnectedController");
#ifdef STOCK_INPUT
  /* OLD RELEASE variant: only the game's own Android controller support. */
  { extern void stock_input_init(so_module *); stock_input_init(&game); }
#else
  controller_init(&game);
  { extern void profiler_init(so_module *); profiler_init(&game); } /* marker `profile` */
  { extern void input_route_init(so_module *); input_route_init(&game); }
  { extern void pointer_init(so_module *, void *, void *); pointer_init(&game, (void *)on_touch, jni_env); }
#endif

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  hidInitializeTouchScreen();
  input_init();

  {
    /* Readback self-test: clear to a known color and screenshot it. */
    struct stat ss;
    if (stat(PORT_ROOT "/screenshots", &ss) == 0) {
      void (*clear_color)(float, float, float, float) = (void *)eglGetProcAddress("glClearColor");
      void (*clear)(unsigned) = (void *)eglGetProcAddress("glClear");
      clear_color(0.0f, 0.5f, 1.0f, 1.0f);
      clear(GL_COLOR_BUFFER_BIT);
      save_screenshot(PORT_ROOT "/screenshots/selftest_blue.tga");
    }
  }
  gl_debug_init();
  { extern void graphics_quality_init(void);graphics_quality_init(); }
  { extern void gpu_profile_init(void);gpu_profile_init(); }
#ifndef STOCK_INPUT
  { extern int text_patch_apply(void); text_patch_apply(); }
#endif
  debugPrintf("[main] entering render loop\n");
  int focused = 1;
  unsigned long frame = 0;
  /* "Close software" from the HOME menu arrives as an exit request (appletMainLoop returns
   * false) instead of killing the process, so the exit path below commits the saves. */
  appletLockExit();
  int system_exit = 1; /* cleared when the loop ends for the port's own reasons */
  while (appletMainLoop()) {
    /* Pause the engine while the HOME menu is open. */
    int now_focused = appletGetFocusState() == AppletFocusState_InFocus;
    if (now_focused != focused) {
      focused = now_focused;
      jfn_v f = focused ? surface_resume : surface_pause;
      if (f) f(jni_env, NULL);
      if (!focused) {
        /* The engine saves on pause; put those saves on the card now, the game may be
         * closed from the HOME menu while it is in the background. */
        extern void memfile_commit_all(unsigned), memfile_drain(void);
        memfile_commit_all(0);
        memfile_drain();
        debugPrintf("[main] focus lost: saves committed\n");
      }
    }
    if (!focused) { svcSleepThread(16000000); continue; }

    InputFrame in;
    input_poll(&pad, frame, &in);   /* one pad poll per frame (live or replayed) */
#ifdef STOCK_INPUT
    handle_input(&in); /* touch only (buttons skipped in this variant) */
    { extern void stock_input_frame(const InputFrame *, unsigned long); stock_input_frame(&in, frame); }
#else
    { extern void input_route_update(void); input_route_update(); }
    { extern void shop_ui_input(InputFrame *); shop_ui_input(&in); } /* the shop's list takes its buttons first */
    { extern void gfx_menu_input(InputFrame *); gfx_menu_input(&in); } /* title: graphics settings menu (takes all input while open) */
    { extern void menus_ui_input(InputFrame *); menus_ui_input(&in); } /* the port's pause / options screens */
    { extern void map_ui_input(InputFrame *); map_ui_input(&in); }
    { extern void pointer_update(InputFrame *); pointer_update(&in); }
    menu_stick_to_dpad(&in, frame);
    handle_input(&in);
    controller_update(&in);
    { extern void net_frame(void); net_frame(); }
    { extern void mp_debug_frame(void); mp_debug_frame(); } /* marker mp_debug: LAN match health log */
    { extern void deco_debug_frame(void); deco_debug_frame(); } /* marker deco_debug: level model diagnostics */
#endif
    if ((in.held & (HidNpadButton_Plus | HidNpadButton_Minus)) == (HidNpadButton_Plus | HidNpadButton_Minus))
      { system_exit = 0; break; } /* + and - together quits */
    if (port_quit_requested) { system_exit = 0; break; } /* the game itself quit (jni.c: Game.Exit / ExitApplication) */

    { extern void social_frame(void); social_frame(); }
    { extern void io_frame_begin(void); io_frame_begin(); }
    { extern void profiler_frame_begin(void); profiler_frame_begin(); }
    u64 t0 = armGetSystemTick();
    port_frame_no = frame;
    { extern const char *current_menu_class;extern void gpu_profile_begin(const char *);gpu_profile_begin(current_menu_class); }
    renderer_render(jni_env, renderer_cls);
    u64 t1 = armGetSystemTick();
    { extern void profiler_frame_end(void); profiler_frame_end(); }
    { extern void social_profiler_menu_check(void); social_profiler_menu_check(); }
    { extern void shop_tabs_frame(void); shop_tabs_frame(); }
    if (frame == 60) { extern void shop_ui_warmup(void); shop_ui_warmup(); }
    { extern void net_browser_frame(void); net_browser_frame(); }
    { extern void shop_grid_nav_frame(void); shop_grid_nav_frame(); }
    if (frame % 120 == 0) { extern void obb_index_frame(void); obb_index_frame(); }
#ifndef STOCK_INPUT
    { extern void screenshot_frame(void); screenshot_frame(); }
    { extern void hints_draw(void); hints_draw(); }
    { extern void options_camera_draw(void); options_camera_draw(); }
    { extern void shop_ui_overlay(void); shop_ui_overlay(); }
    { extern void menus_ui_overlay(void); menus_ui_overlay(); }
    { extern void gfx_overlay(void); gfx_overlay(); } /* graphics button / menu on the title */
    { extern void map_ui_overlay(void); map_ui_overlay(); }
    { extern void pointer_draw(void); pointer_draw(); }
    { extern void screenshot_frame_post(void); screenshot_frame_post(); }
#endif
    { extern void gpu_profile_end(void);gpu_profile_end(); }
    gl_debug_frame(frame, 1800);
    { extern void debug_popups_frame(void); debug_popups_frame(); }
    {
      extern const char *debug_shot_request;
      if (debug_shot_request) {
        char path[160];
        snprintf(path, sizeof(path), PORT_ROOT "/screenshots/%s.tga", debug_shot_request);
        debug_shot_request = NULL;
        save_screenshot(path);
      }
    }
    /* An old screenshots folder must not keep expensive readbacks running during normal play. */
    if (input_debug_capture_active() && (frame < 3 || frame % 300 == 0 || (in.held && frame % 20 == 0))) {
      struct stat ss;
      if (stat(PORT_ROOT "/screenshots", &ss) == 0) {
        char path[128];
        snprintf(path, sizeof(path), PORT_ROOT "/screenshots/frame_%06lu.tga", frame);
        save_screenshot(path);
      }
    }
    /* Loading screens: no vsync. The engine loads a level in slices, one per
     * loading-screen frame, and with vsync every slice that finished early
     * waited for the next screen refresh. Back to vsync for everything else. */
    {
      extern const char *current_menu_class;
      static int interval = 1;
      extern int gfx_swap_interval(void); /* graphics menu: 1 = 60 fps, 2 = 30 fps */
      int want = strstr(current_menu_class, "LoadingScreen") ? 0 : gfx_swap_interval();
      if (want != interval) {
        interval = want;
        eglSwapInterval(display, want);
        debugPrintf("[main] swap interval %d%s\n", want, want ? "" : " (loading screen)");
      }
    }
    eglSwapBuffers(display, surface);
    {
      /* Without vsync the main thread never blocks, and SDL's audio thread
       * shares its core (0): in Ryubing about every other start the sound
       * stopped for good while the title loaded (the audio backend ran dry and
       * never restarted; the game's AudioTrack writer then waited forever).
       * A short sleep per loading frame leaves it time to run. */
      extern const char *current_menu_class;
      if (strstr(current_menu_class, "LoadingScreen")) svcSleepThread(500000);
    }
    u64 t2 = armGetSystemTick();
    handle_keyboard(kb_finish, renderer_cls);
    {
      extern unsigned long jni_call_count;
      static u64 render_ns, swap_ns, input_ns, last_tick;
      static unsigned long last_jni;
      render_ns += armTicksToNs(t1 - t0);
      /* Log frames that miss two vsyncs, with the file I/O they did, to find
       * hitches on hardware (menus, shop, cutscenes). Rate-limited. */
      {
        double ms = armTicksToNs(t1 - t0) / 1e6;
        static unsigned hitches;
        /* all hitches for the first 400, then only the big ones (> 100 ms) */
        if (ms > 34.0 && frame > 120 && (hitches < 400 || (ms > 100.0 && hitches < 3000))) {
          hitches++;
          extern void io_frame_hitch(unsigned long, double);
          io_frame_hitch(frame, ms);
        }
        { extern void profiler_frame(double); profiler_frame(ms); }
      }
      swap_ns += armTicksToNs(t2 - t1);
      if (last_tick) input_ns += armTicksToNs(t0 - last_tick);
      last_tick = t2;
      if (frame % 1800 == 0 && frame) {
        debugPrintf("[perf] last 1800 frames: game %.1f ms, swap %.1f ms, between %.1f ms per frame; %.1f JNI calls/frame\n",
                    render_ns / 1800e6, swap_ns / 1800e6, input_ns / 1800e6, (jni_call_count - last_jni) / 1800.0);
        render_ns = swap_ns = input_ns = 0;
        {
          /* malloc'd bytes in use (the heap itself is reserved up front), to watch
           * growth now that Game::UnloadGameData is a no-op (tweaks.c) */
          struct mallinfo mi = mallinfo();
          debugPrintf("[perf] malloc in use %lu MB (arena %lu MB)\n", (unsigned long)mi.uordblks >> 20,
                      (unsigned long)mi.arena >> 20);
        }
        if (frame % 18000 == 0) { extern void jni_dump_call_counts(void); jni_dump_call_counts(); }
        { extern void io_stats_log(const char *), io_stats_mark(void); io_stats_log("1800 frames"); io_stats_mark(); }
        { extern void mmap_cache_stats(void); mmap_cache_stats(); }
        { extern void profiler_report(void); profiler_report(); }
        { extern void tex_budget_stats(void); tex_budget_stats(); }
        { extern void sprite_cache_stats(void); sprite_cache_stats(); }
        { extern void text_cache_stats(void); text_cache_stats(); }
        { extern void grid_cull_stats(void); grid_cull_stats(); }
        { extern void fdpool_stats(void); fdpool_stats(); }
        { extern void gl_tex_stats(void); gl_tex_stats(); }
        last_jni = jni_call_count;
      }
    }
    if (++frame == 1 || frame % 600 == 0) debugPrintf("[main] frame %lu\n", frame);
    /* Saves live in memory while open (memfile.c); put settled ones on disk. */
    if (frame % 60 == 0) { extern void memfile_commit_all(unsigned); memfile_commit_all(2000); }
  }

  debugPrintf("[main] exiting\n");
  { extern void memfile_commit_all(unsigned), memfile_drain(void); memfile_commit_all(0); memfile_drain(); }
  input_close();
  if (surface_pause) surface_pause(jni_env, NULL); /* the engine writes its saves here */
  { extern void memfile_commit_all(unsigned), memfile_drain(void); memfile_commit_all(0); memfile_drain(); }
  { extern unsigned memfile_error_count(void);
    if(memfile_error_count()) fatal_error("Some progress could not be saved to the SD card. Previous saves were preserved. Check free space and the card before playing again."); }
  { extern void backup_clean_exit(void); backup_clean_exit(); } /* every save written: last clean state */
  /* The engine's own teardown (renderer_destroy -> appDestroy -> Game::Shutdown) crashes
   * offline: hardware logs 2026-11-12 show data aborts in ~ClanWarManager and in
   * MGR_Menus::Unload (~Menu_MP_FriendsMain jumping to 0x111), both after every save was
   * written. The process ends right after, so the teardown is skipped; marker
   * `engine_shutdown` runs it as before. */
  {
    FILE *f = fopen(PORT_ROOT "/engine_shutdown", "r");
    if (f) {
      fclose(f);
      if (renderer_destroy) renderer_destroy(jni_env, renderer_cls);
      extern void memfile_commit_all(unsigned), memfile_drain(void);
      memfile_commit_all(0); memfile_drain(); /* saves made during the teardown */
    } else {
      debugPrintf("[main] saves committed; engine teardown skipped\n");
    }
  }
  /* Closed from the HOME menu: the system owns the screen and audio now, and the process ends in a
   * moment anyway. Tearing EGL/SDL down there ran after the log was closed, and the Switch then
   * reported "the software was closed because an error occurred" (owner, hardware 2026-11-15). */
  if (!system_exit) { deinit_egl(); SDL_Quit(); }
  else debugPrintf("[main] closed from the HOME menu: graphics/audio teardown skipped\n");
  log_close();
  appletUnlockExit(); /* hand the exit back to the system (appletLockExit above) */
  /* The engine stays mapped as code (and its threads alive); end the process
   * so the next launch gets a clean one instead of hbloader reusing this. */
  svcExitProcess();
  return 0;
}

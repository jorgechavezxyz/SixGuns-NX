/* Native analog controller support.
 *
 * The engine contains a complete console-style controller scheme (built for
 * iOS "extended gamepad" controllers) that the Android build never feeds:
 *
 *   JoystickHW (the singleton from JoystickHW::Default()):
 *     +0x00 u8  connected
 *     +0x01 u8  extended (analog sticks present)
 *     +0x04     HWState (0xbc bytes), read by:
 *                 MultipleJoystick::UpdateJoystick  -> state+0x6c  left stick  (x, y floats)
 *                 PlayerCtrl::UpdateFreeCamera      -> state+0x94  right stick (x, y floats, *6.0)
 *
 *   JoystickHW::Dispatch(type, const JoysticMSG&) delivers button events to
 *   subscribers (PlayerCtrl, in-game menus, the menu cursor). JoysticMSG is
 *   16 bytes: int phase; u32 down-flag (bit 0); for button types 6..17
 *   Dispatch derives press/hold/release from the down flag itself.
 *
 *   PlayerCtrl::OnJoystickButtonPressed event types:
 *     0 controller connected (StartJoystickInterfaceMode)   1,2 pause menu
 *     6 reload   7 change weapon   8 elixirs   9 A / interact
 *     10 grenade 11 fire           12 turn 180 13 aim
 *     14 up      15 down           16 left     17 right
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>
#include "config.h"
#include "so_util.h"
#include "input.h"
#include "input_route.h"
#include "util.h"

enum {
  EV_CONNECTED = 0, EV_PAUSE = 1,
  EV_RELOAD = 6, EV_CHANGE_WEAPON = 7, EV_ELIXIRS = 8, EV_A = 9,
  EV_GRENADE = 10, EV_FIRE = 11, EV_TURN180 = 12, EV_AIM = 13,
  EV_UP = 14, EV_DOWN = 15, EV_LEFT = 16, EV_RIGHT = 17,
};

typedef struct { int32_t phase; uint32_t down; uint64_t pad; } joy_msg;

static uint8_t *hw;  /* JoystickHW instance */
static void *(*gameplay_instance)(int create);
static int (*cinematic_enabled)(void *gameplay);
static u64 swallow_release; /* buttons whose press was consumed by a cutscene skip */

/* Engine bug workaround: PlayerCtrl subscribes to JoystickHW in its
 * constructor but its destructor never calls JoystickHW::OnCallerDestroy, so
 * when the player is rebuilt (e.g. skipping the tutorial cutscene) the freed
 * controller stays subscribed and the next button event calls into freed
 * memory. Android never feeds JoystickHW, so this never showed there.
 *
 * Subscribers: std::vector<Entry> at JoystickHW+0xc0 (begin) / +0xc8 (end),
 * Entry = 0x28 bytes { void *caller; boost::function4 fn; }, fn's vtable
 * pointer at +8 identifies the callback type (same for every PlayerCtrl). */
static void *(*level_instance)(int create);
static void *(*level_get_player)(void *level);
static void *(*player_get_controller)(void *player);
static void (*on_caller_destroy)(void *hw, void *caller);
static uintptr_t playerctrl_fn_vtable; /* learned from the live controller's entry */

static void purge_stale_playerctrls(void) {
  if (!level_instance || !level_get_player || !player_get_controller || !on_caller_destroy) return;
  void *ctrl = NULL;
  void *level = level_instance(0);
  void *player = level ? level_get_player(level) : NULL;
  if (player) ctrl = player_get_controller(player);

  uint8_t *begin = *(uint8_t **)(hw + 0xc0), *end = *(uint8_t **)(hw + 0xc8);
  if (ctrl) {
    for (uint8_t *e = begin; e < end; e += 0x28)
      if (*(void **)e == ctrl) { playerctrl_fn_vtable = *(uintptr_t *)(e + 8); break; }
  }
  if (!playerctrl_fn_vtable) return;

  /* Remove PlayerCtrl subscriptions that aren't the live controller. Restart
   * the scan after each removal since the vector shifts. */
  for (int guard = 0; guard < 16; guard++) {
    void *stale = NULL;
    begin = *(uint8_t **)(hw + 0xc0); end = *(uint8_t **)(hw + 0xc8);
    for (uint8_t *e = begin; e < end; e += 0x28)
      if (*(uintptr_t *)(e + 8) == playerctrl_fn_vtable && *(void **)e != ctrl) { stale = *(void **)e; break; }
    if (!stale) break;
    debugPrintf("[pad] removing stale PlayerCtrl subscriber %p (live: %p)\n", stale, ctrl);
    on_caller_destroy(hw, stale);
  }
}
/* Marker `give_grenades` (testing): once per launch, as soon as the player
 * exists, every throwable weapon the player has (AttackMgr at Player+0x1b8,
 * GetThrowableWeaponsIndices -> AttackMgr::GetWeapon -> WeaponDef::GetAmmoType)
 * is topped up to 10 with ItemMgr::IncreaseAmmoCount. R throws the selected one
 * (PlayerCtrl::_FireGrenadeAction, event 10 with JoystickHW::IsExtended),
 * D-pad Down picks the next type. */
so_module *port_engine_module(void);
static void give_grenades_once(void) {
  static int state = -1; /* -1 unchecked, 0 off/done, 1 waiting for the player */
  static void *(*item_inst)(int);
  static int (*ammo_count)(void *, int);
  static void (*ammo_add)(void *, int, int);
  static void *(*throwables)(void *);
  static void *(*get_weapon)(void *, int);
  static int (*ammo_type)(void *);
  static void *(*game_inst)(int);
  static int (*tut_done)(void *);
  if (state < 0) {
    FILE *f = fopen("sdmc:/switch/sixguns/give_grenades", "r");
    state = f != NULL;
    if (f) fclose(f);
    so_module *mod = port_engine_module();
    item_inst = (void *)so_symbol(mod, "_ZN9SingletonI7ItemMgrE11GetInstanceEb");
    ammo_count = (void *)so_symbol(mod, "_ZNK7ItemMgr12GetAmmoCountEi");
    ammo_add = (void *)so_symbol(mod, "_ZN7ItemMgr17IncreaseAmmoCountEii");
    throwables = (void *)so_symbol(mod, "_ZNK9AttackMgr26GetThrowableWeaponsIndicesEv");
    get_weapon = (void *)so_symbol(mod, "_ZN9AttackMgr9GetWeaponEi");
    ammo_type = (void *)so_symbol(mod, "_ZN9WeaponDef11GetAmmoTypeEv");
    game_inst = (void *)so_symbol(mod, "_ZN9SingletonI4GameE11GetInstanceEb");
    tut_done = (void *)so_symbol(mod, "_ZNK4Game19IsTutorialCompletedEv");
    if (state && !(item_inst && ammo_count && ammo_add && throwables && get_weapon && ammo_type)) {
      debugPrintf("[pad] give_grenades: engine functions missing\n");
      state = 0;
    }
  }
  if (state != 1 || !level_instance || !level_get_player) return;
  /* Not during the tutorial: grenades come later in the story, and a fresh save
   * had all of them from the first step (owner's report). Waits for the game's
   * own flag (Game::IsTutorialCompleted, saved as isTutorailCompleted). */
  if (game_inst && tut_done) {
    void *g = game_inst(0);
    if (!g || !(tut_done(g) & 1)) {
      static int said;
      if (!said) { said = 1; debugPrintf("[pad] give_grenades: waiting until the tutorial is completed\n"); }
      return;
    }
  }
  void *level = level_instance(0);
  uint8_t *player = level ? level_get_player(level) : NULL;
  void *im = item_inst(0);
  if (!player || !im) return;
  void *am = player + 0x1b8;
  int **vec = throwables(am); /* std::vector<int>: begin, end */
  if (!vec || !vec[0]) return;
  int n = 0;
  for (int *i = vec[0]; i < vec[1]; i++) {
    void *w = get_weapon(am, *i);
    if (!w) continue;
    int t = ammo_type(w), have = ammo_count(im, t);
    if (have < 10) ammo_add(im, t, 10 - have);
    debugPrintf("[pad] give_grenades: throwable weapon %d (ammo type %d): %d -> %d\n", *i, t, have, ammo_count(im, t));
    n++;
  }
  debugPrintf("[pad] give_grenades: %d throwable type(s) topped up (R throws, D-pad Down switches)\n", n);
  state = 0;
}

so_module *port_engine_module(void);
/* Cutscene skip: hold A for 5 s (owner's request); hints.c shows "Hold A to
 * skip" with a progress bar while a cutscene plays. */
#define CUTSCENE_HOLD_FRAMES 300
int cutscene_active, cutscene_hold, cutscene_comic;
int cutscene_skippable; /* hints.c: draw "Hold to skip" only where holding A can skip */
int cutscene_hold_frames = CUTSCENE_HOLD_FRAMES; /* marker debug_hold_frames (testing): file content = frames */

/* Elixirs (the three power-ups on the bottom bar): free, but each has a
 * 5-minute cooldown (owner's request). Used with L + D-pad Left/Up/Right
 * (PowerUpController::Get(1/2/3)). The game charges badges in PowerUp::
 * DoPressed; the port remembers the balance, lets the game activate the
 * elixir, then puts the balance back. The cooldown starts when
 * PowerUp::IsActivated() turns true. hints.c shows the key and the time left
 * while L is held. */
#define ELIXIR_COOLDOWN_S 300
static u64 elixir_ready[3]; /* armGetSystemTick when usable again */
int elixir_cooldown_s(int k) {
  if (k < 0 || k > 2 || !elixir_ready[k]) return 0;
  u64 now = armGetSystemTick();
  if (now >= elixir_ready[k]) return 0;
  return (int)(armTicksToNs(elixir_ready[k] - now) / 1000000000ULL) + 1;
}
int elixir_key_shown; /* L held in gameplay: hints.c draws the key */
int elixir_hooked; /* PowerUp::DoPressed replaced by elixir_do_pressed (tweaks.c) */

/* Replacement for PowerUp::DoPressed (hooked in tweaks.c, marker `paid_elixirs` keeps the game's).
 * Every way of using an elixir ends there: the game's L/Up/Right actions (PlayerCtrl::_LeftAction
 * etc., which the port's L + D-pad sends) and a touch on the bar icon (PowerUp::UpdateControl
 * tail-calls it at engine +0xc28d30). The original bought one with badges (InstantBuy) or consumed
 * one from the saddle (UseAndActivate(inv)), and a second press cancelled a running elixir, all
 * without any cooldown, so touching the icon bypassed the port's 5-minute cooldown.
 * Now: ignored while running or cooling down; otherwise PowerUp::UseAndActivate(this, NULL), which
 * activates (and in multiplayer sends PlayerUsePowerUp) without using an item: free. */
void elixir_do_pressed(uint8_t *pu) {
  static void (*use_and_activate)(void *, void *);
  static int (*is_active)(void *);
  if (!use_and_activate) {
    so_module *mod = port_engine_module();
    use_and_activate = (void *)so_symbol(mod, "_ZN7PowerUp14UseAndActivateEP13ItemInventory");
    is_active = (void *)so_symbol(mod, "_ZNK7PowerUp11IsActivatedEv");
    if (!use_and_activate || !is_active) return;
  }
  int k = *(int32_t *)(pu + 0x18) - 1; /* PowerUp type 1..3 = bar slot */
  if (is_active(pu) & 1) { debugPrintf("[pad] elixir %d: already running\n", k + 1); return; }
  int left = elixir_cooldown_s(k);
  if (left) { debugPrintf("[pad] elixir %d: cooling down, %d s left\n", k + 1, left); return; }
  use_and_activate(pu, NULL);
  int on = is_active(pu) & 1;
  if (on && k >= 0 && k <= 2) elixir_ready[k] = armGetSystemTick() + armNsToTicks((u64)ELIXIR_COOLDOWN_S * 1000000000ULL);
  debugPrintf("[pad] elixir %d: %s\n", k + 1, on ? "activated (free), 5 min cooldown" : "not activated");
}

static void *local_player_info(void) {
  static void *(*mgr)(void);
  static void *(*local_info)(void *);
  if (!mgr) {
    so_module *mod = port_engine_module();
    mgr = (void *)so_symbol(mod, "_Z27GetMultiplayerPlayerManagerv");
    local_info = (void *)so_symbol(mod, "_ZN24MultiplayerPlayerManager18GetLocalPlayerInfoEv");
  }
  void *m = mgr ? mgr() : NULL;
  return m && local_info ? local_info(m) : NULL;
}

/* Roll / somersault ("Touch the Roll icon to evade" / controller text "Press A
 * to evade"). PlayerCtrl::_A_ButtonAction rolls only when A has nothing else
 * to do (no interaction, no melee target, context icon 0x26 = 0xab, not aiming,
 * not driving or in cover): AttackMgr::AbortGrenade(Player+0x1b8), then
 * PlayerCtrl's vtable +0x10 event(2, model time, 4). B does exactly that, on
 * demand, with the same driving / cover / sniper-scope guards; the
 * turn-around moved to R3 (owner's request). Returns 1 if a roll started. */
static int player_roll(void) {
  static void (*abort_grenade)(void *);
  static int (*model_time)(void *);
  static int (*in_cover)(void *);
  if (!abort_grenade) {
    so_module *mod = port_engine_module();
    abort_grenade = (void *)so_symbol(mod, "_ZN9AttackMgr12AbortGrenadeEv");
    model_time = (void *)so_symbol(mod, "_ZNK3pig5scene13AnimatedModel7GetTimeEv");
    in_cover = (void *)so_symbol(mod, "_ZN5Actor9IsInCoverEv");
  }
  if (!abort_grenade || !model_time || !level_instance || !level_get_player || !player_get_controller) return 0;
  void *level = level_instance(0);
  uint8_t *player = level ? level_get_player(level) : NULL;
  uint8_t *pc = player ? player_get_controller(player) : NULL;
  if (!pc) return 0;
  void **pvt = *(void ***)player;
  if (((int (*)(void *))pvt[0x1c0 / 8])(player) & 1) return 0;   /* driving */
  if (in_cover && (in_cover(player) & 1)) return 0;
  if (pc[0x3c2]) return 0;                                       /* sniper scope */
  void *model = ((void *(*)(void *))pvt[0x158 / 8])(player);
  int t = model ? model_time(model) : 0;
  abort_grenade(player + 0x1b8);
  void **cvt = *(void ***)pc;
  ((void (*)(void *, int, int, int))cvt[0x10 / 8])(pc, 2, t, 4);
  return 1;
}

/* Marker `give_money` (testing): once per launch, as soon as the player exists,
 * coins are set to at least 1,000,000 and badges to at least 5,000. The
 * balance is written directly (MultiplayerPlayerInfo::SetPlayerMoney/
 * SetPlayerCash on the local player info, which is where ItemMgr::GetMoney
 * reads it), not with ItemMgr::IncreaseMoney: that one also counts the amount
 * as earned, which unlocked the "Gold! More Gold!" achievement at start.
 * Prices and balance checks are untouched: buying still costs what it costs. */
static void give_money_once(void) {
  static int state = -1;
  static void *(*mgr)(void);
  static void *(*local_info)(void *);
  static int (*get_coins)(void *), (*get_badges)(void *);
  static void (*set_coins)(void *, int), (*set_badges)(void *, int);
  if (state < 0) {
    FILE *f = fopen("sdmc:/switch/sixguns/give_money", "r");
    state = f != NULL;
    if (f) fclose(f);
    so_module *mod = port_engine_module();
    mgr = (void *)so_symbol(mod, "_Z27GetMultiplayerPlayerManagerv");
    local_info = (void *)so_symbol(mod, "_ZN24MultiplayerPlayerManager18GetLocalPlayerInfoEv");
    get_coins = (void *)so_symbol(mod, "_ZN21MultiplayerPlayerInfo14GetPlayerMoneyEv");
    get_badges = (void *)so_symbol(mod, "_ZN21MultiplayerPlayerInfo13GetPlayerCashEv");
    set_coins = (void *)so_symbol(mod, "_ZN21MultiplayerPlayerInfo14SetPlayerMoneyEi");
    set_badges = (void *)so_symbol(mod, "_ZN21MultiplayerPlayerInfo13SetPlayerCashEi");
    if (state && !(mgr && local_info && get_coins && get_badges && set_coins && set_badges)) {
      debugPrintf("[pad] give_money: engine functions missing\n");
      state = 0;
    }
  }
  if (state != 1 || !level_instance || !level_get_player) return;
  void *level = level_instance(0);
  void *m = mgr();
  void *info = m ? local_info(m) : NULL;
  if (!level || !level_get_player(level) || !info) return;
  int c = get_coins(info), b = get_badges(info);
  if (c < 1000000) set_coins(info, 1000000);
  if (b < 5000) set_badges(info, 5000);
  debugPrintf("[pad] give_money: coins %d -> %d, badges %d -> %d (set directly, not counted as earned)\n", c,
              get_coins(info), b, get_badges(info));
  state = 0;
}

static void (*dispatch)(void *self, int type, const joy_msg *msg);
int plus_was_back; /* main.c: this frame's + press closed a menu */
static void *(*cursor_instance)(int create);

/* Cursor::TriggerClick (run by JoystickHW's A event) taps the screen at the
 * cursor whenever its tappable-area vector (+0x60 begin / +0x68 end) is
 * non-empty. To send A without that tap, the vector is made to look empty
 * for the duration of the dispatch. See input_route.c. */
static void dispatch_a(int ev, const joy_msg *m) {
  extern int input_route_cursor_tap(void);
  uint8_t *cur = (!input_route_cursor_tap() && cursor_instance) ? cursor_instance(0) : NULL;
  void *saved_end = NULL;
  if (cur) { saved_end = *(void **)(cur + 0x68); *(void **)(cur + 0x68) = *(void **)(cur + 0x60); }
  dispatch(hw, ev, m);
  if (cur) *(void **)(cur + 0x68) = saved_end;
}

/* Switch button -> engine event. */
static const struct { u64 key; int ev; } map[] = {
  { HidNpadButton_ZR,    EV_FIRE },
  { HidNpadButton_ZL,    EV_AIM },
  { HidNpadButton_R,     EV_GRENADE },
  { HidNpadButton_L,     EV_ELIXIRS },
  { HidNpadButton_A,     EV_A },
  { HidNpadButton_B,     EV_TURN180 },
  { HidNpadButton_Y,     EV_RELOAD },
  { HidNpadButton_X,     EV_CHANGE_WEAPON },
  { HidNpadButton_Up,    EV_UP },
  { HidNpadButton_Down,  EV_DOWN },
  { HidNpadButton_Left,  EV_LEFT },
  { HidNpadButton_Right, EV_RIGHT },
};

/* JoystickHW::Default() returns boost::shared_ptr<JoystickHW> through the
 * AArch64 indirect-result register x8, which C can't express directly. */
static void call_sret(void *fn, void *result) {
  register void *x8 __asm__("x8") = result;
  __asm__ volatile("blr %1"
                   : "+r"(x8)
                   : "r"(fn)
                   : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11", "x12", "x13", "x14",
                     "x15", "x16", "x17", "x18", "x30", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16",
                     "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29",
                     "v30", "v31", "cc", "memory");
}

int controller_init(so_module *mod) {
  void *get_default = (void *)so_symbol(mod, "_ZN10JoystickHW7DefaultEv");
  dispatch = (void *)so_symbol(mod, "_ZN10JoystickHW8DispatchENS_16eJoystickMsgTypeERKNS_10JoysticMSGE");
  if (!get_default || !dispatch) {
    debugPrintf("[pad] JoystickHW not found, analog controls disabled\n");
    return -1;
  }
  /* Keep our reference forever (the shared_ptr is intentionally leaked). */
  static struct { void *ptr, *ctrl; } sp;
  call_sret(get_default, &sp);
  hw = sp.ptr;
  if (!hw) { debugPrintf("[pad] JoystickHW::Default returned null\n"); return -1; }

  gameplay_instance = (void *)so_symbol(mod, "_ZN9SingletonI11GS_GamePlayE11GetInstanceEb");
  cinematic_enabled = (void *)so_symbol(mod, "_ZN11GS_GamePlay18IsCinematicEnabledEv");
  level_instance = (void *)so_symbol(mod, "_ZN9SingletonI9GameLevelE11GetInstanceEb");
  level_get_player = (void *)so_symbol(mod, "_ZN9GameLevel9GetPlayerEv");
  player_get_controller = (void *)so_symbol(mod, "_ZN6Player13GetControllerEv");
  on_caller_destroy = (void *)so_symbol(mod, "_ZN10JoystickHW15OnCallerDestroyEPv");
  cursor_instance = (void *)so_symbol(mod, "_ZN9SingletonI6CursorE11GetInstanceEb");

  /* The Android build's own gamepad mode: AndroidController::GameController
   * (byte 0 = connected) makes the engine hide the touch stick and touch
   * fire/aim buttons and use controller highlighting in menus. Java normally
   * reports the pad through NativeBridgeHIDControllers.NativeControllerConnected. */
  {
    extern void *jni_env;
    extern void *jni_new_string(const char *s);
    extern void *jni_find_class(const char *name);
    void (*connected)(void *, void *, void *) = (void *)so_symbol(mod,
        "Java_com_gameloft_android_ANMP_GloftSXHM_GLUtils_controller_NativeBridgeHIDControllers_NativeControllerConnected");
    void *(*gc_instance)(void) = (void *)so_symbol(mod, "_ZN17AndroidController14GameController11getInstanceEv");
    if (connected)
      connected(jni_env, jni_find_class("com/gameloft/android/ANMP/GloftSXHM/GLUtils/controller/NativeBridgeHIDControllers"),
                jni_new_string("Nintendo Switch Controller"));
    uint8_t *gc = gc_instance ? gc_instance() : NULL;
    if (gc) {
      int was = gc[0];
      gc[0] = 1;
      debugPrintf("[pad] Android controller mode: bridge %s, flag was %d, now 1\n", connected ? "called" : "missing", was);
    } else {
      debugPrintf("[pad] AndroidController::GameController not found\n");
    }
  }

  hw[0] = 1; /* connected */
  hw[1] = 1; /* extended: analog sticks */
  joy_msg m = { 0 };
  dispatch(hw, EV_CONNECTED, &m);
  debugPrintf("[pad] analog controller attached to engine (JoystickHW %p)\n", (void *)hw);
  return 0;
}

/* Radial deadzone, rescaled so the edge of the deadzone reads as 0. */
static void stick(HidAnalogStickState s, float *ox, float *oy) {
  const float dead = 0.15f;
  float x = s.x / 32767.0f, y = s.y / 32767.0f;
  float len = sqrtf(x * x + y * y);
  if (len < dead) { *ox = *oy = 0.0f; return; }
  float scaled = (len - dead) / (1.0f - dead);
  if (scaled > 1.0f) scaled = 1.0f;
  *ox = x / len * scaled;
  *oy = y / len * scaled;
}

/* Camera speed. PlayerCtrl::UpdateFreeCamera turns the right stick (state+0x94)
 * into camera rotation (x 6.0); owners found it slow. The stick is shaped with
 * a curve (magnitude^1.4: finer small corrections) and scaled: on foot full
 * tilt is 1.35x the game's speed, on a horse 1.55x (the horse is fast and the
 * camera fell behind in turns). Half tilt stays close to the old speed.
 * Override with `camera_speed.txt` in switch/sixguns: "<foot> <horse>", e.g.
 * "1.35 1.55" (1.0 1.0 with curve 1.0 = the game's own feel), optional third
 * number = curve exponent. */
static int (*player_is_driving)(void *);
static float cam_foot = 2.92f, cam_horse = 3.35f, cam_exp = 1.4f; /* default = step 8 of camera_level_set (owner) */
static int cam_init;
static void camera_load(void) {
  if (cam_init) return;
  cam_init = 1;
  player_is_driving = (void *)so_symbol(port_engine_module(), "_ZN6Player9IsDrivingEv");
  FILE *f = fopen("sdmc:/switch/sixguns/camera_speed.txt", "r");
  if (f) {
    float a, b, c;
    int n = fscanf(f, "%f %f %f", &a, &b, &c);
    if (n >= 1 && a > 0.1f && a < 5.0f) cam_foot = a;
    if (n >= 2 && b > 0.1f && b < 5.0f) cam_horse = b;
    if (n >= 3 && c > 0.5f && c < 3.0f) cam_exp = c;
    fclose(f);
  }
  debugPrintf("[pad] camera speed: on foot %.2fx, on horse %.2fx, curve %.2f%s\n", cam_foot, cam_horse, cam_exp,
              f ? " (camera_speed.txt)" : "");
}

/* Camera speed setting (Options screen, ui_menus.c): 10 steps, geometric: step 5 = 1.8x the game's
 * speed on foot, each step x1.175 (step 1 ~0.95x, step 10 ~4.0x); the horse keeps its 1.55/1.35
 * ratio. Owner (hardware 2026-11-14): step 10 (then 2.7x) still felt slow. Saved as
 * camera_speed.txt ("<foot> <horse> <curve>"); a saved speed maps to the nearest step. */
#define CAM_MID 1.8f
#define CAM_STEP 1.175f
int camera_level_get(void) {
  camera_load();
  int l = (int)lroundf(5.0f + logf(cam_foot / CAM_MID) / logf(CAM_STEP));
  return l < 1 ? 1 : l > 10 ? 10 : l;
}
void camera_level_set(int level) {
  camera_load();
  if (level < 1) level = 1;
  if (level > 10) level = 10;
  cam_foot = CAM_MID * powf(CAM_STEP, (float)(level - 5));
  cam_horse = cam_foot * (1.55f / 1.35f);
  FILE *f = fopen("sdmc:/switch/sixguns/camera_speed.txt", "w");
  if (f) { fprintf(f, "%.2f %.2f %.2f\n", cam_foot, cam_horse, cam_exp); fclose(f); }
  debugPrintf("[pad] camera speed set to step %d (on foot %.2fx, on horse %.2fx)\n", level, cam_foot, cam_horse);
}

static void camera_curve(float *x, float *y) {
  camera_load();
  float len = sqrtf(*x * *x + *y * *y);
  if (len <= 0.0f) return;
  int riding = 0;
  if (player_is_driving && level_instance && level_get_player) {
    void *level = level_instance(0);
    void *player = level ? level_get_player(level) : NULL;
    riding = player && (player_is_driving(player) & 1);
  }
  float k = powf(len, cam_exp) / len * (riding ? cam_horse : cam_foot);
  *x *= k;
  *y *= k;
}

/* Called once per frame before the engine renders. */
/* HUD shortcuts in single player (owner's request, 2026-11-15). Two HUD buttons only had touch:
 *  - the "out of item" offer (no ammo / low health: ItemMgr OII page, button at ItemMgr+0x3a8): D-pad Down,
 *    which is ItemMgr::BuyOII (the game's own controller scheme maps it to Down too, without telling anyone);
 *  - the quest teleport under the minimap (GS_GamePlay::RenderPreselectedQuestButton: a mission is
 *    preselected (+0x390/+0x398) and IsQuestTeleportEnabled; pressed = a touch released in the touch area of
 *    action 4): D-pad Right taps that area. Right no longer opens the map in single player (- does).
 * hints.c draws a small D-pad arrow next to each button while it is shown. Not in multiplayer. */
int hud_oii_visible, hud_tp_visible;
float hud_oii_rect[4], hud_tp_rect[4]; /* screen pixels */
static void *(*item_mgr_inst)(int);
static void *(*oii_current)(void *);
static void (*buy_oii)(void *);
static int (*tp_enabled)(void *);
static void *(*touch_mgr_inst)(int);
static float *(*touch_area_by_action)(void *, int);
static int hud_mp_enabled(void) {
  static void *(*mp_inst)(void);
  static int (*mp_en)(void *);
  if (!mp_inst) {
    mp_inst = (void *)so_symbol(port_engine_module(), "_ZN11Multiplayer11GetInstanceEv");
    mp_en = (void *)so_symbol(port_engine_module(), "_ZN11Multiplayer9IsEnabledEv");
  }
  void *m = mp_inst && mp_en ? mp_inst() : NULL;
  return m && (mp_en(m) & 1);
}
static void hud_shortcuts_update(int usable) {
  static int init;
  if (!init) {
    init = 1;
    so_module *m = port_engine_module();
    item_mgr_inst = (void *)so_symbol(m, "_ZN9SingletonI7ItemMgrE11GetInstanceEb");
    oii_current = (void *)so_symbol(m, "_ZNK7ItemMgr18OII_GetCurrentItemEv");
    buy_oii = (void *)so_symbol(m, "_ZN7ItemMgr6BuyOIIEv");
    tp_enabled = (void *)so_symbol(m, "_ZN11GS_GamePlay22IsQuestTeleportEnabledEv");
    touch_mgr_inst = (void *)so_symbol(m, "_ZN12TouchManager11GetInstanceEi");
    touch_area_by_action = (void *)so_symbol(m, "_ZN12TouchManager23FindTouchAreaByActionIdEi");
  }
  hud_oii_visible = hud_tp_visible = 0;
  if (!usable || hud_mp_enabled()) return;
  uint8_t *im = item_mgr_inst && oii_current ? item_mgr_inst(0) : NULL;
  if (im && oii_current(im)) {
    uint8_t *btn = *(uint8_t **)(im + 0x3a8);
    float *r = btn ? (float *)(btn + 0xc4) : NULL;
    if (r && r[2] - r[0] > 4 && r[3] - r[1] > 4) {
      /* the button's rect is relative to the OII page, drawn at ItemMgr+0x398/+0x39c (OII_Render) */
      float ox = (float)*(int32_t *)(im + 0x398), oy = (float)*(int32_t *)(im + 0x39c);
      hud_oii_rect[0] = (r[0] + ox) * (SCREEN_H / 320.0f); hud_oii_rect[2] = (r[2] + ox) * (SCREEN_H / 320.0f);
      hud_oii_rect[1] = (r[1] + oy) * (SCREEN_H / 320.0f); hud_oii_rect[3] = (r[3] + oy) * (SCREEN_H / 320.0f);
      hud_oii_visible = 1;
    }
  }
  uint8_t *gp = gameplay_instance ? gameplay_instance(0) : NULL;
  if (gp && (*(void **)(gp + 0x390) || *(void **)(gp + 0x398)) && tp_enabled && (tp_enabled(gp) & 1) && touch_mgr_inst &&
      touch_area_by_action) {
    void *tm = touch_mgr_inst(-1);
    float *r = tm ? touch_area_by_action(tm, 4) : NULL;
    if (r && r[2] - r[0] > 4 && r[3] - r[1] > 4) {
      for (int k = 0; k < 4; k++) hud_tp_rect[k] = r[k] * (SCREEN_H / 320.0f); /* touch areas are in engine units */
      hud_tp_visible = 1;
      static int logged;
      if (!logged++) debugPrintf("[pad] quest teleport button at %.0f,%.0f-%.0f,%.0f\n", r[0], r[1], r[2], r[3]);
    }
  }
}
static void hud_shortcuts_input(u64 *down) {
  if ((*down & HidNpadButton_Down) && hud_oii_visible && buy_oii && item_mgr_inst) {
    buy_oii(item_mgr_inst(0));
    *down &= ~HidNpadButton_Down;
    debugPrintf("[pad] D-pad Down: bought the offered item\n");
  }
  if (*down & HidNpadButton_Right) {
    if (hud_tp_visible) {
      extern void pointer_tap_screen(float x, float y);
      pointer_tap_screen((hud_tp_rect[0] + hud_tp_rect[2]) / 2, (hud_tp_rect[1] + hud_tp_rect[3]) / 2);
      debugPrintf("[pad] D-pad Right: quest teleport\n");
    }
    *down &= ~HidNpadButton_Right; /* not the map any more (- opens it) */
  }
}

/* Pick-up / interact zone. PlayerCtrl::UpdateInteraction (0xa37218) shows the hand icon, and lets A
 * activate the nearest interactible, only while that object's projected centre lies inside a screen
 * rectangle: GUI level 0 item 0x9f, params 2/3 = x/y, 6/7 = w/h (times the 2D scale). The rectangle
 * suits the Android camera; with the Switch camera items near the feet fell outside it, so the icon
 * appeared only at odd camera angles (owner, hardware 2026-11-15). Which object is nearest is still the
 * game's own distance test (InteractibleBase radius); the port only widens the rectangle to the whole
 * screen. Marker `no_wide_interact` keeps the original zone. */
static void widen_interact_zone(void) {
  static int off = -1, logged;
  static void *(*gui_inst)(int), *(*gui_level)(void *, int);
  static int (*get_p)(void *, int, int);
  static void (*set_p)(void *, int, int, int);
  if (off < 0) {
    FILE *f = fopen(PORT_ROOT "/no_wide_interact", "r");
    off = f != NULL;
    if (f) fclose(f);
    so_module *mod = port_engine_module();
    gui_inst = (void *)so_symbol(mod, "_ZN9SingletonI7GameGUIE11GetInstanceEb");
    gui_level = (void *)so_symbol(mod, "_ZN7GameGUI8GetLevelEi");
    get_p = (void *)so_symbol(mod, "_ZN8GUILevel13GetParamValueEii");
    set_p = (void *)so_symbol(mod, "_ZN8GUILevel13SetParamValueEiii");
    if (!gui_inst || !gui_level || !get_p || !set_p) off = 1;
  }
  if (off) return;
  /* only in the world (the GUI object exists at boot before its element tables: NULL read in the
   * emulator), and only when item 0x9f's parameter block is there */
  void *level = level_instance ? level_instance(0) : NULL;
  if (!level || !level_get_player || !level_get_player(level)) return;
  void *gui = gui_inst(0);
  void *lv = gui ? gui_level(gui, 0) : NULL;
  if (!lv) return;
  void **elems = *(void ***)lv;
  if (!elems || !elems[0x9f]) return;
  int x = get_p(lv, 0x9f, 2), y = get_p(lv, 0x9f, 3), w = get_p(lv, 0x9f, 6), h = get_p(lv, 0x9f, 7);
  if (x == 0 && y == 0 && w == 4000 && h == 4000) return;
  if (logged++ < 3) debugPrintf("[pad] interact zone was %d,%d %dx%d: widened to the whole screen\n", x, y, w, h);
  set_p(lv, 0x9f, 2, 0); set_p(lv, 0x9f, 3, 0); set_p(lv, 0x9f, 6, 4000); set_p(lv, 0x9f, 7, 4000);
}

void controller_update(const InputFrame *in) {
  if (!hw) return;
  float *left = (float *)(hw + 4 + 0x6c);
  float *right = (float *)(hw + 4 + 0x94);
  HidAnalogStickState ls = in->ls, rs = in->rs;
  stick(ls, &left[0], &left[1]);
  stick(rs, &right[0], &right[1]);
  camera_curve(&right[0], &right[1]);
  /* Sniper scope: the zoom slider was touch-only. PlayerCtrl::UpdateSnipper turns the slider into
   * +0xac (between WeaponDef min/max FOV; larger = closer) and applies FOV = +0xa8 / +0xac every
   * frame; RenderSnipper draws the knob from +0xac too. While scoped the left stick moves it (up =
   * zoom in, full range in ~0.8 s at full tilt) instead of walking. */
  {
    static void *(*pc_inst)(int);
    static int (*in_sniper)(void *);
    static void *(*cur_weapon)(void *);
    static float (*fov_max)(void *), (*fov_min)(void *);
    static int init;
    if (!init) {
      init = 1;
      so_module *m = port_engine_module();
      pc_inst = (void *)so_symbol(m, "_ZN10PlayerCtrl11GetInstanceEi");
      in_sniper = (void *)so_symbol(m, "_ZN10PlayerCtrl17IsInSnipperAimingEv");
      cur_weapon = (void *)so_symbol(m, "_ZN9AttackMgr16GetCurrentWeaponEv");
      fov_max = (void *)so_symbol(m, "_ZN9WeaponDef9GetMaxFOVEv");
      fov_min = (void *)so_symbol(m, "_ZN9WeaponDef9GetMinFOVEv");
    }
    /* PlayerCtrl::GetInstance reads the multiplayer player manager, which does not exist before the
     * first level: only ask once there is a player (crashed at start-up otherwise). */
    void *level = level_instance ? level_instance(0) : NULL;
    uint8_t *player = level && level_get_player ? level_get_player(level) : NULL;
    uint8_t *pc = player && pc_inst ? pc_inst(-1) : NULL;
    if (pc && in_sniper && cur_weapon && fov_max && fov_min && (in_sniper(pc) & 1)) {
      void *w = cur_weapon(player + 0x1b8);
      float mx = w ? fov_max(w) : 0, mn = w ? fov_min(w) : 0;
      if (w && mx > mn) {
        float sy = ls.y / 32767.0f;
        if (sy > -0.2f && sy < 0.2f) sy = 0;
        float *zoom = (float *)(pc + 0xac);
        static float wrote = -1;
        static int over_logged;
        if (wrote >= 0 && *zoom != wrote && over_logged < 20) {
          over_logged++;
          debugPrintf("[pad] sniper zoom changed by the game: %.3f -> %.3f (touch %p, drag %d)\n", wrote, *zoom,
                      *(void **)(pc + 0xa0), pc[0x3c3]);
        }
        if (sy) {
          float z = *zoom + sy * (mx - mn) * (1.25f / 60.0f);
          *zoom = z < mn ? mn : z > mx ? mx : z;
          /* Apply it to the camera like UpdateSnipper's end does (CameraParams at +0x118, FOV =
           * +0xa8 / +0xac): the game only does that while its slider has a touch, so after a tap
           * on the slider the knob still moved but the view stopped zooming (owner, 2026-11-14). */
          static void (*set_fov)(void *, float);
          if (!set_fov) set_fov = (void *)so_symbol(port_engine_module(), "_ZN12CameraParams6SetFovEf");
          void *params = *(void **)(pc + 0x118);
          float base = *(float *)(pc + 0xa8);
          if (set_fov && params && *zoom > 0.0f) set_fov(params, base / *zoom);
          static int said;
          if (said++ < 40 && said % 4 == 1) debugPrintf("[pad] sniper zoom by stick: %.2f (%.2f..%.2f)\n", *zoom, mn, mx);
        }
        wrote = *zoom;
        left[0] = left[1] = 0; /* scoped: the left stick zooms, the player stays put */
      }
    }
  }
  {
    static int logged;
    static HidAnalogStickState last_l, last_r;
    if ((ls.x != last_l.x || ls.y != last_l.y || rs.x != last_r.x || rs.y != last_r.y) && logged < 40) {
      logged++;
      debugPrintf("[pad] sticks raw L(%d,%d) R(%d,%d) -> L(%.2f,%.2f) R(%.2f,%.2f); state now L(%.2f,%.2f)\n",
                  ls.x, ls.y, rs.x, rs.y, left[0], left[1], right[0], right[1],
                  ((float *)(hw + 4 + 0x6c))[0], ((float *)(hw + 4 + 0x6c))[1]);
    }
    last_l = ls; last_r = rs;
  }

  u64 down = in->down, up = in->up;
  static int no_hw_buttons = -1;
  if (no_hw_buttons < 0) {
    FILE *f = fopen("sdmc:/switch/sixguns/debug_no_hw_buttons", "r");
    no_hw_buttons = f != NULL;
    if (f) { fclose(f); debugPrintf("[pad] DEBUG: controller-scheme buttons disabled\n"); }
  }
  if (no_hw_buttons) return;
  if (down) debugPrintf("[pad] buttons down 0x%llx\n", (unsigned long long)down);
  if (down | up) purge_stale_playerctrls();

  /* A press during a cutscene skips it (PlayerCtrl::_A_ButtonAction checks
   * IsCinematicEnabled). The matching release has no such check and calls a
   * virtual on the player actor the skip just tore down, so a press that
   * lands in a cutscene must not get a release. */
  int in_cinematic = 0;
  if (gameplay_instance && cinematic_enabled) {
    void *gp = gameplay_instance(0);
    /* Only while gameplay is the running game state: the title menu's 3D
     * background scene runs as a cinematic of the (already created) gameplay
     * instance, and "Hold to skip" showed on the main menu. */
    static void *(*cur_state)(void);
    if (!cur_state) cur_state = (void *)so_symbol(port_engine_module(), "_ZN9GameState15GetCurrentStateEv");
    in_cinematic = gp && cinematic_enabled(gp) && cur_state && cur_state() == gp;
    /* The death screen (Popup::ShowResurrectPopup) is shown while the death camera
     * keeps the cinematic flag on: with a popup or menu up (input route UI) it is not
     * a cutscene, A must reach the popup and "Hold to skip" must not show. */
    extern int input_route(void);
    if (in_cinematic && input_route() == 0) in_cinematic = 0;
  }
  /* A press in a cutscene skips it. Ignore A for the first second of a
   * cutscene, so the press that started a conversation (or a quick second
   * press to "advance" it) can't skip the scene before it has played: the
   * sheriff in Prosperidad was being skipped. */
  /* Cutscenes are skipped only by HOLDING A for 5 s (CUTSCENE_HOLD_FRAMES): taps were
   * skipping them by accident (mashing A at the end of a mission skipped the
   * short closing cutscene). The skip is sent as a press on the 45th held
   * frame. */
  static int a_hold;
  /* The hold is measured in time (frames at 60 per second), not counted frames: slow frames in a heavy
   * cutscene made "5 s" take longer, and the bar's progress no longer depends on the frame rate. */
  static u64 hold_t0;
  /* Comics (game state 10, GS_Comics) count as cutscenes too: one universal
   * skip. Their A goes nowhere (main.c drops it); after the hold the comic is
   * ended the way its old skip button did: SoundMgr::StopMusic(0, false, true)
   * then GameState::PopState(). */
  static int (*check_comic)(void);
  static void *(*snd_inst)(int);
  static void (*stop_music)(void *, unsigned, int, int);
  static void (*pop_state)(void);
  if (!check_comic) {
    so_module *mod = port_engine_module();
    check_comic = (void *)so_symbol(mod, "_ZN14ControllerUtil10checkComicEv");
    snd_inst = (void *)so_symbol(mod, "_ZN9SingletonI8SoundMgrE11GetInstanceEb");
    stop_music = (void *)so_symbol(mod, "_ZN8SoundMgr9StopMusicEjbb");
    pop_state = (void *)so_symbol(mod, "_ZN9GameState8PopStateEv");
  }
  int in_comic = check_comic && (check_comic() & 1);
  /* Debug marker `debug_comic` (file content = comic name, e.g. "comics3"): ~10 s into gameplay
   * the port starts that comic the way Lua PlayComics does (new GS_Comics(name); Init();
   * GameState::SetState), to test comic handling without replaying the story. */
  {
    static int dbg = -1, frames, delay = 600;
    static char name[32];
    if (dbg < 0) {
      dbg = 0;
      FILE *f = fopen(PORT_ROOT "/debug_comic", "r");
      if (f) {
        char line[64];
        if (fgets(line, sizeof(line), f)) {
          int d = 0;
          if (sscanf(line, "%31s %d", name, &d) >= 1 && name[0]) dbg = 1;
          if (d > 0) delay = d; /* optional: frames into gameplay (default 600) */
        }
        fclose(f);
      }
    }
    if (dbg == 1) {
      static void *(*gp_inst)(int), *(*cur)(void), *(*op_new)(size_t);
      static void (*ctor)(void *, const char *), (*init)(void *), (*set_state)(void *);
      if (!cur) {
        so_module *mod = port_engine_module();
        gp_inst = (void *)so_symbol(mod, "_ZN9SingletonI11GS_GamePlayE11GetInstanceEb");
        cur = (void *)so_symbol(mod, "_ZN9GameState15GetCurrentStateEv");
        op_new = (void *)so_symbol(mod, "_Znwm");
        ctor = (void *)so_symbol(mod, "_ZN9GS_ComicsC1EPKc");
        init = (void *)so_symbol(mod, "_ZN9GS_Comics4InitEv");
        set_state = (void *)so_symbol(mod, "_ZN9GameState8SetStateEPS_");
      }
      void *gp = gp_inst ? gp_inst(0) : NULL;
      if (gp && cur && cur() == gp && !cutscene_active) frames++;
      if (frames == delay && op_new && ctor && init && set_state) {
        dbg = 2;
        debugPrintf("[pad] debug_comic: starting %s\n", name);
        void *c = op_new(0x70);
        ctor(c, name);
        init(c);
        set_state(c);
      }
    }
  }
  {
    static int last = -1;
    int now = in_cinematic * 2 + in_comic;
    if (now != last) {
      void *(*cur_state)(void) = (void *)so_symbol(port_engine_module(), "_ZN9GameState15GetCurrentStateEv");
      void *st = cur_state ? cur_state() : NULL;
      const char *cls = "?";
      if (st) { void *ti = (*(void ***)st)[-1]; if (ti) cls = *(const char **)((char *)ti + 8); }
      debugPrintf("[pad] cutscene check: cinematic %d comic %d (state %s)\n", in_cinematic, in_comic, cls);
      last = now;
    }
  }
  cutscene_active = in_cinematic || in_comic;
  cutscene_comic = in_comic;
  /* A tutorial prompt confirmed with A ("Help the lady!") pauses the game
   * behind it: only A and the D-pad (its own controls) act there. + sent the
   * pause event under the prompt (hints gone, game stuck paused), B rolled. */
  int tut_prompt = input_route_tutorial_menu() != NULL;
  int gp_block = cutscene_active || tut_prompt;
  /* After a skip, A must be let go before it counts again: back-to-back cutscenes (comic -> intro
   * cinematic, comics5 -> comics5_1) started with the count already past the trigger frame, the bar
   * stayed full and nothing happened until A was pressed anew. */
  static int hold_needs_release, hold_cfg;
  #define hold_advance(counting) ((counting) ? (a_hold ? (int)(armTicksToNs(armGetSystemTick() - hold_t0) * 60 / 1000000000ULL) + 1 \
                                                       : (hold_t0 = armGetSystemTick(), 1)) : 0)
  if (!hold_cfg) {
    hold_cfg = 1;
    FILE *hf = fopen(PORT_ROOT "/debug_hold_frames", "r");
    int v = 0;
    if (hf) { if (fscanf(hf, "%d", &v) == 1 && v >= 10 && v <= 600) cutscene_hold_frames = v; fclose(hf); }
    if (cutscene_hold_frames != CUTSCENE_HOLD_FRAMES) debugPrintf("[pad] debug_hold_frames: skip after %d frames\n", cutscene_hold_frames);
  }
  if (!(in->held & HidNpadButton_A)) hold_needs_release = 0;
  int a_counts = (in->held & HidNpadButton_A) && !hold_needs_release;
  /* Cinematics the game marks as unskippable (GS_GamePlay +0x170 == 0): skipCinematic ignores them. */
  void *cin_gp = in_cinematic && gameplay_instance ? gameplay_instance(0) : NULL;
  int cin_skippable = cin_gp && *((uint8_t *)cin_gp + 0x170);
  /* Multiplayer: the death camera is a skippable cinematic, but skipping it (UpdateCinematic -> SkipDying)
   * never opens Menu_MP_DeathScreen, which only follows the camera ending on its own: the host stayed dead
   * with no buttons (owner, Switch LAN match 2026-11-15). No hold-to-skip in a match. */
  {
    static void *(*mp_inst)(void);
    static int (*mp_enabled)(void *);
    if (!mp_inst) {
      mp_inst = (void *)so_symbol(port_engine_module(), "_ZN11Multiplayer11GetInstanceEv");
      mp_enabled = (void *)so_symbol(port_engine_module(), "_ZN11Multiplayer9IsEnabledEv");
    }
    void *mpi = cin_skippable && mp_inst && mp_enabled ? mp_inst() : NULL;
    if (mpi && (mp_enabled(mpi) & 1)) cin_skippable = 0;
  }
  cutscene_skippable = in_comic || cin_skippable;
  if (in_comic) {
    a_hold = hold_advance(a_counts);
    cutscene_hold = a_hold;
    if (a_hold >= cutscene_hold_frames && pop_state) {
      debugPrintf("[pad] A held: skipping the comic\n");
      if (snd_inst && stop_music) stop_music(snd_inst(0), 0, 0, 1);
      pop_state();
      hold_needs_release = 1; a_hold = 0;
    }
  } else if (in_cinematic) {
    a_hold = hold_advance(a_counts && cin_skippable);
    cutscene_hold = a_hold;
    if (down & HidNpadButton_A) debugPrintf("[pad] A in a cutscene: %s\n", cin_skippable ? "hold 5 s to skip" : "this one cannot be skipped");
    down &= ~HidNpadButton_A;
    if (a_hold >= cutscene_hold_frames) {
      /* GS_GamePlay::skipCinematic sets the skip flag (+0x59, only if +0x170), which UpdateCinematic
       * acts on (SkipDying on the death camera). This is what Android's A release does
       * (DefaultButtonListener::aReleased). The old injected A press reached UpdateCinematic's
       * touch-button branch, which needs the +0x16c timer that tweaks.c removed with the button. */
      static void (*skip_cin)(void *);
      if (!skip_cin) skip_cin = (void *)so_symbol(port_engine_module(), "_ZN11GS_GamePlay13skipCinematicEv");
      debugPrintf("[pad] A held: skipping the cutscene%s\n", skip_cin ? "" : " (skipCinematic missing)");
      if (skip_cin) skip_cin(cin_gp);
      hold_needs_release = 1; a_hold = 0;
    }
  } else {
    a_hold = 0;
    cutscene_hold = 0;
  }
  /* B in a menu is Back (main.c), not turn-around. A release is only sent for
   * a press we sent: Dispatch would treat a lone release as a new press. */
  extern int game_menu_active(void);
  int in_menu = game_menu_active();
  if (!in_menu && !gp_block) { give_grenades_once(); give_money_once(); }
  if (!in_menu) widen_interact_zone();
  static u64 sent;
  /* L/R as modifiers (see tweaks.c joystick_state_kept): while L is held,
   * D-pad Left/Up/Right use elixir 1/2/3 from the bottom bar (L's own press
   * still shows the elixir controls). R: a tap throws the selected grenade
   * when released; held, D-pad Up/Down switch the grenade type and Left/Right
   * the weapon, and then nothing is thrown. */
  {
    extern int shop_menu_active(void);
    uint8_t *st = hw + 4; /* HWState: ButtonInput at +0xc and +0x1c, first byte = pressed */
    const u64 dpad = HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right;
    static int r_down, r_used;
    int usable = !in_menu && !shop_menu_active();
    st[0xc] = usable && (in->held & HidNpadButton_L) ? 1 : 0;
    if ((down & HidNpadButton_R) && usable) { r_down = 1; r_used = 0; }
    if (r_down && (down & dpad)) r_used = 1;
    st[0x1c] = r_down && (in->held & HidNpadButton_R) ? 1 : 0;
    /* R + D-pad Up/Down: the engine has Up = previous, Down = next grenade;
     * owner wanted it the other way round (natural scrolling). The swapped
     * event is sent as press + release right away and the D-pad press is
     * consumed, so its later release isn't sent either. */
    if (st[0x1c] && (down & (HidNpadButton_Up | HidNpadButton_Down))) {
      int ev = (down & HidNpadButton_Up) ? EV_DOWN : EV_UP;
      joy_msg pm = { 0, 1 }, rm = { 2, 0 };
      hw[0xd8 + ev] = 0;
      dispatch(hw, ev, &pm);
      dispatch(hw, ev, &rm);
      down &= ~(HidNpadButton_Up | HidNpadButton_Down);
      r_used = 1;
      debugPrintf("[pad] R + D-pad: %s grenade\n", ev == EV_DOWN ? "next" : "previous");
    }
    /* The key only while the game itself shows its elixir bar on L (it doesn't
     * before elixirs are unlocked) and in real gameplay: L showed the key over
     * the tutorial's "Help the lady!" prompt, before elixirs exist. */
    {
      static void *(*pu_ctrl3)(void *);
      if (!pu_ctrl3) pu_ctrl3 = (void *)so_symbol(port_engine_module(), "_ZNK6Player20GetPowerUpControllerEv");
      void *level = level_instance ? level_instance(0) : NULL;
      void *player = level && level_get_player ? level_get_player(level) : NULL;
      uint8_t *puc = player && pu_ctrl3 ? pu_ctrl3(player) : NULL;
      int bar = puc && puc[0];
      static int held_frames;
      int ok = st[0xc] && input_route() == ROUTE_GAME && !in_menu && !gp_block;
      held_frames = ok ? held_frames + 1 : 0;
      elixir_key_shown = ok && bar; /* the bar appears a frame after L goes down */
      if (ok && !bar && held_frames == 30) debugPrintf("[pad] L held but the game shows no elixir bar: no elixir key\n");
    }
    /* The elixir bar (PowerUpController controls) starts visible when a level
     * loads; L shows it (press) and hides it (release). Owner: hidden unless
     * L is held, so it always appears together with the D-pad key. */
    if (!(in->held & HidNpadButton_L) && !in_menu && !gp_block) {
      static void *(*pu_ctrl2)(void *);
      static void *(*get_tm)(void *);
      static void (*hide)(void *, void *);
      if (!hide) {
        so_module *mod = port_engine_module();
        pu_ctrl2 = (void *)so_symbol(mod, "_ZNK6Player20GetPowerUpControllerEv");
        get_tm = (void *)so_symbol(mod, "_ZN21MultiplayerPlayerInfo15GetTouchManagerEv");
        hide = (void *)so_symbol(mod, "_ZN17PowerUpController13HideControllsEP12TouchManager");
      }
      void *level = level_instance ? level_instance(0) : NULL;
      void *player = level && level_get_player ? level_get_player(level) : NULL;
      uint8_t *pc0 = player && player_get_controller ? player_get_controller(player) : NULL;
      uint8_t *puc = player && pu_ctrl2 ? pu_ctrl2(player) : NULL;
      void *info = pc0 ? *(void **)(pc0 + 0x3c8) : NULL;
      if (puc && puc[0] && info && get_tm && hide) { /* PowerUpController::IsControllsVisible = byte 0 */
        hide(puc, get_tm(info));
        static int logged;
        if (!logged++) debugPrintf("[pad] elixir bar hidden (shown while L is held)\n");
      }
    }
    /* L + D-pad never reaches the game's own elixir handler (it has no
     * cooldown): consumed always, used only while the elixir key is shown. */
    if (st[0xc] && !elixir_key_shown && (down & (HidNpadButton_Left | HidNpadButton_Up | HidNpadButton_Right)))
      down &= ~(HidNpadButton_Left | HidNpadButton_Up | HidNpadButton_Right);
    if (st[0xc] && elixir_key_shown && (down & (HidNpadButton_Left | HidNpadButton_Up | HidNpadButton_Right))) {
      static int (*get_money)(void *), (*get_cash)(void *);
      static void (*set_money)(void *, int), (*set_cash)(void *, int);
      static void *(*pu_ctrl)(void *);
      static void *(*pu_get)(void *, int);
      static int (*pu_active)(void *);
      if (!get_money) {
        so_module *mod = port_engine_module();
        get_money = (void *)so_symbol(mod, "_ZN21MultiplayerPlayerInfo14GetPlayerMoneyEv");
        get_cash = (void *)so_symbol(mod, "_ZN21MultiplayerPlayerInfo13GetPlayerCashEv");
        set_money = (void *)so_symbol(mod, "_ZN21MultiplayerPlayerInfo14SetPlayerMoneyEi");
        set_cash = (void *)so_symbol(mod, "_ZN21MultiplayerPlayerInfo13SetPlayerCashEi");
        pu_ctrl = (void *)so_symbol(mod, "_ZNK6Player20GetPowerUpControllerEv");
        pu_get = (void *)so_symbol(mod, "_ZNK17PowerUpController3GetEi");
        pu_active = (void *)so_symbol(mod, "_ZNK7PowerUp11IsActivatedEv");
      }
      int k = (down & HidNpadButton_Left) ? 0 : (down & HidNpadButton_Up) ? 1 : 2;
      int ev = k == 0 ? EV_LEFT : k == 1 ? EV_UP : EV_RIGHT;
      down &= ~(HidNpadButton_Left | HidNpadButton_Up | HidNpadButton_Right);
      /* Free use and the cooldown live in elixir_do_pressed (PowerUp::DoPressed hook), so touch
       * on the icon gets the same rules. Without the hook (marker `paid_elixirs`) the old
       * behaviour applies: the game charges, the port restores the balance, cooldown here. */
      extern int elixir_hooked;
      int left = elixir_hooked ? 0 : elixir_cooldown_s(k);
      if (left) {
        debugPrintf("[pad] elixir %d: cooling down, %d s left\n", k + 1, left);
      } else {
        void *info = elixir_hooked ? NULL : local_player_info();
        int money = info && get_money ? get_money(info) : -1, cash = info && get_cash ? get_cash(info) : -1;
        joy_msg pm = { 0, 1 }, rm = { 2, 0 };
        hw[0xd8 + ev] = 0;
        dispatch(hw, ev, &pm);
        dispatch(hw, ev, &rm);
        if (!elixir_hooked) {
          int activated = 0;
          void *level = level_instance ? level_instance(0) : NULL;
          void *player = level && level_get_player ? level_get_player(level) : NULL;
          void *pc = player && pu_ctrl ? pu_ctrl(player) : NULL;
          void *pu = pc && pu_get ? pu_get(pc, k + 1) : NULL;
          if (pu && pu_active) activated = pu_active(pu) & 1;
          if (info && set_money && money >= 0 && get_money(info) < money) set_money(info, money);
          if (info && set_cash && cash >= 0 && get_cash(info) < cash) set_cash(info, cash);
          if (activated) elixir_ready[k] = armGetSystemTick() + armNsToTicks((u64)ELIXIR_COOLDOWN_S * 1000000000ULL);
          debugPrintf("[pad] elixir %d: %s (badges kept at %d)\n", k + 1, activated ? "activated, 5 min cooldown" : "not activated", cash);
        }
      }
    }

    hud_shortcuts_update(usable && !gp_block && input_route() != ROUTE_UI);
    if (!st[0xc] && !st[0x1c] && usable && !gp_block && input_route() != ROUTE_UI && !hud_mp_enabled()) hud_shortcuts_input(&down);
    if (st[0x1c] && (down & dpad)) debugPrintf("[pad] R + D-pad: %s\n", (down & (HidNpadButton_Up | HidNpadButton_Down)) ? "grenade type" : "weapon");
    if (r_down && !(in->held & HidNpadButton_R)) {
      r_down = 0;
      if (!r_used && !in_menu && !gp_block) {
        joy_msg m = { 0, 1 };
        hw[0xd8 + EV_GRENADE] = 0;
        dispatch(hw, EV_GRENADE, &m);
        joy_msg r = { 2, 0 };
        dispatch(hw, EV_GRENADE, &r);
      }
    }
  }
  for (size_t i = 0; i < sizeof(map) / sizeof(*map); i++) {
    extern int input_route_hw(u64 key);
    /* B in a menu is Back (main.c); presses the Android scheme has are skipped. */
    extern int shop_menu_active(void);
    int shoulder_in_shop = (map[i].key & (HidNpadButton_L | HidNpadButton_R)) && shop_menu_active(); /* main.c: tabs */
    if (map[i].key == HidNpadButton_R) continue; /* tap/hold handling above */
    if (map[i].key == HidNpadButton_B) continue; /* roll (below); Back in menus is main.c */
    if ((down & map[i].key) && !(map[i].key == HidNpadButton_B && in_menu) && !shoulder_in_shop && !tut_prompt &&
        !(gp_block && map[i].key != HidNpadButton_A) && /* cutscenes: only A (hold to skip) */
        input_route_hw(map[i].key)) {
      joy_msg m = { 0, 1 };
      if (map[i].ev == EV_A) dispatch_a(map[i].ev, &m);
      else dispatch(hw, map[i].ev, &m);
      sent |= map[i].key;
      if (in_cinematic) swallow_release |= map[i].key;
    }
    if (up & map[i].key) {
      if (!(sent & map[i].key)) continue;
      sent &= ~map[i].key;
      if (swallow_release & map[i].key) {
        swallow_release &= ~map[i].key;
        debugPrintf("[pad] release swallowed (press skipped a cutscene)\n");
        /* Reset Dispatch's held-state byte for this event so the next press
         * is seen as a fresh press. */
        hw[0xd8 + map[i].ev] = 0;
        continue;
      }
      joy_msg m = { 2, 0 };
      dispatch(hw, map[i].ev, &m);
    }
  }
  /* ZL reliability. PlayerCtrl::_AimAction starts aiming (DoAiming) only on
   * the press edge (phase 0; holds are ignored), and DoAiming refuses while the
   * gun is holstered, being drawn, reloaded or switched. The touch aim button
   * keeps trying while held; a ZL press that landed in one of those moments
   * did nothing until ZL was pressed again. So while ZL is held and the player
   * isn't aiming (PlayerCtrl+0x111, set by AimingStart), the press is re-sent
   * every 6 frames: Dispatch's held byte (hw+0xd8+type) is cleared so it is
   * seen as a fresh press. The release still goes out once (CancelAiming). */
  {
    static int retry, retry_total, aimed;
    uint8_t *pc = NULL;
    /* ZL went down while a popup/menu had the input (Android scheme) and is
     * still held now that gameplay has it: send the press late. */
    {
      extern int input_route_hw(u64 key);
      if ((in->held & HidNpadButton_ZL) && !(down & HidNpadButton_ZL) && !(sent & HidNpadButton_ZL) && !in_menu &&
          !gp_block && input_route_hw(HidNpadButton_ZL)) {
        debugPrintf("[pad] ZL held into gameplay: sending the aim press now\n");
        hw[0xd8 + EV_AIM] = 0;
        joy_msg m = { 0, 1 };
        dispatch(hw, EV_AIM, &m);
        sent |= HidNpadButton_ZL;
      }
    }
    if ((sent & HidNpadButton_ZL) && (in->held & HidNpadButton_ZL) && level_instance && level_get_player &&
        player_get_controller) {
      void *level = level_instance(0);
      void *player = level ? level_get_player(level) : NULL;
      pc = player ? player_get_controller(player) : NULL;
    }
    if ((up & HidNpadButton_ZL) && retry_total)
      debugPrintf("[pad] ZL released: aiming %s, %d frame(s) held\n", aimed ? "started" : "never started", retry_total);
    if (!(in->held & HidNpadButton_ZL)) { retry_total = 0; aimed = 0; }
    else if (sent & HidNpadButton_ZL) { retry_total++; if (pc && pc[0x111]) aimed = 1; }
    if (pc && !pc[0x111] && !gp_block) {
      if (++retry % 6 == 0) {
        if (retry <= 60 && retry % 30 == 0) debugPrintf("[pad] ZL held, not aiming yet: re-sending aim (%d)\n", retry);
        hw[0xd8 + EV_AIM] = 0;
        joy_msg m = { 0, 1 };
        dispatch(hw, EV_AIM, &m);
      }
    } else {
      retry = 0;
    }
  }
  /* L3: take / leave cover. The HUD's cover icon (GUI item 0x26) is action 7:
   * PlayerCtrl::UpdateInteraction calls Player::FindCover / LeaveCover when
   * ActionManager::IsActionPressed(7). R3: evade (the "Roll icon", action 26),
   * i.e. the same as B. Gameplay only. */
  {
    static void *(*get_am)(void *);
    static void (*act_press)(void *, unsigned), (*act_release)(void *, unsigned);
    static int resolved, cover_sent;
    if (!resolved) {
      resolved = 1;
      so_module *mod = port_engine_module();
      get_am = (void *)so_symbol(mod, "_ZN21MultiplayerPlayerInfo16GetActionManagerEv");
      act_press = (void *)so_symbol(mod, "_ZN13ActionManager15OnActionPressedEj");
      act_release = (void *)so_symbol(mod, "_ZN13ActionManager16OnActionReleasedEj");
      if (!get_am || !act_press || !act_release) debugPrintf("[pad] L3 cover: engine functions missing\n");
    }
    void *am = NULL;
    if (get_am && act_press && act_release && level_instance && level_get_player && player_get_controller) {
      void *level = level_instance(0);
      void *player = level ? level_get_player(level) : NULL;
      uint8_t *pc = player ? player_get_controller(player) : NULL;
      void *info = pc ? *(void **)(pc + 0x3c8) : NULL;
      am = info ? get_am(info) : NULL;
    }
    /* Action 7 is the general context button (door, hand, wall icon: GUI item
     * 0x26); A already covers the rest. L3 only presses it while that icon is
     * the cover one: frame 0x56 "take cover" / 0x57 "leave cover"
     * (PlayerCtrl::UpdateInteraction, GUILevel::SetParamValue(0x26, 7, f)). */
    /* Popups in gameplay (death screen, quest popups, achievement, mission complete) leave no current
     * menu but route input to the UI: B/R3/L3 must not roll, turn or take cover behind them
     * (player_roll calls PlayerCtrl directly, past its own popup guard). */
    int ui_popup_up = input_route() == ROUTE_UI;
    if ((down & HidNpadButton_StickL) && am && !in_menu && !gp_block && !ui_popup_up) {
      static void *(*get_gui)(void *);
      static int (*get_param)(void *, int, int);
      if (!get_gui) {
        get_gui = (void *)so_symbol(port_engine_module(), "_ZN10PlayerCtrl11GetGUILevelEv");
        get_param = (void *)so_symbol(port_engine_module(), "_ZN8GUILevel13GetParamValueEii");
      }
      void *level = level_instance(0);
      void *player = level ? level_get_player(level) : NULL;
      void *pc = player ? player_get_controller(player) : NULL;
      void *gui = pc && get_gui ? get_gui(pc) : NULL;
      int icon = gui && get_param ? get_param(gui, 0x26, 7) : -1;
      if (icon == 0x56 || icon == 0x57) {
        act_press(am, 7);
        cover_sent = 1;
      }
      debugPrintf("[pad] L3: context icon 0x%x -> %s\n", icon,
                  icon == 0x56 ? "take cover" : icon == 0x57 ? "leave cover" : "no cover here");
    }
    if ((up & HidNpadButton_StickL) && cover_sent) {
      if (am) act_release(am, 7);
      cover_sent = 0;
    }
    if ((down & HidNpadButton_StickR) && !in_menu && !gp_block && !ui_popup_up) {
      joy_msg pm = { 0, 1 }, rm = { 2, 0 };
      hw[0xd8 + EV_TURN180] = 0;
      dispatch(hw, EV_TURN180, &pm);
      dispatch(hw, EV_TURN180, &rm);
      debugPrintf("[pad] R3: turn around\n");
    }
    if ((down & HidNpadButton_B) && !in_menu && !gp_block && !ui_popup_up)
      debugPrintf("[pad] B: roll %s\n", player_roll() ? "started" : "not possible here");
  }
  /* − opens the map in gameplay (classic layout). The controller scheme's own
   * map shortcut is PlayerCtrl::_RightAction without a shoulder held:
   * GS_GamePlay::SetStatePause(4) (the pause menu is SetStatePause(3)); the
   * same call is made here. Not with + held (+ and − together = quit), not
   * in menus, cutscenes or the tutorial (IsTutorialEnabled, as the game does). */
  {
    static void (*set_pause)(void *, int);
    static int (*tutorial_on)(void *);
    static int resolved;
    if (!resolved) {
      resolved = 1;
      so_module *mod = port_engine_module();
      set_pause = (void *)so_symbol(mod, "_ZN11GS_GamePlay13SetStatePauseEi");
      tutorial_on = (void *)so_symbol(mod, "_ZN11GS_GamePlay17IsTutorialEnabledEv");
    }
    if ((down & HidNpadButton_Minus) && !(in->held & HidNpadButton_Plus) && !in_menu && !gp_block && set_pause &&
        gameplay_instance) {
      extern int input_route(void);
      void *gp = gameplay_instance(0);
      if (gp && input_route() == 2 /* ROUTE_GAME */ && !(tutorial_on && (tutorial_on(gp) & 1))) {
        debugPrintf("[pad] - : map\n");
        set_pause(gp, 4);
      }
    }
  }
  /* + opens the pause menu in gameplay; in a menu main.c sends Back instead. */
  if ((down & HidNpadButton_Plus) && !plus_was_back && !in_comic && !tut_prompt) { joy_msg m = { 0, 1 }; dispatch(hw, EV_PAUSE, &m); }
}

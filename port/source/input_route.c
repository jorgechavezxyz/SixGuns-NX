/* Which of the engine's two controller schemes gets each button press.
 *
 * The engine has two complete gamepad schemes and the port used to feed both:
 *   - Android: ButtonManager -> DefaultButtonListener (main.c sends the
 *     nativeButton... / nativeDpad... calls). Handles every menu, popup, tutorial
 *     dialog and comic through ControllableMenuSystemList, and also has a
 *     gameplay mode (A fakes a touch on the interact button, ZR sets the fire
 *     action, ...).
 *   - iOS "extended gamepad": JoystickHW::Dispatch (controller.c). Drives
 *     PlayerCtrl in gameplay (analog, fire/aim/reload/grenade/horse), plus a
 *     few menus via Cursor/Menu_IGM_* subscribers.
 * Fed both, one press acted twice: dialogs skipped several pages, fire was
 * pressed and released by two paths, and on a horse the Android A path's fake
 * touch landed on the touch fire button and was never lifted (stuck fire).
 * Decompiled code: docs/input/.
 *
 * The controller scheme's Cursor turns an A press into a tap at the cursor
 * (screen centre) whenever any tappable area is registered, which the HUD
 * always does. In gameplay that tap hit whatever dialog the same A press
 * opened (talking to the sheriff jumped straight into the fight), so
 * controller.c hides the cursor's areas during A in gameplay
 * (input_route_cursor_tap()).
 *
 * Rule, decided at the press (the release always follows its press):
 *   ROUTE_UI        menus, popups, achievement popup, quest menu, lottery
 *                   tutorial, comic, not playing -> Android scheme only
 *   ROUTE_TUTORIAL  a tutorial step is up -> A and the D-pad to the Android
 *                   scheme (it confirms the step and handles tutorial menus),
 *                   everything else to JoystickHW
 *   ROUTE_GAME      gameplay, cutscenes -> JoystickHW only
 * L/R (no Android mapping), + (pause) and B (Back in menus, turn in game) are
 * handled separately in main.c / controller.c. */
#include <switch.h>
#include "so_util.h"
#include "input_route.h"
#include "util.h"

static void *(*popup_inst)(int);
static int (*popup_enabled)(void *);
static void *(*ach_inst)(int);
static int (*ach_popup)(void *);
static void *(*menus_inst)(int);
static void *(*cur_menu)(void *);
static void *(*quest_menu)(void *);
static int (*quest_shown)(void *);
static void *(*tut_menu)(void *);
static int (*tut_id)(void *);
static void *(*lot_menu)(void *);
static int (*lot_shown)(void *);
static int (*chk_comic)(void);
static void *(*gp_inst)(int);
static int (*gp_cinematic)(void *);
static int (*chk_playing)(void);
static int (*chk_actor)(void);

static int route = ROUTE_UI;
static int tut_step = -1;

/* Tutorial steps that have an Android controller menu (ControllableMenuSystem
 * Tutorial::confirm only acts on these; ControllableMenuSystemList::
 * createMenuSystem(Menu_Tutorial*) registers them). Every other tutorial
 * prompt is advanced by the controller scheme (PlayerCtrl actions and the
 * Cursor's tap), so A/D-pad go there instead. */
static const char *const tut_menu_step_syms[] = {
  "_ZN13Menu_Tutorial16k_tutorial_IntroE", "_ZN13Menu_Tutorial18k_tutorial_WelcomeE",
  "_ZN13Menu_Tutorial22k_tutorial_FirstRewardE", "_ZN13Menu_Tutorial22k_tutorial_BuyWeapon_0E",
  "_ZN13Menu_Tutorial22k_tutorial_BuyWeapon_1E", "_ZN13Menu_Tutorial22k_tutorial_BuyWeapon_2E",
  "_ZN13Menu_Tutorial21k_tutorial_FastTravelE", "_ZN13Menu_Tutorial19k_tutorial_EnterMapE",
  "_ZN13Menu_Tutorial21k_tutorial_EnterQuestE", "_ZN13Menu_Tutorial22k_tutorial_EnterQuest2E",
  "_ZN13Menu_Tutorial22k_tutorial_EnterQuest3E", "_ZN13Menu_Tutorial21k_tutorial_SpawnHorseE",
  "_ZN13Menu_Tutorial20k_tutorial_QuickItemE", "_ZN13Menu_Tutorial16k_tutorial_End_2E",
  "_ZN13Menu_Tutorial23k_tutorial_FourthRewardE", "_ZN13Menu_Tutorial24k_tutorial_QuestTeleportE",
};
#define N_TUT_MENU (sizeof(tut_menu_step_syms) / sizeof(*tut_menu_step_syms))
static int tut_menu_steps[N_TUT_MENU];
static int tut_menu_known;

static int tutorial_step_has_menu(int step) {
  for (int i = 0; i < tut_menu_known; i++) if (tut_menu_steps[i] == step) return 1;
  return 0;
}

void input_route_init(so_module *mod) {
#define S(var, name) var = (void *)so_symbol(mod, name)
  S(popup_inst, "_ZN9SingletonI5PopupE11GetInstanceEb");
  S(popup_enabled, "_ZN5Popup9IsEnabledEv");
  S(ach_inst, "_ZN9SingletonI13AchivementMgrE11GetInstanceEb");
  S(ach_popup, "_ZNK13AchivementMgr14IsEnabledPopupEv");
  S(menus_inst, "_ZN9SingletonI9MGR_MenusE11GetInstanceEb");
  S(cur_menu, "_ZNK9MGR_Menus21GetCurrentPlayingMenuEv");
  S(quest_menu, "_ZN9MGR_Menus12GetQuestMenuEv");
  S(quest_shown, "_ZNK10Menu_Quest7isShownEv");
  S(tut_menu, "_ZN9MGR_Menus15GetMenuTutorialEv");
  S(tut_id, "_ZN13Menu_Tutorial11GetTutorialEv");
  S(lot_menu, "_ZN9MGR_Menus22GetLotteryTutorialMenuEv");
  S(lot_shown, "_ZNK20Menu_LotteryTutorial15IsShowInterfaceEv");
  S(chk_comic, "_ZN14ControllerUtil10checkComicEv");
  S(gp_inst, "_ZN9SingletonI11GS_GamePlayE11GetInstanceEb");
  S(gp_cinematic, "_ZN11GS_GamePlay18IsCinematicEnabledEv");
  S(chk_playing, "_ZN14ControllerUtil12checkPlayingEv");
  S(chk_actor, "_ZN14ControllerUtil16checkPlayerActorEv");
#undef S
  for (size_t i = 0; i < N_TUT_MENU; i++) {
    const int *v = (const int *)so_symbol(mod, tut_menu_step_syms[i]);
    if (v) tut_menu_steps[tut_menu_known++] = *v;
  }
  debugPrintf("[route] %d tutorial steps use the Android menu controls\n", tut_menu_known);
  int ok = popup_inst && popup_enabled && ach_inst && ach_popup && menus_inst && cur_menu && quest_menu &&
           quest_shown && tut_menu && tut_id && lot_menu && lot_shown && chk_comic && chk_playing && chk_actor;
  if (!ok) {
    debugPrintf("[route] some engine state queries are missing; using the gameplay route only when no menu is up\n");
  }
}

static int compute(void) {
  if (!menus_inst || !cur_menu) return ROUTE_GAME;
  void *mm = menus_inst(0);
  if (mm && cur_menu(mm)) return ROUTE_UI;
  if (popup_inst && popup_enabled) {
    void *p = popup_inst(0);
    if (p && popup_enabled(p)) return ROUTE_UI;
  }
  if (ach_inst && ach_popup) {
    void *a = ach_inst(0);
    if (a && ach_popup(a)) return ROUTE_UI;
  }
  if (mm && quest_menu && quest_shown) {
    void *q = quest_menu(mm);
    if (q && quest_shown(q)) return ROUTE_UI;
  }
  if (mm && lot_menu && lot_shown) {
    void *l = lot_menu(mm);
    if (l && lot_shown(l)) return ROUTE_UI;
  }
  if (chk_comic && chk_comic()) return ROUTE_UI;
  /* In-game cutscenes: the game isn't "playing", but only the controller
   * scheme can skip them (PlayerCtrl::_A_ButtonAction -> UpdateCinematic). */
  if (gp_inst && gp_cinematic) {
    void *gp = gp_inst(0);
    if (gp && gp_cinematic(gp)) return ROUTE_GAME;
  }
  if (chk_playing && !chk_playing()) return ROUTE_UI;
  if (chk_actor && !chk_actor()) return ROUTE_UI;
  tut_step = -1;
  if (mm && tut_menu && tut_id) {
    void *t = tut_menu(mm);
    if (t) tut_step = tut_id(t);
    if (tut_step != -1) return ROUTE_TUTORIAL;
  }
  return ROUTE_GAME;
}

void input_route_update(void) {
  int r = compute();
  static int last_step = -2;
  if (r == ROUTE_TUTORIAL && tut_step != last_step)
    debugPrintf("[route] tutorial step %d: A/D-pad -> %s\n", tut_step,
                tutorial_step_has_menu(tut_step) ? "Android menu controls" : "controller scheme");
  last_step = r == ROUTE_TUTORIAL ? tut_step : -2;
  if (r != route) {
    static const char *names[] = { "menus (Android scheme)", "tutorial", "gameplay (controller scheme)" };
    debugPrintf("[route] %s\n", names[r]);
  }
  route = r;
}

int input_route(void) { return route; }

/* The tutorial step whose Android menu controls A currently confirms, else -1. */
int input_route_menu_step(void) {
  return route == ROUTE_TUTORIAL && tutorial_step_has_menu(tut_step) ? tut_step : -1;
}

/* The tutorial menu while its current step is one that A confirms through the
 * Android menu controls (e.g. "Help the lady!"), else NULL. */
void *input_route_tutorial_menu(void) {
  if (route != ROUTE_TUTORIAL || !tutorial_step_has_menu(tut_step) || !menus_inst || !tut_menu) return NULL;
  void *mm = menus_inst(0);
  return mm ? tut_menu(mm) : NULL;
}

/* May the Cursor turn this A press into a screen tap? Only on tutorial
 * prompts that are advanced by it. */
int input_route_cursor_tap(void) { return route == ROUTE_TUTORIAL; }

/* Which scheme a press of `key` goes to under the current route. */
int input_route_android(u64 key) {
  switch (route) {
    case ROUTE_UI: return 1;
    case ROUTE_TUTORIAL:
      return (key & (HidNpadButton_A | HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left |
                     HidNpadButton_Right)) != 0 && tutorial_step_has_menu(tut_step);
    default: return 0;
  }
}
int input_route_hw(u64 key) {
  if (key & (HidNpadButton_L | HidNpadButton_R)) return 1; /* no Android mapping */
  return !input_route_android(key);
}

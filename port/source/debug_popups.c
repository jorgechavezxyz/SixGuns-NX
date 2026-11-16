/* Debug (emulator only): show every Popup type the engine can open, one after another, so their
 * controller support, the highlighted button and the port's touch pointer can be checked on
 * screenshots. Marker `debug_popups` (ignored without `emulator_mode`; never on a real card).
 *
 * Once gameplay runs with no menu or popup up, every popup in the table below is opened through its
 * own Popup::Show... function (texts are plain strings, the engine passes already-translated text),
 * left up for ~1.5 s, captured as screenshots/popup_NN_<name>.tga, and closed with Popup::HidePopup.
 * The log line `[popups] NN name: type T, controller menu yes/no, ...` says whether the Android
 * controller scheme has a menu for that popup type (otherwise the port's focus navigation and
 * pointer handle it, see pointer.c). Optional `debug_popups_nav`: also press D-pad Right and take a
 * second shot, to see the highlight move. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "config.h"
#include "so_util.h"
#include "util.h"

so_module *port_engine_module(void);
const char *debug_shot_request; /* main.c saves screenshots/<name>.tga on the next frame */

enum { NOARG, TEXT1, TEXT3, TEXTBOOL, INT1, ITEM, NOTMONEY, NOTSLOTS };
static const struct { const char *name, *sym; int kind; } popups[] = {
  /* ShowResurrectPopup: crashes when the player isn't dead (null access), left out */
  { "powerup_tutorial", "_ZN5Popup24ShowPowerUpTutorialPopupEv", NOARG },
  { "new_item_promotions", "_ZN5Popup21ShowNewItemPromotionsEv", NOARG },
  { "technical_issue", "_ZN5Popup18ShowTechnicalIssueEv", NOARG },
  { "weekly_no_events", "_ZN5Popup24ShowWeeklyNoEnventsPopupEv", NOARG },
  { "connect", "_ZN5Popup16ShowConnectPopupEv", NOARG },
  { "push_notification", "_ZN5Popup25ShowPushNotificationPopupEv", NOARG },
  { "rating", "_ZN5Popup15ShowRatingPopupEv", NOARG },
  { "fb_like", "_ZN5Popup15ShowFBLikePopupEv", NOARG },
  { "fb_connect_now", "_ZN5Popup21ShowFBConnectNowPopupEv", NOARG },
  { "fb_connect_no_reward", "_ZN5Popup29ShowFBConnectNowNoRewardPopupEv", NOARG },
  { "invite_friends", "_ZN5Popup22ShowInviteFriendsPopupEv", NOARG },
  { "add_friends", "_ZN5Popup19ShowAddFriendsPopupEv", NOARG },
  { "fb_invite_friends", "_ZN5Popup28ShowFBInviteYourFriendsPopupEv", NOARG },
  { "powerup_tutorial_mp", "_ZN5Popup35ShowPowerUpTutorialPopupMultiplayerEv", NOARG },
  { "info_big", "_ZN5Popup16ShowInfoBigPopupEPKc", TEXT1 },
  { "join_friends", "_ZN5Popup22ShowJoinToFriendsPopupEPKc", TEXT1 },
  { "question", "_ZN5Popup17ShowQuestionPopupEPKcS1_S1_", TEXT3 },
  { "question_big", "_ZN5Popup20ShowQuestionBigPopupEPKcS1_S1_", TEXT3 },
  { "fake_update", "_ZN5Popup23ShowInfoFakeUpdatePopupEPKcS1_S1_", TEXT3 },
  { "saddle_slots", "_ZN5Popup22ShowToSaddleSlotsPopupEPKcb", TEXTBOOL },
  { "lottery_slots", "_ZN5Popup24ShowNotSlotsLotteryPopupEi", INT1 },
  /* item popups (marker debug_popups_items starts here): the item is the dynamite ammo ItemDef */
  { "buy_congratulations", "_ZN5Popup22ShowBuyCongratulationsEP7ItemDef", ITEM },
  { "not_armed_enough_promo", "_ZN5Popup23ShowNotArmedEnoughPromoEP7ItemDef", ITEM },
  { "not_money_coins", "_ZN5Popup12ShowNotMoneyEiiPK7ItemDef", NOTMONEY },
  { "not_slots_item", "_ZN5Popup17ShowNotSlotsPopupEP7ItemDefiii", NOTSLOTS },
};
#define N_POPUPS (sizeof(popups) / sizeof(*popups))

static int enabled = -1, nav;
static void *(*popup_inst)(int);
static int (*popup_enabled)(void *), (*popup_type)(void *);
static void (*popup_hide)(void *);
static int idx, phase, wait;
static void *item_def;

static int has_marker(const char *name) {
  char p[128];
  struct stat st;
  snprintf(p, sizeof(p), PORT_ROOT "/%s", name);
  return stat(p, &st) == 0;
}

/* Emulator markers `debug_quest_finish` / `debug_quest_fail`: opens the mission-complete screen of a story quest
 * the way the Lua FinishQuest does for a quest with a linker (GS_GamePlay::StartFinishQuestMenu).
 * Confirming it really finishes the quest: emulator save only. */
static void quest_finish_step(void) {
  static int state = -1, wait_q,fail_result;
  if (state < 0) {
    fail_result=has_marker("debug_quest_fail");
    state=has_marker("emulator_mode") && (has_marker("debug_quest_finish") || fail_result) ? 0:99;
  }
  if (state == 99) return;
  if (wait_q > 0) { wait_q--; return; }
  so_module *m = port_engine_module();
  extern int game_menu_active(void);
  extern int input_route(void);
  if (state == 0) {
    if (game_menu_active() || input_route() != 2) { wait_q = 30; return; }
    void *(*lvl_inst)(int) = (void *)so_symbol(m, "_ZN9SingletonI9GameLevelE11GetInstanceEb");
    void *(*entities)(void *) = (void *)so_symbol(m, "_ZN5clara9ClaraFile11GetEntitiesEv");
    void *(*find)(void *, void *) = (void *)so_symbol(m, "_ZN5clara11TSearchableINS_6EntityEE10FindByNameERKN3pig6StringE");
    void (*str_ctor)(void *, const char *) = (void *)so_symbol(m, "_ZN3pig6StringC2EPKc");
    void (*str_dtor)(void *) = (void *)so_symbol(m, "_ZN3pig6StringD1Ev");
    void *(*get_linker)(void *) = (void *)so_symbol(m, "_ZN5Quest9GetLinkerEv");
    void *(*gp_inst)(int) = (void *)so_symbol(m, "_ZN9SingletonI11GS_GamePlayE11GetInstanceEb");
    void (*start_finish)(void *, void *, int) = (void *)so_symbol(m, "_ZN11GS_GamePlay20StartFinishQuestMenuEP11QuestLinkerb");
    void (*start_stop)(void *,void *,int)=(void *)so_symbol(m,"_ZN11GS_GamePlay18StartStopQuestMenuEP11QuestLinkerb");
    if (!lvl_inst || !entities || !find || !str_ctor || !str_dtor || !get_linker || !gp_inst || !start_finish || (fail_result && !start_stop)) {
      debugPrintf("[popups] quest finish: engine functions missing\n");
      state = 99;
      return;
    }
    void *lvl = lvl_inst(0), *ents = lvl ? entities(lvl) : NULL;
    uint64_t name[16] = { 0 };
    str_ctor(name, "Quest_New_Arizona_Help_the_Lady");
    void *quest = ents ? find(ents, name) : NULL;
    str_dtor(name);
    void *linker = quest ? get_linker(quest) : NULL;
    debugPrintf("[popups] quest finish: quest %p linker %p\n", quest, linker);
    if (!linker) { state = 99; return; }
    debugPrintf("[popups] quest result fixture: %s\n",fail_result ? "failure":"success");
    if(fail_result && start_stop) start_stop(gp_inst(0),linker,1);
    else start_finish(gp_inst(0),linker,1);
    state = 1;
    wait_q = 240;
    return;
  }
  if (state == 1) {
    debug_shot_request = fail_result ? "quest_fail":"quest_finish";
    extern int pointer_has_controller_menu_now(void);
    debugPrintf("[popups] quest finish screen: controller menu %s\n", pointer_has_controller_menu_now() ? "yes" : "no (port focus)");
    state = 2;
    wait_q = 300;
    return;
  }
  if (state == 2) { debug_shot_request = fail_result ? "quest_fail_later":"quest_finish_later"; state = 99; }
}

/* Marker `debug_help` (+ emulator_mode): opens the Help screen (menu 23, what Info -> HELP pushes)
 * from gameplay and screenshots it, then after D-pad Down presses, to check the reworded pages. */
static void help_step(void) {
  static int state = -1, wait_h, shots;
  if (state < 0) state = has_marker("debug_help") && has_marker("emulator_mode") ? 0 : 99;
  if (state == 99) return;
  if (wait_h > 0) { wait_h--; return; }
  so_module *m = port_engine_module();
  extern int game_menu_active(void);
  extern int input_route(void);
  extern void port_call_game_v(const char *);
  if (state == 0) {
    /* MGR_Menus only runs its stack while a menu is up: the replay opens the pause menu (+) first. */
    extern const char *current_menu_class;
    if (!strstr(current_menu_class, "IngameMenu") && !strstr(current_menu_class, "IGM")) { wait_h = 30; return; }
    void *(*mgr)(int) = (void *)so_symbol(m, "_ZN9SingletonI9MGR_MenusE11GetInstanceEb");
    void (*push)(void *, int) = (void *)so_symbol(m, "_ZN9MGR_Menus8PushMenuEi");
    if (!mgr || !push || !mgr(0)) { debugPrintf("[help] engine functions missing\n"); state = 99; return; }
    debugPrintf("[help] DEBUG: pushing the Help menu (23)\n");
    push(mgr(0), 23);
    state = 1;
    wait_h = 180;
    return;
  }
  static char name[32];
  snprintf(name, sizeof(name), "help_%d", shots);
  debug_shot_request = name;
  debugPrintf("[help] screenshot %s\n", name);
  if (++shots > 6) { state = 99; return; }
  for (int i = 0; i < 3; i++) { port_call_game_v("nativeDpadDownPressed"); port_call_game_v("nativeDpadDownReleased"); }
  wait_h = 90;
}

/* Called once per frame (main.c) after the engine frame. */
void debug_popups_frame(void) {
  help_step();
  if (enabled > 0 && idx >= (int)N_POPUPS) { quest_finish_step(); return; }
  if (enabled < 0) {
    enabled = has_marker("debug_popups") && has_marker("emulator_mode");
    nav = has_marker("debug_popups_nav");
    if (!enabled) {quest_finish_step();return;}
    so_module *m = port_engine_module();
    popup_inst = (void *)so_symbol(m, "_ZN9SingletonI5PopupE11GetInstanceEb");
    popup_enabled = (void *)so_symbol(m, "_ZN5Popup9IsEnabledEv");
    popup_type = (void *)so_symbol(m, "_ZNK5Popup7GetTypeEv");
    popup_hide = (void *)so_symbol(m, "_ZN5Popup9HidePopupEv");
    if (!popup_inst || !popup_enabled || !popup_type || !popup_hide) { debugPrintf("[popups] engine functions missing\n"); enabled = 0; return; }
    void *(*im_inst)(int) = (void *)so_symbol(m, "_ZN9SingletonI7ItemMgrE11GetInstanceEb");
    void *(*ammo_def)(void *, int) = (void *)so_symbol(m, "_ZN7ItemMgr14GetAmmoItemDefEi");
    if (im_inst && ammo_def && im_inst(0)) item_def = ammo_def(im_inst(0), 28); /* dynamite */
    if (has_marker("debug_popups_items"))
      for (idx = 0; idx < (int)N_POPUPS && popups[idx].kind < ITEM; idx++) {}
    debugPrintf("[popups] DEBUG: will open %d popup types once gameplay runs (item %p)\n", (int)N_POPUPS - idx, item_def);
  }
  if (!enabled) {quest_finish_step();return;}
  if (idx >= (int)N_POPUPS) return;
  extern int game_menu_active(void);
  void *p = popup_inst(0);
  if (!p) return;
  if (wait > 0) { wait--; return; }
  switch (phase) {
    case 0: { /* wait for plain gameplay */
      extern int input_route(void);
      if (game_menu_active() || popup_enabled(p) || input_route() != 2) { wait = 30; return; }
      uintptr_t fn = so_symbol(port_engine_module(), popups[idx].sym);
      if (!fn) { debugPrintf("[popups] %02d %s: no such function\n", idx, popups[idx].name); idx++; return; }
      debugPrintf("[popups] opening %02d %s\n", idx, popups[idx].name);
      static const char *t1 = "Debug popup: title text", *t2 = "Debug popup: body text for checking the layout",
                        *t3 = "Debug popup: third text";
      switch (popups[idx].kind) {
        case NOARG: ((void (*)(void *))fn)(p); break;
        case TEXT1: ((void (*)(void *, const char *))fn)(p, t2); break;
        case TEXT3: ((void (*)(void *, const char *, const char *, const char *))fn)(p, t1, t2, t3); break;
        case TEXTBOOL: ((void (*)(void *, const char *, int))fn)(p, t2, 0); break;
        case INT1: ((void (*)(void *, int))fn)(p, 1); break;
        case ITEM: case NOTMONEY: case NOTSLOTS:
          if (!item_def) { /* the item manager is filled when the level loads */
            void *(*im_inst)(int) = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI7ItemMgrE11GetInstanceEb");
            void *(*ammo_def)(void *, int) = (void *)so_symbol(port_engine_module(), "_ZN7ItemMgr14GetAmmoItemDefEi");
            if (im_inst && ammo_def && im_inst(0)) item_def = ammo_def(im_inst(0), 28);
          }
          if (!item_def) { debugPrintf("[popups] %02d %s: no item\n", idx, popups[idx].name); idx++; phase = 0; return; }
          if (popups[idx].kind == ITEM) ((void (*)(void *, void *))fn)(p, item_def);
          else if (popups[idx].kind == NOTMONEY) ((void (*)(void *, int, int, void *))fn)(p, 0, 100, item_def);
          else ((void (*)(void *, void *, int, int, int))fn)(p, item_def, 100, 0, 0x2dd0);
          break;
      }
      phase = 1;
      wait = 90;
      return;
    }
    case 1: {
      static char name[96];
      int up = popup_enabled(p) & 1;
      extern int pointer_has_controller_menu_now(void);
      debugPrintf("[popups] %02d %s: %s, type %d, controller menu %s\n", idx, popups[idx].name,
                  up ? "shown" : "NOT shown", up ? popup_type(p) : -1,
                  up ? (pointer_has_controller_menu_now() ? "yes" : "no (port focus/pointer)") : "-");
      snprintf(name, sizeof(name), "popup_%02d_%s", idx, popups[idx].name);
      debug_shot_request = name;
      phase = up && nav ? 2 : 4;
      wait = 10;
      return;
    }
    case 2: { /* press D-pad Right through the engine's Android path */
      extern void port_call_game_v(const char *);
      port_call_game_v("nativeDpadRightPressed");
      port_call_game_v("nativeDpadRightReleased");
      phase = 3;
      wait = 40;
      return;
    }
    case 3: {
      static char name[96];
      snprintf(name, sizeof(name), "popup_%02d_%s_right", idx, popups[idx].name);
      debug_shot_request = name;
      phase = 4;
      wait = 10;
      return;
    }
    case 4:
      if (popup_enabled(p) & 1) popup_hide(p);
      phase = 0;
      idx++;
      wait = 60;
      if (idx == (int)N_POPUPS) debugPrintf("[popups] all %d done\n", (int)N_POPUPS);
      return;
  }
}

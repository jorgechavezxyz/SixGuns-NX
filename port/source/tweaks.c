/* Gameplay-flow tweaks applied to the engine before it starts.
 *
 * First-launch prompts (decided in GS_MainMenu::UpdateState):
 *   - ui::TermsOfUsePopup (age entry) shows if age < 13, or age < 16 in an EU
 *     country, or Game::IsAlreadyShowedTermsOfUsePopup() is false.
 *   - ui::FriendCodePopup shows if !IsAlreadyShowedFriendCodePopup(), player
 *     level <= 5 and the terms popup was already shown.
 * All four getters below are one-instruction field reads on Game, so they're
 * replaced with constants: the player is always 21 and both prompts count as
 * already shown. Everything else reading the age (ads, 13+ restrictions)
 * sees the same 21.
 *
 * "Connect now" (link Facebook / YouTube for a reward) popups: shown once from
 * GS_MainMenu::UpdateState, which ignores the return value and then marks
 * them as shown, so they're made no-ops.
 *
 * "What's new in this update" notes (Game::HandleNewUpdatePopup: if
 * IsNewUpdate(), show a big info popup and save settings), called from the
 * main menu and gameplay update loops, which ignore its result.
 *
 * On-screen movement stick: Joystick::Render only draws it (MultipleJoystick::
 * Render forwards to it); movement logic lives in Update*, so the analog stick
 * and invisible touch-dragging keep working. */
#include "so_util.h"
#include "util.h"

#define PLAYER_AGE 21

static const struct { const char *name; uint16_t value; } fixed[] = {
  { "_ZNK4Game31GetPlayerAgeFromTermsOfUsePopupEv", PLAYER_AGE },
  { "_ZNK4Game30IsAlreadyShowedTermsOfUsePopupEv", 1 },
  { "_ZN4Game30IsAlreadyShowedFriendCodePopupEv", 1 },
  { "_ZNK4Game23Is13yoRestrictionActiveEv", 0 },
  { "_ZN5Popup21ShowFBConnectNowPopupEv", 0 },
  { "_ZN5Popup29ShowFBConnectNowNoRewardPopupEv", 0 },
  { "_ZN4Game20HandleNewUpdatePopupEv", 0 },
#ifndef STOCK_INPUT
  { "_ZN8Joystick6RenderEv", 0 },
#endif
  /* "Share on Facebook" prompts after achievements, level-ups and quests, and
   * the other social popups (like, rate, invite, connect, notifications). */
  { "_ZN5Popup15ShowFBLikePopupEv", 0 },
  { "_ZN5Popup15ShowRatingPopupEv", 0 },
  { "_ZN5Popup16ShowConnectPopupEv", 0 },
  { "_ZN5Popup19ShowAddFriendsPopupEv", 0 },
  { "_ZN5Popup22ShowInviteFriendsPopupEv", 0 },
  { "_ZN5Popup23ShowInviteFriendsGLliveEv", 0 },
  { "_ZN5Popup25ShowPushNotificationPopupEv", 0 },
  { "_ZN5Popup28ShowFBInviteYourFriendsPopupEv", 0 },
#ifndef STOCK_INPUT
  /* On-screen touch buttons that duplicate controller buttons (fire, aim,
   * turn, horse accelerate/gallop, movement stick). Touch still works where
   * they were; the context-sensitive interact button stays visible. */
  { "_ZN10PlayerCtrl18RenderAttackButtonEv", 0 },
  { "_ZN10PlayerCtrl18RenderRotateButtonEv", 0 },
  { "_ZN10PlayerCtrl15RenderAimButtonEv", 0 },
  { "_ZN10PlayerCtrl22RenderAccelerateButtonEv", 0 },
  { "_ZN10PlayerCtrl13RenderJoystikEv", 0 },
#endif
};

/* ---- free_shop marker: everything bought with coins or badges is free ----
 * ItemMgr::IncreaseMoney(int amount, int type) is the single place money is
 * added or spent (type 0 coins, 1 badges/"cash"). For a spend it checks the
 * balance (neg w8,w19; cmp w0,w8; b.ge <apply>) and returns false with an
 * error sound if it's short; <apply> does Set*(balance + amount) and returns
 * true. With the marker, those three instructions become
 *   tbnz w19, #31, <return true>   ; amount < 0: a purchase -> succeed, no deduction
 *   b <apply>                      ; amount >= 0: earned money, add it as usual
 *   nop
 * so every purchase succeeds and costs nothing, and earning still works.
 * Offsets are for this libAndroid.so; each instruction is checked first. */
#include <stdio.h>
#include "config.h"

static int patch_word(so_module *mod, uintptr_t off, uint32_t expect, uint32_t replace) {
  if (mod->finalized || !mod->image || off + 4 > mod->image_size) return -1;
  uint32_t *p = (uint32_t *)(mod->image + off);
  if (*p != expect) {
    debugPrintf("[tweaks] patch: unexpected %08x at +%lx (want %08x), not patching\n", *p, (unsigned long)off, expect);
    return -1;
  }
  *p = replace;
  return 0;
}

static void free_shop(so_module *mod) {
  FILE *f = fopen(PORT_ROOT "/free_shop", "r");
  if (!f) return;
  fclose(f);
  static const struct { uintptr_t off; uint32_t expect, replace; } p[] = {
    /* coins: check at +0xaf10a0, apply at +0xaf1190, success exit (stats, return 1) at +0xaf11d0 */
    { 0xaf10a0, 0x4b1303e8 /* neg w8,w19 */, 0x37f80993 /* tbnz w19,#31,af11d0 */ },
    { 0xaf10a4, 0x6b08001f /* cmp w0,w8 */,  0x1400003b /* b af1190 */ },
    { 0xaf10a8, 0x5400074a /* b.ge af1190 */, 0xd503201f /* nop */ },
    /* badges: check at +0xaf1100, apply at +0xaf1200, return 1 at +0xaf1224 */
    { 0xaf1100, 0x4b1303e8, 0x37f80933 /* tbnz w19,#31,af1224 */ },
    { 0xaf1104, 0x6b08001f, 0x1400003f /* b af1200 */ },
    { 0xaf1108, 0x540007ca /* b.ge af1200 */, 0xd503201f },
  };
  /* all-or-nothing per currency: verify the three words before writing */
  for (int c = 0; c < 2; c++) {
    int ok = 1;
    for (int i = c * 3; i < c * 3 + 3; i++)
      if (*(uint32_t *)(mod->image + p[i].off) != p[i].expect) ok = 0;
    if (!ok) { debugPrintf("[tweaks] free_shop: %s check not as expected, left alone\n", c ? "badge" : "coin"); continue; }
    for (int i = c * 3; i < c * 3 + 3; i++) patch_word(mod, p[i].off, p[i].expect, p[i].replace);
  }
  debugPrintf("[tweaks] free_shop: purchases with coins and badges cost nothing\n");
}

/* Game::UnloadGameData(bool, bool) frees everything not in use right now
 * (AnimationLoader::UnloadAll, optionally ModelLoader::UnloadAll, scratchpad,
 * TextureLoader::UnloadUnbinded, GameLevel::GarbageCollect). The game calls it
 * on every pause (Start/Select/Y, PlayerCtrl::UpdatePauseButton), shop visit
 * (PlayerCtrl::Update, CommonGameState::StartInAppStore), Menu_SixGunsAndMe and
 * quest start/stop/finish -- a low-RAM-phone measure -- and then reloads it all
 * when play resumes: leaving the shop re-read ~150 MB and stalled a frame for
 * 240 ms in Ryubing (much longer from the SD card). The Switch build has
 * ~3 GB of heap, so it is a no-op by default. Marker `unload_game_data`
 * restores the original behaviour. */
static void keep_game_data(so_module *mod) {
  FILE *f = fopen(PORT_ROOT "/unload_game_data", "r");
  if (f) { fclose(f); debugPrintf("[tweaks] unload_game_data marker: Game::UnloadGameData left as is\n"); return; }
  if (so_stub_function(mod, "_ZN4Game14UnloadGameDataEbb") == 0)
    debugPrintf("[tweaks] Game::UnloadGameData disabled (no unload/reload around pause, shop, quests)\n");
}

/* Shop: the chest tab (Menu_IGM_Inventory::UpdateUI, button text 0x261 ->
 * inventory type 6) asked IsNetworkConnectionActive() first and showed "No
 * Internet connection available" (text 0x287) without it. That one call is
 * replaced by "connected" (mov w0, #1); every other network check stays. */
static void shop_chest_tab_offline(so_module *mod) {
  if (patch_word(mod, 0xb77994, 0x97f5d4df /* bl IsNetworkConnectionActive */, 0x52800020 /* mov w0,#1 */) == 0)
    debugPrintf("[tweaks] shop chest tab opens without an internet connection\n");
}

/* JoystickHW::Dispatch copied a fresh default HWState (GetState() just runs
 * the constructor) over the stored one before every button event, so the
 * subscribers never saw a held button. PlayerCtrl's D-pad handlers read two of
 * them as modifiers: state+0xc (L, port) + D-pad Left/Up/Right = use elixir
 * (PowerUp::DoPressed on PowerUpController::Get(1/2/3)), state+0x1c (R, port)
 * + D-pad Up/Down = previous/next grenade type, Left/Right = previous/next
 * weapon. Without the copy, the state the port writes (sticks + held flags,
 * controller.c) reaches them. */
static void joystick_state_kept(so_module *mod) {
  if (patch_word(mod, 0xb31538, 0x97f73d5a /* bl memcpy */, 0xd503201f /* nop */) == 0)
    debugPrintf("[tweaks] JoystickHW keeps the port's button state (L/R + D-pad combos)\n");
}

/* Shop tabs. Menu_IGM_Inventory::MakeLogicalTabNodeTable builds the tab bar
 * from fixed nodes (type, text ID): 9 NEW (0x264), 8 (0x263), 11 (0x265,
 * optional), 10 lottery tickets (0x262), 6 chest / restore purchase (0x261),
 * 4 weapons, 3, 2, 1. Offline the NEW, second and chest tabs are empty and
 * the lottery-ticket tab keeps showing "No Internet connection" (owner's
 * request: remove them). Each node is its own block; the first instruction
 * of a removed block becomes a branch to where the normal flow continues:
 *   +0xb74bf0 tbnz (option bit 0, before NEW)  -> b +0xb74ccc (after node 8)
 *   +0xb74d58 mov w1,#10 (lottery tickets)      -> b +0xb74dcc
 *   +0xb74dcc mov w2,#0x261 (chest)             -> b +0xb74e40
 * Marker `all_shop_tabs` keeps every tab. */
static void shop_tabs_trim(so_module *mod) {
  FILE *f = fopen(PORT_ROOT "/all_shop_tabs", "r");
  if (f) { fclose(f); return; }
  int ok = patch_word(mod, 0xb74bf0, 0x370003d4, 0x14000037) == 0;
  ok &= patch_word(mod, 0xb74d58, 0x52800141, 0x1400001d) == 0;
  ok &= patch_word(mod, 0xb74dcc, 0x52804c22, 0x1400001d) == 0;
  debugPrintf("[tweaks] shop: NEW, second, lottery-ticket and chest tabs removed (%s)\n", ok ? "ok" : "partly");
}

void tweaks_apply(so_module *mod) {
  int n = 0;
  for (size_t i = 0; i < sizeof(fixed) / sizeof(*fixed); i++)
    if (so_stub_function_value(mod, fixed[i].name, fixed[i].value) == 0) n++;
  debugPrintf("[tweaks] age fixed at %d, social prompts skipped, touch buttons hidden (%d/%d patches)\n", PLAYER_AGE, n,
              (int)(sizeof(fixed) / sizeof(*fixed)));
  free_shop(mod);
  keep_game_data(mod);
  shop_chest_tab_offline(mod);
  shop_tabs_trim(mod);
  /* Quit crash (hardware log 2026-11-12, after 74 min): Game::Shutdown ->
   * Singleton<ClanWarManager>::FreeInstance -> ~ClanWarManager ->
   * ~ClanWarBattleController -> ThreadPool::OnCallerDestroy(NULL) = data abort. Clan wars
   * are online-only and never set up offline; at exit the process ends anyway, so the
   * singleton is simply not freed. */
  if (so_stub_function_value(mod, "_ZN9SingletonI14ClanWarManagerE12FreeInstanceEv", 0) == 0)
    debugPrintf("[tweaks] quit: ClanWarManager teardown skipped (crashed at exit)\n");
  /* Offline, FederationManager::Update (from CommonGameState::UpdateState) calls
   * Gaia::Initialize again every 20 s while Gaia is down; IsCanTryInitialize
   * (0x9bce90) is the 20 s gate. Returning 0 stops the retries (Gaia stays down,
   * as it does anyway without servers). docs/re/subsystems/online_services.md.
   * Marker `online_retry` keeps the retries. */
  {
    FILE *f = fopen(PORT_ROOT "/online_retry", "r");
    if (f) fclose(f);
    else if (so_stub_function_value(mod, "_ZN17FederationManager18IsCanTryInitializeEi", 0) == 0)
      debugPrintf("[tweaks] offline: Gaia re-initialisation every 20 s stopped\n");
  }
  /* Comics: the engine's own skip button (GS_Comics::RenderState: any touch
   * starts a timer at +0x60, the button is drawn and touchable while it runs)
   * is removed -- the universal "hold A 5 s" skip replaces it (controller.c).
   * The two timer writes become nops. */
  if (patch_word(mod, 0xb1b8b8, 0xf9003268, 0xd503201f) == 0 && patch_word(mod, 0xb1b8d4, 0xf9003268, 0xd503201f) == 0)
    debugPrintf("[tweaks] comic skip button removed (hold A 5 s to skip instead)\n");
  /* Cinematics (in-mission cutscenes) have the same kind of button: GS_GamePlay::UpdateCinematic
   * shows it for 5 s after any touch (+0x16c = 5000 at +0xc37990) and a touch on it skips. The
   * port's hold-A skip sets the skip flag directly (skipCinematic, +0x59), so only the touch
   * button goes away. */
  if (patch_word(mod, 0xc37990, 0xb9016e68 /* str w8, [x19, #0x16c] */, 0xd503201f) == 0)
    debugPrintf("[tweaks] cinematic skip button removed (hold A 5 s to skip instead)\n");
  /* Experiment (marker `quest_bg_nostretch`): Menu_QuestStartAnimIntro stretches the
   * mission-complete background (page obj 1, with the white gun cutout) to the
   * full screen (LayerGraph::SetSize at +0xbbb6a4) while the guns sit at fixed
   * layout coordinates, so on 16:9 the cutout drifts from the gun. With the
   * marker the call is skipped (background keeps its own size). */
  {
    FILE *f = fopen(PORT_ROOT "/quest_bg_nostretch", "r");
    if (f) {
      fclose(f);
      if (patch_word(mod, 0xbbb6a4, 0x97f34c23, 0xd503201f) == 0)
        debugPrintf("[tweaks] quest_bg_nostretch: mission-complete background not stretched\n");
    }
  }
  /* LAN multiplayer (marker `lan`, also turns on real sockets in net.c): the
   * Multiplayer button enters local mode instead of online. MultiplayerContext
   * network type 1 = local (no sign-in, CMatchingLocal: UDP broadcast on port
   * 7513), 2 = online (Gameloft login + Anubis lobby, dead).
   * GS_InitialMultiplayerBase's constructor and ReLoginWithSNS pass a constant
   * 2 to EnterMultiplayer: `orr w1, wzr, #2` -> `orr w1, wzr, #1`.
   * See tools/decomp_mp/README.md. */
  {
    extern int net_on;
    if (net_on) {
      int a = patch_word(mod, 0xb1c130, 0x321f03e1, 0x320003e1);
      int b = patch_word(mod, 0xb1e17c, 0x321f03e1, 0x320003e1);
      debugPrintf("[tweaks] lan: Multiplayer enters local (LAN) mode%s\n", a == 0 && b == 0 ? "" : " (PATCH FAILED)");
    }
  }
  /* A no longer rolls (B does, controller.c player_roll). In
   * PlayerCtrl::_A_ButtonAction the roll branch starts with `cbz x0, +0xa3b680`
   * at +0xa3b714 (player null check before AbortGrenade + event(2,t,4)); it
   * becomes `b +0xa3b680`, the "A did nothing" exit. Interaction, bash and
   * horse spur are handled before that branch. Marker `a_rolls` keeps it. */
  {
    FILE *f = fopen(PORT_ROOT "/a_rolls", "r");
    if (f) fclose(f);
    else if (patch_word(mod, 0xa3b714, 0xb4fffb60, 0x17ffffdb) == 0)
      debugPrintf("[tweaks] A no longer rolls (B does)\n");
  }
  {
    extern int tex_get_finished_job(void *, void *);
    extern int tex_budget_enabled(void);
    if (tex_budget_enabled() &&
        so_hook_function(mod, "_ZN3pig6thread12WorkerThread14GetFinishedJobEPNS0_9WorkerJobE", (void *)tex_get_finished_job) == 0)
      debugPrintf("[tweaks] texture streaming: at most ~4 ms of finalizing per frame (marker no_tex_budget to disable)\n");
  }
  joystick_state_kept(mod);
  {
    extern void ui_scroll_panel_render(void *, int, int);
    extern int ui_cull_enabled(void);
    if (ui_cull_enabled() &&
        so_hook_function(mod, "_ZN16LayerScrollPanel6RenderEii", (void *)ui_scroll_panel_render) == 0)
      debugPrintf("[tweaks] scroll panels draw only on-screen pages (marker no_ui_cull to disable)\n");
  }
  {
    extern int sprite_cache_install(so_module *);
    if (sprite_cache_install(mod) == 0)
      debugPrintf("[tweaks] sprite scale/offset lookups cached (marker no_sprite_cache to disable)\n");
  }
  /* VIP for everyone, without the VIP screen (marker `keep_vip` restores the game's VIP).
   * VIP in this build = shop items locked behind VIP tiers (VipManager::IsItemLockedByVip; locked
   * items are also hidden by ItemDef::IsHiddenForSale) plus a daily-reward screen
   * (Menu_VipDailyBonusMenu, id 0x3a) that the game pushes by itself after loading
   * (VipDailyBonusController::Update, every frame) and from the HUD VIP icon
   * (PlayerCtrl::IsVipButtonActive) and the promotion popup. Unlocking the items directly keeps
   * IsVipActive (server-time based, offline-only false) untouched, so no timer code runs. */
  {
    FILE *f = fopen(PORT_ROOT "/keep_vip", "r");
    if (f) fclose(f);
    else {
      int n = 0;
      n += so_stub_function_value(mod, "_ZNK10VipManager17IsItemLockedByVipEPK7ItemDef", 0) == 0;
      n += so_stub_function(mod, "_ZN23VipDailyBonusController6UpdateEv") == 0;
      n += so_stub_function_value(mod, "_ZNK10PlayerCtrl17IsVipButtonActiveEv", 0) == 0;
      n += so_stub_function(mod, "_ZNK10VipManager21ShowVipPromotionPopupEv") == 0;
      debugPrintf("[tweaks] VIP items unlocked, VIP screen/icon/popup off (%d/4; marker keep_vip to restore)\n", n);
    }
  }
  {
    extern int shop_tabs_install(so_module *);
    if (shop_tabs_install(mod) == 0)
      debugPrintf("[tweaks] shop tab bar sized to its tabs (no empty ring slots; marker no_shop_tab_fix to disable)\n");
  }
  { extern int shop_reset_timing_install(so_module *); shop_reset_timing_install(mod); }
  { extern int shop_grid_nav_install(so_module *); shop_grid_nav_install(mod); }
  { extern int shop_ui_install(so_module *); shop_ui_install(mod); }
  { extern int menus_ui_install(so_module *); menus_ui_install(mod); }
  { extern int map_ui_install(so_module *); map_ui_install(mod); }
  {
    extern int grid_cull_install(so_module *);
    if (grid_cull_install(mod) == 0)
      debugPrintf("[tweaks] grids (shop item list) draw only visible cells (marker no_grid_cull to disable)\n");
  }
  /* Elixirs: free and always under the port's 5-minute cooldown, touch included
   * (controller.c elixir_do_pressed; marker `paid_elixirs` keeps the game's own). */
  {
    extern void elixir_do_pressed(void *);
    extern int elixir_hooked;
    FILE *f = fopen(PORT_ROOT "/paid_elixirs", "r");
    if (f) fclose(f);
    else if (so_hook_function(mod, "_ZN7PowerUp9DoPressedEv", (void *)elixir_do_pressed) == 0) {
      elixir_hooked = 1;
      debugPrintf("[tweaks] elixirs free, 5 min cooldown on every use incl. touch (marker paid_elixirs to restore)\n");
    }
  }
  {
    extern int text_cache_install(so_module *);
    if (text_cache_install(mod) == 0)
      debugPrintf("[tweaks] glyph and bidi lookups memoised (marker no_text_cache to disable)\n");
  }
  /* Lua Print() -> log (marker `lua_print`). The engine's Print(lua_State*)
   * (0xc40a64) is compiled out in this release: it reads its argument and
   * returns. Mission scripts call it ~1,100 times and mods need a way to
   * trace their scripts, so with the marker each call logs `[lua] <text>`.
   * See docs/re/LUA_API.md and tools/modding/README.md (levels/scripts). */
  {
    FILE *f = fopen(PORT_ROOT "/lua_print", "r");
    if (f) {
      fclose(f);
      extern int lua_print_hook(void *L);
      extern void *lua_tolstring_fn;
      lua_tolstring_fn = (void *)so_symbol(mod, "lua_tolstring");
      if (lua_tolstring_fn && so_hook_function(mod, "_Z5PrintP9lua_State", (void *)lua_print_hook) == 0)
        debugPrintf("[tweaks] lua_print marker: Lua Print() writes [lua] lines to the log\n");
    }
  }
  /* Share buttons -> screenshot (screenshot.c). */
  {
    extern int screenshot_share_hook(void);
    if (so_hook_function(mod, "_ZN5Popup14ShowSharePopupERNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEES7_S7_S7_S7_S7_S7_i",
                         (void *)screenshot_share_hook) == 0)
      debugPrintf("[tweaks] share buttons take a screenshot instead\n");
  }
  /* PLT bypass last: patch_word() checks the original instruction, and the bypass rewrites every
   * `bl <plt stub>` into a direct `bl`, so patches of such calls (chest tab +0xb77994,
   * quest_bg_nostretch +0xbbb6a4) failed with "unexpected" while it ran first (2026-11-12 logs). */
  {
    FILE *f = fopen(PORT_ROOT "/no_plt_bypass", "r");
    if (f) fclose(f);
    else so_bypass_plt(mod);
  }
}

/* Replacement for the engine's empty Lua Print (see the lua_print marker above). */
void *lua_tolstring_fn;
int lua_print_hook(void *L) {
  const char *(*tolstring)(void *, int, size_t *) = lua_tolstring_fn;
  const char *s = tolstring ? tolstring(L, 1, NULL) : NULL;
  if (s) debugPrintf("[lua] %s\n", s);
  return 0;
}

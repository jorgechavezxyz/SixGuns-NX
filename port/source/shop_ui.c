/* Shop list: the port's own controller UI for the shop (Menu_IGM_Inventory), replacing the
 * engine's paged item grid.
 *
 * Why: the grid (a horizontally paged LayerGrid, two rows of cards) was built for swipes. Under the
 * Android controller scheme it moved by faking swipes, skipped items on page flips, lost the
 * selection after popups and rebuilt slowly; patching it (shop_grid_nav.c, main.c's A tap, the
 * shoulder-button routing) never made it pleasant (owner, hardware 2026-11-13: "navigation is
 * absolutely awful ... completely redo the shop").
 *
 * What stays the engine's: the whole shop menu (top bar with money, tab icons, the left panel with
 * name, description, 3D model and stats), every purchase/equip/unlock rule, its popups and sounds.
 * What is new: the grid area shows a vertical list drawn by the port, driven only by the port:
 *   D-pad / left stick up-down   move through the items (hold to repeat)
 *   D-pad left-right, L/R, ZL/ZR  previous / next category (the engine's tab buttons)
 *   A                             the item's main action (buy / equip / use / unlock ...)
 *   Y                             the second action when the card has two (buy with badges)
 *   B / +                         back (unchanged)
 *   touch                         tap a row to select it, tap it again for A, drag to scroll,
 *                                 tap the header arrows to change category
 *
 * How it talks to the engine (nothing re-implemented):
 *  - the rows are the grid's own cards (Page_InventoryButton, LayerGrid object 0x86 of the menu
 *    page): card +0x2c = inventory index -> ItemMgr::GetInventoryElement -> ItemDef;
 *  - selecting a row does what Menu_IGM_Inventory::SelectItem/PreselectItem do (CenterScroll,
 *    SetReleasedObject, the card's vtable +0xd8), and snaps the hidden grid's scroll so the card is
 *    inside the grid's visible area: LayerGrid::Update only updates visible cells;
 *  - an action "releases" the card's real button: ButtonActionMgr::AddReleasedButton right before
 *    that card's own Page_InventoryButton::Update (vtable slot wrapped), exactly what a touch on the
 *    button does, so prices, popups, level locks, saving all stay the engine's;
 *  - a category change is Menu_IGM_Inventory::TouchOnTableTab (the joystick path of the tab bar);
 *  - the list is drawn inside the engine's frame where the grid would have been drawn
 *    (grid_cull.c's LayerGrid::Render hook), so popups the engine draws later stay on top.
 *
 * Not taken over: the tutorial's shop steps (until Game::IsTutorialCompleted), the upgrades view
 * (inventory type 5) and any inventory use that is not one of the shop's tabs; there the engine's
 * own grid and controls run as before. Marker `old_shop` turns the whole thing off. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "config.h"
#include "input.h"
#include "ui_text.h"
#include "so_util.h"
#include "util.h"

#define ENGINE_SCALE (SCREEN_H / 320.0f) /* engine units -> pixels (pointer.c) */
so_module *port_engine_module(void);

/* ------------------------------------------------------------------ engine */

static void *(*menus_inst)(int);
static void *(*playing_menu)(void *);
static void *(*get_iobj)(void *page, int id);
static int (*grid_count)(void *grid);
static void *(*grid_obj)(void *grid, int idx);
static void (*grid_center)(void *grid, void *obj);
static void (*grid_set_released)(void *grid, void *obj);
static void *(*item_inst)(int);
static uint8_t *(*inv_elem)(void *mgr, int idx);
static int (*item_count)(void *mgr, void *def);
static void *(*str_inst)(int);
static const char *(*get_string)(void *mgr, int id);
static int (*is_equipped)(void *def);
static int (*pint_get)(const void *pi);
static void (*add_released)(void *btn);
static void (*touch_tab)(void *menu, int tab);
static void *(*game_inst)(int);
static int (*tut_done)(void *game);
static int (*btn_text_id)(void *btn);
static int (*btn_marked)(void *btn);
static void *(*popup_inst)(int);
static int (*popup_enabled)(void *);
static void *(*level_inst)(int);
static void *(*level_player)(void *);
static int (*player_level)(void *);
static void (*orig_card_update)(void *card, int a1, int a2, int a3);
static void *(*snd_inst)(int);
static void (*snd_play)(void *mgr, const void *label, const void *pos, unsigned flags);
static void (*pstr_ctor)(void *s, const char *c);
static void (*pstr_dtor)(void *s);

static int enabled; /* installed and not turned off */

static int vis(void *o) { return o && ((*(int (***)(void *))o)[0xb0 / 8](o) & 1); }
/* InterfaceObject virtuals (offsets from the vtable address point): GetPosX +0x60, GetPosY +0x68,
 * GetWidth +0xc0, GetHeight +0xc8, all float. (+0x40..+0x58 are ClearLayer, Deactivate, SetPos,
 * SetPosAndSize: never call those to read a position.) */
static float vflt(void *o, int off) { return (*(float (**)(void *))(*(uint8_t **)o + off))(o); }

static uint8_t *shop_menu(void);
/* The weapon upgrade view (inventory type 5: the tier grid + Full Upgrade). Its controller chain moves
 * nothing visible, so pointer.c gives it the port's focus frame (owner, hardware 2026-11-15). */
int shop_ui_upgrade_view(void) {
  uint8_t *m = shop_menu();
  return m && *(int32_t *)(m + 0x44) == 5;
}
static uint8_t *shop_menu(void) {
  uint8_t *mgr = menus_inst ? menus_inst(0) : NULL;
  return mgr ? *(uint8_t **)(mgr + 0x68) : NULL; /* MGR_Menus::GetInventoryMenu */
}
static uint8_t *shop_page(uint8_t *menu) { return menu ? *(uint8_t **)(menu + 0x8) : NULL; }
static uint8_t *shop_grid(uint8_t *menu) {
  uint8_t *page = shop_page(menu);
  return page ? get_iobj(page, 0x86) : NULL;
}

static __attribute__((unused)) void play(const char *label) {
  if (!snd_inst || !snd_play || !pstr_ctor || !pstr_dtor) return;
  void *m = snd_inst(0);
  if (!m) return;
  uint64_t s[16] = {0};
  float pos[3] = {0, 0, 0};
  pstr_ctor(s, label);
  snd_play(m, s, pos, 0);
  pstr_dtor(s);
}

/* ------------------------------------------------------------------ tabs */

enum { TAB_SIZE = 0x1c };
static int tab_count(uint8_t *menu) {
  uint8_t *b = *(uint8_t **)(menu + 0x748), *e = *(uint8_t **)(menu + 0x750);
  if (!b || e < b || (e - b) % TAB_SIZE || (e - b) / TAB_SIZE > 16) return 0;
  return (int)((e - b) / TAB_SIZE);
}
static int32_t *tab_node(uint8_t *menu, int i) { return (int32_t *)(*(uint8_t **)(menu + 0x748) + i * TAB_SIZE); }
/* The tab being shown: the one whose inventory type is the menu's current type (+0x44). The
 * menu's own index (+0x2c) is not reliable: closing a purchase popup rebuilds the first tab
 * without changing it. -1: the menu shows something that is not a tab (upgrades view ...). */
static int cur_tab(uint8_t *menu) {
  int nt = tab_count(menu), type = *(int32_t *)(menu + 0x44), idx = *(int32_t *)(menu + 0x2c);
  if (idx >= 0 && idx < nt && tab_node(menu, idx)[0] == type) return idx;
  for (int i = 0; i < nt; i++) if (tab_node(menu, i)[0] == type) return i;
  return -1;
}

/* ------------------------------------------------------------------ rows */

typedef struct {
  uint8_t *card;
  uint8_t *card2; /* the item's second card (Supplies: one to use what you own, one to buy more) */
  void *def;
  int inv;
} row_t;
enum { MAX_ROWS = 160 };
static row_t rows[MAX_ROWS];
static int nrows;

static const char *cls(void *o) {
  void *ti = (*(void ***)o)[-1];
  const char *n = ti ? *(const char **)((char *)ti + 8) : NULL;
  return n ? n : "?";
}

/* The grid's cells (LayerGrid +0xb8 begin / +0xc0 end, what GetInterfaceObjectByIdx reads). Cards
 * of the same item (Supplies shows an owned item twice) become one row. */
static int rows_merge_all; /* the saddle bag (menu type 0) */
static int read_rows(uint8_t *grid, row_t *out) {
  void **b = grid ? *(void ***)(grid + 0xb8) : NULL, **e = grid ? *(void ***)(grid + 0xc0) : NULL;
  int n = b && e > b ? (int)(e - b) : 0, k = 0;
  if (n > MAX_ROWS) n = MAX_ROWS;
  void *im = item_inst(0);
  for (int i = 0; i < n; i++) {
    uint8_t *card = b[i];
    if (!card || !strstr(cls(card), "Page_InventoryButton")) continue;
    int inv = *(int32_t *)(card + 0x2c);
    uint8_t *el = *(uint8_t **)(card + 0x110); /* the card's ItemInventory (shop row or saddle slot) */
    if (!el) el = inv >= 0 && im ? inv_elem(im, inv) : NULL;
    void *def = el ? *(void **)el : NULL;
    if (rows_merge_all && (!def || !im || item_count(im, def) <= 0)) continue; /* saddle bag: an empty slot */
    int merged = 0;
    for (int j = 0; j < k && def && !merged; j++) {
      /* saddle bag: one slot per pack (a dozen "Gun Ammo" rows with the same total): one row per
       * item, its actions on the first slot */
      if (out[j].def == def && rows_merge_all) merged = 1;
      else if (out[j].def == def && !out[j].card2) { out[j].card2 = card; merged = 1; }
    }
    if (merged) continue;
    out[k].card = card;
    out[k].card2 = NULL;
    out[k].inv = inv;
    out[k].def = def;
    k++;
  }
  return k;
}

/* The card's action buttons, in the order the main action is picked. Each entry is either a
 * pointer field of the card or an object id of its page (see Page_InventoryButton::Update). */
typedef struct { int field, id; const char *what; } act_src;
static const act_src act_srcs[] = {
  { 0xd8, 0, "buy" },       /* coins price button  -> Menu_IGM_Inventory::Buy(idx, 0) */
  { 0xf0, 0, "buy" },       /* coins (other layout) */
  { 0xe0, 0, "buy" },       /* badges price button -> Buy(idx, 1) */
  { 0, 0x3b, "equip" },     /* OnEquipButton */
  { 0, 0x41, "use" },       /* consumables: Use */
  { 0, 0x44, "remove" },    /* saddle bag: throw away (Menu_IGM_Inventory::Remove) */
  { 0, 0x71, "unlock" },    /* level lock: unlock for badges popup */
  { 0, 0x6e, "locked" },    /* achievement lock: "finish ... to unlock" popup */
  { 0, 0x3e, "upgrade" },   /* weapon upgrades view */
  { 0, 0x1a, "free" },      /* get it free (timer) */
  { 0, 0x20, "lottery" },   /* win it in the lottery */
};
typedef struct { void *btn; int src; uint8_t *card; } act_t;

static int card_actions(uint8_t *card, act_t *out, int max) {
  int n = 0;
  for (size_t i = 0; i < sizeof(act_srcs) / sizeof(*act_srcs) && n < max; i++) {
    void *b = act_srcs[i].field ? *(void **)(card + act_srcs[i].field) : get_iobj(card, act_srcs[i].id);
    if (!b || !vis(b)) continue;
    int dup = 0;
    for (int k = 0; k < n; k++) dup |= out[k].btn == b;
    if (dup) continue;
    out[n].btn = b;
    out[n].src = (int)i;
    out[n].card = card;
    n++;
  }
  return n;
}

/* The card's actions minus "win it in the lottery" for items already owned (the card shows that
 * button on owned items too; as A it opened the lottery instead of doing anything useful). */
/* Level lock, as Page_InventoryButton::Init decides it: required level (ItemDef +0x1d0) above the
 * player's (Player::GetCurrentLevel) and not unlocked for badges (Player::IsItemUnlocked). The card
 * can keep its buy button "visible" on a locked item, and releasing that button bought the item past
 * its level (owner, hardware 2026-11-13). */
static int level_locked(row_t *r) {
  static void *(*li)(int), *(*lp)(void *);
  static int (*plev)(void *), (*unl)(void *, void *);
  if (!li) {
    so_module *m = port_engine_module();
    li = (void *)so_symbol(m, "_ZN9SingletonI9GameLevelE11GetInstanceEb");
    lp = (void *)so_symbol(m, "_ZN9GameLevel9GetPlayerEv");
    plev = (void *)so_symbol(m, "_ZN6Player15GetCurrentLevelEv");
    unl = (void *)so_symbol(m, "_ZNK6Player14IsItemUnlockedEP7ItemDef");
  }
  if (!r->def || !li || !lp || !plev || !unl) return 0;
  int need = *(int32_t *)((uint8_t *)r->def + 0x1d0);
  if (need <= 0) return 0;
  void *lvl = li(0), *pl = lvl ? lp(lvl) : NULL;
  if (!pl) return 0;
  if (plev(pl) >= need) return 0;
  if (unl(pl, r->def) & 1) return 0;
  if (item_count(item_inst(0), r->def) > 0) return 0;
  return 1;
}

static int row_actions(row_t *r, act_t *out, int max) {
  int n = card_actions(r->card, out, max);
  if (r->card2 && n < max) {
    /* merged row: the other card's actions too (one of each kind), buying first, then using */
    act_t more[8];
    int m = card_actions(r->card2, more, 8);
    for (int i = 0; i < m && n < max; i++) {
      int have = 0;
      for (int j = 0; j < n; j++) have |= !strcmp(act_srcs[out[j].src].what, act_srcs[more[i].src].what);
      if (!have) out[n++] = more[i];
    }
    for (int i = 1; i < n; i++) /* stable: "buy" entries to the front */
      if (!strcmp(act_srcs[out[i].src].what, "buy"))
        for (int j = i; j > 0 && strcmp(act_srcs[out[j - 1].src].what, "buy"); j--) { act_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
  }
  int owned = r->def && ((is_equipped(r->def) & 1) || item_count(item_inst(0), r->def) > 0);
  int locked = level_locked(r);
  for (int i = 0; i < n && !locked; i++) {
    const char *w = act_srcs[out[i].src].what;
    locked = !strcmp(w, "unlock") || !strcmp(w, "locked");
  }
  int k = 0;
  for (int i = 0; i < n; i++) {
    const char *w = act_srcs[out[i].src].what;
    if (owned && !strcmp(w, "lottery")) continue;
    if (locked && !strcmp(w, "buy")) continue; /* locked: unlock (or lottery) only, never buy */
    out[k++] = out[i];
  }
  return k;
}

/* The A and Y actions of a row. By position (A = first, Y = second), an equipped weapon (no Equip
 * button) put Upgrade on A while other weapons had it on Y (owner, hardware 2026-11-15). Upgrade is
 * always Y and never A; otherwise A is the first action and Y the second. slot[k].src < 0: none. */
static void ay_slots(row_t *r, act_t slot[2]) {
  act_t a[8];
  int n = row_actions(r, a, 8), up = -1;
  slot[0].src = slot[1].src = -1;
  for (int i = 0; i < n; i++) if (!strcmp(act_srcs[a[i].src].what, "upgrade")) { up = i; break; }
  int k = 0;
  for (int i = 0; i < n && k < (up >= 0 ? 1 : 2); i++) if (i != up) slot[k++] = a[i];
  if (up >= 0) slot[1] = a[up];
}

/* ------------------------------------------------------------------ state */

static int owning;          /* this frame: the list runs the shop */
static int sel, sel_valid;  /* selected row */
static void *sel_def;       /* to find the same item again after a rebuild */
static float scroll, scroll_target; /* first visible row (fractional while animating) */
static int last_tab = -1, last_n = -1;
static uint8_t *last_first_card;
static uint8_t *pending_card;
static void *pending_btn;
static int pending_frames;
static int rep_dir, rep_next; /* hold-to-repeat */
static unsigned long frame_no;
static int vis_rows = 8;    /* rows that fit (set by the layout) */

void shop_ui_reset(void) {
  pending_card = NULL;
  pending_btn = NULL;
}

static int tutorial_done(void) {
  void *g = game_inst ? game_inst(0) : NULL;
  return g && tut_done && (tut_done(g) & 1);
}

static int popup_up(void) {
  void *p = popup_inst ? popup_inst(0) : NULL;
  if (p && popup_enabled && (popup_enabled(p) & 1)) return 1;
  /* The achievement popup (AchivementMgr flag +8) is drawn over the shop only in gameplay (shop
   * from the pause menu); from the title menu's shop the flag is set but nothing is shown, so
   * there it must not stop the list. */
  {
    static void *(*ach)(int), *(*state)(void), *(*gp)(int);
    if (!ach) {
      ach = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI13AchivementMgrE11GetInstanceEb");
      state = (void *)so_symbol(port_engine_module(), "_ZN9GameState15GetCurrentStateEv");
      gp = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI11GS_GamePlayE11GetInstanceEb");
    }
    uint8_t *am = ach ? ach(0) : NULL;
    if (am && am[8] && state && gp) {
      if (state() == gp(0)) return 1;
      /* Title-menu shop: the achievement (already granted by the purchase) has a popup that is never
       * drawn here but still takes input (A on it took an album screenshot). Closed like the
       * screenshot code closes it, once the purchase popup is gone. */
      static uint8_t *closed_flag;
      if (!closed_flag) closed_flag = (uint8_t *)so_symbol(port_engine_module(), "_ZN13AchivementMgr22achievementPopupClosedE");
      am[8] = 0;
      if (closed_flag) *closed_flag = 1;
      debugPrintf("[shopui] achievement popup pending outside gameplay (not drawn): closed\n");
    }
  }
  extern int input_route(void);
  extern const char *current_menu_class;
  return !strstr(current_menu_class, "Menu_IGM_Inventory");
}

/* Is the shop up in a state the list handles? */
static int should_own(uint8_t **menu_out) {
  if (!enabled) return 0;
  extern int shop_menu_active(void);
  if (!shop_menu_active()) return 0;
  uint8_t *menu = shop_menu();
  void *mgr = menus_inst ? menus_inst(0) : NULL;
  if (!mgr || !playing_menu || playing_menu(mgr) != menu) return 0;
  if (!menu || !shop_page(menu)) return 0;
  int nt = tab_count(menu), t = cur_tab(menu);
  int saddle = *(int32_t *)(menu + 0x44) == 0; /* the saddle bag (pause menu SADDLE BAG): no tabs */
  if (!saddle && (nt < 2 || t < 0 || t >= nt)) return 0; /* not showing a shop tab (upgrades view, ...) */
  if (!tutorial_done()) return 0;
  if (!shop_grid(menu)) return 0;
  *menu_out = menu;
  return 1;
}

int shop_ui_owning(void) { return owning; }
static int grid_blank;
int shop_ui_transitional_now(void) { return grid_blank; }
static int popup_now;
/* The list has the controls right now (no popup over it): pointer.c keeps its pointer hidden. */
int shop_ui_list_active(void) { return owning && !popup_now; }

/* The page's round "next page" arrows (LayerButtons 187 and 190, engine 492,58) sit on the
 * list's top-right corner and are drawn after it: hidden while the list is up, shown again after. */
static const int arrow_ids[] = { 187, 190 };
static int arrows_hidden;
static unsigned arrows_hidden_mask; /* which arrow_ids[k] were hidden by hide_arrows */
static void hide_arrows(uint8_t *menu, int hide) {
  uint8_t *page = shop_page(menu);
  if (!page) return;
  void **pb = *(void ***)(page + 0x68), **pe = *(void ***)(page + 0x70);
  for (size_t k = 0; k < sizeof(arrow_ids) / sizeof(*arrow_ids); k++) {
    if (!pb || pe - pb <= arrow_ids[k]) continue;
    uint8_t *o = pb[arrow_ids[k]];
    if (!o) continue;
    void *ti = (*(void ***)o)[-1];
    const char *n = ti ? *(const char **)((char *)ti + 8) : "";
    if (!strstr(n, "LayerButton")) continue;
    if (hide && !vis(o)) continue;
    /* restore only the arrows this code hid: forcing all of them visible showed arrows the
     * engine had hidden (e.g. entering the upgrades view), a stray round button at the
     * top right of the grid */
    if (!hide && !(arrows_hidden_mask & (1u << k))) continue;
    (*(void (**)(void *, int))(*(uint8_t **)o + 0xa8))(o, !hide); /* SetVisible */
    if (hide) { arrows_hidden = 1; arrows_hidden_mask |= 1u << k; }
  }
  if (!hide) { arrows_hidden = 0; arrows_hidden_mask = 0; }
}

static void select_row(uint8_t *grid, int i, int sound) {
  if (i < 0 || i >= nrows) return;
  uint8_t *card = rows[i].card;
  grid_center(grid, card);
  /* snap the (hidden) grid to its scroll target so the card is inside the visible area at once */
  if (*(int32_t *)(grid + 0x108) == 0) *(float *)(grid + 0x110) = *(float *)(grid + 0x10c);
  else *(float *)(grid + 0x118) = *(float *)(grid + 0x114);
  grid_set_released(grid, card);
  (*(void (**)(void *))(*(uint8_t **)card + 0xd8))(card);
  sel = i;
  sel_valid = 1;
  sel_def = rows[i].def;
  (void)sound; /* no sound here: the shop plays sfx_ui_select itself when the marked card changes */
}

static void keep_in_view(int jump) {
  float top = scroll_target;
  if (sel < top + 1) top = sel - 1;
  if (sel > top + vis_rows - 2) top = sel - vis_rows + 2;
  float maxtop = nrows - vis_rows;
  if (maxtop < 0) maxtop = 0;
  if (top > maxtop) top = maxtop;
  if (top < 0) top = 0;
  scroll_target = top;
  if (jump) scroll = top;
}

static int dbg_ach_armed;
static void do_action(int which) {
  if (!sel_valid || sel >= nrows) return;
  act_t a[2];
  ay_slots(&rows[sel], a);
  if (which > 1 || a[which].src < 0) {
    if (which == 0) debugPrintf("[shopui] A on %s: no action on this item\n", rows[sel].def ? "an item" : "a card");
    return;
  }
  pending_card = a[which].card;
  pending_btn = a[which].btn;
  if (pending_card != rows[sel].card) {
    /* the button is on the item's other card: bring that one into the hidden grid's visible area,
     * or its Update (which delivers the press) does not run */
    uint8_t *grid = shop_grid(shop_menu());
    if (grid) {
      grid_center(grid, pending_card);
      if (*(int32_t *)(grid + 0x108) == 0) *(float *)(grid + 0x110) = *(float *)(grid + 0x10c);
      else *(float *)(grid + 0x118) = *(float *)(grid + 0x114);
    }
  }
  pending_frames = 0;
  debugPrintf("[shopui] %s: %s (row %d, inventory %d)\n", which ? "Y" : "A", act_srcs[a[which].src].what, sel,
              rows[sel].inv);
  /* Test (emulator): marker debug_ach_pending raises the achievement manager's "popup pending" flag
   * with a purchase, like a purchase that unlocks an achievement. */
  {
    static int dbg = -1;
    if (dbg < 0) { FILE *m = fopen(PORT_ROOT "/debug_ach_pending", "r"); dbg = m != NULL; if (m) fclose(m); }
    if (dbg) dbg_ach_armed = 1; /* set when the purchase popup is up (shop_ui_input) */
  }
}

static void change_tab(uint8_t *menu, int dir) {
  if (*(int32_t *)(menu + 0x44) == 0) return; /* the saddle bag has no categories */
  int nt = tab_count(menu), t = cur_tab(menu) + dir;
  if (nt <= 0) return;
  if (t < 0) t = nt - 1;
  if (t >= nt) t = 0;
  touch_tab(menu, t);
  /* (the shop plays sfx_ui_tab_change itself for the tab button) */
  sel_valid = 0;
  sel_def = NULL;
  pending_card = NULL;
  debugPrintf("[shopui] category %d -> %d\n", t - dir, t);
}

/* ------------------------------------------------------------------ layout (screen px) */

static float px0, py0, px1, py1; /* panel */
enum { HEAD_H = 46, FOOT_H = 40, ROW_H = 52 };
static void layout(uint8_t *grid) {
  /* The grid's dark frame (a separate page graphic) is wider than the grid object itself; the
   * panel covers the frame (measured at 1280x720: 455,127-1163,605) and the grid, whichever is larger. */
  float gx = vflt(grid, 0x60), gy = vflt(grid, 0x68), gw = vflt(grid, 0xc0), gh = vflt(grid, 0xc8);
  px0 = 452; py0 = 124; px1 = 1166; py1 = 608;
  float x0 = gx * ENGINE_SCALE - 6, y0 = gy * ENGINE_SCALE - 6, x1 = (gx + gw) * ENGINE_SCALE + 6, y1 = (gy + gh) * ENGINE_SCALE + 6;
  if (x1 - x0 > 300 && y1 - y0 > 200 && x0 >= 0 && y0 >= 0 && x1 <= SCREEN_W && y1 <= SCREEN_H) {
    if (x0 < px0) px0 = x0;
    if (y0 < py0) py0 = y0;
    if (x1 > px1) px1 = x1;
    if (y1 > py1) py1 = y1;
  }
  vis_rows = (int)((py1 - py0 - HEAD_H - FOOT_H) / ROW_H);
  if (vis_rows < 3) vis_rows = 3;
}

/* ------------------------------------------------------------------ input */

static int touch_owner = -1, touch_x0, touch_y0, touch_moved;
static float touch_scroll0;
/* -2 Back, -1 no control, 0/1 the displayed action; defined with the text renderer below. */
static int footer_action_at(int x);

/* Called once per frame before the pointer and the engine see the input. */
/* ------------------------------------------------------------------ test: buy everything
 * Marker `debug_buy_all` (emulator): walks every category and row, presses the row's main action
 * (not Upgrade / Lottery, which leave the shop), waits, logs the popup and where the pointer's
 * highlight is, saves a screenshot when a highlight is up, closes what opened, next row. For the
 * owner's "after buying, the highlight sits on a random thing (the level on the top bar)". */
static const char *item_name(row_t *r);
static void buyall_step(uint8_t *menu) {
  static int dbg = -1, st, cat, row, wait, tries, shots, ncats;
  if (dbg < 0) { FILE *m = fopen(PORT_ROOT "/debug_buy_all", "r"); dbg = m != NULL; if (m) fclose(m); }
  if (dbg != 1) return;
  extern int pointer_debug_state(float r[4]);
  extern void port_request_popup_close(void), port_request_back(void);
  extern const char *current_menu_class;
  if (wait > 0) { wait--; return; }
  switch (st) {
  case 0: /* start once the list runs the shop */
    if (!menu) return;
    ncats = tab_count(menu);
    debugPrintf("[buyall] start: %d categories\n", ncats);
    cat = 0; st = 1;
    return;
  case 1: /* category */
    if (!menu) return;
    if (cat >= ncats) { debugPrintf("[buyall] done\n"); dbg = 2; return; }
    touch_tab(menu, cat);
    row = 0; st = 2; wait = 45;
    return;
  case 2: { /* row */
    if (!menu) return;
    if (row >= nrows) { cat++; st = 1; return; }
    select_row(shop_grid(menu), row, 0);
    keep_in_view(1);
    st = 3; wait = 12;
    return;
  }
  case 3: { /* main action */
    if (!menu || row >= nrows) { st = 2; return; }
    act_t a[8];
    int n = row_actions(&rows[row], a, 8), pick = -1;
    for (int i = 0; i < n && pick < 0; i++) {
      const char *w = act_srcs[a[i].src].what;
      if (strcmp(w, "upgrade") && strcmp(w, "lottery")) pick = i;
    }
    const char *nm = item_name(&rows[row]);
    if (pick < 0) { debugPrintf("[buyall] %d/%d %s: nothing to press\n", cat, row, nm ? nm : "?"); row++; st = 2; return; }
    pending_card = a[pick].card;
    pending_btn = a[pick].btn;
    pending_frames = 0;
    debugPrintf("[buyall] %d/%d %s: %s\n", cat, row, nm ? nm : "?", act_srcs[a[pick].src].what);
    st = 4; wait = 45; tries = 0;
    return;
  }
  case 4: { /* what is up now, and where is the highlight */
    float r[4] = {0};
    int ps = pointer_debug_state(r);
    debugPrintf("[buyall]   -> popup %d, menu %s, highlight %d at %.0f,%.0f-%.0f,%.0f\n", popup_up() && strstr(current_menu_class, "Inventory") ? 1 : 0,
                current_menu_class, ps, r[0], r[1], r[2], r[3]);
    if ((ps == 1 || ps == 2) && shots < 120) {
      static char nm[48];
      snprintf(nm, sizeof(nm), "ba_%d_%02d", cat, row);
      extern const char *debug_shot_request;
      debug_shot_request = nm;
      shots++;
    }
    st = 5;
    return;
  }
  case 5: { /* close whatever opened */
    void *p = popup_inst ? popup_inst(0) : NULL;
    int pop = p && popup_enabled && (popup_enabled(p) & 1);
    if (pop) { port_request_popup_close(); wait = 30; if (++tries > 4) { port_request_back(); } if (tries > 8) { row++; st = 2; } return; }
    if (popup_up() && tries < 8) {
      /* the achievement popup (AchivementMgr, not the Popup singleton): press the highlighted
       * button like a player's A would, a tap on the focus frame */
      extern void port_touch(int down, int x, int y, int id);
      static int tx, ty, held;
      float r[4] = {0};
      if (held) { port_touch(0, tx, ty, 8); held = 0; wait = 30; tries++; return; }
      if (pointer_debug_state(r) && r[2] > r[0]) {
        tx = (int)((r[0] + r[2]) / 2 * 2.25f); ty = (int)((r[1] + r[3]) / 2 * 2.25f);
        debugPrintf("[buyall]   achievement popup: tap on the highlight %d,%d\n", tx, ty);
        port_touch(1, tx, ty, 8); held = 1; wait = 4; return;
      }
      tries++; wait = 30; return;
    }
    if (!strstr(current_menu_class, "Menu_IGM_Inventory")) { port_request_back(); wait = 40; if (++tries > 8) { row++; st = 2; } return; }
    row++; st = 2; wait = 10;
    return;
  }
  }
}

void shop_ui_input(InputFrame *in) {
  frame_no++;
  uint8_t *menu = NULL;
  int own = should_own(&menu);
  {
    /* Opened on the "offers" view (HUD SHOP button, type 8): the engine left it only through the
     * controller's simulated tap on the first tab, which is dropped now; the list opens Weapons. */
    extern int shop_ui_transitional(uint8_t *);
    static unsigned long asked;
    uint8_t *m = shop_menu();
    if (!own && m && shop_ui_transitional(m) && frame_no > asked + 20) {
      asked = frame_no;
      int nt = tab_count(m), pick = 0;
      for (int i = 0; i < nt; i++) if (tab_node(m, i)[0] == 4) pick = i;
      debugPrintf("[shopui] shop opened on the offers view: category %d\n", pick);
      touch_tab(m, pick);
    }
  }
  if (own != owning) {
    debugPrintf("[shopui] %s\n", own ? "list on" : "list off");
    owning = own;
    sel_valid = 0;
    last_tab = -1;
    pending_card = NULL;
    touch_owner = -1;
    rep_dir = 0;
    if (!own && arrows_hidden) { uint8_t *m = shop_menu(); if (m) hide_arrows(m, 0); }
  }
  if (!owning) { popup_now = 0; buyall_step(NULL); return; }
  hide_arrows(menu, 1);
  uint8_t *grid = shop_grid(menu);
  layout(grid);
  rows_merge_all = *(int32_t *)(menu + 0x44) == 0;
  nrows = read_rows(grid, rows);

  /* list rebuilt (tab switch, purchase): find the selected item again, else the engine's choice */
  int t = cur_tab(menu);
  uint8_t *first = nrows ? rows[0].card : NULL;
  if (t != last_tab || nrows != last_n || first != last_first_card) {
    int found = -1;
    for (int i = 0; i < nrows && sel_def; i++) if (rows[i].def == sel_def) { found = i; break; }
    if (found < 0) /* the card the engine marked (e.g. the equipped weapon) */
      for (int i = 0; i < nrows; i++) {
        void *b = get_iobj(rows[i].card, 2);
        if (b && btn_marked && (btn_marked(b) & 1)) { found = i; break; }
      }
    if (t != last_tab) {
      debugPrintf("[shopui] category %d (type %d): %d items\n", t, *(int32_t *)(menu + 0x44), nrows);
      { /* each category once per session: what every row offers (support for lock/price reports) */
        static unsigned logged;
        if (t >= 0 && t < 32 && !(logged & (1u << t))) {
          logged |= 1u << t;
          for (int i = 0; i < nrows; i++) {
            act_t a[8];
            int na = row_actions(&rows[i], a, 8);
            act_t raw[8];
            int nr = card_actions(rows[i].card, raw, 8);
            extern const char *shop_ui_item_name(void *);
            const char *nm = shop_ui_item_name(&rows[i]);
            debugPrintf("[shopui]  %d %s inv %d need L%d locked %d: %s%s%s (card: %s%s%s)\n", i, nm ? nm : "?", rows[i].inv,
                        rows[i].def ? *(int32_t *)((uint8_t *)rows[i].def + 0x1d0) : -1, level_locked(&rows[i]),
                        na ? act_srcs[a[0].src].what : "-", na > 1 ? "/" : "", na > 1 ? act_srcs[a[1].src].what : "",
                        nr ? act_srcs[raw[0].src].what : "-", nr > 1 ? "/" : "", nr > 1 ? act_srcs[raw[1].src].what : "");
          }
        }
      }
      static int dumped = 1; /* 0: dump the page's objects once (debug) */
      if (!dumped++) {
        void **b = *(void ***)(grid + 0xb8), **e = *(void ***)(grid + 0xc0);
        debugPrintf("[shopui] grid %s, %d cells, first %s\n", cls(grid), (int)(e - b), b && e > b ? cls(b[0]) : "-");
        uint8_t *page = shop_page(menu);
        void **pb = *(void ***)(page + 0x68), **pe = *(void ***)(page + 0x70);
        for (void **it = pb; pb && pe > pb && pe - pb < 400 && it < pe; it++) {
          uint8_t *o = *it;
          if (!o || !vis(o)) continue;
          debugPrintf("[shopui]  page obj %d %s at %.0f,%.0f %.0fx%.0f\n", (int)(it - pb), cls(o), vflt(o, 0x60), vflt(o, 0x68),
                      vflt(o, 0xc0), vflt(o, 0xc8));
        }
      }
      if (found < 0) found = 0;
    }
    if (found < 0) found = sel < nrows ? sel : nrows - 1;
    last_tab = t;
    last_n = nrows;
    last_first_card = first;
    if (nrows) { select_row(grid, found, 0); keep_in_view(1); }
    else sel_valid = 0;

  }

  int popup = popup_up();
  {
    /* a popup closed (e.g. "drop this object?" -> Yes): the engine cleared its selected card (empty
     * description panel, the grid not drawn any more); select the current row again */
    static int was_popup, reselect_at;
    if (was_popup && !popup) reselect_at = 1;
    if (popup) reselect_at = 0;
    else if (reselect_at && ++reselect_at > 1 && (reselect_at == 10 || reselect_at == 30) && nrows) {
      if (sel >= nrows) sel = nrows - 1;
      select_row(grid, sel, 0);
      keep_in_view(1);
      if (reselect_at == 30) reselect_at = 0;
    }
    was_popup = popup;
  }
  popup_now = popup;
  buyall_step(menu);
  if (dbg_ach_armed && popup) { /* debug_ach_pending: an achievement unlocked by the purchase */
    void *(*ai)(int) = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI13AchivementMgrE11GetInstanceEb");
    uint8_t *am = ai ? ai(0) : NULL;
    if (am && *(void **)(am + 0xe8)) { am[8] = 1; debugPrintf("[shopui] DEBUG: achievement popup flag set\n"); }
    dbg_ach_armed = 0;
  }
  /* (The jumps to the first tab after the shop opened or a purchase popup closed came from an A
   * release reaching the shop's own controller scheme; main.c keeps it. A category the player picks
   * by touching a tab icon is simply followed.) */
  if (pending_card && ++pending_frames > 30) {
    debugPrintf("[shopui] action not delivered (card not updated)\n");
    pending_card = NULL;
  }

  /* --- buttons (only while no popup is up; then everything goes to the engine as before) --- */
  const u64 mine = HidNpadButton_A | HidNpadButton_Y | HidNpadButton_X | HidNpadButton_Up | HidNpadButton_Down |
                   HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL |
                   HidNpadButton_ZR | HidNpadButton_StickLUp | HidNpadButton_StickLDown | HidNpadButton_StickLLeft |
                   HidNpadButton_StickLRight | HidNpadButton_StickRUp | HidNpadButton_StickRDown |
                   HidNpadButton_StickRLeft | HidNpadButton_StickRRight;
  if (popup) { rep_dir = 0; touch_owner = -1; return; }

  /* stick as D-pad */
  int dir = 0;
  if (in->held & HidNpadButton_Up) dir = -1;
  if (in->held & HidNpadButton_Down) dir = 1;
  if (!dir) {
    int y = in->ls.y, x = in->ls.x;
    if (abs(y) > abs(x) && abs(y) > 16000) dir = y > 0 ? -1 : 1;
    if (!dir && abs(in->rs.y) > 16000) dir = in->rs.y > 0 ? -1 : 1;
  }
  int step = 0;
  if (dir != rep_dir) { if (dir) step = dir; rep_next = frame_no + 22; }
  else if (dir && frame_no >= (unsigned long)rep_next) { step = dir; rep_next = frame_no + 6; }
  rep_dir = dir;
  if (step && nrows) {
    int ns = sel + step;
    if (ns < 0) ns = 0;
    if (ns >= nrows) ns = nrows - 1;
    if (ns != sel) { select_row(grid, ns, 1); keep_in_view(0); }
  }
  static int lx_prev;
  int lx = in->ls.x > 20000 ? 1 : in->ls.x < -20000 ? -1 : 0;
  int tabdir = 0;
  if (in->down & (HidNpadButton_Left | HidNpadButton_L | HidNpadButton_ZL)) tabdir = -1;
  if (in->down & (HidNpadButton_Right | HidNpadButton_R | HidNpadButton_ZR)) tabdir = 1;
  if (!tabdir && lx != lx_prev && lx && abs(in->ls.x) > abs(in->ls.y)) tabdir = lx;
  lx_prev = lx;
  if (tabdir) change_tab(menu, tabdir);
  if (!(in->down & HidNpadButton_B)) {
    if (in->down & HidNpadButton_A) do_action(0);
    if (in->down & HidNpadButton_Y) do_action(1);
  }

  /* --- touch inside the panel --- */
  int seen = 0;
  for (int i = 0; i < in->ntouch; i++) {
    int x = in->t[i].x, y = in->t[i].y, id = in->t[i].id;
    int inside = x >= px0 && x < px1 && y >= py0 && y < py1;
    if (touch_owner < 0 && inside && id >= 0 && id < 16 && (in->touch_started & (1u << id))) {
      touch_owner = id; touch_x0 = x; touch_y0 = y; touch_moved = 0; touch_scroll0 = scroll_target;
    }
    if (id != touch_owner) continue;
    seen = 1;
    if (abs(y - touch_y0) > 18 || abs(x - touch_x0) > 18) touch_moved = 1;
    if (touch_moved && touch_y0 > py0 + HEAD_H) {
      float maxtop = nrows - vis_rows < 0 ? 0 : (float)(nrows - vis_rows);
      float s = touch_scroll0 - (y - touch_y0) / (float)ROW_H;
      scroll_target = scroll = s < 0 ? 0 : s > maxtop ? maxtop : s;
    }
    /* remove it: the engine must not see touches on the list (its hidden cards are there) */
    in->t[i] = in->t[--in->ntouch];
    i--;
  }
  if (touch_owner >= 0 && !seen) { /* released */
    if (!touch_moved) {
      if (touch_y0 < py0 + HEAD_H) change_tab(menu, touch_x0 < (px0 + px1) / 2 ? -1 : 1);
      else if (touch_y0 >= py1 - FOOT_H) {
        int action = footer_action_at(touch_x0);
        if (action == -2) { in->down |= HidNpadButton_B; pending_card = NULL; pending_btn = NULL;
          debugPrintf("[shopui] touch Back\n"); }
        else if (action >= 0 && !(in->down & HidNpadButton_B)) do_action(action);
      }
      else {
        int r = (int)(scroll + (touch_y0 - (py0 + HEAD_H)) / ROW_H);
        if (r >= 0 && r < nrows) {
          if (r == sel) do_action(0);
          else { select_row(grid, r, 1); keep_in_view(0); }
        }
      }
    }
    touch_owner = -1;
  }

  /* everything above is the list's: nobody else gets these presses or the sticks */
  in->down &= ~mine;
  in->held &= ~mine;
  in->ls.x = in->ls.y = in->rs.x = in->rs.y = 0;
}

/* Page_InventoryButton::Update, wrapped: delivers a pending action to its card. */
static void card_update_hook(void *card, int a1, int a2, int a3) {
  if (card == pending_card && pending_btn && a1 > 0) {
    add_released(pending_btn);
    pending_card = NULL;
    pending_btn = NULL;
  }
  orig_card_update(card, a1, a2, a3);
}

/* ------------------------------------------------------------------ text (FreeType, glyph cache) */

static struct {
  typeof(&glGetIntegerv) GetIntegerv; typeof(&glIsEnabled) IsEnabled; typeof(&glEnable) Enable;
  typeof(&glDisable) Disable; typeof(&glBlendFuncSeparate) BlendFuncSeparate;
  typeof(&glBlendEquationSeparate) BlendEquationSeparate; typeof(&glColorMask) ColorMask;
  typeof(&glGetBooleanv) GetBooleanv; typeof(&glUseProgram) UseProgram; typeof(&glBindBuffer) BindBuffer;
  typeof(&glBindFramebuffer) BindFramebuffer; typeof(&glViewport) Viewport;
  typeof(&glGetVertexAttribiv) GetVertexAttribiv; typeof(&glGetVertexAttribPointerv) GetVertexAttribPointerv;
  typeof(&glVertexAttribPointer) VertexAttribPointer; typeof(&glEnableVertexAttribArray) EnableVertexAttribArray;
  typeof(&glDisableVertexAttribArray) DisableVertexAttribArray; typeof(&glDrawArrays) DrawArrays;
  typeof(&glCreateShader) CreateShader; typeof(&glShaderSource) ShaderSource; typeof(&glCompileShader) CompileShader;
  typeof(&glCreateProgram) CreateProgram; typeof(&glAttachShader) AttachShader;
  typeof(&glBindAttribLocation) BindAttribLocation; typeof(&glLinkProgram) LinkProgram;
  typeof(&glGetUniformLocation) GetUniformLocation; typeof(&glUniform2f) Uniform2f; typeof(&glUniform4f) Uniform4f;
  typeof(&glUniform1i) Uniform1i; typeof(&glGetProgramiv) GetProgramiv; typeof(&glGenTextures) GenTextures;
  typeof(&glBindTexture) BindTexture; typeof(&glTexImage2D) TexImage2D; typeof(&glTexSubImage2D) TexSubImage2D;
  typeof(&glTexParameteri) TexParameteri; typeof(&glActiveTexture) ActiveTexture; typeof(&glPixelStorei) PixelStorei;
} G;

enum { AW = 1024, AH = 512 };
static unsigned char atlas[AW * AH];
static int atlas_dirty_y0 = AH, atlas_dirty_y1 = 0;
static int shelf_x, shelf_y, shelf_h;
static FT_Library ftlib;
static FT_Face face_std, face_ext;
static GLuint tex, prog;
static GLint u_screen, u_color;
static int gl_state; /* 0 untried, 1 ok, -1 failed */

typedef struct { uint32_t key; short x, y, w, h, left, top, adv; } glyph_t;
enum { GCAP = 2048 };
static glyph_t gcache[GCAP];
static int gcount;

static void atlas_clear(void) {
  static unsigned resets;
  if (gcount && resets++ < 8) debugPrintf("[shopui] glyph atlas reset: %d glyphs, shelf %d\n", gcount, shelf_y);
  memset(atlas, 0, sizeof(atlas));
  memset(gcache, 0, sizeof(gcache));
  gcount = 0;
  /* row 0..31: a filled disc (coin / badge icon) at 0,0 32x32 */
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      float d = sqrtf((x - 15.5f) * (x - 15.5f) + (y - 15.5f) * (y - 15.5f));
      float a = 15.5f - d;
      atlas[y * AW + x] = a >= 1 ? 255 : a <= 0 ? 0 : (unsigned char)(a * 255);
    }
  /* a solid texel for plain quads at 40,0 */
  for (int y = 0; y < 4; y++) for (int x = 40; x < 44; x++) atlas[y * AW + x] = 255;
  shelf_x = 48; shelf_y = 0; shelf_h = 34;
  atlas_dirty_y0 = 0; atlas_dirty_y1 = AH;
}

static glyph_t *glyph(unsigned cp, int px) {
  uint32_t key = (cp & 0x1fffff) | ((uint32_t)px << 21);
  uint32_t h = (key * 2654435761u) & (GCAP - 1);
  for (int probe = 0; probe < GCAP; probe++) {
    glyph_t *g = &gcache[(h + probe) & (GCAP - 1)];
    if (g->key == key) return g;
    if (g->key) continue;
    if (gcount > GCAP * 3 / 4) { atlas_clear(); return glyph(cp, px); }
    FT_Face f = face_std;
    if (cp >= 0xE000 && cp < 0xF900 && face_ext && FT_Get_Char_Index(face_ext, cp)) f = face_ext;
    FT_Set_Pixel_Sizes(f, 0, px);
    if (FT_Load_Char(f, cp, FT_LOAD_RENDER)) return NULL;
    FT_GlyphSlot s = f->glyph;
    int w = s->bitmap.width, hh = s->bitmap.rows;
    if (shelf_x + w + 1 > AW) { shelf_x = 0; shelf_y += shelf_h; shelf_h = 0; }
    if (shelf_y + hh + 1 > AH) { atlas_clear(); return glyph(cp, px); }
    for (int r = 0; r < hh; r++) memcpy(&atlas[(shelf_y + r) * AW + shelf_x], s->bitmap.buffer + r * s->bitmap.pitch, w);
    g->key = key;
    g->x = shelf_x; g->y = shelf_y; g->w = w; g->h = hh;
    g->left = s->bitmap_left; g->top = s->bitmap_top; g->adv = (short)(s->advance.x >> 6);
    if (shelf_y < atlas_dirty_y0) atlas_dirty_y0 = shelf_y;
    if (shelf_y + hh > atlas_dirty_y1) atlas_dirty_y1 = shelf_y + hh;
    shelf_x += w + 1;
    if (hh + 1 > shelf_h) shelf_h = hh + 1;
    gcount++;
    return g;
  }
  return NULL;
}

static unsigned next_cp(const unsigned char **pp) {
  return ui_next_cp(pp);
}

static float text_w(const char *s, int px) {
  float w = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p;) {
    glyph_t *g = glyph(next_cp(&p), px);
    if (g) w += g->adv;
  }
  return w;
}

static int gl_setup(void) {
  if (gl_state) return gl_state > 0;
  gl_state = -1;
#define L(n) if (!(G.n = (void *)eglGetProcAddress("gl" #n))) { debugPrintf("[shopui] no gl" #n "\n"); return 0; }
  L(GetIntegerv) L(IsEnabled) L(Enable) L(Disable) L(BlendFuncSeparate) L(BlendEquationSeparate) L(ColorMask)
  L(GetBooleanv) L(UseProgram) L(BindBuffer) L(BindFramebuffer) L(Viewport) L(GetVertexAttribiv)
  L(GetVertexAttribPointerv) L(VertexAttribPointer) L(EnableVertexAttribArray) L(DisableVertexAttribArray)
  L(DrawArrays) L(CreateShader) L(ShaderSource) L(CompileShader) L(CreateProgram) L(AttachShader)
  L(BindAttribLocation) L(LinkProgram) L(GetUniformLocation) L(Uniform2f) L(Uniform4f) L(Uniform1i) L(GetProgramiv)
  L(GenTextures) L(BindTexture) L(TexImage2D) L(TexSubImage2D) L(TexParameteri) L(ActiveTexture) L(PixelStorei)
#undef L
  if (R_FAILED(plInitialize(PlServiceType_User))) { debugPrintf("[shopui] pl:u unavailable\n"); return 0; }
  PlFontData std, ext;
  if (R_FAILED(plGetSharedFontByType(&std, PlSharedFontType_Standard))) { debugPrintf("[shopui] no system font\n"); return 0; }
  if (FT_Init_FreeType(&ftlib) || FT_New_Memory_Face(ftlib, std.address, std.size, 0, &face_std)) return 0;
  if (R_SUCCEEDED(plGetSharedFontByType(&ext, PlSharedFontType_NintendoExt)))
    if (FT_New_Memory_Face(ftlib, ext.address, ext.size, 0, &face_ext)) face_ext = NULL;
  /* pl:u stays open: FreeType reads the shared font memory for every new glyph */

  static const char *vs =
      "attribute vec4 a; uniform vec2 u_screen; varying vec2 uv;\n"
      "void main() { uv = a.zw; gl_Position = vec4(a.x / u_screen.x * 2.0 - 1.0, 1.0 - a.y / u_screen.y * 2.0, 0.0, 1.0); }\n";
  static const char *fs =
      "precision mediump float; uniform vec4 u_color; uniform sampler2D u_tex; varying vec2 uv;\n"
      "void main() { gl_FragColor = vec4(u_color.rgb, u_color.a * texture2D(u_tex, uv).a); }\n";
  GLint prev_prog = 0;
  G.GetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  GLuint v = G.CreateShader(GL_VERTEX_SHADER), fr = G.CreateShader(GL_FRAGMENT_SHADER);
  G.ShaderSource(v, 1, &vs, NULL); G.CompileShader(v);
  G.ShaderSource(fr, 1, &fs, NULL); G.CompileShader(fr);
  prog = G.CreateProgram();
  G.AttachShader(prog, v); G.AttachShader(prog, fr);
  G.BindAttribLocation(prog, 0, "a");
  G.LinkProgram(prog);
  GLint ok = 0;
  G.GetProgramiv(prog, GL_LINK_STATUS, &ok);
  G.UseProgram(prev_prog);
  if (!ok) { debugPrintf("[shopui] shader link failed\n"); return 0; }
  u_screen = G.GetUniformLocation(prog, "u_screen");
  u_color = G.GetUniformLocation(prog, "u_color");
  G.UseProgram(prog);
  G.Uniform1i(G.GetUniformLocation(prog, "u_tex"), 0);
  G.UseProgram(prev_prog);

  atlas_clear();
  GLint prev_tex = 0, prev_unit = 0, prev_align = 4;
  G.GetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
  G.ActiveTexture(GL_TEXTURE0);
  G.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  G.GetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
  G.GenTextures(1, &tex);
  G.BindTexture(GL_TEXTURE_2D, tex);
  G.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
  G.TexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, AW, AH, 0, GL_ALPHA, GL_UNSIGNED_BYTE, atlas);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  G.PixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
  G.BindTexture(GL_TEXTURE_2D, prev_tex);
  G.ActiveTexture(prev_unit);
  atlas_dirty_y0 = AH; atlas_dirty_y1 = 0;
  gl_state = 1;
  debugPrintf("[shopui] renderer ready\n");
  return 1;
}

static float cr, cg, cb, ca;
static void color(float r, float g, float b, float a) { cr = r; cg = g; cb = b; ca = a; G.Uniform4f(u_color, r, g, b, a); }
static void quad_uv(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
  GLfloat q[16] = { x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y1, u1, v1 };
  G.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, q);
  G.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void rect(float x0, float y0, float x1, float y1) {
  quad_uv(x0, y0, x1, y1, 41.5f / AW, 1.5f / AH, 42.5f / AW, 2.5f / AH);
}
static void frame_rect(float x0, float y0, float x1, float y1, float t) {
  rect(x0, y0, x1, y0 + t); rect(x0, y1 - t, x1, y1); rect(x0, y0 + t, x0 + t, y1 - t); rect(x1 - t, y0 + t, x1, y1 - t);
}
static void disc(float cx, float cy, float r) { quad_uv(cx - r, cy - r, cx + r, cy + r, 0, 0, 32.0f / AW, 32.0f / AH); }

/* Glyphs are created before drawing (uploads happen in flush_atlas). */
static void prepare(const char *s, int px) { text_w(s, px); }
static void flush_atlas(void) {
  if (atlas_dirty_y1 <= atlas_dirty_y0) return;
  GLint align = 4;
  G.GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
  G.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
  G.TexSubImage2D(GL_TEXTURE_2D, 0, 0, atlas_dirty_y0, AW, atlas_dirty_y1 - atlas_dirty_y0, GL_ALPHA, GL_UNSIGNED_BYTE,
                  atlas + atlas_dirty_y0 * AW);
  G.PixelStorei(GL_UNPACK_ALIGNMENT, align);
  atlas_dirty_y0 = AH; atlas_dirty_y1 = 0;
}

/* Bound the client-side buffer while reducing one draw per glyph to one per run. */
static GLfloat text_vertices[128 * 24];
static unsigned text_runs, text_glyphs, text_draws;
static unsigned wrap_hits,wrap_builds;
static void text_flush(unsigned *count) {
  if (!*count) return;
  G.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, text_vertices);
  G.DrawArrays(GL_TRIANGLES, 0, *count * 6);
  text_draws++;
  *count = 0;
}
void sui_stats(void) {
  debugPrintf("[ui] text: %u runs, %u glyphs, %u draw calls\n", text_runs, text_glyphs, text_draws);
  debugPrintf("[ui] wrapping: %u cached runs, %u layout builds\n",wrap_hits,wrap_builds);
}

/* Draws s with its baseline at y; returns the end x. maxw > 0: cut with "..." to fit. */
static float text(float x, float y, const char *s, int px, float maxw) {
  char buf[256];
  if (maxw > 0 && text_w(s, px) > maxw) {
    size_t n = strlen(s);
    if (n > sizeof(buf) - 4) n = sizeof(buf) - 4;
    while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) n--;
    memcpy(buf, s, n);
    buf[n] = 0;
    float ew = text_w("...", px);
    while (n > 0 && text_w(buf, px) + ew > maxw) {
      n--;
      while (n > 0 && ((unsigned char)buf[n] & 0xc0) == 0x80) n--; /* UTF-8 boundary */
      buf[n] = 0;
    }
    if (ew > maxw) return x;
    strcat(buf, "...");
    s = buf;
  }
  prepare(s, px);
  flush_atlas();
  unsigned count = 0;
  text_runs++;
  for (const unsigned char *p = (const unsigned char *)s; *p;) {
    glyph_t *g = glyph(next_cp(&p), px);
    if (!g) continue;
    /* A cache reset may occur in a very long run. Draw old UVs before uploading. */
    if (atlas_dirty_y1 > atlas_dirty_y0) { text_flush(&count); flush_atlas(); }
    if (g->w && g->h) {
      float gx = x + g->left, gy = y - g->top;
      float x1=gx+g->w, y1=gy+g->h, u=(float)g->x/AW, v=(float)g->y/AH;
      float u1=(float)(g->x+g->w)/AW, v1=(float)(g->y+g->h)/AH;
      GLfloat q[24]={gx,gy,u,v, x1,gy,u1,v, gx,y1,u,v1,
                     gx,y1,u,v1, x1,gy,u1,v, x1,y1,u1,v1};
      memcpy(text_vertices + count * 24, q, sizeof(q));
      count++; text_glyphs++;
      if (count == 128) text_flush(&count);
    }
    x += g->adv;
  }
  text_flush(&count);
  return x;
}
static void text_shadow(float x, float y, const char *s, int px, float maxw) {
  float r = cr, g = cg, b = cb, a = ca;
  color(0, 0, 0, 0.55f * a);
  text(x + 1.5f, y + 1.5f, s, px, maxw);
  color(r, g, b, a);
  text(x, y, s, px, maxw);
}

/* ------------------------------------------------------------------ drawing */

#define G_A "\xee\x83\xa0"  /* U+E0E0 */
#define G_B "\xee\x83\xa1"
#define G_Y "\xee\x83\xa3"
#define G_L "\xee\x83\xa4"
#define G_R "\xee\x83\xa5"
#define G_UPDOWN "\xee\x83\xbe" /* U+E0FE? falls back to arrows below */

static const char *item_name(row_t *r) {
  if (!r->def) return NULL;
  int id = *(int32_t *)((uint8_t *)r->def + 0xd8);
  void *sm = str_inst(0);
  const char *s = sm && id > 0 ? get_string(sm, id) : NULL;
  return s && *s ? s : NULL;
}
static const char *card_label(row_t *r) {
  /* not an inventory item (extras like "more slots"): the text of its first visible action */
  act_t a[4];
  int n = row_actions(r, a, 4);
  void *sm = str_inst(0);
  for (int i = 0; i < n && sm; i++) {
    int id = btn_text_id(a[i].btn);
    const char *s = id > 0 ? get_string(sm, id) : NULL;
    if (s && *s && strlen(s) < 64) return s;
  }
  return "?";
}
static int price(void *def, int cur) { return pint_get((uint8_t *)def + (cur ? 0x120 : 0x100)); }
static void fmt_num(char *b, size_t n, int v) {
  if (v >= 1000000) snprintf(b, n, "%d,%03d,%03d", v / 1000000, v / 1000 % 1000, v % 1000);
  else if (v >= 1000) snprintf(b, n, "%d,%03d", v / 1000, v % 1000);
  else snprintf(b, n, "%d", v);
}
static void coin(float cx, float cy, int badge) {
  if (badge) { color(0.20f, 0.30f, 0.45f, 1); disc(cx, cy, 10); color(0.75f, 0.82f, 0.92f, 1); disc(cx, cy, 8); }
  else { color(0.55f, 0.36f, 0.02f, 1); disc(cx, cy, 10); color(1.0f, 0.80f, 0.20f, 1); disc(cx, cy, 8); }
}

/* Status at the right end of a row; returns its left x. */
static int draw_type = -1; /* inventory type of the category being drawn (1 = Supplies) */
static float row_status(row_t *r, float right, float base, int selected) {
  char b[48];
  act_t a[8];
  int n = row_actions(r, a, 8);
  const char *what = n ? act_srcs[a[0].src].what : "";
  if (r->def && (is_equipped(r->def) & 1)) {
    color(0.08f, 0.45f, 0.08f, 1);
    float w = text_w("Equipped", 22);
    text(right - w, base, "Equipped", 22, 0);
    return right - w;
  }
  if (!strcmp(what, "buy") && r->def) {
    int badge = act_srcs[a[0].src].field == 0xe0;
    fmt_num(b, sizeof(b), price(r->def, badge));
    float w = text_w(b, 22);
    color(0.25f, 0.14f, 0.05f, 1);
    text(right - w, base, b, 22, 0);
    coin(right - w - 16, base - 8, badge);
    float left = right - w - 30;
    if (n > 1 && !strcmp(act_srcs[a[1].src].what, "buy") && act_srcs[a[1].src].field == 0xe0) {
      fmt_num(b, sizeof(b), price(r->def, 1));
      float w2 = text_w(b, 22);
      color(0.25f, 0.14f, 0.05f, 1);
      text(left - 14 - w2, base, b, 22, 0);
      coin(left - 14 - w2 - 16, base - 8, 1);
      left = left - 14 - w2 - 30;
    }
    /* consumables you have some of: the count in the same row (Supplies used to show two rows) */
    int has_use = 0;
    for (int i = 0; i < n; i++) has_use |= !strcmp(act_srcs[a[i].src].what, "use");
    (void)has_use; /* a buyable item you already have some of is a consumable: always show the count */
    int cnt = item_count(item_inst(0), r->def);
    if (cnt > 0 || draw_type == 1) { /* Supplies: always, 0 included (owner) */
      snprintf(b, sizeof(b), "Owned: %d", cnt);
      float w3 = text_w(b, 22);
      color(0.35f, 0.25f, 0.12f, 1);
      text(left - 22 - w3, base, b, 22, 0);
      left = left - 22 - w3;
    }
    return left;
  }
  if (!strcmp(what, "unlock") || !strcmp(what, "locked") || level_locked(r)) {
    int lvl = r->def ? *(int32_t *)((uint8_t *)r->def + 0x1d0) : 0;
    if (lvl > 0) snprintf(b, sizeof(b), "Level %d", lvl);
    else snprintf(b, sizeof(b), "Locked");
    color(0.60f, 0.14f, 0.08f, 1);
    float w = text_w(b, 22);
    text(right - w, base, b, 22, 0);
    return right - w;
  }
  if (r->def) {
    int cnt = item_count(item_inst(0), r->def);
    if (cnt > 1 || !strcmp(what, "use") || draw_type <= 1) snprintf(b, sizeof(b), "Owned: %d", cnt);
    else if (cnt > 0) snprintf(b, sizeof(b), "Owned");
    else b[0] = 0;
    if (b[0]) {
      color(0.35f, 0.25f, 0.12f, 1);
      float w = text_w(b, 22);
      text(right - w, base, b, 22, 0);
      return right - w;
    }
  }
  (void)selected;
  return right;
}

static float text_shadow_ret(float x, float y, const char *glyph_s, const char *label) {
  char b[96];
  snprintf(b, sizeof(b), "%s%s", glyph_s, label);
  text_shadow(x, y, b, 22, 0);
  return x + text_w(b, 22);
}

static const char *action_label(int src, void *btn, char *buf, size_t n) {
  const char *w = act_srcs[src].what;
  if (!strcmp(w, "buy")) return act_srcs[src].field == 0xe0 ? "Buy with badges" : "Buy";
  if (!strcmp(w, "equip")) return "Equip";
  if (!strcmp(w, "use")) return "Use";
  if (!strcmp(w, "remove")) return "Throw away";
  if (!strcmp(w, "unlock")) return "Pay badges to unlock early"; /* the game's level-skip popup */
  if (!strcmp(w, "locked")) return "How to unlock";
  if (!strcmp(w, "upgrade")) return "Upgrade";
  if (!strcmp(w, "free")) return "Get it free";
  if (!strcmp(w, "lottery")) return "Win in the lottery";
  (void)btn; (void)buf; (void)n;
  return "Select";
}

static int footer_action_at(int x) {
  if (x >= px1 - 140) return -2;
  if (!sel_valid || sel < 0 || sel >= nrows || gl_state != 1) return -1;
  act_t a[2];
  ay_slots(&rows[sel], a);
  float left = px0 + 16;
  for (int i = 0; i < 2; i++) {
    if (a[i].src < 0) continue;
    char label[64], full[128];
    snprintf(full, sizeof(full), "%s %s", i ? G_Y : G_A, action_label(a[i].src, a[i].btn, label, sizeof(label)));
    float right = left + text_w(full, 22);
    if (x >= left - 8 && x <= right + 8 && x < px1 - 140) return i;
    left = right + 22;
  }
  return -1;
}

static void draw_panel(uint8_t *menu, uint8_t *grid, int blank) {
  layout(grid);
  draw_type = *(int32_t *)(menu + 0x44);
  row_t cur[MAX_ROWS];
  rows_merge_all = draw_type == 0;
  int n = blank ? 0 : read_rows(grid, cur);
  if (sel >= n) sel = n ? n - 1 : 0;
  scroll += (scroll_target - scroll) * 0.35f;
  if (fabsf(scroll - scroll_target) < 0.01f) scroll = scroll_target;

  if (blank) {
    /* the offers view's empty description box and "Full Upgrade" offer, for the frames before the
     * list switches to Weapons: covered with the shop's backdrop colour */
    color(0.93f, 0.76f, 0.47f, 1); rect(0, 62, 1280, 720);
  }
  /* panel: dark wood frame, parchment inside */
  color(0.20f, 0.11f, 0.04f, 0.97f); rect(px0, py0, px1, py1);
  color(0.78f, 0.58f, 0.26f, 1); frame_rect(px0, py0, px1, py1, 3);
  /* header: category */
  int nt = tab_count(menu), t = cur_tab(menu);
  /* by inventory type (the tab node's text id does not give the tab's label: 0x265 is "God Mode") */
  static const char *names[] = { "Saddlebag", "Supplies", "Horses", "Outfits", "Weapons", "Upgrades", "Chests",
                                 "", "", "New", "Lottery Tickets", "Legendary" };
  int type = *(int32_t *)(menu + 0x44); (void)t; (void)nt;
  const char *cat = type >= 0 && type < (int)(sizeof(names) / sizeof(*names)) && names[type][0] ? names[type] : "Shop";
  char hb[96];
  snprintf(hb, sizeof(hb), "%s", cat);
  float cx = (px0 + px1) / 2, hy = py0 + 32;
  color(1.0f, 0.86f, 0.45f, 1);
  float hw = text_w(hb, 28);
  text_shadow(cx - hw / 2, hy, hb, 28, px1 - px0 - 220);
  color(0.95f, 0.88f, 0.70f, 0.95f);
  float rw = 0;
  if (type != 0 && nt > 1) {
    text_shadow(px0 + 14, hy - 2, G_L " \xe2\x97\x80", 24, 0);
    rw = text_w("\xe2\x96\xb6 " G_R, 24);
    text_shadow(px1 - 14 - rw, hy - 2, "\xe2\x96\xb6 " G_R, 24, 0);
  }
  if (n) {
    char pos[32];
    snprintf(pos, sizeof(pos), "%d / %d", sel + 1, n);
    color(0.85f, 0.75f, 0.55f, 0.9f);
    float pw = text_w(pos, 18);
    text(px1 - 14 - rw - 18 - pw, hy - 4, pos, 18, 0);
  }

  /* rows */
  float lx0 = px0 + 8, lx1 = px1 - 18, ly0 = py0 + HEAD_H, ly1 = py1 - FOOT_H;
  G.Enable(GL_SCISSOR_TEST);
  {
    /* scissor in GL window coordinates (origin bottom-left) */
    void (*scissor)(GLint, GLint, GLsizei, GLsizei) = (void *)eglGetProcAddress("glScissor");
    if (scissor) scissor((GLint)lx0, (GLint)(SCREEN_H - ly1), (GLsizei)(lx1 - lx0), (GLsizei)(ly1 - ly0));
  }
  int first = (int)floorf(scroll);
  for (int i = first; i < n && i <= first + vis_rows + 1; i++) {
    if (i < 0) continue;
    float y0 = ly0 + (i - scroll) * ROW_H + 2, y1 = y0 + ROW_H - 4;
    int s = sel_valid && i == sel;
    if (s) { color(1.0f, 0.84f, 0.40f, 1); rect(lx0, y0, lx1, y1); color(0.45f, 0.24f, 0.05f, 1); frame_rect(lx0, y0, lx1, y1, 3); }
    else if (i & 1) { color(0.90f, 0.80f, 0.60f, 1); rect(lx0, y0, lx1, y1); }
    else { color(0.95f, 0.87f, 0.69f, 1); rect(lx0, y0, lx1, y1); }
    float base = y0 + (y1 - y0) / 2 + 8;
    float left = row_status(&cur[i], lx1 - 14, base, s);
    const char *nm = item_name(&cur[i]);
    if (!nm) nm = card_label(&cur[i]);
    color(0.22f, 0.12f, 0.04f, 1);
    text(lx0 + 14, base, nm, 24, left - (lx0 + 14) - 16);
  }
  G.Disable(GL_SCISSOR_TEST);
  if (!n && !blank) {
    color(0.95f, 0.88f, 0.70f, 1);
    const char *e = "Nothing here";
    text(cx - text_w(e, 24) / 2, (ly0 + ly1) / 2, e, 24, 0);
  }
  /* scroll bar */
  if (n > vis_rows) {
    float h = ly1 - ly0, bh = h * vis_rows / n, by = ly0 + h * scroll / n;
    color(0.45f, 0.30f, 0.12f, 1); rect(px1 - 13, ly0, px1 - 7, ly1);
    color(0.95f, 0.78f, 0.38f, 1); rect(px1 - 13, by, px1 - 7, by + bh);
  }

  /* footer: what the buttons do for the selected item */
  float fy = py1 - 12, fx = px0 + 16;
  color(0.95f, 0.88f, 0.70f, 1);
  if (sel_valid && sel < n) {
    act_t a[2];
    ay_slots(&cur[sel], a);
    char lb[64];
    if (a[0].src >= 0) { fx = text_shadow_ret(fx, fy, G_A " ", action_label(a[0].src, a[0].btn, lb, sizeof(lb))); }
    if (a[1].src >= 0) { fx = text_shadow_ret(fx + (a[0].src >= 0 ? 22 : 0), fy, G_Y " ", action_label(a[1].src, a[1].btn, lb, sizeof(lb))); }
  }
  {
    const char *bk = G_B " Back";
    float w = text_w(bk, 22);
    text_shadow(px1 - 16 - w, fy, bk, 22, 0);
  }
}

/* LayerGrid::Render hook (grid_cull.c): draws the list instead of the shop's grid. */
/* The shop opened from the HUD's SHOP button starts on the "offers" view (type 8, a tab the port
 * removes because it is empty offline) and switches to the first tab ~0.2 s later: the old grid
 * showed for that moment. The panel covers it, empty. */
static int transitional(uint8_t *menu) {
  extern int shop_menu_active(void);
  return enabled && menu && shop_menu_active() && tutorial_done() && tab_count(menu) >= 2 && cur_tab(menu) < 0 &&
         *(int32_t *)(menu + 0x44) == 8;
}

/* GL state around the port's drawing inside / after the engine's frame (saved, then restored). */
static GLint prog0, abuf, fbo, vp[4], bsrc, bdst, basrc, badst, beq, beqa, unit0, tex0, sc[4];
static GLboolean cmask[4];
static const GLenum caps[] = { GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST };
static GLboolean on[5];
enum { MAXA = 16 };
static GLint en[MAXA], maxa;
static GLint a0_size, a0_type, a0_norm, a0_stride, a0_buf;
static void *a0_ptr;
static int ui_keep_target; /* sui_begin_here: draw into the engine's current framebuffer */
static void ui_begin(void) {

  G.GetIntegerv(GL_CURRENT_PROGRAM, &prog0);
  G.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
  G.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  G.GetIntegerv(GL_VIEWPORT, vp);
  G.GetIntegerv(GL_SCISSOR_BOX, sc);
  G.GetIntegerv(GL_BLEND_SRC_RGB, &bsrc); G.GetIntegerv(GL_BLEND_DST_RGB, &bdst);
  G.GetIntegerv(GL_BLEND_SRC_ALPHA, &basrc); G.GetIntegerv(GL_BLEND_DST_ALPHA, &badst);
  G.GetIntegerv(GL_BLEND_EQUATION_RGB, &beq); G.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &beqa);
  G.GetBooleanv(GL_COLOR_WRITEMASK, cmask);
  G.GetIntegerv(GL_ACTIVE_TEXTURE, &unit0);
  G.ActiveTexture(GL_TEXTURE0);
  G.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
  for (int i = 0; i < 5; i++) on[i] = G.IsEnabled(caps[i]);
  memset(en, 0, sizeof(en));
  maxa = 0;
  G.GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxa);
  if (maxa > MAXA) maxa = MAXA;
  for (int i = 0; i < maxa; i++) G.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en[i]);
  a0_ptr = NULL;
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a0_size);
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a0_type);
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a0_norm);
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a0_stride);
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a0_buf);
  G.GetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a0_ptr);

  if (!ui_keep_target) {
    G.BindFramebuffer(GL_FRAMEBUFFER, 0);
    G.Viewport(0, 0, SCREEN_W, SCREEN_H);
  }
  for (int i = 1; i < 5; i++) G.Disable(caps[i]);
  G.Enable(GL_BLEND);
  G.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  G.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  G.ColorMask(1, 1, 1, 1);
  for (int i = 1; i < maxa; i++) if (en[i]) G.DisableVertexAttribArray(i);
  G.EnableVertexAttribArray(0);
  G.BindBuffer(GL_ARRAY_BUFFER, 0);
  G.UseProgram(prog);
  G.Uniform2f(u_screen, SCREEN_W, SCREEN_H);
  G.BindTexture(GL_TEXTURE_2D, tex);

}
static void ui_end(void) {

  G.BindTexture(GL_TEXTURE_2D, tex0);
  G.ActiveTexture(unit0);
  G.BindBuffer(GL_ARRAY_BUFFER, a0_buf);
  G.VertexAttribPointer(0, a0_size, a0_type, a0_norm, a0_stride, a0_ptr);
  G.BindBuffer(GL_ARRAY_BUFFER, abuf);
  if (!en[0]) G.DisableVertexAttribArray(0);
  for (int i = 1; i < maxa; i++) if (en[i]) G.EnableVertexAttribArray(i);
  G.UseProgram(prog0);
  G.ColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
  G.BlendEquationSeparate(beq, beqa);
  G.BlendFuncSeparate(bsrc, bdst, basrc, badst);
  {
    void (*scissor)(GLint, GLint, GLsizei, GLsizei) = (void *)eglGetProcAddress("glScissor");
    if (scissor) scissor(sc[0], sc[1], sc[2], sc[3]);
  }
  for (int i = 0; i < 5; i++) { if (on[i]) G.Enable(caps[i]); else G.Disable(caps[i]); }
  G.Viewport(vp[0], vp[1], vp[2], vp[3]);
  G.BindFramebuffer(GL_FRAMEBUFFER, fbo);
}

/* The engine's grid render (grid_cull.c): suppressed while the list runs; the list itself is drawn
 * after the engine's frame (shop_ui_overlay). Drawn from inside the grid render it was covered by
 * the engine's batched sprites after a saddle bag drop (Yes on "drop this object?"), the screen
 * stayed an empty panel (emulator 2026-11-14). */
static unsigned long grid_frame;
int shop_ui_grid_render(uint8_t *grid) {
  uint8_t *menu = shop_menu();
  int blank = !owning && transitional(menu);
  if (!owning && !blank) return 0;
  if (!menu || grid != shop_grid(menu)) return 0;
  if (owning) hide_arrows(menu, 1); /* in case the menu's update showed them again this frame */
  grid_frame = frame_no;
  grid_blank = blank;
  if (popup_up() && gl_setup()) { /* under the engine's popup, which is drawn after this */
    ui_begin();
    draw_panel(menu, grid, blank);
    ui_end();
  }
  return 1;
}

/* After the engine's frame (main.c); not over a popup, which stays the engine's. */
void shop_ui_overlay(void) {
  if (grid_frame != frame_no || popup_up()) return;
  uint8_t *menu = shop_menu();
  uint8_t *grid = menu ? shop_grid(menu) : NULL;
  if (!grid || !gl_setup()) return;
  ui_begin();
  draw_panel(menu, grid, grid_blank);
  ui_end();
}

/* The Android controller scheme "highlights" its focused widget by a simulated touch on it
 * (SixGunsTouchSystem press/drag/release). In the shop its focus sits on the tab bar's first tab:
 * on opening and after every popup it tapped Legendary, and the shop jumped there for a moment
 * (owner, hardware 2026-11-14). While the list runs the shop (no popup up) those simulated touches
 * are dropped; real touches don't go through this path. */
static void (*orig_ts_press)(void *, int, int), (*orig_ts_drag)(void *, int, int), (*orig_ts_release)(void *, int, int);
static int controller_touch_blocked(void) {
  static void *(*cur)(void *);
  if (!cur) cur = (void *)so_symbol(port_engine_module(), "_ZNK9MGR_Menus21GetCurrentPlayingMenuEv");
  uint8_t *menu = shop_menu(), *mgr = menus_inst ? menus_inst(0) : NULL;
  if (!mgr || !cur) return 0;
  {
    /* The port's pause menu and Options screen (ui_menus.c) too: the controller held its "highlight"
     * press on the pause menu's focused SHOP button, and the port's own tap on SHOP was ignored
     * (the button follows the first finger). Checked on the current menu itself, since the press
     * comes in the frame the menu opens. */
    void *c = cur(mgr);
    extern int map_ui_active(void);
    if (map_ui_active()) return 1;
    extern int menus_ui_enabled(void);
    if (c && menus_ui_enabled()) {
      const char *n = cls(c);
      void *pp = popup_inst ? popup_inst(0) : NULL;
      extern int menus_ui_mp_enabled(void);
      if ((!strcmp(n, "13Menu_IGM_Main") || !strcmp(n, "16Menu_IGM_Options") ||
           (!strcmp(n, "18Menu_MP_CreateHost") && menus_ui_mp_enabled())) && !(pp && popup_enabled && (popup_enabled(pp) & 1))) {
        static unsigned long m;
        if (m++ < 6) debugPrintf("[menus] controller's simulated touch on %s dropped\n", n);
        return 1;
      }
    }
  }
  if (!enabled || !menu || cur(mgr) != menu) return 0;
  if (!tutorial_done()) return 0;
  void *p = popup_inst ? popup_inst(0) : NULL;
  if (p && popup_enabled && (popup_enabled(p) & 1)) return 0;
  int t = cur_tab(menu), type = *(int32_t *)(menu + 0x44);
  if (t < 0 && type != 8 && type != 0) return 0; /* upgrades view etc.: the engine's own controls */
  static unsigned long n;
  if (n++ < 10) debugPrintf("[shopui] controller's simulated touch on the shop dropped\n");
  return 1;
}
static int ts_down; /* a simulated press that went through: its drag/release always go through too */
static void ts_press_hook(void *ts, int x, int y) {
  if (controller_touch_blocked()) return;
  ts_down = 1;
  orig_ts_press(ts, x, y);
}
static void ts_drag_hook(void *ts, int x, int y) { if (ts_down || !controller_touch_blocked()) orig_ts_drag(ts, x, y); }
static void ts_release_hook(void *ts, int x, int y) {
  if (!ts_down && controller_touch_blocked()) return;
  ts_down = 0;
  orig_ts_release(ts, x, y);
}

int shop_ui_install(so_module *mod) {
  FILE *f = fopen(PORT_ROOT "/old_shop", "r");
  if (f) { fclose(f); debugPrintf("[shopui] off (old_shop marker): the engine's grid as before\n"); return -1; }
#define S(v, n) if (!(*(void **)&v = (void *)so_symbol(mod, n))) { debugPrintf("[shopui] missing %s, off\n", n); return -1; }
  S(menus_inst, "_ZN9SingletonI9MGR_MenusE11GetInstanceEb");
  S(playing_menu, "_ZNK9MGR_Menus21GetCurrentPlayingMenuEv");
  S(get_iobj, "_ZN9BasicPage15GetInterfaceObjEi");
  S(grid_count, "_ZN9LayerGrid17GetBasicPageCountEv");
  S(grid_obj, "_ZN9LayerGrid23GetInterfaceObjectByIdxEi");
  S(grid_center, "_ZN9LayerGrid12CenterScrollEP15InterfaceObject");
  S(grid_set_released, "_ZN9LayerGrid17SetReleasedObjectEP15InterfaceObject");
  S(item_inst, "_ZN9SingletonI7ItemMgrE11GetInstanceEb");
  S(inv_elem, "_ZN7ItemMgr19GetInventoryElementEi");
  S(item_count, "_ZN7ItemMgr12GetItemCountEP7ItemDef");
  S(str_inst, "_ZN9SingletonI9StringMgrE11GetInstanceEb");
  S(get_string, "_ZN9StringMgr9GetStringEi");
  S(is_equipped, "_ZN7ItemDef10IsEquippedEv");
  S(pint_get, "_ZNK12ProtectedIntcviEv");
  S(add_released, "_ZN15ButtonActionMgr17AddReleasedButtonEP11LayerButton");
  S(touch_tab, "_ZN18Menu_IGM_Inventory15TouchOnTableTabEi");
  S(game_inst, "_ZN9SingletonI4GameE11GetInstanceEb");
  S(tut_done, "_ZNK4Game19IsTutorialCompletedEv");
  S(btn_text_id, "_ZN11LayerButton9GetTextIDEv");
  S(btn_marked, "_ZN11LayerButton8IsMarkedEv");
  S(popup_inst, "_ZN9SingletonI5PopupE11GetInstanceEb");
  S(popup_enabled, "_ZN5Popup9IsEnabledEv");
  S(snd_inst, "_ZN9SingletonI8SoundMgrE11GetInstanceEb");
  S(snd_play, "_ZN8SoundMgr14PlaySoundLabelERKN3pig6StringERKNS0_4core9TVector3DIfEEj");
  S(pstr_ctor, "_ZN3pig6StringC2EPKc");
  S(pstr_dtor, "_ZN3pig6StringD2Ev");
#undef S
  (void)level_inst; (void)level_player; (void)player_level;
  /* Page_InventoryButton::Update vtable slot */
  uintptr_t upd = so_symbol(mod, "_ZN20Page_InventoryButton6UpdateEiii");
  uintptr_t vt = so_symbol(mod, "_ZTV20Page_InventoryButton");
  if (!upd || !vt || !mod->image) { debugPrintf("[shopui] card update not found, off\n"); return -1; }
  uintptr_t *slots = (uintptr_t *)(mod->image + (vt - mod->base));
  int n = 0;
  for (int i = 2; i < 40; i++) if (slots[i] == upd) { slots[i] = (uintptr_t)card_update_hook; n++; }
  if (!n) { debugPrintf("[shopui] card update slot not found, off\n"); return -1; }
  orig_card_update = (void *)upd;
  {
    uintptr_t tvt = so_symbol(mod, "_ZTVN17AndroidController18SixGunsTouchSystemE");
    uintptr_t pr = so_symbol(mod, "_ZN17AndroidController18SixGunsTouchSystem5pressEii");
    uintptr_t dr = so_symbol(mod, "_ZN17AndroidController18SixGunsTouchSystem4dragEii");
    uintptr_t rl = so_symbol(mod, "_ZN17AndroidController18SixGunsTouchSystem7releaseEii");
    int k = 0;
    if (tvt && pr && dr && rl) {
      uintptr_t *ts = (uintptr_t *)(mod->image + (tvt - mod->base));
      for (int i = 2; i < 8; i++) {
        if (ts[i] == pr) { ts[i] = (uintptr_t)ts_press_hook; k++; }
        else if (ts[i] == dr) { ts[i] = (uintptr_t)ts_drag_hook; k++; }
        else if (ts[i] == rl) { ts[i] = (uintptr_t)ts_release_hook; k++; }
      }
      orig_ts_press = (void *)pr; orig_ts_drag = (void *)dr; orig_ts_release = (void *)rl;
    }
    debugPrintf("[shopui] controller touch filter: %d of 3 slots\n", k);
  }
  enabled = 1;
  debugPrintf("[shopui] shop list installed (marker old_shop for the engine's grid)\n");
  return 0;
}

int shop_ui_transitional(uint8_t *menu) { return transitional(menu); }

/* ------------------------------------------------------------------ Options: camera speed row
 * Drawn in the empty strip under SHOP / INFO on the Options screen (input: pointer.c; the setting:
 * controller.c camera_level_get/set). Engine units would be nicer, but the screen is fixed at
 * 1280x720 and the strip is empty in every language. */
enum { CAM_BAR_X0 = 700, CAM_BAR_X1 = 1060, CAM_ROW_Y = 640 };
int options_cam_active; /* the row has the D-pad (pointer.c) */

/* Level for a touch at (x, y), or 0 when it is not on the bar. */
int options_cam_level_at(int x, int y) {
  if (y < CAM_ROW_Y - 32 || y > CAM_ROW_Y + 32 || x < CAM_BAR_X0 - 30 || x > CAM_BAR_X1 + 30) return 0;
  float t = (float)(x - CAM_BAR_X0) / (CAM_BAR_X1 - CAM_BAR_X0);
  int l = 1 + (int)(t * 9 + 0.5f);
  return l < 1 ? 1 : l > 10 ? 10 : l;
}

void options_camera_draw(void) {
  extern const char *current_menu_class;
  extern int menus_ui_options_active(void);
  if (!enabled || strcmp(current_menu_class, "16Menu_IGM_Options") || menus_ui_options_active()) return;
  void *p = popup_inst ? popup_inst(0) : NULL;
  if (p && popup_enabled && (popup_enabled(p) & 1)) return;
  if (!gl_setup()) return;
  extern int camera_level_get(void);
  int lvl = camera_level_get();
  ui_begin();
  const float y = CAM_ROW_Y;
  /* label, right-aligned like the screen's own ("Music", "Sound FX" ...) */
  color(0.42f, 0.24f, 0.06f, 1);
  const char *lab = "Camera speed";
  text(445 - text_w(lab, 34), y + 12, lab, 34, 0);
  /* track with 10 ticks */
  color(0.45f, 0.30f, 0.12f, 1);
  rect(CAM_BAR_X0, y - 3, CAM_BAR_X1, y + 3);
  for (int i = 0; i < 10; i++) {
    float x = CAM_BAR_X0 + (CAM_BAR_X1 - CAM_BAR_X0) * i / 9.0f;
    rect(x - 2, y - 10, x + 2, y + 10);
  }
  /* knob: the screen's green slider knob, roughly */
  float kx = CAM_BAR_X0 + (CAM_BAR_X1 - CAM_BAR_X0) * (lvl - 1) / 9.0f;
  color(0.20f, 0.12f, 0.04f, 1); rect(kx - 30, y - 22, kx + 30, y + 22);
  color(0.10f, 0.62f, 0.12f, 1); rect(kx - 27, y - 19, kx + 27, y + 19);
  color(0.45f, 0.85f, 0.35f, 1); rect(kx - 24, y - 16, kx + 24, y - 6);
  /* slow / fast ends and the value */
  color(0.42f, 0.24f, 0.06f, 1);
  text(CAM_BAR_X0 - 20 - text_w("Slow", 22), y + 8, "Slow", 22, 0);
  text(CAM_BAR_X1 + 20, y + 8, "Fast", 22, 0);
  char b[16];
  snprintf(b, sizeof(b), "%d", lvl);
  color(0.42f, 0.24f, 0.06f, 1);
  text(kx - text_w(b, 22) / 2, y + 46, b, 22, 0);
  /* how to change it */
  color(0.42f, 0.24f, 0.06f, 0.85f);
  const char *hint = "\xee\x83\xa6 \xee\x83\xa7"; /* ZL ZR */
  text(462, y + 10, hint, 26, 0);
  if (options_cam_active) { /* selected with the D-pad: the port's gold frame */
    float x0 = CAM_BAR_X0 - 95, x1 = CAM_BAR_X1 + 80, y0 = y - 34, y1 = y + 60;
    color(0.15f, 0.07f, 0.0f, 0.85f); frame_rect(x0 - 3, y0 - 3, x1 + 3, y1 + 3, 11);
    color(1.0f, 0.95f, 0.70f, 1); frame_rect(x0, y0, x1, y1, 6);
    color(1.0f, 0.70f, 0.10f, 1); frame_rect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, 2);
  }
  ui_end();
}
const char *shop_ui_item_name(void *row) { return item_name((row_t *)row); }

/* ------------------------------------------------------------------ shared with ui_menus.c */
int sui_ready(void) { return gl_setup(); }
/* Set up the renderer early (main.c, a few frames after boot): done lazily on the shop's first frame,
 * the ~100 ms setup let the game's old shop show for several frames (owner, hardware 2026-11-14). */
void shop_ui_warmup(void) { gl_setup(); }
void sui_begin(void) { ui_begin(); }
/* Inside a menu's RenderUI in states that draw their UI into an offscreen framebuffer first (the
 * multiplayer menus): draw there, so the copy to the screen carries the port's drawing too. */
void sui_begin_here(void) {
  ui_keep_target = 1;
  ui_begin();
  ui_keep_target = 0;
  static GLint last = -1;
  if (fbo != last) { last = fbo; debugPrintf("[sui] drawing into framebuffer %d, viewport %d,%d %dx%d\n", fbo, vp[0], vp[1], vp[2], vp[3]); }
}
void sui_end(void) { ui_end(); }
void sui_color(float r, float g, float b, float a) { color(r, g, b, a); }
void sui_rect(float x0, float y0, float x1, float y1) { rect(x0, y0, x1, y1); }
void sui_frame(float x0, float y0, float x1, float y1, float t) { frame_rect(x0, y0, x1, y1, t); }
float sui_text(float x, float y, const char *s, int px, float maxw) { return text(x, y, s, px, maxw); }
void sui_text_shadow(float x, float y, const char *s, int px, float maxw) { text_shadow(x, y, s, px, maxw); }
float sui_text_w(const char *s, int px) { return text_w(s, px); }
/* Measure every line, but draw only visible baselines. Long words split on UTF-8 boundaries. */
typedef struct {
  char text[8193];
  unsigned short start[512],len[512];
  unsigned used;
  size_t bytes;
  float width;
  int px,lines;
} wrap_layout;
static wrap_layout wrap_cache[8];
static unsigned wrap_clock;
float sui_wrap(float x,float y,float w,const char *s,int px,float lh,float top,float bottom) {
  if(!s || !*s) return y;
  size_t bytes=strlen(s);
  wrap_layout *layout=NULL;
  if(bytes<=8192) {
    unsigned oldest=~0u;
    for(int i=0;i<8;i++) {
      wrap_layout *c=&wrap_cache[i];
      if(c->used && c->bytes==bytes && c->px==px && c->width==w && !memcmp(c->text,s,bytes+1)) {
        layout=c;wrap_hits++;goto draw_layout;
      }
      if(c->used<oldest) {oldest=c->used;layout=c;}
    }
    layout->bytes=bytes;layout->px=px;layout->width=w;layout->lines=0;
    memcpy(layout->text,s,bytes+1);wrap_builds++;
  }
  const char *p=s;
  while (p && *p) {
    const char *start=p,*space=NULL;
    float width=0;
    while (*p && *p!='\n') {
      const unsigned char *next=(const unsigned char *)p; ui_next_cp(&next);
      char cp[5]={0}; memcpy(cp,p,(const char *)next-p);
      float cw=text_w(cp,px);
      if ((p>start && width+cw>w) || next-(const unsigned char *)start>=511) break;
      if (*p==' ') space=p;
      width+=cw; p=(const char *)next;
    }
    if (*p && *p!='\n' && space) p=space;
    char line[512]; size_t n=p-start; memcpy(line,start,n); line[n]=0;
    if(layout) {
      if(layout->lines>=512) {layout->used=0;layout=NULL;} /* pathological text uses the streaming path */
      else {int nline=layout->lines++;layout->start[nline]=start-s;layout->len[nline]=n;}
    }
    if (y>=top && y<=bottom) text(x,y,line,px,w);
    y+=lh;
    if (*p=='\n') p++; else while(*p==' ') p++;
  }
  if(layout) layout->used=++wrap_clock;
  return y;
draw_layout:
  layout->used=++wrap_clock;
  for(int i=0;i<layout->lines;i++) {
    float baseline=y+i*lh;
    if(baseline<top || baseline>bottom) continue;
    char line[512];size_t n=layout->len[i];memcpy(line,layout->text+layout->start[i],n);line[n]=0;
    text(x,baseline,line,px,w);
  }
  return y+layout->lines*lh;
}

/* ------------------------------------------------------------------ Legendary: league rewards for sale
 * The golden weapons of the Legendary tab are "Most Wanted" league / clan rewards (ItemDef +0x16d,
 * Page_InventoryButton::IsLockByMostWanted: only for players whose clan is on an award list), shown
 * as "Locked" with no way to get them offline. Owner (2026-11-14): sell them as ordinary weapons for
 * 1000 badges. Runs before every shop tab rebuild (shop_tabs.c), so the cards are built from the
 * changed ItemDefs. Marker `keep_league_locks` leaves them as they were. */
void shop_ui_unlock_league_items(void) {
  static int off = -1;
  if (off < 0) { FILE *m = fopen(PORT_ROOT "/keep_league_locks", "r"); off = m != NULL; if (m) fclose(m); }
  if (off || !enabled) return;
  static void (*pint_set)(void *, int);
  if (!pint_set) pint_set = (void *)so_symbol(port_engine_module(), "_ZN12ProtectedIntaSEi");
  uint8_t *im = item_inst ? item_inst(0) : NULL;
  if (!im || !pint_set) return;
  uint8_t *b = *(uint8_t **)(im + 0x328), *e = *(uint8_t **)(im + 0x330);
  static int logged;
  int changed = 0;
  for (uint8_t *it = b; b && e > b && (e - b) / 0x28 < 2000 && it < e; it += 0x28) {
    uint8_t *def = *(uint8_t **)it;
    if (!def) continue;
    int type = *(int32_t *)(def + 0x198);
    uint8_t *ab = *(uint8_t **)(def + 0x178), *ae = *(uint8_t **)(def + 0x180);
    int ach = ab && ae > ab;
    if (!logged && (def[0x16d] || def[0x16e] || def[0x16c] || ach || *(int32_t *)(def + 0x170)))
      debugPrintf("[shopui] lock flags: %s type %d mw %d/%d ach %d/%d vip %d price %d/%d\n",
                  str_inst && get_string ? (get_string(str_inst(0), *(int32_t *)(def + 0xd8)) ?: "?") : "?", type,
                  def[0x16d], def[0x16e], def[0x16c], ach, *(int32_t *)(def + 0x170), pint_get(def + 0x100), pint_get(def + 0x120));
    if (type != 5 || !def[0x16d]) continue; /* weapons marked Most Wanted only */
    def[0x16d] = 0;
    def[0x16e] = 0;
    pint_set(def + 0x100, 0);    /* coins: none */
    pint_set(def + 0x120, 1000); /* badges */
    changed++;
  }
  if (!logged) debugPrintf("[shopui] league-reward weapons for sale at 1000 badges: %d\n", changed);
  logged = 1;
}

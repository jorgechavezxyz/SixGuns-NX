/* On-screen pointer for screens the controller can't drive.
 *
 * Some screens exist only for touch in the Android build: picking a mission
 * on the map, the OK on a new-quest popup, and others. The engine's Android
 * controller support (ControllableMenuSystemList) has no entry for them, and
 * the iOS-era Cursor is stubbed out in this build. So the port draws its own
 * pointer and turns A into a touch where it points:
 *
 *   - On a menu or popup with no controller support (looked up in the
 *     engine's own tables, see has_controller_menu), the pointer appears and
 *     the left stick moves it.
 *   - On any menu, moving the right stick shows it too; it stays until the
 *     menu changes or the D-pad / left stick is used to navigate.
 *   - While it is shown, A presses at the pointer and releases on A release;
 *     moving while holding A drags (scrolls the map, lists).
 *
 * Drawing is a small ring rendered after the engine's frame; every bit of GL
 * state touched is saved and restored, because the engine caches bindings. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>
#include "config.h"
#include "input.h"
#include "input_route.h"
#include "so_util.h"
#include "util.h"

so_module *port_engine_module(void);

/* Touch id for the pointer's taps. Real fingers are 0..9 (main.c masks to 0..15).
 * NOT 9: the engine's own Cursor owns touch id 9 (Cursor::TriggerClick presses
 * and releases id 9, Cursor::Reset cancels it, and every LayerButton release
 * calls Cursor::Reset), so with id 9 the engine cancelled the pointer's taps:
 * the first tap on a button only highlighted it, a second one fired it. */
#define FINGER_DEFAULT 14
/* The mission-complete screen (Menu_Quest, shown outside MGR_Menus' current
 * menu) only reacted to the real touchscreen: it seems to read the first
 * finger only, so the pointer taps there with id 0. */
static int quest_screen_shown(void);
#define FINGER (quest_screen_shown() ? 0 : FINGER_DEFAULT)

typedef void (*touch_fn)(void *env, void *obj, int action, int x, int y, int id);
static touch_fn touch;
static void *touch_env;
enum { T_DOWN = 1, T_UP = 0, T_MOVE = 2 }; /* this game's nativeOnTouch codes, see main.c */

static float px = SCREEN_W / 2, py = SCREEN_H / 2;
static int visible, auto_mode, tapping;
/* The touch id used for the current press: FINGER can change between press and release (a popup over
 * the mission-complete screen), and a release with another id left the engine's touch held. */
static int tap_id = FINGER_DEFAULT;
/* End a press now (screen changed or a comic started while A was still held): otherwise the engine's
 * touch stayed down and the next A press was eaten by the pending release. */
static void release_tap(const char *why) {
  if (!tapping) return;
  tapping = 0;
  if (touch) touch(touch_env, NULL, T_UP, (int)px, (int)py, tap_id);
  debugPrintf("[pointer] press released early (%s)\n", why);
}
static u64 last_move;

/* ---- engine lookups: does the current screen have Android controller support? */
static uint8_t *cms_list;          /* ControllableMenuSystemList::s_instance */
static int (*menu_id)(void);       /* AndroidController::MenuIdentifier::id() */
static void *(*popup_inst)(int);
static int (*popup_enabled)(void *);
static int (*popup_type)(void *);
static void *(*menus_inst)(int);
static void *(*cur_menu)(void *);
static void *(*cursor_inst)(int);  /* Singleton<Cursor>::GetInstance (focus navigation) */

/* libc++ std::map<int, ControllableMenuSystem*>: root at map+8 (the end node's
 * left child); node = {left, right, parent, is_black, pair<int, ptr> at +0x20}. */
static uint8_t *map_get(uint8_t *map, int key) {
  uint8_t *n = *(uint8_t **)(map + 8);
  for (int guard = 0; n && guard < 64; guard++) {
    int k = *(int *)(n + 0x20);
    if (key == k) return *(uint8_t **)(n + 0x28);
    n = *(uint8_t **)(n + (key < k ? 0 : 8));
  }
  return NULL;
}
static int map_has(uint8_t *map, int key) {
  uint8_t *n = *(uint8_t **)(map + 8);
  for (int guard = 0; n && guard < 64; guard++) {
    int k = *(int *)(n + 0x20);
    if (key == k) return *(void **)(n + 0x28) != NULL;
    n = *(uint8_t **)(n + (key < k ? 0 : 8));
  }
  return 0;
}

/* Mirrors DefaultButtonListener::aReleased: a popup is confirmed through the
 * popup map (key Popup::GetType), a menu through the menu map (key
 * MenuIdentifier::id). */
static void *quest_menu_ptr; /* set by quest_screen_shown() */
static int quest_screen_shown(void) {
  void *front = popup_inst ? popup_inst(0) : NULL;
  if (front && popup_enabled && popup_enabled(front)) {quest_menu_ptr=NULL;return 0;}
  static void *(*mgr_inst)(int);
  static void *(*q_menu)(void *);
  static int (*q_shown)(void *);
  static int resolved;
  if (!resolved) {
    resolved = 1;
    so_module *mod = port_engine_module();
    mgr_inst = (void *)so_symbol(mod, "_ZN9SingletonI9MGR_MenusE11GetInstanceEb");
    q_menu = (void *)so_symbol(mod, "_ZN9MGR_Menus12GetQuestMenuEv");
    q_shown = (void *)so_symbol(mod, "_ZNK10Menu_Quest7isShownEv");
  }
  void *mm = mgr_inst ? mgr_inst(0) : NULL;
  void *q = mm && q_menu ? q_menu(mm) : NULL;
  int shown = q && q_shown && (q_shown(q) & 1);
  quest_menu_ptr = shown ? q : NULL;
  return shown;
}

/* The achievement popup (AchivementMgr, flag +8, page +0xe8) has no working
 * controller confirm or visible focus: the port's focus navigation runs there. */
static char *achievement_page(void) {
  static void *(*ach_inst)(int);
  if (!ach_inst) ach_inst = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI13AchivementMgrE11GetInstanceEb");
  uint8_t *am = ach_inst ? ach_inst(0) : NULL;
  if (!am || !am[8]) return NULL;
  /* A generic Popup up at the same time (buying an item that unlocks an achievement: the
   * "Congratulations" popup) is the one on screen; from the title menu's shop the achievement
   * popup isn't even drawn, and the focus sat on its two invisible buttons (owner, 2026-11-13).
   * The achievement popup gets its turn when the Popup is closed. */
  void *p = popup_inst ? popup_inst(0) : NULL;
  if (p && popup_enabled && (popup_enabled(p) & 1)) return NULL;
  return *(char **)(am + 0xe8);
}

/* ---- the game's own D-pad graph around hidden buttons ----
 * Menus navigate an AndroidController::Widget graph (links are boost::weak_ptr
 * {px, pn}: left +0x18, right +0x28, down +0x38, up +0x48; the menu system's
 * current widget at ControllableMenuSystem+0x8). WidgetImp/CompoundWidget keep
 * their button at +0x98. Buttons social.c hides (Settings: Facebook and Google
 * Play) stayed in the graph, so Down from the toggles walked into them and
 * Language/SHOP/INFO could not be reached. Links that lead into hidden widgets
 * are re-pointed (with boost's own weak_ptr assignment) past them, to the first
 * visible widget in the same direction, or to nothing at a dead end. */
static void *(*wp_assign)(void *, const void *);
static int widget_hidden(uint8_t *w) {
  void *ti = (*(void ***)w)[-1];
  const char *cls = ti ? *(const char **)((char *)ti + 8) : "";
  if (!strstr(cls, "9WidgetImpE") && !strstr(cls, "14CompoundWidgetE")) return 0;
  uint8_t *o = *(uint8_t **)(w + 0x98);
  if (!o) return 0;
  return !((*(int (***)(void *))o)[0xb0 / 8](o) & 1);
}
static void reroute_hidden_widgets(uint8_t *cms) {
  if (!wp_assign) wp_assign = (void *)so_symbol(port_engine_module(), "_ZN5boost8weak_ptrIN17AndroidController6WidgetEEaSERKS3_");
  uint8_t *start = cms ? *(uint8_t **)(cms + 0x8) : NULL;
  if (!wp_assign || !start) return;
  static const int link[4] = { 0x18, 0x28, 0x38, 0x48 };
  static const void *empty[2];
  enum { MAXW = 64 };
  uint8_t *seen[MAXW];
  int n = 0, head = 0, fixed = 0;
  seen[n++] = start;
  while (head < n) {
    uint8_t *w = seen[head++];
    for (int d = 0; d < 4; d++) {
      uint8_t *t = *(uint8_t **)(w + link[d]);
      if (t) {
        int known = 0;
        for (int i = 0; i < n && !known; i++) known = seen[i] == t;
        if (!known && n < MAXW) seen[n++] = t;
      }
      if (!t || widget_hidden(w) || !widget_hidden(t)) continue;
      uint8_t *last = t; /* last hidden widget on the way */
      for (int k = 0; k < 16; k++) {
        uint8_t *nx = *(uint8_t **)(last + link[d]);
        if (!nx || !widget_hidden(nx)) break;
        last = nx;
      }
      uint8_t *final = *(uint8_t **)(last + link[d]);
      wp_assign(w + link[d], final && !widget_hidden(final) ? (const void *)(last + link[d]) : (const void *)empty);
      fixed++;
    }
  }
  if (fixed) debugPrintf("[pointer] D-pad graph: %d link(s) re-routed past hidden buttons (%d widgets)\n", fixed, n);
}

/* Settings' own graph (ControllableMenuSystemList::createMenuSystem(Menu_IGM_Options*),
 * widgets in the list's std::map<int, shared_ptr<Widget>> at +0x1100): toggles
 * and sliders are keys 0-7 (two columns, bottom row 7 left / 6 right); the
 * bottom buttons are a chain 0x18 SHOP - 0x17 INFO (plus the hidden Google
 * Play / Facebook 0x19-0x1b), whose "up" points at key 0x14, which is never
 * created, and nothing links down to them: SHOP and INFO were unreachable
 * with the D-pad. Adds 7 <-> SHOP and 6 <-> INFO. */
static uint8_t *map_val_addr(uint8_t *map, int key) {
  uint8_t *n = *(uint8_t **)(map + 8);
  for (int guard = 0; n && guard < 64; guard++) {
    int k = *(int *)(n + 0x20);
    if (key == k) return *(void **)(n + 0x28) ? n + 0x28 : NULL;
    n = *(uint8_t **)(n + (key < k ? 0 : 8));
  }
  return NULL;
}
/* Settings: the game's highlight is a held simulated touch on the focused
 * widget, which only half-lights the toggles. The port draws its gold frame
 * around the focused widget too (Widget vtable: +0x40 x, +0x48 y, +0x50 h,
 * +0x58 w, in screen pixels). */
static int opt_frame_on;
static int death_decline_selected;
static float opt_frame[4];
static void options_focus_frame(uint8_t *cms) {
  opt_frame_on = 0;
  uint8_t *w = cms ? *(uint8_t **)(cms + 0x8) : NULL;
  if (!w) return;
  int (**vt)(void *) = *(int (***)(void *))w;
  int x = vt[0x40 / 8](w), y = vt[0x48 / 8](w), h = vt[0x50 / 8](w), wd = vt[0x58 / 8](w);
  {
    /* Slider (the toggles and volume bars) answers 0 for all of these; its
     * LayerSlideBar is at +0x90 (InterfaceObject vtable: +0x60 x, +0x68 y,
     * +0xc0 width, +0xc8 height, floats, engine units). */
    void *ti = (*(void ***)w)[-1];
    if (ti && !strcmp(*(const char **)((char *)ti + 8), "6Slider") && *(void **)(w + 0x90)) {
      /* Slider::highlight presses at (LayerSlideBar::GetBar(), GetPositionY() + 2):
       * the knob. Frame the Cursor target containing that point (the whole
       * toggle / bar), else a box around the knob. */
      static float (*get_bar)(void *), (*get_py)(void *);
      if (!get_bar) {
        get_bar = (void *)so_symbol(port_engine_module(), "_ZN13LayerSlideBar6GetBarEv");
        get_py = (void *)so_symbol(port_engine_module(), "_ZN13LayerSlideBar12GetPositionYEv");
      }
      void *o = *(void **)(w + 0x90);
      float px0 = get_bar ? get_bar(o) : 0, py0 = get_py ? get_py(o) + 2 : 0;
      x = (int)px0 - 15; y = (int)py0 - 12; wd = 30; h = 24;
      uint8_t *c = cursor_inst ? cursor_inst(0) : NULL;
      float *b = c ? *(float **)(c + 0x60) : NULL, *e = c ? *(float **)(c + 0x68) : NULL;
      float best = 1e30f;
      for (float *r = b; b && e >= b && e - b < 4 * 512 && r < e; r += 4) {
        if (px0 < r[0] || px0 > r[2] || py0 < r[1] || py0 > r[3]) continue;
        float area = (r[2] - r[0]) * (r[3] - r[1]);
        if (area < 16 || area > 20000 || area >= best) continue;
        best = area;
        x = (int)r[0]; y = (int)r[1]; wd = (int)(r[2] - r[0]); h = (int)(r[3] - r[1]);
      }
    }
  }
  static void *logged;
  if (logged != w) {
    logged = w;
    void *ti = (*(void ***)w)[-1];
    debugPrintf("[pointer] native focus %s %d,%d %dx%d\n", ti ? *(const char **)((char *)ti + 8) : "?", x, y, wd, h);
  }
  if (wd <= 0 || h <= 0 || wd > SCREEN_W || h > SCREEN_H) return;
  /* engine units (screen px / (SCREEN_H / 320.0f)) */
  opt_frame[0] = x * (SCREEN_H / 320.0f); opt_frame[1] = y * (SCREEN_H / 320.0f);
  opt_frame[2] = (x + wd) * (SCREEN_H / 320.0f); opt_frame[3] = (y + h) * (SCREEN_H / 320.0f);
  opt_frame_on = 1;
}

static void options_link_bottom(void) {
  if (!cms_list || !wp_assign) return;
  uint8_t *map = cms_list + 0x1100;
  uint8_t *s7 = map_val_addr(map, 7), *s6 = map_val_addr(map, 6);
  uint8_t *shop = map_val_addr(map, 0x18), *info = map_val_addr(map, 0x17);
  if (!s7 || !s6 || !shop || !info) return;
  uint8_t *w7 = *(uint8_t **)s7, *w6 = *(uint8_t **)s6, *ws = *(uint8_t **)shop, *wi = *(uint8_t **)info;
  if (*(void **)(w7 + 0x38) || *(void **)(w6 + 0x38)) return; /* already linked (or the layout changed) */
  /* a shared_ptr and a weak_ptr are both {px, pn}: weak_ptr = shared_ptr */
  wp_assign(w7 + 0x38, shop);
  wp_assign(w6 + 0x38, info);
  wp_assign(ws + 0x48, s7);
  wp_assign(wi + 0x48, s6);
  debugPrintf("[pointer] Settings: D-pad links toggles <-> SHOP / INFO added\n");
}

static int has_controller_menu(void);
int pointer_has_controller_menu_now(void) { return has_controller_menu(); }
static int has_controller_menu(void) {
  if (popup_inst && popup_enabled && popup_type) {
    void *p = popup_inst(0);
    /* Lottery win dialog (Popup type 19, "You WON! / Try again / CLOSE"): listed as controller-ready,
     * but its confirm never fires; A did nothing and the player was stuck in the lottery (owner,
     * hardware 2026-11-14). The port's focus frame over its buttons instead. */
    if (p && popup_enabled(p) && popup_type(p) == 19) return 0;
    if (p && popup_enabled(p)) return cms_list ? map_has(cms_list + 0x1460, popup_type(p)) : 1;
  }
  { extern int map_ui_active(void); if (map_ui_active()) return 1; }
  /* the shop's weapon upgrade view: the engine's controller chain there moved nothing (shop_ui.c) */
  { extern int shop_ui_upgrade_view(void); if (!popup_inst || !popup_enabled || !popup_inst(0) || !(popup_enabled(popup_inst(0)) & 1)) if (shop_ui_upgrade_view()) return 0; }
  /* Port-owned screens already consumed their input. Do not run the old menu-specific patches
   * against a new object while social.c still reports the previous frame's menu class. */
  { extern int menus_ui_mp_active(void), menus_ui_options_active(void), menus_ui_pause_active(void), menus_ui_journal_active(void);
    if (menus_ui_mp_active() || menus_ui_options_active() || menus_ui_pause_active() || menus_ui_journal_active()) return 1; }
  if (achievement_page()) return 0;
  /* Mission complete / quest summary: not the "current" menu, no working
   * controller menu -> the port's focus navigation (A taps the framed NEXT). */
  if (quest_screen_shown()) return 0;
  if (!cms_list || !menu_id) return 1;
  /* Only menus count (not loading, comics or cutscenes, which take A as is). */
  void *mm = menus_inst ? menus_inst(0) : NULL;
  if (!mm || !cur_menu || !cur_menu(mm)) return 1;
  /* Menus the engine lists as controller-ready but whose controller menu
   * doesn't actually select anything: the map's mission card
   * (Menu_SelectQuestIteration, id 24; A did nothing, only touch worked).
   * These get the port's focus navigation instead. */
  {
    extern const char *current_menu_class;
    /* The title menu: its dead online icons, hidden by social.c, stay in its
     * controller chain, so the D-pad landed on invisible targets (the settings
     * cog was hard to reach). The port's focus frame only visits visible ones.
     * (Info keeps the game's own navigation, re-routed past its hidden web
     * links below.) */
    if (strstr(current_menu_class, "Menu_SelectQuestIteration") || strstr(current_menu_class, "Menu_LevelUp") ||
        !strcmp(current_menu_class, "15Menu_MainMenu3D"))
      return 0;
  }
  int id = menu_id();
  if (id < 0) return 0;
  {
    extern const char *current_menu_class;
    if (!strcmp(current_menu_class, "13Menu_IGM_Main") || !strcmp(current_menu_class, "16Menu_IGM_Options") ||
        !strcmp(current_menu_class, "9Menu_Info")) {
      void *mm = menus_inst ? menus_inst(0) : NULL;
      void *m = mm && cur_menu ? cur_menu(mm) : NULL;
      void *ti = m ? (*(void ***)m)[-1] : NULL;
      const char *actual = ti ? *(const char **)((char *)ti + 8) : "";
      if (!actual || strcmp(actual, current_menu_class)) return 1;
    }
    if (!strcmp(current_menu_class, "9Menu_Info")) reroute_hidden_widgets(map_get(cms_list + 0x1448, id));
    extern int menus_ui_pause_active(void);
    if (!strcmp(current_menu_class, "13Menu_IGM_Main") && !menus_ui_pause_active()) {
      /* Pause menu: the game's highlight is a slightly lighter yellow (invisible on the red LOTTERY
       * button), and Up/Down stopped at both ends (owner: "make the navigation a little better").
       * The port's gold frame around the focused button, and the list wraps. */
      uint8_t *cms = map_get(cms_list + 0x1448, id);
      if (!wp_assign) wp_assign = (void *)so_symbol(port_engine_module(), "_ZN5boost8weak_ptrIN17AndroidController6WidgetEEaSERKS3_");
      uint8_t *w = cms ? *(uint8_t **)(cms + 0x8) : NULL;
      if (w && wp_assign) {
        uint8_t *top = w, *bot = w;
        for (int k = 0; k < 16 && *(uint8_t **)(top + 0x48); k++) top = *(uint8_t **)(top + 0x48);
        for (int k = 0; k < 16 && *(uint8_t **)(bot + 0x38); k++) bot = *(uint8_t **)(bot + 0x38);
        uint8_t *top_next = *(uint8_t **)(top + 0x38), *bot_prev = *(uint8_t **)(bot + 0x48);
        /* both walks must end at a real end: once wrapped, the chain is a loop and stays as it is */
        if (!*(uint8_t **)(top + 0x48) && !*(uint8_t **)(bot + 0x38) && top != bot && top_next && bot_prev && *(uint8_t **)(top_next + 0x48) == top && *(uint8_t **)(bot_prev + 0x38) == bot) {
          wp_assign(bot + 0x38, top_next + 0x48); /* down from the last -> the first */
          wp_assign(top + 0x48, bot_prev + 0x38); /* up from the first -> the last */
          debugPrintf("[pointer] pause menu: Up/Down wrap around\n");
        }
      }
      options_focus_frame(cms);
      /* The side columns (free elixirs left, ammo offers right: OII_Button grids, page objects 0x23
       * and 0x25, filled by Menu_IGM_Main::CreateOIIButtons) are hidden: owner wants a streamlined
       * menu, and the D-pad never reached them. A hidden LayerGrid neither draws nor takes touches. */
      {
        static void *(*gio)(void *, int);
        if (!gio) gio = (void *)so_symbol(port_engine_module(), "_ZN9BasicPage15GetInterfaceObjEi");
        uint8_t *mm2 = menus_inst ? menus_inst(0) : NULL;
        uint8_t *pm = mm2 && cur_menu ? cur_menu(mm2) : NULL;
        uint8_t *page = pm ? *(uint8_t **)(pm + 0x8) : NULL;
        static const int side[2] = { 0x23, 0x25 };
        for (int k = 0; k < 2 && page && gio; k++) {
          uint8_t *g = gio(page, side[k]);
          if (!g || !((*(int (***)(void *))g)[0xb0 / 8](g) & 1)) continue;
          (*(void (**)(void *, int))(*(uint8_t **)g + 0xa8))(g, 0);
          static int said;
          if (said++ < 2) debugPrintf("[pointer] pause menu: side column 0x%x hidden\n", side[k]);
        }
      }
    }
    extern int menus_ui_options_active(void);
    if (!strcmp(current_menu_class, "16Menu_IGM_Options") && !menus_ui_options_active()) {
      reroute_hidden_widgets(map_get(cms_list + 0x1448, id)); /* also resolves wp_assign */
      options_link_bottom();
      options_focus_frame(map_get(cms_list + 0x1448, id));
    }
  }
  return map_has(cms_list + 0x1448, id);
}

/* ---- tap trace (debug): for a few frames after each pointer tap, log the
 * engine's touch slots and every button on the current menu that is busy.
 * TouchManager (TouchManager::Update @0xb32548): 10 slots of 0x20 bytes at +0:
 * {int id (-1 free); int state (1 press, 2 drag, 4 release, 8 cancel);
 *  vec2 start; vec2 cur; int action; u8 changed_this_update}.
 * LayerButton (UpdateTouches_Release @0xb44508): +0xa0 state (0 idle, 1 pressed,
 * 3/4 fired), +0xa8 Touch* it follows, +0xb0 that touch's id, +0x148 ms held. */
static void *(*tm_inst)(int);
static unsigned long (*q_size)(void *); /* ustl::queue<Touch>::size() const, queue at TouchManager+0x140 */
static int *(*q_front)(void *);         /* ustl::queue<Touch>::front() -> Touch {int id; int state; ...} */
static int trace_frames;
static int trace_enabled = -1; /* debug_touch_trace marker */
static void trace_touch_state(void) {
  if (trace_enabled < 0) {
    FILE *f = fopen(PORT_ROOT "/debug_touch_trace", "r");
    trace_enabled = f != NULL;
    if (f) fclose(f);
  }
  if (!trace_enabled) { trace_frames = 0; return; }
  if (trace_frames <= 0) return;
  trace_frames--;
  char line[512];
  int n = 0;
  uint8_t *tm = tm_inst ? tm_inst(-1) : NULL;
  if (tm && q_size && q_front) {
    long qn = (long)q_size(tm + 0x140);
    n += snprintf(line + n, sizeof(line) - n, " queue %ld", qn);
    if (qn > 0) {
      int *f = q_front(tm + 0x140);
      n += snprintf(line + n, sizeof(line) - n, " (front id %d st %d)", f[0], f[1]);
    }
  }
  if (tm) {
    for (int i = 0; i < 10; i++) {
      uint8_t *s = tm + i * 0x20;
      if (*(int *)s == -1) continue;
      n += snprintf(line + n, sizeof(line) - n, " slot%d{id %d st %d at %.0f,%.0f ch %d}", i, *(int *)s,
                    *(int *)(s + 4), *(float *)(s + 0x10), *(float *)(s + 0x14), s[0x1c]);
      if (n > (int)sizeof(line) - 80) break;
    }
  }
  void *mm = menus_inst ? menus_inst(0) : NULL;
  void *m = mm && cur_menu ? cur_menu(mm) : NULL;
  if (m) {
    char *page = *(char **)((char *)m + 0x8);
    void **b = page ? *(void ***)(page + 0x68) : NULL, **e = page ? *(void ***)(page + 0x70) : NULL;
    if (b && e >= b && e - b < 1000) {
      for (void **it = b; it < e && n < (int)sizeof(line) - 80; it++) {
        uint8_t *o = *it;
        if (!o) continue;
        void *ti = (*(void ***)o)[-1];
        const char *cls = ti ? *(const char **)((char *)ti + 8) : "";
        if (!strstr(cls, "LayerButton")) continue;
        int st = *(int *)(o + 0xa0);
        void *t = *(void **)(o + 0xa8);
        if (st == 0 && !t) continue;
        n += snprintf(line + n, sizeof(line) - n, " btn#%d{st %d touch %s id %d ms %d}", (int)(it - b), st,
                      t ? "yes" : "no", *(int *)(o + 0xb0), *(int *)(o + 0x148));
      }
    }
  }
  debugPrintf("[ptrace] tapping %d:%s\n", tapping, n ? line : " (nothing)");
}

/* Same trace for a real finger (main.c, on touch down), for comparison. */
void pointer_trace_start(void) { trace_frames = 40; }

void pointer_init(so_module *mod, void *touch_cb, void *env) {
  touch = touch_cb;
  touch_env = env;
  tm_inst = (void *)so_symbol(mod, "_ZN12TouchManager11GetInstanceEi");
  q_size = (void *)so_symbol(mod, "_ZNK4ustl5queueI5TouchE4sizeEv");
  q_front = (void *)so_symbol(mod, "_ZN4ustl5queueI5TouchE5frontEv");
  cms_list = (void *)so_symbol(mod, "_ZN26ControllableMenuSystemList10s_instanceE");
  menu_id = (void *)so_symbol(mod, "_ZN17AndroidController14MenuIdentifier2idEv");
  cursor_inst = (void *)so_symbol(mod, "_ZN9SingletonI6CursorE11GetInstanceEb");
  popup_inst = (void *)so_symbol(mod, "_ZN9SingletonI5PopupE11GetInstanceEb");
  popup_enabled = (void *)so_symbol(mod, "_ZN5Popup9IsEnabledEv");
  popup_type = (void *)so_symbol(mod, "_ZNK5Popup7GetTypeEv");
  menus_inst = (void *)so_symbol(mod, "_ZN9SingletonI9MGR_MenusE11GetInstanceEb");
  cur_menu = (void *)so_symbol(mod, "_ZNK9MGR_Menus21GetCurrentPlayingMenuEv");
  debugPrintf("[pointer] ready (menu tables %s)\n", cms_list && menu_id && popup_type ? "found" : "MISSING");
}

/* ---- focus navigation (screens without the game's own controller support) ----
 * Everything clickable registers its touch rect with the engine's (otherwise
 * stubbed) iOS-era Cursor every frame: LayerButton::UpdateTouches_Release,
 * LayerMap (map icons), LayerSlideBar, Menu_MainMenu3D, the inventory, ... The
 * engine clears that list itself when a menu/popup changes state
 * (Menu_Base::SetState, MGR_Menus::Update, Popup show/hide). So
 * Singleton<Cursor>+0x60/+0x68 (vector<TRect<float>>, {x0,y0,x1,y1}, touch
 * units = screen px / ENGINE_SCALE) is the current screen's list of targets.
 * The D-pad / left stick moves a highlight between them; A taps its centre. */
#define ENGINE_SCALE (SCREEN_H / 320.0f) /* engine touch units: 720 px -> 320 */
enum { MAX_AREAS = 96 };
static float areas[MAX_AREAS][4];
static int n_areas;
static int focus_mode, free_ptr, has_focus, remembered;
static float fcx, fcy, frect[4]; /* focused area: centre + rect (engine units) */

/* Tutorial prompts confirmed with A ("Help the lady!"): the button's touch
 * rect (engine units) for the highlight, and whether the pointer may be
 * summoned there. A on the button (or with no pointer) stays the game's own
 * confirm; only a pointer away from the button taps. */
static int tut_btn;
static float tut_rect[4];
static void read_tutorial_button(void) {
  tut_btn = 0;
  char *t = input_route_tutorial_menu();
  char *page = t ? *(char **)(t + 0x8) : NULL;
  void **pb = page ? *(void ***)(page + 0x68) : NULL, **pe = page ? *(void ***)(page + 0x70) : NULL;
  for (void **it = pb; pb && pe >= pb && pe - pb < 1000 && it < pe; it++) {
    uint8_t *o = *it;
    if (!o) continue;
    void *oti = (*(void ***)o)[-1];
    if (!oti || !strstr(*(const char **)((char *)oti + 8), "LayerButton")) continue;
    if (!((*(int (***)(void *))o)[0xb0 / 8](o) & 1)) continue; /* IsVisible */
    float *r = (float *)(o + 0xc4);
    if (r[2] - r[0] < 4 || r[3] - r[1] < 4) continue;
    memcpy(tut_rect, r, sizeof(tut_rect));
    tut_btn = 1;
    static float logged[4];
    if (memcmp(logged, r, sizeof(logged))) {
      memcpy(logged, r, sizeof(logged));
      debugPrintf("[pointer] tutorial button %.0f,%.0f-%.0f,%.0f (highlighted; A confirms)\n", r[0], r[1], r[2], r[3]);
    }
    return;
  }
}

static int lottery_now; /* pointer_update: 1 + LotteryMgr state while the lottery has the pointer, else 0 */
static void read_areas(void) {
  n_areas = 0;
  /* Shooting lottery, shooting (LotteryMgr states 5 and 6): the nine WANTED posters are 3D models in
   * front of an animated camera. Match LotteryMgr::OnTouchRelease by projecting their live
   * bounding boxes without its per-shot heap allocations. The D-pad picks a poster, A / ZR
   * fires at it (a touch on it, what LotteryMgr::OnTouch* takes as a shot). */
  if (lottery_now == 6 || lottery_now == 7) {
    static void *(*lm_get)(int), *(*cam_get)(int), *(*scene_cam)(void *), *(*bounds)(void *);
    static void (*edges)(void *,float *);
    static void *(*driver_get)(void);
    static int (*project)(void *,float *,const float *);
    if (!lm_get) {
      so_module *mod=port_engine_module();
      driver_get=(void *)so_symbol(mod,"_ZN3pig6System9GetDriverEv");
      lm_get=(void *)so_symbol(mod,"_ZN9SingletonI10LotteryMgrE11GetInstanceEb");
      cam_get=(void *)so_symbol(mod,"_ZN9CameraMgr11GetInstanceEi");
      scene_cam=(void *)so_symbol(mod,"_ZN9CameraMgr14GetSceneCameraEv");
      bounds=(void *)so_symbol(mod,"_ZN3pig5scene10Renderable14GetBoundingBoxEv");
      edges=(void *)so_symbol(mod,"_ZNK3pig4core7TAABB3DIfE8GetEdgesEPNS0_9TVector3DIfEE");
      project=(void *)so_symbol(mod,"_ZNK3pig5scene6Camera7ProjectERNS_4core9TVector3DIfEERKS4_");
    }
    uint8_t *lm=lm_get ? lm_get(0):NULL;
    void *mgr=cam_get ? cam_get(-1):NULL,*camera=mgr && scene_cam ? scene_cam(mgr):NULL;
    void *driver=driver_get ? driver_get():NULL;
    int *viewport=driver ? (*(int *(***)(void *))driver)[0x100/8](driver):NULL;
    float sx=1,sy=1;
    if(viewport && viewport[2]>viewport[0] && viewport[3]>viewport[1]) {
      sx=(float)SCREEN_W/(viewport[2]-viewport[0]);sy=(float)SCREEN_H/(viewport[3]-viewport[1]);
      static int logged; if(!logged++) debugPrintf("[focus] lottery projection viewport %d,%d-%d,%d\n",viewport[0],viewport[1],viewport[2],viewport[3]);
    }
    void **b=lm ? *(void ***)(lm+0x1b0):NULL,**e=lm ? *(void ***)(lm+0x1b8):NULL;
    if(camera && bounds && edges && project && b && e>=b && e-b<=32) {
      for(void **it=b;it<e && n_areas<MAX_AREAS;it++) {
        void *entity=*it;
        if(!entity) continue;
        void *renderable=(*(void *(***)(void *))entity)[0x158/8](entity);
        void *box=renderable ? bounds(renderable):NULL;
        if(!box) continue;
        float corners[24],q[3],x0=1e9f,y0=1e9f,x1=-1e9f,y1=-1e9f;
        edges(box,corners);int valid=1;
        for(int k=0;k<8;k++) {
          if(!(project(camera,q,corners+k*3)&1) || !isfinite(q[0]) || !isfinite(q[1])) {valid=0;break;}
          q[0]=(q[0]-(viewport ? viewport[0]:0))*sx;
          q[1]=(q[1]-(viewport ? viewport[1]:0))*sy;
          x0=fminf(x0,q[0]);y0=fminf(y0,q[1]);x1=fmaxf(x1,q[0]);y1=fmaxf(y1,q[1]);
        }
        if(!valid || x1<=0 || y1<=0 || x0>=SCREEN_W || y0>=SCREEN_H) continue;
        x0=fmaxf(0,x0);y0=fmaxf(0,y0);x1=fminf(SCREEN_W,x1);y1=fminf(SCREEN_H,y1);
        if(x1-x0<4 || y1-y0<4) continue;
        areas[n_areas][0]=x0/ENGINE_SCALE;areas[n_areas][1]=y0/ENGINE_SCALE;
        areas[n_areas][2]=x1/ENGINE_SCALE;areas[n_areas++][3]=y1/ENGINE_SCALE;
      }
    }
    return;
  }
  /* Mission complete (Menu_Quest): the areas its buttons register with the
   * Cursor are ~65 units right of where the buttons are drawn and hit by
   * real touches (focus tapped beside NEXT). Use the buttons' own touch
   * rects instead (LayerButton +0xc4, {x0,y0,x1,y1} in engine units), visible
   * buttons of the quest page (Menu_Base+0x8 -> elements +0x68/+0x70). */
  void *btn_menu = NULL;
  void *front = popup_inst ? popup_inst(0) : NULL;
  int popup_up = front && popup_enabled && popup_enabled(front);
  if (!popup_up && quest_screen_shown() && quest_menu_ptr) btn_menu = quest_menu_ptr;
  else {
    /* Level-up screen ("Level N!" + NEXT): same treatment */
    extern const char *current_menu_class;
    void *mm0 = menus_inst ? menus_inst(0) : NULL;
    if (!popup_up && mm0 && cur_menu && strstr(current_menu_class, "Menu_LevelUp")) btn_menu = cur_menu(mm0);
  }
  /* Diagnostics for the mission-complete screen: intro animation state
   * (Menu_Quest +0x70 done flag, +0x74.. 6 progress floats, +0x8c/+0x90
   * shake) and NEXT's raw touch rect, for the first 40 frames. */
  if (btn_menu && btn_menu == quest_menu_ptr) {
    static int diag, tick;
    if (diag < 8 && tick++ % 30 == 0) {
      diag++;
      float *f = (float *)((char *)btn_menu + 0x74);
      debugPrintf("[questdiag] done %d prog %.2f %.2f %.2f %.2f %.2f %.2f shake %.2f %.2f\n",
                  *(int *)((char *)btn_menu + 0x70), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    }
  }
  char *ach_page = popup_up ? NULL : achievement_page();
  if (btn_menu || ach_page) {
    char *page = ach_page ? ach_page : *(char **)((char *)btn_menu + 0x8);
    void **pb = page ? *(void ***)(page + 0x68) : NULL, **pe = page ? *(void ***)(page + 0x70) : NULL;
    for (void **it = pb; pb && pe >= pb && pe - pb < 1000 && it < pe && n_areas < MAX_AREAS; it++) {
      uint8_t *o = *it;
      if (!o) continue;
      void *oti = (*(void ***)o)[-1];
      if (!oti || !strstr(*(const char **)((char *)oti + 8), "LayerButton")) continue;
      if (!((*(int (***)(void *))o)[0xb0 / 8](o) & 1)) continue; /* IsVisible */
      float *r = (float *)(o + 0xc4);
      if (r[2] - r[0] < 4 || r[3] - r[1] < 4) continue;
      /* On the mission-complete screen this rect alternates every frame between
       * two x positions 65 units apart (seen: 162 and 227 for NEXT); the drawn
       * button and real touches are at the larger one. Keep, per button, the
       * larger x of this frame and the last one, so the frame doesn't flicker
       * and A taps the drawn button. */
      static struct { void *obj; float x0; } last[MAX_AREAS];
      float x0 = r[0], w = r[2] - r[0];
      {
        static int rd;
        if (rd < 6 && btn_menu == quest_menu_ptr) { rd++; debugPrintf("[questdiag] button %p rect %.0f,%.0f-%.0f,%.0f\n", (void *)o, r[0], r[1], r[2], r[3]); }
      }
      int k = n_areas;
      if (last[k].obj == o && last[k].x0 > x0 && last[k].x0 - x0 < 100) x0 = last[k].x0;
      last[k].obj = o;
      last[k].x0 = r[0];
      areas[n_areas][0] = x0; areas[n_areas][1] = r[1]; areas[n_areas][2] = x0 + w; areas[n_areas][3] = r[3];
      n_areas++;
    }
    if (n_areas) return;
  }
  uint8_t *c = cursor_inst ? cursor_inst(0) : NULL;
  if (!c) return;
  float *b = *(float **)(c + 0x60), *e = *(float **)(c + 0x68);
  if (!b || e < b || e - b > 4 * 512) return;
  const float W = SCREEN_W / ENGINE_SCALE, H = SCREEN_H / ENGINE_SCALE;
  /* Buttons the port hides (social.c: the title menu's dead online icons,
   * Info's web links) still register their rects with the Cursor: collect the
   * current menu's invisible buttons and skip targets that are theirs. */
  enum { MAX_HIDDEN = 32 };
  float hidden[MAX_HIDDEN][4];
  int n_hidden = 0;
  {
    void *mm0 = menus_inst ? menus_inst(0) : NULL;
    char *m0 = mm0 && cur_menu ? cur_menu(mm0) : NULL;
    char *page = m0 ? *(char **)(m0 + 0x8) : NULL;
    void **pb = page ? *(void ***)(page + 0x68) : NULL, **pe = page ? *(void ***)(page + 0x70) : NULL;
    for (void **it = pb; pb && pe >= pb && pe - pb < 1000 && it < pe && n_hidden < MAX_HIDDEN; it++) {
      uint8_t *o = *it;
      if (!o) continue;
      void *oti = (*(void ***)o)[-1];
      if (!oti || !strstr(*(const char **)((char *)oti + 8), "LayerButton")) continue;
      if ((*(int (***)(void *))o)[0xb0 / 8](o) & 1) continue; /* visible: keep */
      memcpy(hidden[n_hidden++], o + 0xc4, sizeof(hidden[0]));
    }
  }
  for (float *r = b; r < e && n_areas < MAX_AREAS; r += 4) {
    float x0 = r[0], y0 = r[1], x1 = r[2], y1 = r[3];
    int is_hidden = 0;
    for (int h = 0; h < n_hidden && !is_hidden; h++)
      is_hidden = fabsf(hidden[h][0] - x0) < 2 && fabsf(hidden[h][1] - y0) < 2 && fabsf(hidden[h][2] - x1) < 2 &&
                  fabsf(hidden[h][3] - y1) < 2;
    if (is_hidden) continue;
    if (x1 - x0 < 4 || y1 - y0 < 4) continue;                       /* degenerate */
    { extern int port_top_bar_stats(float, float); if (port_top_bar_stats((x0 + x1) / 2 * ENGINE_SCALE, (y0 + y1) / 2 * ENGINE_SCALE)) continue; }
    if (x1 - x0 > W * 0.85f && y1 - y0 > H * 0.85f) continue;       /* whole-screen default area */
    if (lottery_now == 3 && fabsf((x0 + x1) / 2 - 279) < 40 && fabsf((y0 + y1) / 2 - 292) < 22 && x1 - x0 > 120) {
      static int logged;
      if (!logged++) debugPrintf("[focus] lottery picker: 'Watch a video' (no ads in the port) skipped\n");
      continue;
    }
    {
      /* title EVENTS banner (Gameloft's online events): only ever "Event has not started yet";
       * the D-pad goes SINGLE PLAYER -> MULTIPLAYER (touch still reaches it) */
      extern const char *current_menu_class;
      if (!strcmp(current_menu_class, "15Menu_MainMenu3D") && fabsf(x0 - 403) < 14 && fabsf(y0 - 162) < 14 &&
          fabsf(y1 - 214) < 14)
        continue;
      /* title Gangs (clans, online only; hidden by social.c) */
      if (!strcmp(current_menu_class, "15Menu_MainMenu3D") && fabsf(x0 - 77) < 6 && fabsf(y0 - 30) < 6 && fabsf(x1 - 150) < 6) continue;
    }
    if (x1 < 0 || y1 < 0 || x0 > W || y0 > H) continue;             /* off screen (moved-away buttons) */
    int dup = 0;
    for (int i = 0; i < n_areas && !dup; i++)
      dup = fabsf(areas[i][0] - x0) < 1 && fabsf(areas[i][1] - y0) < 1 && fabsf(areas[i][2] - x1) < 1 &&
            fabsf(areas[i][3] - y1) < 1;
    if (dup) continue;
    areas[n_areas][0] = x0; areas[n_areas][1] = y0; areas[n_areas][2] = x1; areas[n_areas][3] = y1;
    n_areas++;
  }
  {
    /* Multiplayer death screen: the green "Spawn now" (page object 0xc, the free respawn once the wait is
     * over; Menu_MP_DeathScreen::UpdateUI -> Multiplayer::RevivePlayer(0)) never registers with the Cursor,
     * only the badge-priced one (object 9) does, and the game disables that one once the free respawn is
     * open: the focus sat on a dead button and A did nothing (owner, hardware 2026-11-15). While the green
     * button is shown it is the target and the grey one is dropped. */
    extern const char *current_menu_class;
    static void *(*page_obj)(void *, int);
    if (!page_obj) page_obj = (void *)so_symbol(port_engine_module(), "_ZN9BasicPage15GetInterfaceObjEi");
    void *mm2 = menus_inst ? menus_inst(0) : NULL;
    char *m2 = mm2 && cur_menu ? cur_menu(mm2) : NULL;
    if (m2 && page_obj && !strcmp(current_menu_class, "19Menu_MP_DeathScreen")) {
      void *page = *(void **)(m2 + 0x8);
      uint8_t *green = page ? page_obj(page, 0xc) : NULL, *grey = page ? page_obj(page, 9) : NULL;
      if (green && ((*(int (***)(void *))green)[0xb0 / 8](green) & 1)) {
        float *gr = (float *)(green + 0xc4);
        if (gr[2] - gr[0] > 8 && gr[3] - gr[1] > 8) {
          if (grey) { /* drop the disabled priced button */
            float *yr = (float *)(grey + 0xc4);
            for (int i = 0; i < n_areas; i++)
              if (fabsf(areas[i][0] - yr[0]) < 3 && fabsf(areas[i][1] - yr[1]) < 3) { areas[i][0] = areas[i][2] = -1000; }
          }
          if (n_areas < MAX_AREAS) { memcpy(areas[n_areas++], gr, sizeof(areas[0])); }
          static int logged;
          if (!logged++) debugPrintf("[focus] MP death screen: free 'Spawn now' added as target\n");
        }
      }
    }
  }
  {
    /* The shop's weapon upgrade view registers only Home, Back and the Full Upgrade price with the
     * Cursor; its 5 x 4 tier cells (drawn by an inner page, not page buttons) are added here at their
     * fixed layout positions, measured at 1280x720 (px / ENGINE_SCALE) (owner, 2026-11-15). */
    extern int shop_ui_upgrade_view(void);
    if (shop_ui_upgrade_view()) {
      static const float cx0[5] = { 509, 623, 740, 857, 974 }, cy0[4] = { 110, 240, 371, 501 };
      for (int r = 0; r < 4; r++)
        for (int c = 0; c < 5 && n_areas < MAX_AREAS; c++) {
          areas[n_areas][0] = cx0[c] / ENGINE_SCALE; areas[n_areas][1] = cy0[r] / ENGINE_SCALE;
          areas[n_areas][2] = (cx0[c] + 88) / ENGINE_SCALE; areas[n_areas][3] = (cy0[r] + 88) / ENGINE_SCALE;
          n_areas++;
        }
    }
  }
  {
    /* Title with an empty target list (a save made in the lottery: after its restored win dialog
     * closes, Menu_MainMenu3D registers nothing more, only touch works): its known buttons */
    extern const char *current_menu_class;
    void *pp = popup_inst ? popup_inst(0) : NULL;
    if (!n_areas && !strcmp(current_menu_class, "15Menu_MainMenu3D") && !(pp && popup_enabled && (popup_enabled(pp) & 1))) {
      static const float title_btns[][4] = {
        { 376, 110, 568, 144 }, /* SINGLE PLAYER */
        { 376, 228, 568, 264 }, /* MULTIPLAYER */
        { 376, 268, 568, 304 }, /* LOTTERY */
        { 4, 271, 50, 317 },    /* settings */
        { 0, 1, 66, 29 },       /* SHOP */
      };
      static int logged;
      if (!logged++) debugPrintf("[focus] title registered no targets: using its known buttons\n");
      for (size_t i = 0; i < sizeof(title_btns) / sizeof(*title_btns) && n_areas < MAX_AREAS; i++, n_areas++)
        memcpy(areas[n_areas], title_btns[i], sizeof(areas[0]));
    }
    /* the port's graphics button next to the settings gear (gfx_settings.c) */
    extern int gfx_title_button(float r[4]);
    float gr[4];
    if (n_areas < MAX_AREAS && gfx_title_button(gr)) memcpy(areas[n_areas++], gr, sizeof(gr));
  }
  /* Map icons: LayerMap::UpdateTouches only registers them with the Cursor
   * while Cursor::IsPointing(map) is true, and IsPointing is a stub (always 0)
   * in this build. Read LayerMap's own icon list instead: vector at +0xe0/+0xe8
   * of 0x18-byte {float x, float y, ..., MapObject* at +0x10}; the engine's hit
   * box is +-20 around (x,y) (LayerMap::GetRectForObject). */
  void *mm = menus_inst ? menus_inst(0) : NULL;
  void *m = mm && cur_menu ? cur_menu(mm) : NULL;
  if (m) {
    void *ti = (*(void ***)m)[-1];
    const char *cls = ti ? *(const char **)((char *)ti + 8) : "";
    char *page = strcmp(cls, "12Menu_IGM_Map") == 0 ? *(char **)((char *)m + 0x8) : NULL;
    void **pb = page ? *(void ***)(page + 0x68) : NULL, **pe = page ? *(void ***)(page + 0x70) : NULL;
    for (void **it = pb; pb && pe >= pb && pe - pb < 1000 && it < pe; it++) {
      uint8_t *o = *it;
      if (!o) continue;
      void *oti = (*(void ***)o)[-1];
      if (!oti || strcmp(*(const char **)((char *)oti + 8), "8LayerMap") != 0) continue;
      uint8_t *ib = *(uint8_t **)(o + 0xe0), *ie = *(uint8_t **)(o + 0xe8);
      if (!ib || ie < ib || ie - ib > 0x18 * 256) break;
      static long logged_n = -1;
      if ((ie - ib) / 0x18 != logged_n) {
        logged_n = (ie - ib) / 0x18;
        for (uint8_t *e = ib; e < ie; e += 0x18)
          debugPrintf("[focus] map icon %d: %.1f,%.1f (%08x %08x obj %p)\n", (int)((e - ib) / 0x18), *(float *)e,
                      *(float *)(e + 4), *(uint32_t *)(e + 8), *(uint32_t *)(e + 12), *(void **)(e + 0x10));
      }
      for (uint8_t *e = ib; e < ie && n_areas < MAX_AREAS; e += 0x18) {
        float x = *(float *)e, y = *(float *)(e + 4);
        if (x < 0 || y < 0 || x > W || y > H) continue;
        areas[n_areas][0] = x - 20; areas[n_areas][1] = y - 20;
        areas[n_areas][2] = x + 20; areas[n_areas][3] = y + 20;
        n_areas++;
      }
      break;
    }
  }
}

static void set_focus(int i) {
  memcpy(frect, areas[i], sizeof(frect));
  fcx = (frect[0] + frect[2]) / 2;
  fcy = (frect[1] + frect[3]) / 2;
  has_focus = 1;
}

/* Keeps the focus on the same target while the list is rebuilt each frame;
 * picks a new one (nearest to the old spot, or to the screen centre) when the
 * screen changed. */
static int was_popup, was_type = -1, popup_settle; /* popup state seen by the last track_focus call */
static void track_focus(void) {
  if (!n_areas) { has_focus = 0; return; }
  if (!has_focus || frect[3] >= 40) {
    /* only the top-left SHOP / home button so far: the screen's own buttons register a frame later
     * (back on the title from the shop or Options, the frame stayed on SHOP; owner's log 2026-11-14).
     * Also while the focus is on a main button: pressing MULTIPLAYER cleared the title's list for a
     * frame, the focus jumped to SHOP and the title remembered SHOP (owner, hardware 2026-11-15). */
    int all_top = 1;
    for (int i = 0; i < n_areas && all_top; i++) all_top = areas[i][3] < 40;
    if (all_top) return;
  }
  /* A popup that just opened starts fresh (not near where the focus was on the screen below), and on
   * its action button rather than its close "X": buying an item opened e.g. the level-lock popup
   * with the frame on the X (owner, 2026-11-14). */
  int fresh_popup = 0;
  {
    void *p = popup_inst ? popup_inst(0) : NULL;
    int up = p && popup_enabled && (popup_enabled(p) & 1), type = up && popup_type ? popup_type(p) : -1;
    if (up && (!was_popup || type != was_type)) fresh_popup = 1, popup_settle = 40;
    if (!up) popup_settle = 0;
    was_popup = up;
    was_type = type;
  }
  int near_old = (has_focus || remembered) && !fresh_popup;
  {
    /* Each screen remembers its own last focus: a screen seen before starts where it was left
     * (map -> quest card -> map), a new one starts fresh (the Journal opened with the frame on its
     * back button, where the title's focus had been). */
    extern const char *current_menu_class;
    static struct { char cls[48]; float x, y; } mem[12];
    static int nmem;
    static char last_cls[48];
    if (strncmp(last_cls, current_menu_class, sizeof(last_cls) - 1)) {
      int k;
      for (k = 0; k < nmem && strcmp(mem[k].cls, last_cls); k++) {}
      if (last_cls[0] && (has_focus || remembered)) {
        if (k == nmem && nmem < 12) nmem++;
        if (k < 12) { snprintf(mem[k].cls, sizeof(mem[k].cls), "%s", last_cls); mem[k].x = fcx; mem[k].y = fcy; }
      }
      for (k = 0; k < nmem && strcmp(mem[k].cls, current_menu_class); k++) {}
      if (!has_focus && !fresh_popup) {
        if (k < nmem) { fcx = mem[k].x; fcy = mem[k].y; near_old = 1; }
        else near_old = 0;
      }
      snprintf(last_cls, sizeof(last_cls), "%s", current_menu_class);
    }
  }
  remembered = 0;
  /* the popup's own button can register a few frames after the popup (the level-lock popup listed
   * only the X and the shop's home button at first): until the player moves, keep choosing */
  if (popup_settle > 0) popup_settle--;
  if (fresh_popup || popup_settle > 0) {
    int best = -1;
    float bd = 1e30f, tx = SCREEN_W / ENGINE_SCALE / 2, ty = SCREEN_H / ENGINE_SCALE / 2;
    {
      /* The lottery win dialog (type 19) restored on the title by a save made in the lottery: start on
       * CLOSE (its bottom button), not "Try again", which would go into the lottery. */
      extern const char *current_menu_class;
      void *pp = popup_inst ? popup_inst(0) : NULL;
      if (pp && popup_type && popup_type(pp) == 19 && !strcmp(current_menu_class, "15Menu_MainMenu3D")) ty = 1e4f;
    }
    for (int i = 0; i < n_areas; i++) {
      float w = areas[i][2] - areas[i][0], h = areas[i][3] - areas[i][1];
      if (w < 48 && h < 48) continue; /* small square: a close X */
      if (areas[i][3] < 40) continue;   /* the top strip (home / back button behind the popup) */
      float cx = (areas[i][0] + areas[i][2]) / 2, cy = (areas[i][1] + areas[i][3]) / 2;
      float d = (cx - tx) * (cx - tx) + (cy - ty) * (cy - ty);
      if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0 && !fresh_popup && has_focus && fabsf(frect[0] - areas[best][0]) < 0.5f && fabsf(frect[1] - areas[best][1]) < 0.5f) {
      set_focus(best);
      return;
    }
    if (best >= 0) {
      debugPrintf("[focus] popup opened: %d targets, focus on its button %.0f,%.0f-%.0f,%.0f\n", n_areas,
                  areas[best][0], areas[best][1], areas[best][2], areas[best][3]);
      set_focus(best);
      return;
    }
  }
  float tx = near_old ? fcx : SCREEN_W / ENGINE_SCALE / 2, ty = near_old ? fcy : SCREEN_H / ENGINE_SCALE / 2;
  {
    /* the title menu starts on SINGLE PLAYER (right side, upper middle) */
    extern const char *current_menu_class;
    if (!near_old && !strcmp(current_menu_class, "15Menu_MainMenu3D")) {
      tx = SCREEN_W / ENGINE_SCALE * 0.9f;
      ty = SCREEN_H / ENGINE_SCALE * 0.4f;
    }
  }
  int best = 0;
  float bd = 1e30f;
  for (int i = 0; i < n_areas; i++) {
    float cx = (areas[i][0] + areas[i][2]) / 2, cy = (areas[i][1] + areas[i][3]) / 2;
    float d = (cx - tx) * (cx - tx) + (cy - ty) * (cy - ty);
    if (d < bd) { bd = d; best = i; }
  }
  if (!has_focus || (bd > 0.5f && lottery_now != 6 && lottery_now != 7)) debugPrintf("[focus] %d targets, focus %s %.0f,%.0f-%.0f,%.0f\n", n_areas,
                                             has_focus ? "moved to" : "on", areas[best][0], areas[best][1],
                                             areas[best][2], areas[best][3]);
  if (!has_focus) {
    static int dumps;
    for (int i = 0; i < n_areas && dumps < 400; i++, dumps++)
      debugPrintf("[focus]   target %d: %.0f,%.0f-%.0f,%.0f\n", i, areas[i][0], areas[i][1], areas[i][2], areas[i][3]);
  }
  set_focus(best);
}

/* Spatial navigation: the nearest target whose centre lies in the pressed
 * direction (within a 60-degree-ish cone), preferring ones in line. */
static void move_focus(int dx, int dy) {
  popup_settle = 0; /* the player chose */
  int best = -1;
  float bs = 1e30f;
  for (int i = 0; i < n_areas; i++) {
    float cx = (areas[i][0] + areas[i][2]) / 2, cy = (areas[i][1] + areas[i][3]) / 2;
    float ax = cx - fcx, ay = cy - fcy;
    float along = ax * dx + ay * dy, across = fabsf(ax * dy - ay * dx);
    if (along < 2 || across > along * 1.8f) continue;
    float s = along + across * 2.5f;
    if (s < bs) { bs = s; best = i; }
  }
  if (best >= 0) {
    set_focus(best);
    debugPrintf("[focus] -> %.0f,%.0f-%.0f,%.0f\n", frect[0], frect[1], frect[2], frect[3]);
  }
}

static float axis(int v) {
  float f = v / 32767.0f;
  float a = fabsf(f);
  if (a < 0.18f) return 0;
  a = (a - 0.18f) / 0.82f;
  return (f < 0 ? -1 : 1) * a * a; /* fine control near the centre */
}

/* Called once per frame after input_route_update(), before the frame's input
 * is handed to the engine. Takes A (and, in auto mode, the left stick) out of
 * `in` while the pointer is in charge. */
static int force_visible = -1; /* debug_pointer marker: always show (drawing test) */
/* A tap on a HUD button from a D-pad shortcut (controller.c: quest teleport): press now, release a few
 * frames later, on its own finger id so it never mixes with the pointer's press (id 14) or a real finger. */
static int hud_tap_frames;
static float hud_tap_x, hud_tap_y;
enum { HUD_TAP_ID = 13 };
void pointer_tap_screen(float x, float y) {
  if (!touch || hud_tap_frames) return;
  hud_tap_x = x; hud_tap_y = y;
  touch(touch_env, NULL, T_DOWN, (int)x, (int)y, HUD_TAP_ID);
  hud_tap_frames = 4;
  debugPrintf("[pointer] HUD tap at %d,%d\n", (int)x, (int)y);
}
static void hud_tap_frame(void) {
  if (hud_tap_frames && !--hud_tap_frames && touch) touch(touch_env, NULL, T_UP, (int)hud_tap_x, (int)hud_tap_y, HUD_TAP_ID);
}

void pointer_update(InputFrame *in) {
  hud_tap_frame();
  {
    /* A popup closed: its buttons' rects stayed in the Cursor's target list (the lottery win dialog
     * restored on the title by a save made in the lottery: after CLOSE the frame sat on its invisible
     * button, emulator 2026-11-14). Clear the list; live screens register theirs again. */
    static int was_up;
    static void (*clear)(void *);
    if (!clear) clear = (void *)so_symbol(port_engine_module(), "_ZN6Cursor18ClearInteractablesEv");
    void *p = popup_inst ? popup_inst(0) : NULL;
    int up = p && popup_enabled && (popup_enabled(p) & 1);
    if (was_up && !up) {
      void *c = cursor_inst ? cursor_inst(0) : NULL;
      if (c && clear) { clear(c); debugPrintf("[focus] popup closed: target list cleared\n"); }
    }
    was_up = up;
  }
  {
    /* A comic started from the Journal plays while Menu_IGM_Quests is still the current menu: the
     * focus framed the Journal's buttons behind it and its A taps ate the hold-A skip (owner,
     * hardware 2026-11-14). During a comic the pointer steps aside; A is controller.c's skip. */
    extern int cutscene_comic;
    if (cutscene_comic) {
      release_tap("comic");
      if (visible || has_focus) debugPrintf("[pointer] comic playing: pointer and focus off\n");
      visible = 0;
      has_focus = 0;
      return;
    }
  }
  /* A press that carries over a screen change (the "leave match" loading
   * screen closes on A down, then the same held A pressed a title-menu
   * button): when the menu changes while A is held, A is ignored until it is
   * released. */
  {
    extern const char *current_menu_class;
    static const char *last_cls;
    static int swallow_a;
    if (last_cls && current_menu_class != last_cls && strcmp(last_cls, current_menu_class) && (in->held & HidNpadButton_A)) {
      swallow_a = 1;
      debugPrintf("[pointer] A held across a screen change (%s -> %s): ignored until released\n", last_cls, current_menu_class);
    }
    last_cls = current_menu_class;
    if (swallow_a) {
      release_tap("screen change");
      if (in->up & HidNpadButton_A) swallow_a = 0;
      in->down &= ~HidNpadButton_A;
      in->held &= ~HidNpadButton_A;
      in->up &= ~HidNpadButton_A;
    }
  }
  trace_touch_state(); /* what the engine made of last frame's events */
  if (force_visible < 0) {
    FILE *f = fopen(PORT_ROOT "/debug_pointer", "r");
    force_visible = f != NULL;
    if (f) fclose(f);
  }
  read_tutorial_button();
  opt_frame_on = 0; /* set again below while Settings is up */
  {
    static int was_tut;
    if (tut_btn != was_tut) { debugPrintf("[pointer] tutorial button frame %s\n", tut_btn ? "on" : "off"); was_tut = tut_btn; }
  }
  int ui = input_route() == ROUTE_UI;
  /* Shooting lottery (level a07, LotteryMgr): its chest picker (state 2) is drawn by the lottery
   * manager, not a menu, and shooting the posters is a touch on them (LotteryMgr::OnTouch*). Neither
   * had controller support (owner, hardware 2026-11-14). In the lottery level, with no menu or popup
   * up, the pointer is always shown: sticks move it, A or ZR taps (fires) where it is. */
  int lottery = 0;
  {
    static void *(*lm_inst)(int);
    static int (*lm_is_level)(void *), (*lm_state)(void *);
    if (!lm_inst) {
      so_module *m = port_engine_module();
      lm_inst = (void *)so_symbol(m, "_ZN9SingletonI10LotteryMgrE11GetInstanceEb");
      lm_is_level = (void *)so_symbol(m, "_ZNK10LotteryMgr14IsLotteryLevelEv");
      lm_state = (void *)so_symbol(m, "_ZNK10LotteryMgr15GetStateCurrentEv");
    }
    void *lm = lm_inst ? lm_inst(0) : NULL;
    void *mm = menus_inst ? menus_inst(0) : NULL;
    void *p = popup_inst ? popup_inst(0) : NULL;
    int popup_on = p && popup_enabled && (popup_enabled(p) & 1);
    void *cm = mm && cur_menu ? cur_menu(mm) : NULL;
    extern const char *current_menu_class;
    int st = lm && lm_state ? lm_state(lm) : 0;
    /* the chest picker (state 2) comes up while the lottery's loading screen is still the menu */
    int picker = lm && st == 2 && (!cm || strstr(current_menu_class, "Menu_LoadingScreen"));
    int shooting = lm && lm_is_level && (lm_is_level(lm) & 1) && !cm && (st == 5 || st == 6); /* not in the prize cutscenes */
    if ((picker || shooting) && !popup_on) {
      lottery = 1 + (lm_state ? lm_state(lm) : 0); /* 1 + state; state 2 = the chest picker */
      if (in->down & HidNpadButton_ZR) in->down |= HidNpadButton_A; /* ZR fires like A */
      if (in->up & HidNpadButton_ZR) in->up |= HidNpadButton_A;
      if (in->held & HidNpadButton_ZR) in->held |= HidNpadButton_A;
      in->down &= ~HidNpadButton_ZR; in->held &= ~HidNpadButton_ZR; in->up &= ~HidNpadButton_ZR;
    }
    static int was_lottery = -1;
    if (lottery != was_lottery) { debugPrintf("[pointer] lottery mode %d\n", lottery); was_lottery = lottery; }
  }
  int summon_ok = ui || tut_btn || lottery; /* the right stick may bring the pointer up */
  int was_auto = auto_mode;
  auto_mode = (ui && !has_controller_menu()) || lottery;
  lottery_now = lottery;
  if (auto_mode != was_auto) {
    void *mm = menus_inst ? menus_inst(0) : NULL;
    void *m = mm && cur_menu ? cur_menu(mm) : NULL;
    const char *cls = "(no menu)";
    if (m) { void *ti = (*(void ***)m)[-1]; cls = ti ? *(const char **)((char *)ti + 8) : "?"; }
    debugPrintf("[pointer] %s (menu %s, id %d)\n", auto_mode ? "on: screen has no controller support" : "off",
                cls, menu_id ? menu_id() : -9);
  }


  /* Options screen: the port's "Camera speed" row under SHOP / INFO (drawn by shop_ui.c, setting in
   * controller.c). Down from SHOP or INFO selects it (the game's focus stays on that button), Left /
   * Right (D-pad or stick) change it, Up goes back; ZL / ZR change it from anywhere on the screen;
   * a touch on its bar sets it. */
  {
    extern const char *current_menu_class;
    extern int options_cam_active, options_cam_level_at(int, int);
    extern int camera_level_get(void);
    extern void camera_level_set(int);
    static int stick_prev, touch_id = -1;
    void *pp = popup_inst ? popup_inst(0) : NULL;
    int popup_up = pp && popup_enabled && (popup_enabled(pp) & 1);
    extern int menus_ui_options_active(void);
    if (strcmp(current_menu_class, "16Menu_IGM_Options") || popup_up || !cms_list || !menu_id || menus_ui_options_active()) {
      options_cam_active = 0;
      touch_id = -1;
    } else {
      int id = menu_id();
      uint8_t *cms = id >= 0 ? map_get(cms_list + 0x1448, id) : NULL;
      uint8_t *cur = cms ? *(uint8_t **)(cms + 0x8) : NULL;
      uint8_t *ms = map_val_addr(cms_list + 0x1100, 0x18), *mi = map_val_addr(cms_list + 0x1100, 0x17);
      uint8_t *ws = ms ? *(uint8_t **)ms : NULL, *wi = mi ? *(uint8_t **)mi : NULL;
      int sx = in->ls.x > 20000 ? 1 : in->ls.x < -20000 ? -1 : 0, sy = in->ls.y > 20000 ? -1 : in->ls.y < -20000 ? 1 : 0;
      int sdir = abs(in->ls.x) > abs(in->ls.y) ? sx * 2 : sy; /* +-2 horizontal, +-1 vertical */
      int sedge = sdir != stick_prev ? sdir : 0;
      stick_prev = sdir;
      int delta = 0;
      /* the widget focused when Options opened = the top of the right column, which wraps:
       * Down from its last item (Lock V-Pad) led back to it */
      static uint8_t *top_w;
      static const char *was_cls;
      if (was_cls != current_menu_class) { was_cls = current_menu_class; top_w = cur; }
      if (!options_cam_active) {
        /* from SHOP / INFO, from a widget with nothing below it, or where Down would wrap to the top */
        uint8_t *below = cur ? *(uint8_t **)(cur + 0x38) : NULL;
        if (((in->down & HidNpadButton_Down) || sedge == 1) && cur &&
            (cur == ws || cur == wi || !below || (top_w && below == top_w))) {
          options_cam_active = 1;
          debugPrintf("[pointer] Options: camera speed row selected\n");
          in->down &= ~HidNpadButton_Down; in->held &= ~HidNpadButton_Down;
          in->ls.x = in->ls.y = 0;
        }
      } else {
        if ((in->down & HidNpadButton_Up) || sedge == -1) options_cam_active = 0;
        if ((in->down & HidNpadButton_Left) || sedge == -2) delta = -1;
        if ((in->down & HidNpadButton_Right) || sedge == 2) delta = 1;
        const u64 DP = HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_A;
        in->down &= ~DP; in->held &= ~DP;
        in->ls.x = in->ls.y = 0;
      }
      if (in->down & HidNpadButton_ZL) delta = -1;
      if (in->down & HidNpadButton_ZR) delta = 1;
      in->down &= ~(HidNpadButton_ZL | HidNpadButton_ZR);
      in->held &= ~(HidNpadButton_ZL | HidNpadButton_ZR);
      if (delta) camera_level_set(camera_level_get() + delta);
      /* touch on the bar (taken away from the game) */
      int seen = 0;
      for (int i = 0; i < in->ntouch; i++) {
        int lv = options_cam_level_at(in->t[i].x, in->t[i].y);
        if (touch_id < 0 && lv) touch_id = in->t[i].id;
        if (in->t[i].id != touch_id) continue;
        seen = 1;
        int l2 = options_cam_level_at(in->t[i].x < 700 ? 700 : in->t[i].x > 1060 ? 1060 : in->t[i].x, 640);
        if (l2 && l2 != camera_level_get()) camera_level_set(l2);
        in->t[i] = in->t[--in->ntouch];
        i--;
      }
      if (!seen) touch_id = -1;
      if (options_cam_active) opt_frame_on = 0; /* the frame is on the row, not on SHOP / INFO */
    }
  }

  /* Focus navigation (see read_areas): on screens without controller support
   * the D-pad / left stick move a highlight between the screen's own targets;
   * the right stick switches to the free pointer, a direction switches back. */
  if (!auto_mode) {
    /* keep fcx/fcy: coming back to the same screen (e.g. map -> quest card ->
     * map) puts the focus back on the nearest target to where it was */
    if (has_focus) remembered = 1;
    focus_mode = free_ptr = has_focus = 0;
    /* popups that open while the focus is off still count as fresh when it comes back (the
     * level-lock popup started on its X because the buy popup before it had the same type) */
    was_popup = 0;
    was_type = -1;
  }
  else if (!tapping) {
    read_areas();
    if ((axis(in->rs.x) || axis(in->rs.y)) && !free_ptr) {
      free_ptr = 1;
      debugPrintf("[focus] right stick: free pointer\n");
    }
    const u64 DPAD = HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right;
    int dx = 0, dy = 0;
    if (in->down & HidNpadButton_Up) dy = -1;
    else if (in->down & HidNpadButton_Down) dy = 1;
    else if (in->down & HidNpadButton_Left) dx = -1;
    else if (in->down & HidNpadButton_Right) dx = 1;
    { /* left stick as a D-pad: one step, then repeats after 0.4 s every 0.15 s */
      static int prev_sd, held_frames;
      int lx = in->ls.x, ly = in->ls.y, sd = 0;
      if (abs(lx) > 16000 || abs(ly) > 16000) sd = abs(lx) > abs(ly) ? (lx > 0 ? 2 : 4) : (ly > 0 ? 1 : 3);
      held_frames = sd == prev_sd ? held_frames + 1 : 0;
      prev_sd = sd;
      if (sd && !dx && !dy && (held_frames == 0 || (held_frames >= 24 && (held_frames - 24) % 9 == 0))) {
        if (sd == 1) dy = -1; else if (sd == 3) dy = 1; else if (sd == 2) dx = 1; else dx = -1;
      }
    }
    if ((dx || dy) && free_ptr) { free_ptr = 0; debugPrintf("[focus] direction: back to focus\n"); }
    focus_mode = !free_ptr && n_areas > 0;
    if (focus_mode) {
      track_focus();
      if (dx || dy) move_focus(dx, dy);
      px = fcx * ENGINE_SCALE;
      py = fcy * ENGINE_SCALE;
      in->down &= ~DPAD; /* the Android scheme has nothing to navigate here */
      in->held &= ~DPAD;
    }
    if (n_areas > 0) in->ls.x = in->ls.y = 0;
  }

  float sx = axis(in->rs.x), sy = axis(in->rs.y);
  if (auto_mode) { sx += axis(in->ls.x); sy += axis(in->ls.y); }
  u64 now = armGetSystemTick();
  static u64 last_tick; /* frame time for the pointer speed below */
  float dt = last_tick ? armTicksToNs(now - last_tick) / 1e9f : 1.0f / 60.0f;
  if (dt > 1.0f / 15.0f) dt = 1.0f / 15.0f;
  last_tick = now;
  if (summon_ok && (sx || sy)) {
    if (!visible) { visible = 1; }
    /* Speed in px per second (960 = the old 16 px/frame at 60 fps), scaled by
     * the real time since the last frame, so the pointer keeps its speed when
     * the game runs slower (the shop ran at ~40 fps on hardware and the
     * per-frame step made the pointer crawl). Clamped to 1/15 s per step. */
    last_move = now;
    const float speed = 960.0f * dt; /* px this frame at full deflection */
    float ox = px, oy = py;
    px += sx * speed;
    py -= sy * speed;
    if (px < 0) px = 0;
    if (px > SCREEN_W - 1) px = SCREEN_W - 1;
    if (py < 0) py = 0;
    if (py > SCREEN_H - 1) py = SCREEN_H - 1;
    if (tapping && touch && ((int)px != (int)ox || (int)py != (int)oy))
      touch(touch_env, NULL, T_MOVE, (int)px, (int)py, tap_id);
  }
  /* A pointer summoned with the right stick stays until the menu changes or
   * the player navigates the menu's own way (D-pad / left stick). */
  {
    static void *last_menu;
    void *mm = menus_inst ? menus_inst(0) : NULL;
    void *m = mm && cur_menu ? cur_menu(mm) : NULL;
    int nav = (in->down & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right)) ||
              (!auto_mode && (axis(in->ls.x) || axis(in->ls.y)));
    if (!auto_mode && visible && !tapping && (m != last_menu || nav)) visible = 0;
    /* ...and only while a menu or popup is up: comics and cutscenes count as UI
     * but have no menu, and a pointer left over from the quest popup kept
     * taking A there instead of the comic's own skip (2026-11-11). */
    if (!auto_mode && visible && !tapping && !m) {
      void *p = popup_inst ? popup_inst(0) : NULL;
      /* The achievement popup (AchivementMgr, flag at +8) isn't the generic
       * Popup; without this the pointer could not be summoned on it. */
      static void *(*ach_inst)(int);
      if (!ach_inst) ach_inst = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI13AchivementMgrE11GetInstanceEb");
      uint8_t *am = ach_inst ? ach_inst(0) : NULL;
      if (!(p && popup_enabled && popup_enabled(p)) && !(am && am[8]) && !tut_btn) visible = 0;
    }
    last_menu = m;
  }
  (void)now;
  if (auto_mode) visible = 1;
  if (!summon_ok && !tapping) visible = 0;
  if (tut_btn && visible) in->rs.x = in->rs.y = 0; /* the stick moves the pointer, not the camera */
  if (force_visible) visible = 1;
  /* The shop's list (shop_ui.c) has the controls and its own touch handling: no pointer there
   * (one left over from a popup over the shop stayed, since the list takes the D-pad that
   * would otherwise dismiss it). */
  { extern int shop_ui_list_active(void); if (shop_ui_list_active() && !tapping) visible = 0; }
  {
    static int was_visible;
    if (visible != was_visible) {
      void *mm = menus_inst ? menus_inst(0) : NULL;
      void *m = mm && cur_menu ? cur_menu(mm) : NULL;
      const char *cls = "(no menu)";
      if (m) { void *ti = (*(void ***)m)[-1]; cls = ti ? *(const char **)((char *)ti + 8) : "?"; }
      debugPrintf("[pointer] %s (%s, menu %s, route %d)\n", visible ? "shown" : "hidden",
                  auto_mode ? "auto" : "summoned", cls, input_route());
      was_visible = visible;
    }
  }

  if (auto_mode) { in->ls.x = in->ls.y = 0; } /* the stick moves the pointer, not the menu */

  /* A popup with the game's own controller confirm (e.g. "You need to complete
   * the single-player tutorial first" from SHOP on the title menu) ignores
   * pointer taps on its OK. A there is the game's confirm, not a tap: the
   * pointer steps aside and A goes through the Android scheme (main.c). */
  if (visible && !tapping && (in->down & HidNpadButton_A) && tut_btn) {
    float ex = px / ENGINE_SCALE, ey = py / ENGINE_SCALE;
    if (ex >= tut_rect[0] - 8 && ex <= tut_rect[2] + 8 && ey >= tut_rect[1] - 8 && ey <= tut_rect[3] + 8) {
      visible = 0;
      debugPrintf("[pointer] A on the tutorial button: game's confirm\n");
      return;
    }
  }
  /* Death screen (Popup::ShowResurrectPopup, type 4): the game's controller menu covers the four
   * elixir/gun offers only; "Just die!" (popup page object 0x37) could not be reached without touch
   * (owner, hardware 2026-11-14). Down (D-pad or stick) selects it with the port's gold frame, A
   * presses it (a touch on its centre), Up goes back to the game's row. */
  {
    static int jd, stick_prev,was_death;
    static void *(*gio)(void *, int);
    if (!gio) gio = (void *)so_symbol(port_engine_module(), "_ZN9BasicPage15GetInterfaceObjEi");
    uint8_t *p = popup_inst ? popup_inst(0) : NULL, *btn = NULL;
    if (p && popup_enabled && (popup_enabled(p) & 1) && popup_type && popup_type(p) == 4 && gio) {
      uint8_t *page = *(uint8_t **)(p + 0x58);
      btn = page ? gio(page, 0x37) : NULL;
      if (btn && !((*(int (***)(void *))btn)[0xb0 / 8](btn) & 1)) btn = NULL;
    }
    if (!btn) {jd=stick_prev=was_death=0;}
    else {
      int sy = in->ls.y > 20000 ? -1 : in->ls.y < -20000 ? 1 : 0; /* -1 up, 1 down */
      if(!was_death) {stick_prev=sy;was_death=1;} /* a stick held during death is not a new selection */
      int sedge = sy != stick_prev ? sy : 0;
      stick_prev = sy;
      if ((in->down & HidNpadButton_Down) || sedge == 1) {
        if (!jd) debugPrintf("[pointer] death screen: Just die! selected\n");
        jd = 1;
      } else if (jd && ((in->down & HidNpadButton_Up) || sedge == -1)) {
        jd = 0;
        in->down &= ~HidNpadButton_Up; in->held &= ~HidNpadButton_Up;
      }
      in->down &= ~HidNpadButton_Down; in->held &= ~HidNpadButton_Down;
      if (sy) in->ls.y = 0;
      if (jd) {
        if(!tapping) visible=0;
        float *r = (float *)(btn + 0xc4);
        opt_frame[0] = r[0] * ENGINE_SCALE; opt_frame[1] = r[1] * ENGINE_SCALE;
        opt_frame[2] = r[2] * ENGINE_SCALE; opt_frame[3] = r[3] * ENGINE_SCALE;
        opt_frame_on = 1;
        in->down &= ~(HidNpadButton_Left | HidNpadButton_Right);
        in->held &= ~(HidNpadButton_Left | HidNpadButton_Right);
        in->ls.x = 0;
        if ((in->down & HidNpadButton_A) && !tapping) {
          px = (r[0] + r[2]) / 2 * ENGINE_SCALE;
          py = (r[1] + r[3]) / 2 * ENGINE_SCALE;
          tapping = 1; tap_id = FINGER;
          if (touch) touch(touch_env, NULL, T_DOWN, (int)px, (int)py, tap_id);
          debugPrintf("[pointer] death screen: A on Just die! (tap at %d,%d)\n", (int)px, (int)py);
          in->down &= ~HidNpadButton_A;
          in->held &= ~HidNpadButton_A;
        }
      }
    }
    death_decline_selected=jd;
  }
  if (visible && !tapping && (in->down & HidNpadButton_A) && popup_inst && popup_enabled && popup_type && cms_list) {
    void *p = popup_inst(0);
    if (p && popup_enabled(p) && popup_type(p) != 19 && map_has(cms_list + 0x1460, popup_type(p))) {
      visible = 0;
      debugPrintf("[pointer] A on a controller popup (type %d): game's confirm, pointer hidden\n", popup_type(p));
      return;
    }
  }

  extern int port_top_bar_stats(float, float);
  {
    /* Loading screen (the port draws only its backdrop and prompt): A is the game's "continue"; a
     * pointer left up from the right stick turned it into a tap where the hidden tip / elixir offer
     * buttons still are, and nothing happened (owner, hardware 2026-11-14). The lottery's chest
     * picker, drawn while the loading screen is current, keeps the pointer. */
    extern const char *current_menu_class;
    extern int menus_ui_enabled(void);
    if (visible && !lottery_now && menus_ui_enabled() && strstr(current_menu_class, "Menu_LoadingScreen")) {
      visible = 0;
      debugPrintf("[pointer] hidden on the loading screen (A continues)\n");
    }
  }
  if (visible && (in->down & HidNpadButton_A) && !tapping && !port_top_bar_stats(px, py)) {
    tapping = 1; tap_id = FINGER;
    if (trace_frames <= 0) { trace_frames = 1; trace_touch_state(); } /* state before the press */
    if (touch) touch(touch_env, NULL, T_DOWN, (int)px, (int)py, tap_id);
    debugPrintf("[pointer] tap at %d,%d\n", (int)px, (int)py);
    trace_frames = 40;
    in->down &= ~HidNpadButton_A;
    in->held &= ~HidNpadButton_A;
  } else if (tapping) {
    in->down &= ~HidNpadButton_A;
    in->held &= ~HidNpadButton_A;
    if (in->up & HidNpadButton_A) {
      tapping = 0;
      if (touch) touch(touch_env, NULL, T_UP, (int)px, (int)py, tap_id);
      debugPrintf("[pointer] release at %d,%d\n", (int)px, (int)py);
    }
  }
  if (tapping || (visible && (in->up & HidNpadButton_A))) in->up &= ~HidNpadButton_A;
}

/* ---- drawing ---- */
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
  typeof(&glGetProgramiv) GetProgramiv;
} G;
static GLuint prog;
static GLint u_screen, u_color;
static int gl_state; /* 0 untried, 1 ok, -1 failed */

static int gl_setup(void) {
  if (gl_state) return gl_state > 0;
  gl_state = -1;
#define L(n) if (!(G.n = (void *)eglGetProcAddress("gl" #n))) { debugPrintf("[pointer] no gl" #n "\n"); return 0; }
  L(GetIntegerv) L(IsEnabled) L(Enable) L(Disable) L(BlendFuncSeparate) L(BlendEquationSeparate) L(ColorMask)
  L(GetBooleanv) L(UseProgram) L(BindBuffer) L(BindFramebuffer) L(Viewport) L(GetVertexAttribiv)
  L(GetVertexAttribPointerv) L(VertexAttribPointer) L(EnableVertexAttribArray) L(DisableVertexAttribArray)
  L(DrawArrays) L(CreateShader) L(ShaderSource) L(CompileShader) L(CreateProgram) L(AttachShader)
  L(BindAttribLocation) L(LinkProgram) L(GetUniformLocation) L(Uniform2f) L(Uniform4f) L(GetProgramiv)
#undef L
  static const char *vs =
      "attribute vec2 a_pos; uniform vec2 u_screen;\n"
      "void main() { gl_Position = vec4(a_pos.x / u_screen.x * 2.0 - 1.0, 1.0 - a_pos.y / u_screen.y * 2.0, 0.0, 1.0); }\n";
  static const char *fs = "precision mediump float; uniform vec4 u_color; void main() { gl_FragColor = u_color; }\n";
  GLint prev_prog = 0;
  G.GetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  GLuint v = G.CreateShader(GL_VERTEX_SHADER), f = G.CreateShader(GL_FRAGMENT_SHADER);
  G.ShaderSource(v, 1, &vs, NULL); G.CompileShader(v);
  G.ShaderSource(f, 1, &fs, NULL); G.CompileShader(f);
  prog = G.CreateProgram();
  G.AttachShader(prog, v); G.AttachShader(prog, f);
  G.BindAttribLocation(prog, 0, "a_pos");
  G.LinkProgram(prog);
  GLint ok = 0;
  G.GetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) { debugPrintf("[pointer] shader link failed\n"); return 0; }
  u_screen = G.GetUniformLocation(prog, "u_screen");
  u_color = G.GetUniformLocation(prog, "u_color");
  G.UseProgram(prev_prog);
  gl_state = 1;
  return 1;
}

static void ring(float cx, float cy, float r0, float r1, float r, float g, float b, float a) {
  enum { SEG = 32 };
  static GLfloat v[(SEG + 1) * 4];
  for (int i = 0; i <= SEG; i++) {
    float t = i * 6.2831853f / SEG, c = cosf(t), s = sinf(t);
    v[i * 4 + 0] = cx + c * r1; v[i * 4 + 1] = cy + s * r1;
    v[i * 4 + 2] = cx + c * r0; v[i * 4 + 3] = cy + s * r0;
  }
  G.Uniform4f(u_color, r, g, b, a);
  G.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
  G.DrawArrays(GL_TRIANGLE_STRIP, 0, (SEG + 1) * 2);
}

static void quad(float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
  GLfloat v[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
  G.Uniform4f(u_color, r, g, b, a);
  G.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
  G.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Rectangle outline, `t` px thick, inside x0,y0-x1,y1. */
static void frame(float x0, float y0, float x1, float y1, float t, float r, float g, float b, float a) {
  quad(x0, y0, x1, y0 + t, r, g, b, a);
  quad(x0, y1 - t, x1, y1, r, g, b, a);
  quad(x0, y0 + t, x0 + t, y1 - t, r, g, b, a);
  quad(x1 - t, y0 + t, x1, y1 - t, r, g, b, a);
}

/* Called after the engine has drawn the frame, before the buffer swap. */
void pointer_draw(void) {
  { extern int gfx_menu_open(void); if (gfx_menu_open()) return; } /* the graphics menu covers the screen */
  { extern int map_ui_active(void); if (map_ui_active()) return; }
  void *front=popup_inst ? popup_inst(0):NULL;
  int front_up=front && popup_enabled && (popup_enabled(front)&1);
  if(front_up && popup_type && popup_type(front)==4 && !death_decline_selected && cms_list) {
    uint8_t *cms=map_get(cms_list+0x1460,4);
    uint8_t *grid=cms ? *(uint8_t **)(cms+8):NULL;
    void *ti=grid ? (*(void ***)grid)[-1]:NULL;
    if(ti && strstr(*(const char **)((char *)ti+8),"GridWidgetImp"))
      options_focus_frame(grid+0xa0); /* selected cell, not the whole offer grid */
  }
  if (!(visible || tut_btn || opt_frame_on) || !gl_setup()) return;
  { extern int menus_ui_mp_active(void), menus_ui_options_active(void), menus_ui_pause_active(void), menus_ui_journal_active(void);
    extern int shop_ui_transitional_now(void);
    if (!front_up && (menus_ui_mp_active() || menus_ui_options_active() || menus_ui_pause_active() || menus_ui_journal_active() ||
        shop_ui_transitional_now())) return; } /* the port's screens have their own focus */
  /* save */
  GLint prog0, abuf, fbo, vp[4], bsrc, bdst, basrc, badst, beq, beqa;
  GLboolean cmask[4];
  G.GetIntegerv(GL_CURRENT_PROGRAM, &prog0);
  G.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
  G.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  G.GetIntegerv(GL_VIEWPORT, vp);
  G.GetIntegerv(GL_BLEND_SRC_RGB, &bsrc); G.GetIntegerv(GL_BLEND_DST_RGB, &bdst);
  G.GetIntegerv(GL_BLEND_SRC_ALPHA, &basrc); G.GetIntegerv(GL_BLEND_DST_ALPHA, &badst);
  G.GetIntegerv(GL_BLEND_EQUATION_RGB, &beq); G.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &beqa);
  G.GetBooleanv(GL_COLOR_WRITEMASK, cmask);
  static const GLenum caps[] = { GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST };
  GLboolean on[5];
  for (int i = 0; i < 5; i++) on[i] = G.IsEnabled(caps[i]);
  enum { MAXA = 16 };
  GLint enabled[MAXA] = {0}, maxa = 0;
  G.GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxa);
  if (maxa > MAXA) maxa = MAXA;
  for (int i = 0; i < maxa; i++) G.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
  GLint a0_size, a0_type, a0_norm, a0_stride, a0_buf;
  void *a0_ptr = NULL;
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a0_size);
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a0_type);
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a0_norm);
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a0_stride);
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a0_buf);
  G.GetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a0_ptr);

  /* draw */
  G.BindFramebuffer(GL_FRAMEBUFFER, 0);
  G.Viewport(0, 0, SCREEN_W, SCREEN_H);
  for (int i = 1; i < 5; i++) G.Disable(caps[i]);
  G.Enable(GL_BLEND);
  G.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  G.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  G.ColorMask(1, 1, 1, 1);
  for (int i = 1; i < maxa; i++) if (enabled[i]) G.DisableVertexAttribArray(i);
  G.EnableVertexAttribArray(0);
  G.BindBuffer(GL_ARRAY_BUFFER, 0);
  G.UseProgram(prog);
  G.Uniform2f(u_screen, SCREEN_W, SCREEN_H);
  if (tut_btn) {
    /* tutorial prompt: gold frame around the button A confirms */
    float x0 = tut_rect[0] * ENGINE_SCALE - 4, y0 = tut_rect[1] * ENGINE_SCALE - 4;
    float x1 = tut_rect[2] * ENGINE_SCALE + 4, y1 = tut_rect[3] * ENGINE_SCALE + 4;
    frame(x0 - 2, y0 - 2, x1 + 2, y1 + 2, 7, 0, 0, 0, 0.55f);
    frame(x0, y0, x1, y1, 4, 1.0f, 0.82f, 0.25f, 1.0f);
  }
  if (opt_frame_on && !visible) {
    /* bold: on the pause menu the game's own highlight is a barely lighter yellow */
    float x0 = opt_frame[0] - 6, y0 = opt_frame[1] - 6, x1 = opt_frame[2] + 6, y1 = opt_frame[3] + 6;
    frame(x0 - 3, y0 - 3, x1 + 3, y1 + 3, 11, 0.15f, 0.07f, 0.0f, 0.85f);
    frame(x0, y0, x1, y1, 6, 1.0f, 0.95f, 0.70f, 1.0f);
    frame(x0 + 2, y0 + 2, x1 - 2, y1 - 2, 2, 1.0f, 0.70f, 0.10f, 1.0f);
  }
  if (!visible) {
  } else if (focus_mode && has_focus) {
    /* focus highlight: a gold frame around the target (screen px), dark outline */
    float x0 = frect[0] * ENGINE_SCALE - 4, y0 = frect[1] * ENGINE_SCALE - 4;
    float x1 = frect[2] * ENGINE_SCALE + 4, y1 = frect[3] * ENGINE_SCALE + 4;
    frame(x0 - 2, y0 - 2, x1 + 2, y1 + 2, 7, 0, 0, 0, 0.55f);
    if (tapping) frame(x0, y0, x1, y1, 4, 1, 1, 1, 1);
    else frame(x0, y0, x1, y1, 4, 1.0f, 0.82f, 0.25f, 1.0f);
  } else {
    ring(px, py, 9, 17, 0, 0, 0, 0.55f);                                  /* shadow */
    if (tapping) ring(px, py, 10, 15, 1.0f, 0.82f, 0.25f, 1.0f);           /* pressed: gold */
    else ring(px, py, 11, 15, 1, 1, 1, 0.95f);
    ring(px, py, 0, 3, 1, 1, 1, 0.95f);                                   /* centre dot */
  }

  /* restore */
  G.BindBuffer(GL_ARRAY_BUFFER, a0_buf);
  G.VertexAttribPointer(0, a0_size, a0_type, a0_norm, a0_stride, a0_ptr);
  G.BindBuffer(GL_ARRAY_BUFFER, abuf);
  if (!enabled[0]) G.DisableVertexAttribArray(0);
  for (int i = 1; i < maxa; i++) if (enabled[i]) G.EnableVertexAttribArray(i);
  G.UseProgram(prog0);
  G.ColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
  G.BlendEquationSeparate(beq, beqa);
  G.BlendFuncSeparate(bsrc, bdst, basrc, badst);
  for (int i = 0; i < 5; i++) { if (on[i]) G.Enable(caps[i]); else G.Disable(caps[i]); }
  G.Viewport(vp[0], vp[1], vp[2], vp[3]);
  G.BindFramebuffer(GL_FRAMEBUFFER, fbo);
}

/* Debug (shop_ui.c buy-all test): what the pointer shows now. 1 = focus frame (rect in engine
 * units), 2 = free pointer, 3 = the bold frame of a menu row (screen px), 0 = nothing. */
int pointer_debug_state(float r[4]) {
  if (visible && focus_mode && has_focus) { memcpy(r, frect, sizeof(frect)); return 1; }
  if (visible) { r[0] = r[2] = px; r[1] = r[3] = py; return 2; }
  if (opt_frame_on) { memcpy(r, opt_frame, sizeof(opt_frame)); return 3; }
  return 0;
}

/* The top bar's stat items (coins, badges, level, health and their "+" buttons) do nothing useful
 * offline but kept taking the controller focus and touches (owner, 2026-11-14). Their area, screen
 * px: right of the SHOP button, left of the weapon switcher, top 64 px. SHOP and the weapon switcher
 * stay usable. */
int port_top_bar_stats(float x, float y) { return y < 64 && x > 230 && x < 930; }

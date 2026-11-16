/* Hides the main menu's dead online buttons (bottom-left icon row):
 *
 *   Menu_MainMenu3D (menu id 28), bottom-left row at layout y~271 (480x320 space):
 *     x=3    gear graphic (#4/#6), settings; touch handled via ui_obj(6/7), kept
 *     x=54   ui_btn(55)  Facebook login        -> ClientSNSInterface login/logout
 *     x=108  ui_btn(58)  Google Play login     -> ClientSNSInterface login/logout
 *     x=158  ui_btn(29)  "G" online hub        -> PushMenu(43)
 *     x=-102 ui_btn(26)  Gameloft Live (off-screen, unused in this build)
 * (Positions from the debug_ui_dump listing.)
 *
 * UpdateUI forces button 58 visible every frame, so that call is patched to
 * pass false; 26 and 55 are only visible because the layout says so, and get
 * hidden each frame while the main menu is the active menu.
 * LayerObject vtable +0xa8 is SetVisible(bool). */
#include <stdint.h>
#include <string.h>
#include "so_util.h"
#include "util.h"

so_module *port_engine_module(void);
#include "input.h"
#include "config.h"
#include <stdio.h>

#define MENU_MAIN3D 28

static void *(*get_main_menu)(int id);
static void *(*get_menus_singleton)(int create);
static void *(*current_menu)(void *mgr);
static void *(*ui_btn)(void *menu, int id);
static void *(*ui_obj)(void *menu, int id);
static int want_dump;
static void *(*gui_instance)(int create);
static void *(*gui_get_level)(void *gui, int level);

/* Debug: list visible items of an in-game HUD level (GameGUI/GUILevel):
 * items[] at +0, count at +0x18, each item a row of int16 params
 * (0 = type, 9 = visible). */
static void dump_hud_level(int level) {
  if (!gui_instance || !gui_get_level) return;
  uint8_t *gui = gui_instance(0);
  if (!gui || !*(void **)(gui + 8)) return; /* level table not created yet */
  uint8_t *lvl = gui_get_level(gui, level);
  if (!lvl || !*(void **)lvl) return;
  int n = *(int *)(lvl + 0x18);
  if (n <= 0 || n > 2000) return;
  int16_t **items = *(int16_t ***)lvl;
  int count = *(int *)(lvl + 0x18);
  debugPrintf("[social] HUD level %d: %d items\n", level, count);
  for (int i = 0; i < count && i < 400; i++) {
    int16_t *it = items[i];
    if (!it || !it[9]) continue;
    debugPrintf("[social]   item %-3d type %d p1 %d p2 %d p3 %d p4 %d p5 %d p6 %d p7 %d p8 %d\n", i, it[0], it[1], it[2],
                it[3], it[4], it[5], it[6], it[7], it[8]);
  }
}
static void *(*get_tutorial_menu)(void *mgr);
static int (*tutorial_get_id)(void *menu);

/* Each icon is a group: RootLayer container, LayerList, LayerButton. Hiding
 * the button alone leaves Gameloft Live's icon drawn by its container, so
 * hide the whole group. */
static const int hidden_objects[] = {
  27, 28, 29,   /* "G" icon (Six Guns & Me / Gameloft online hub), layout x=158 */
  53, 54, 55,   /* Facebook login, layout x=54 */
  56, 57, 58,   /* Google Play Games login, layout x=108 */
};

static float (*btn_x)(void *), (*btn_y)(void *), (*grf_x)(void *), (*grf_y)(void *);
static int (*grf_sprite)(void *), (*grf_frame)(void *);
static void (*grf_set_frame)(void *, int);

/* C++ class name of a polymorphic object, from its vtable's type_info. */
const char *current_menu_class = ""; /* type name of the active menu, "" in gameplay */
/* The shop (weapons/outfits/horses/elixirs tabs). */
int shop_menu_active(void) { return strstr(current_menu_class, "Menu_IGM_Inventory") != NULL; }

static const char *class_name(void *obj) {
  void **vt = *(void ***)obj;
  void *ti = vt[-1];
  const char *n = ti ? *(const char **)((char *)ti + 8) : NULL;
  return n ? n : "?";
}

/* Debug: log every element on a menu's page with its type and position. */
/* "Share" buttons (text id 0x290, e.g. on the level-up screen; the share
 * popup itself is stubbed in tweaks.c, so they only did nothing for a moment)
 * are hidden on whatever menu is up. Menu_LevelUp::UpdateUI identifies its
 * share button by this text id. */
#define TEXT_SHARE 0x290
static int (*btn_text_id)(void *btn);
static void *(*ach_inst)(int);
static int (*ach_popup_on)(void *);
static void (*btn_set_text_id)(void *btn, int id);
static void *(*strmgr_inst)(int);
static const char *(*strmgr_get)(void *, int);
static float (*btn_w)(void *btn);
static void (*btn_set_pos)(void *btn, float x, float y, int, int); /* LayerButton::SetPos(x, y, false, false) */
static void *(*quest_menu)(void *mgr);
static int (*quest_shown)(void *qm);
static void (*orig_quest_render)(void *,int,int);
static void mission_dead_offers(void *menu) {
  /* Dynamically inserted real-money offer, outside the normal Share/Go Home row. */
  if(!ui_obj) return;
  void *paid=*(void **)((char *)menu+0x118);
  if(paid && strstr(class_name(paid),"LayerButton")) {
    (*(void (***)(void *,int))paid)[0xa8/8](paid,0);
    void *list=ui_obj(menu,0x30);
    if(list) (*(void (***)(void *,int))list)[0xa8/8](list,0);
  }
}
static void mission_art_diagnostic(void *menu) {
  static int enabled=-1,tick,visible[4];
  if(enabled<0) {FILE *f=fopen(PORT_ROOT "/debug_quest_layers","r"),*em=f ? fopen(PORT_ROOT "/emulator_mode","r"):NULL;
    enabled=em!=NULL;if(f) fclose(f);if(em) fclose(em);}
  if(!enabled || !*(int *)((char *)menu+0x70)) return;
  void *g[4];for(int i=0;i<4;i++) {g[i]=ui_obj(menu,21+i);if(!g[i]) return;}
  tick++;extern const char *debug_shot_request;
  if(tick==1) for(int i=0;i<4;i++) visible[i]=(*(int (***)(void *))g[i])[0xb0/8](g[i])&1;
  if(tick==1 || tick==31 || tick==61 || tick==91) for(int i=0;i<4;i++) {
    int show=tick==1 ? i%2:tick==31 ? !(i%2):tick==91;
    (*(void (***)(void *,int))g[i])[0xa8/8](g[i],show && visible[i]);
  }
  if(tick==30) debug_shot_request="mission_guns_only";
  if(tick==60) debug_shot_request="mission_masks_only";
  if(tick==90) debug_shot_request="mission_no_guns";
  if(tick==91) enabled=0;
}
static void mission_art_frame(void *menu) {
  static int off=-1;static void (*offset)(void *,float,float);
  if(off<0) {FILE *f=fopen(PORT_ROOT "/old_mission_art","r");off=f!=NULL;if(f) fclose(f);}
  if(off || !ui_obj || !grf_sprite || !grf_frame) return;
  if(!offset) offset=(void *)so_symbol(port_engine_module(),"_ZN10LayerGraph15SetCustomOffsetEff");
  void *silhouette=ui_obj(menu,23),*gun=ui_obj(menu,24);
  if(!offset || !silhouette || !gun || strcmp(class_name(gun),"10LayerGraph") ||
     strcmp(class_name(silhouette),"10LayerGraph") || grf_sprite(gun)!=1 || grf_frame(gun)!=101 ||
     grf_sprite(silhouette)!=1 || grf_frame(silhouette)!=102) return;
  float t=*(float *)((char *)menu+0x88);
  if(t<0) t=0;
  if(t>1) t=1;
  t=t*t*(3-2*t);
  /* Derived from isolated native renders with measure_mission_art.py. Colored gun bounds
   * are centered at 144/1136,360. Hide the separate white cutouts completely (owner request).
   * Slide both colored sprites in during native intro phase six.
   * Native scale/rotation/reward timing/actions are retained. Coordinates are pixels/2.25. */
  (*(void (***)(void *,int))silhouette)[0xa8/8](silhouette,0);
  offset(gun,133.77778f,-133.33333f+(1-t)*320);
  void *left_mask=ui_obj(menu,21),*left_gun=ui_obj(menu,22);
  if(left_gun && left_mask && !strcmp(class_name(left_gun),"10LayerGraph") &&
     !strcmp(class_name(left_mask),"10LayerGraph") && grf_sprite(left_mask)==1 && grf_frame(left_mask)==97 &&
     grf_sprite(left_gun)==1 && grf_frame(left_gun)==96) {
    (*(void (***)(void *,int))left_mask)[0xa8/8](left_mask,0);
    offset(left_gun,-37.33333f,.22222f+(1-t)*320);
  }
  mission_art_diagnostic(menu);
}

/* A button left alone in a row where a Share button was hidden moves to the
 * centre of the pair (level-up and mission-complete screens). Its original x
 * is remembered so this is the same every frame, even if the menu re-lays
 * itself out. */
static struct { void *btn; float orig_x; } moved[16];
/* A new menu or quest page: the remembered buttons may be freed and their addresses reused by other
 * buttons (which would then be moved to a stale position), and a full table stopped the centring for the
 * rest of the session. Forget them; nothing to move back (those buttons are gone). */
static void forget_moved(void) { memset(moved, 0, sizeof(moved)); }
static void centre_partner(void **begin, void **end, void *share) {
  float sx = btn_x(share), sy = btn_y(share), sw = btn_w(share);
  void *partner = NULL;
  int n = 0;
  for (void **it = begin; it < end; it++) {
    void *o = *it;
    if (!o || o == share || !strstr(class_name(o), "LayerButton")) continue;
    int (*is_visible)(void *) = (*(int (***)(void *))o)[0xb0 / 8];
    if (!(is_visible(o) & 1)) continue;
    float y = btn_y(o);
    if (y > sy - 6 && y < sy + 6) { partner = o; n++; }
  }
  if (n != 1) {
    /* Not (or no longer) a lone partner: e.g. the mission-failed screen shows
     * GO HOME first and AUTO-COMPLETE a moment later, and a centred GO HOME
     * then sat on top of it. Put anything we moved in this row back. */
    for (void **it = begin; it < end; it++) {
      void *o = *it;
      if (!o || o == share) continue;
      for (int i = 0; i < 16; i++) {
        if (moved[i].btn != o) continue;
        if (btn_set_pos && btn_x(o) != moved[i].orig_x) btn_set_pos(o, moved[i].orig_x, btn_y(o), 0, 0);
        moved[i].btn = NULL;
        debugPrintf("[social] row has %d buttons now, moved a button back\n", n);
      }
    }
    return;
  }
  int slot = -1;
  for (int i = 0; i < 16; i++) if (moved[i].btn == partner) { slot = i; break; }
  if (slot < 0) {
    for (int i = 0; i < 16; i++) if (!moved[i].btn) { slot = i; break; }
    if (slot < 0) return;
    moved[slot].btn = partner;
    moved[slot].orig_x = btn_x(partner);
    debugPrintf("[social] centring the button next to the hidden Share button\n");
  }
  float px = moved[slot].orig_x, pw = btn_w(partner);
  float left = px < sx ? px : sx, right = px + pw > sx + sw ? px + pw : sx + sw;
  float target = (left + right) / 2 - pw / 2;
  if (btn_x(partner) != target && btn_set_pos) btn_set_pos(partner, target, btn_y(partner), 0, 0);
}

/* "Share" buttons (text id 0x290: Menu_LevelUp and Menu_Quest identify their
 * share button by it; the share popup itself is stubbed in tweaks.c) are
 * hidden on whatever menu or quest screen is up. */
static void hide_share_buttons(void *menu) {
  if (!menu || !btn_text_id || !btn_x || !btn_y || !btn_w) return;
  char *page = *(char **)((char *)menu + 0x8);
  if (!page) return;
  void **begin = *(void ***)(page + 0x68), **end = *(void ***)(page + 0x70);
  if (!begin || end < begin || end - begin > 1000) return;
  for (void **it = begin; it < end; it++) {
    void *o = *it;
    if (!o || !strstr(class_name(o), "LayerButton")) continue;
    if (btn_text_id(o) != TEXT_SHARE) continue;
    int (*is_visible)(void *) = (*(int (***)(void *))o)[0xb0 / 8];
    if (is_visible(o) & 1) {
      (*(void (***)(void *, int))o)[0xa8 / 8](o, 0);
      debugPrintf("[social] hid a Share button on %s\n", class_name(menu));
    }
    centre_partner(begin, end, o);
  }
}
static void quest_render_hook(void *menu,int x,int y) {
  /* Apply after native UpdateUI has advanced the intro and laid out the buttons. */
  mission_dead_offers(menu);mission_art_frame(menu);hide_share_buttons(menu);
  orig_quest_render(menu,x,y);
}

static void dump_page(void *menu) {
  char *page = *(char **)((char *)menu + 0x8);
  if (!page) return; /* between pages during a transition */
  void **begin = *(void ***)(page + 0x68), **end = *(void ***)(page + 0x70);
  if (!begin || end < begin || end - begin > 1000) { debugPrintf("[social] page layout unexpected\n"); return; }
  debugPrintf("[social] page has %d elements\n", (int)(end - begin));
  for (int i = 0; i < end - begin; i++) {
    void *o = begin[i];
    if (!o) continue;
    const char *n = class_name(o);
    float x = -1, y = -1;
    if (strstr(n, "LayerButton") && btn_x) {
      x = btn_x(o); y = btn_y(o);
      float *r = (float *)((char *)o + 0xc4); /* touch rect (engine units = screen px / 2.25) */
      debugPrintf("[social]   #%-3d   touch rect %.1f,%.1f - %.1f,%.1f  state %d type %d\n", i, r[0], r[1], r[2], r[3],
                  *(int *)((char *)o + 0xa0), *(int *)((char *)o + 0x104));
    }
    else if (strstr(n, "LayerGraph") && grf_x) {
      x = grf_x(o); y = grf_y(o);
      if (grf_sprite && grf_frame) { debugPrintf("[social]   #%-3d sprite %d frame %d\n", i, grf_sprite(o), grf_frame(o)); }
    }
    int (*is_visible)(void *) = (*(int (***)(void *))o)[0xb0 / 8]; /* InterfaceObject::IsVisible */
    debugPrintf("[social]   #%-3d %-24s pos %.0f,%.0f %s\n", i, n, x, y, (is_visible(o) & 1) ? "VISIBLE" : "hidden");
  }
}

/* One-instruction patch at an engine offset, applied only if the original
 * instruction matches (protects against a different engine build). */
static int patch_insn(so_module *mod, uintptr_t off, uint32_t expect, uint32_t replace, const char *what) {
  if (!mod->image || off + 4 > mod->image_size) return -1;
  uint32_t *p = (uint32_t *)(mod->image + off);
  if (*p != expect) {
    debugPrintf("[social] not patching %s: found %08x, expected %08x\n", what, *p, expect);
    return -1;
  }
  *p = replace;
  return 0;
}

void social_init(so_module *mod) {
  /* Menu_MainMenu3D::UpdateUI: `orr w1, wzr, #1` before ui_btn(58)->SetVisible(w1) */
  patch_insn(mod, 0xc32030, 0x320003e1, 0x2a1f03e1 /* mov w1, wzr */, "button 58 visibility");

  get_main_menu = (void *)so_symbol(mod, "_ZN9MGR_Menus7GetMenuI15Menu_MainMenu3DEEPT_i");
  get_menus_singleton = (void *)so_symbol(mod, "_ZN9SingletonI9MGR_MenusE11GetInstanceEb");
  current_menu = (void *)so_symbol(mod, "_ZNK9MGR_Menus21GetCurrentPlayingMenuEv");
  ui_btn = (void *)so_symbol(mod, "_ZN9Menu_Base6ui_btnEi");
  btn_x = (void *)so_symbol(mod, "_ZNK11LayerButton7GetPosXEv");
  btn_y = (void *)so_symbol(mod, "_ZNK11LayerButton7GetPosYEv");
  grf_x = (void *)so_symbol(mod, "_ZNK10LayerGraph7GetPosXEv");
  grf_y = (void *)so_symbol(mod, "_ZNK10LayerGraph7GetPosYEv");
  ui_obj = (void *)so_symbol(mod, "_ZN9Menu_Base6ui_objEi");
  btn_text_id = (void *)so_symbol(mod, "_ZN11LayerButton9GetTextIDEv");
  btn_w = (void *)so_symbol(mod, "_ZNK11LayerButton8GetWidthEv");
  btn_set_pos = (void *)so_symbol(mod, "_ZN11LayerButton6SetPosEffbb");
  btn_set_text_id = (void *)so_symbol(mod, "_ZN11LayerButton9SetTextIDEi");
  strmgr_inst = (void *)so_symbol(mod, "_ZN9SingletonI9StringMgrE11GetInstanceEb");
  strmgr_get = (void *)so_symbol(mod, "_ZN9StringMgr9GetStringEi");
  ach_inst = (void *)so_symbol(mod, "_ZN9SingletonI13AchivementMgrE11GetInstanceEb");
  ach_popup_on = (void *)so_symbol(mod, "_ZNK13AchivementMgr14IsEnabledPopupEv");
  quest_menu = (void *)so_symbol(mod, "_ZN9MGR_Menus12GetQuestMenuEv");
  quest_shown = (void *)so_symbol(mod, "_ZNK10Menu_Quest7isShownEv");
  {
    uintptr_t vt=so_symbol(mod,"_ZTV10Menu_Quest"),method=so_symbol(mod,"_ZN10Menu_Quest8RenderUIEii");
    if(vt && method && mod->image && !mod->finalized) {
      uintptr_t *slots=(uintptr_t *)(mod->image+vt-mod->base);
      for(int i=2;i<40;i++) if(slots[i]==method) {
        orig_quest_render=(void *)method;slots[i]=(uintptr_t)quest_render_hook;
        debugPrintf("[social] mission result artwork/layout hook installed\n");break;
      }
    }
  }
  grf_sprite = (void *)so_symbol(mod, "_ZN10LayerGraph11GetSpriteIdEv");
  grf_frame = (void *)so_symbol(mod, "_ZN10LayerGraph8GetFrameEv");
  grf_set_frame = (void *)so_symbol(mod, "_ZN10LayerGraph8SetFrameEi");
  get_tutorial_menu = (void *)so_symbol(mod, "_ZN9MGR_Menus15GetMenuTutorialEv");
  gui_instance = (void *)so_symbol(mod, "_ZN9SingletonI7GameGUIE11GetInstanceEb");
  gui_get_level = (void *)so_symbol(mod, "_ZN7GameGUI8GetLevelEi");
  tutorial_get_id = (void *)so_symbol(mod, "_ZN13Menu_Tutorial11GetTutorialEv");
  FILE *f = fopen(PORT_ROOT "/debug_ui_dump", "r");
  want_dump = f != NULL;
  if (f) fclose(f);
  debugPrintf("[social] main-menu button hiding %s\n",
              get_main_menu && get_menus_singleton && current_menu && ui_btn ? "ready" : "unavailable");
}

/* Per-menu elements to keep hidden while that menu is active, matched by the
 * menu's RTTI class name. Element ids from the debug_ui_dump listing. */
static const struct { const char *menu; int ids[16]; } hide_by_menu[] = {
  /* Settings (gear), verified by screenshot: 101-109 and 82-84 are the Google
   * Play Games icon and its button groups, 75-77 the Facebook button group. */
  { "16Menu_IGM_Options", { 101, 102, 103, 104, 105, 106, 107, 108, 109, 82, 83, 84, 75, 76, 77, -1 } },
  /* Info (Settings -> Info): FORUM, PRIVACY POLICY, TERMS OF USE open dead web
   * pages. Each button is a group of three objects (Menu_Info::UpdateUI matches
   * the presses on 0x16/0x19/0x1d; the controller slots are 0x14/0x17/0x1b). */
  { "9Menu_Info", { 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, -1 } },
  /* Title: Gangs (clans: Gameloft's online service, dead): its button group 0x4a / 0x4c. Owner asked for
   * gangs on the title; it cannot work without the servers, so it is not shown. */
  { "15Menu_MainMenu3D", { 0x4a, 0x4c, -1 } },
};

static void hide_in_active_menu(void *mgr) {
  void *cur = current_menu(mgr);
  if (!cur || !ui_obj) return;
  const char *name = class_name(cur);
  for (size_t m = 0; m < sizeof(hide_by_menu) / sizeof(*hide_by_menu); m++) {
    if (strcmp(name, hide_by_menu[m].menu) != 0) continue;
    for (int i = 0; i < 16 && hide_by_menu[m].ids[i] >= 0; i++) {
      void *o = ui_obj(cur, hide_by_menu[m].ids[i]);
      if (o) (*(void (***)(void *, int))o)[0xa8 / 8](o, 0);
    }
  }
}

/* Pause menu: CUSTOMER CARE -> SHOP. Menu_IGM_Main::UpdateUI handles a released
 * button by its text ID first (switch on LayerButton::GetTextID) and only then
 * by object; Customer Care is matched by object (BasicPage obj 0x21) and opens
 * Gameloft's dead support page. Text ID 0x25b is the shop entry
 * (GetInventoryMenu()->SetInventoryType(0); PushMenu(7), the same as the HUD's
 * SHOP button), so giving the Customer Care button that text ID changes its
 * label (SetTextID refreshes the string) and its action. Re-applied whenever
 * the menu resets its strings. */
enum { PAUSE_CC_OBJ = 0x21, TEXT_SHOP = 0x72 };

/* Info's HELP opened the game's help pages (touch-screen instructions). It
 * becomes CONTROLS: same button and screen (Menu_Help, which lists text IDs
 * 0x2842..0x2876 in a scroll list, skipping "" and " "), with those strings
 * overwritten in place by the Switch controls. Each line goes into the next
 * help string long enough to hold it; the rest are emptied. */
static const char *const controls_lines[] = {
  "CONTROLS", /* the list's title; also the Info button's label (see below) */
  "MOVING",
  "Left stick: move",
  "Right stick: look around",
  "B: roll",
  "R3 (press the right stick): turn around",
  "L3 (press the left stick): take or leave cover",
  "A: interact, talk, get on a horse",
  "D-pad Left: call your horse",
  "FIGHTING",
  "ZL: aim      ZR: shoot",
  "Y: reload      X: next weapon",
  "R (tap): throw a grenade or dynamite",
  "Hold R + D-pad Up / Down: grenade type",
  "Hold R + D-pad Left / Right: change weapon",
  "ELIXIRS",
  "Hold L: show the elixirs",
  "Hold L + D-pad Left / Up / Right: use elixir 1 / 2 / 3",
  "Elixirs are free, each has a 5 minute cooldown",
  "ON THE HORSE",
  "A: spur      D-pad Left: get off",
  "CUTSCENES",
  "Hold A for 5 seconds to skip",
  "MENUS",
  "+: pause menu (its first button opens the shop)",
  "-: map (press - again to close it)",
  "A: confirm      B: back",
  "D-pad or left stick: move the gold frame",
  "Right stick: show the pointer, A taps with it",
  "L / R or ZL / ZR: shop tabs",
  "+ and - together: quit the game",
};
enum { HELP_FIRST = 0x2842, HELP_LAST = 0x2876, HELP_EXTRA = 0x2884, INFO_HELP_OBJ = 0x13, INFO_PRIVACY_OBJ = 0x19 };

static int controls_title_id = -1; /* text ID now holding "CONTROLS" */
static void controls_help_texts(void) {
  static int done;
  if (done || !strmgr_inst || !strmgr_get) return;
  void *sm = strmgr_inst(0);
  if (!sm) return;
  done = 1;
  size_t li = 0, n_lines = sizeof(controls_lines) / sizeof(*controls_lines);
  int written = 0, emptied = 0;
  for (int id = HELP_FIRST; id <= HELP_LAST + 1; id++) {
    int tid = id <= HELP_LAST ? id : HELP_EXTRA;
    char *str = (char *)strmgr_get(sm, tid);
    if (!str) continue;
    size_t cap = strlen(str);
    if (li < n_lines && strlen(controls_lines[li]) <= cap) {
      memcpy(str, controls_lines[li], strlen(controls_lines[li]) + 1);
      if (li == 0) controls_title_id = tid;
      li++;
      written++;
    } else if (cap) {
      str[0] = 0;
      emptied++;
    }
  }
  debugPrintf("[social] Info -> CONTROLS: %d help line(s) rewritten, %d emptied, %d of %d control line(s) placed\n",
              written, emptied, (int)li, (int)n_lines);
}

static void info_controls_button(void *cur) {
  if (!cur || !ui_obj || strcmp(class_name(cur), "9Menu_Info")) return;
  controls_help_texts();
  char *b = ui_obj(cur, INFO_HELP_OBJ);
  if (!b || !strstr(class_name(b), "LayerButton") || !btn_text_id || !btn_set_text_id) return;
  /* The label comes from a text ID (replacing the label pointer instead broke
   * the button's pressed look, which is also its D-pad highlight). The game
   * has no "CONTROLS" string: HELP takes the text ID of the rewritten help
   * list's title line. Menu_Info::UpdateUI matches presses by object, so the
   * button still opens the (rewritten) help screen. */
  int ctrl_id = controls_title_id;
  {
    static int logged;
    if (ctrl_id > 0 && !logged++) debugPrintf("[social] Info: HELP -> CONTROLS (text %d)\n", ctrl_id);
  }
  if (ctrl_id > 0 && btn_text_id(b) != ctrl_id) btn_set_text_id(b, ctrl_id);
}

static void pause_menu_shop_button(void *cur) {
  if (!cur || !ui_obj || !btn_text_id || !btn_set_text_id || strcmp(class_name(cur), "13Menu_IGM_Main")) return;
  void *b = ui_obj(cur, PAUSE_CC_OBJ);
  if (!b || !strstr(class_name(b), "LayerButton")) return;
  int id = btn_text_id(b);
  if (id == TEXT_SHOP) return;
  static int logged;
  if (!logged++ && strmgr_inst && strmgr_get) {
    void *sm = strmgr_inst(0);
    const char *was = sm ? strmgr_get(sm, id) : NULL, *now = sm ? strmgr_get(sm, TEXT_SHOP) : NULL;
    debugPrintf("[social] pause menu: button text %d \"%s\" -> %d \"%s\" (opens the shop)\n", id, was ? was : "?",
                TEXT_SHOP, now ? now : "?");
  }
  btn_set_text_id(b, TEXT_SHOP);
}

/* Achievement popup (AchivementMgr, page at +0xe8; BasicPage elements at
 * +0x68/+0x70): its SHARE button (text 0x290) is relabelled SCREENSHOT. Only
 * the label pointer (LayerButton+0x108) is replaced; the text ID stays 0x290
 * because AchivementMgr::Update dispatches the press by text ID, which ends in
 * Popup::ShowSharePopup -> screenshot.c. */
static void achievement_screenshot_button(void) {
  if (!ach_inst || !ach_popup_on || !btn_text_id) return;
  char *am = ach_inst(0);
  if (!am || !(ach_popup_on(am) & 1)) return;
  char *page = *(char **)(am + 0xe8);
  if (!page) return;
  void **begin = *(void ***)(page + 0x68), **end = *(void ***)(page + 0x70);
  if (!begin || end < begin || end - begin > 1000) return;
  static const char label[] = "SCREENSHOT";
  for (void **it = begin; it < end; it++) {
    char *o = *it;
    if (!o || !strstr(class_name(o), "LayerButton") || btn_text_id(o) != TEXT_SHARE) continue;
    if (o[0x12a]) continue; /* owns its text: ~LayerButton would delete[] our static label */
    if (*(const char **)(o + 0x108) != label) {
      *(const char **)(o + 0x108) = label;
      debugPrintf("[social] achievement popup: SHARE -> SCREENSHOT\n");
    }
  }
}

/* Generic Popup (purchase "congratulations" etc., page at Popup+0x58,
 * enabled byte +0x60): its SHARE (text 0x290) becomes CLOSE; the press is
 * handled in screenshot.c (closes the popup). Label pointer only. */
static void popup_close_button(void) {
  static void *(*pop)(int);
  if (!pop) pop = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI5PopupE11GetInstanceEb");
  char *pp = pop ? pop(0) : NULL;
  if (!pp || !pp[0x60] || !btn_text_id) return;
  char *page = *(char **)(pp + 0x58);
  if (!page) return;
  void **begin = *(void ***)(page + 0x68), **end = *(void ***)(page + 0x70);
  if (!begin || end < begin || end - begin > 1000) return;
  static const char label[] = "CLOSE";
  for (void **it = begin; it < end; it++) {
    char *o = *it;
    if (!o || !strstr(class_name(o), "LayerButton") || btn_text_id(o) != TEXT_SHARE) continue;
    if (o[0x12a]) continue; /* owns its text: ~LayerButton would delete[] our static label */
    if (*(const char **)(o + 0x108) != label) {
      static int logged;
      *(const char **)(o + 0x108) = label;
      if (!logged++) debugPrintf("[social] popup: SHARE -> CLOSE\n");
    }
  }
}

/* GS_GamePlay::SetStatePause(4) (the map: − or D-pad Right) pauses the sound
 * groups m_ambiance / sfx_amb / sfx_3d and the game music and plays the menu
 * track m_map (also the shop's). Only the pause menu's own resume undoes that,
 * so closing the map straight back to gameplay (B / −) left m_map playing.
 * When the map closes to gameplay with the menu track still on, the port
 * stops it and resumes what was paused. */
static void map_music_restore(void) {
  so_module *mod = port_engine_module();
  void *(*snd)(int) = (void *)so_symbol(mod, "_ZN9SingletonI8SoundMgrE11GetInstanceEb");
  void *(*menus)(int) = (void *)so_symbol(mod, "_ZN9SingletonI9MGR_MenusE11GetInstanceEb");
  int (*get_mm)(void *) = (void *)so_symbol(mod, "_ZN9MGR_Menus12GetMenuMusicEv");
  void (*set_mm)(void *, int) = (void *)so_symbol(mod, "_ZN9MGR_Menus12SetMenuMusicEi");
  int (*playing)(void *, int) = (void *)so_symbol(mod, "_ZNK8SoundMgr14IsSoundPlayingEi");
  void (*stop)(void *, int, unsigned) = (void *)so_symbol(mod, "_ZN8SoundMgr9StopSoundEij");
  void (*res_group)(void *, const char *) = (void *)so_symbol(mod, "_ZN8SoundMgr11ResumeGroupEPKc");
  void (*res_music)(void *, int, int) = (void *)so_symbol(mod, "_ZN8SoundMgr11ResumeMusicEbb");
  void *sm = snd ? snd(0) : NULL, *mm = menus ? menus(0) : NULL;
  if (!sm || !mm || !get_mm || !set_mm || !playing || !stop || !res_group || !res_music) {
    debugPrintf("[social] map music: engine functions missing\n");
    return;
  }
  int id = get_mm(mm);
  if (id < 0 || !(playing(sm, id) & 1)) return;
  stop(sm, id, 0);
  set_mm(mm, -1);
  res_group(sm, "m_ambiance");
  res_group(sm, "sfx_amb");
  res_group(sm, "sfx_3d");
  res_music(sm, 1, 1);
  extern const char *current_menu_class;
  debugPrintf("[social] %s closed to gameplay: menu track %d stopped, game sound resumed\n", current_menu_class, id);
}

/* Whether a menu (pause, options, shop, ...) is open, as opposed to gameplay. */
int game_menu_active(void) {
  if (!get_menus_singleton || !current_menu) return 0;
  void *mgr = get_menus_singleton(0);
  return mgr && current_menu(mgr) != NULL;
}

/* Called every frame before the engine updates. */
void social_frame(void) {
  if (!get_main_menu || !get_menus_singleton || !current_menu || !ui_btn) return;
  void *mgr = get_menus_singleton(0);
  if (!mgr) return;

  hide_in_active_menu(mgr);
  hide_share_buttons(current_menu(mgr));
  pause_menu_shop_button(current_menu(mgr));
  info_controls_button(current_menu(mgr));
  achievement_screenshot_button();
  popup_close_button();
  /* The mission-complete screen (Menu_Quest) isn't the "current" menu. */
  if (quest_menu && quest_shown) {
    void *qm = quest_menu(mgr);
    if (qm && quest_shown(qm)) {
      if(!orig_quest_render) {mission_dead_offers(qm);mission_art_frame(qm);hide_share_buttons(qm);}
      static void *last_page;void *page=*(void **)((char *)qm+8);
      if(page!=last_page) {last_page=page;forget_moved();FILE *f=fopen(PORT_ROOT "/debug_ui_dump","r");if(f) {fclose(f);dump_page(qm);}}
    }
  }

  /* Log menu changes (cheap); with debug_ui_dump also dump the new menu. */
  {
    extern const char *current_menu_class;
    static void *last_menu = (void *)1;
    void *cur = current_menu(mgr);
    if (cur != last_menu) {
      /* Any menu closed into gameplay with its menu track still playing (the map; also the shop /
       * saddle bag, whose "m_map" music kept playing in the game: owner, hardware 2026-11-14) */
      if (!cur && last_menu != (void *)1 && current_menu_class[0]) map_music_restore();
      last_menu = cur;
      forget_moved();
      current_menu_class = cur ? class_name(cur) : "";
      debugPrintf("[menu] active menu now %s\n", cur ? class_name(cur) : "(none: gameplay)");
      char ev[128];
      snprintf(ev, sizeof(ev), "menu:%s", cur ? class_name(cur) : "gameplay");
      input_sync(ev);
      if (cur && want_dump) dump_page(cur);
    }
  }

  if (get_tutorial_menu && tutorial_get_id) {
    static int last_id = -2;
    void *tm = get_tutorial_menu(mgr);
    int id = tm ? tutorial_get_id(tm) : -1;
    if (id != last_id) {
      last_id = id;
      {
        char ev[32];
        snprintf(ev, sizeof(ev), "tutorial:%d", id);
        input_sync(ev);
      }
      if (want_dump) {
        debugPrintf("[social] tutorial step now %d\n", id);
        if (tm && id != -1) dump_page(tm);
        if (id != -1) dump_hud_level(0);
      }
      /* Strings may load after startup; retry the text replacement once per step. */
#ifndef STOCK_INPUT
      extern int text_patch_apply(void);
      if (id != -1) text_patch_apply();
#endif
    }
#ifndef STOCK_INPUT
    /* Tutorial steps that teach touch controls. RenderTutorialFrames draws a
     * glowing ring + screen dimming around any graphic with sprite 1 frame
     * 87/88 in the step's range regardless of visibility, so the ring marker's
     * frame is changed; the arrow/icon graphics are hidden. Ids from the
     * debug_ui_dump listing, verified by screenshot. */
    static const struct { int step, ring; int hide[3]; } touch_steps[] = {
      { 0, 5,  { 7, -1, -1 } },   /* "Drag the V-Pad...": arrow */
      { 4, 35, { 36, 37, -1 } },  /* "Touch or hold the Fire icon...": arrow, revolver icon */
    };
    for (size_t k = 0; tm && ui_obj && k < sizeof(touch_steps) / sizeof(*touch_steps); k++) {
      if (id != touch_steps[k].step) continue;
      for (int h = 0; h < 3 && touch_steps[k].hide[h] >= 0; h++) {
        void *o = ui_obj(tm, touch_steps[k].hide[h]);
        if (o) (*(void (***)(void *, int))o)[0xa8 / 8](o, 0);
      }
      void *spot = ui_obj(tm, touch_steps[k].ring);
      if (spot && grf_frame && grf_set_frame) {
        int f = grf_frame(spot);
        if (f == 87 || f == 88) { grf_set_frame(spot, 0); debugPrintf("[social] tutorial step %d ring removed\n", id); }
        (*(void (***)(void *, int))spot)[0xa8 / 8](spot, 0);
      }
    }
#endif
  }
  void *menu = get_main_menu(MENU_MAIN3D);
  if (!menu || current_menu(mgr) != menu) return;
  for (size_t i = 0; i < sizeof(hidden_objects) / sizeof(*hidden_objects); i++) {
    void *b = ui_obj ? ui_obj(menu, hidden_objects[i]) : NULL;
    if (!b) continue;
    void (*set_visible)(void *, int) = (*(void (***)(void *, int))b)[0xa8 / 8];
    set_visible(b, 0);
  }
}

/* Profiler screen sections (profiler.c): checked right after the frame's game work
 * (main.c), before that frame's hitch is reported, so a hitch caused by opening a
 * screen is labelled with that screen. */
void social_profiler_menu_check(void) {
  if (!get_menus_singleton || !current_menu) return;
  void *mgr = get_menus_singleton(0);
  if (!mgr) return;
  static void *last = (void *)1;
  void *cur = current_menu(mgr);
  if (cur == last) return;
  last = cur;
  extern void profiler_section(const char *);
  profiler_section(cur ? class_name(cur) : "gameplay");
}

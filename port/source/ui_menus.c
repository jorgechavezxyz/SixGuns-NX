/* The pause menu, the Options screen and the loading screen, redrawn by the port.
 *
 * Owner (2026-11-14): "redo the entire UI of the pause menu / options menu ... more performant and
 * easier to navigate"; the side columns of the pause menu flashed for a moment when it opened, its
 * focus came back where it was left, Auto aim / Push Notifications / Lock V-Pad looked dead, the
 * language flags took a whole row, and the loading screen should be just the backdrop.
 *
 * Same principle as the shop (shop_ui.c): the engine's menus stay alive and keep all their logic,
 * only their drawing and their controls are the port's.
 *
 *  Pause menu (Menu_IGM_Main): RenderUI is replaced (vtable slot): the game scene, a dim, a centred
 *    list, the money bar. The list is the page's own central buttons (visible LayerButtons in the
 *    menu column, top to bottom, labelled with their own text ids, so translations and the port's
 *    SHOP button come for free). A presses the hidden button with a touch on its centre, which is
 *    exactly what a finger did. The page itself is never drawn: the side columns can't flash.
 *    Focus starts on the first item every time the menu opens.
 *  Options (Menu_IGM_Options): RenderUI replaced. Rows Music, Sound FX, Aim assist (the game's own
 *    settings, set through the page's own slider objects, which its UpdateUI turns into Game::Set...
 *    every frame; Back saves them with Game::SaveSettings as before), Camera speed (the port's,
 *    controller.c), Language (a dropdown; picking one releases that language's own button, the
 *    game's whole language switch runs), Help & Info (MGR_Menus::PushMenu(0x15), as INFO did).
 *    Push Notifications and Lock V-Pad (touch-only V-Pad mode) are not shown.
 *  Loading screen (Menu_LoadingScreen): RenderUI draws LoadingScreen::Render (backdrop + spinner)
 *    and the "press a button" prompt only, no tips and no elixir offer.
 *
 * Marker `old_menus` keeps the game's pause / options / loading screens. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <arpa/inet.h>
#include "config.h"
#include "input.h"
#include "ui_text.h"
#include "so_util.h"
#include "util.h"

#define ENGINE_SCALE (SCREEN_H / 320.0f)
so_module *port_engine_module(void);

int sui_ready(void);
void sui_begin(void);
void sui_begin_here(void);
void sui_end(void);
void sui_color(float r, float g, float b, float a);
void sui_rect(float x0, float y0, float x1, float y1);
void sui_frame(float x0, float y0, float x1, float y1, float t);
float sui_text(float x, float y, const char *s, int px, float maxw);
void sui_text_shadow(float x, float y, const char *s, int px, float maxw);
float sui_text_w(const char *s, int px);
float sui_wrap(float,float,float,const char *,int,float,float,float);
void port_touch(int down, int x, int y, int id);
int camera_level_get(void);
void camera_level_set(int level);

#define G_A "\xee\x83\xa0"
#define G_B "\xee\x83\xa1"
#define G_DPAD "\xee\x83\xaa" /* U+E0EA */

static int enabled;
static void *(*menus_inst)(int);
static void *(*cur_menu)(void *);
static void *(*get_iobj)(void *, int);
static int (*btn_text_id)(void *);
static void *(*str_inst)(int);
static const char *(*get_string)(void *, int);
static int (*loaded_lang)(void *);
static void *(*popup_inst)(int);
static int (*popup_enabled)(void *);
static void *(*gp_inst)(int);
static void (*gp_render)(void *);
static void *(*pc_inst)(int);
static void (*pc_render_info)(void *, int);
static void (*slider_set)(void *, float);
static float (*slider_get)(void *);
static void (*push_menu)(void *, int);
static void (*grid_center)(void *, void *);
static void (*add_released)(void *);
static void *(*ls_inst)(int);
static void (*ls_render_bg)(void *);
static int (*ls_active)(void *);
static void (*orig_pause_render)(void *, int, int);
static void (*orig_opts_render)(void *, int, int);
static void (*orig_ls_render)(void *, int, int);
static void (*orig_lang_update)(void *, int, int, int);

static const char *cls(void *o) {
  void *ti = (*(void ***)o)[-1];
  const char *n = ti ? *(const char **)((char *)ti + 8) : NULL;
  return n ? n : "?";
}
static int vis(void *o) { return o && ((*(int (***)(void *))o)[0xb0 / 8](o) & 1); }
static uint8_t *current_menu(void) {
  void *mm = menus_inst ? menus_inst(0) : NULL;
  return mm && cur_menu ? cur_menu(mm) : NULL;
}
static int popup_up(void) {
  void *p = popup_inst ? popup_inst(0) : NULL;
  return p && popup_enabled && (popup_enabled(p) & 1);
}
static const char *str(int id) {
  void *sm = str_inst ? str_inst(0) : NULL;
  const char *s = sm && id > 0 ? get_string(sm, id) : NULL;
  return s && *s ? s : NULL;
}

/* ------------------------------------------------------------------ synthetic tap */
static int tap_x, tap_y, tap_frames; /* >0: frames until the release */
static void tap(float ex, float ey) { /* engine units */
  if (tap_frames) return;
  tap_x = (int)(ex * ENGINE_SCALE);
  tap_y = (int)(ey * ENGINE_SCALE);
  port_touch(1, tap_x, tap_y, 13); /* 9 belongs to the engine Cursor, 14 to the free pointer */
  tap_frames = 4;
}
static void tap_frame(void) {
  if (tap_frames && --tap_frames == 0) port_touch(0, tap_x, tap_y, 13);
}

/* ------------------------------------------------------------------ input helpers */
static unsigned long frame_no;
/* Hardware: the first keyboard attempt can return LibAppletBadExit at once without showing (NOTES round
 * 13). A user's Cancel returns the same code, so retry only when the applet came back almost at once;
 * a cancelled keyboard is not reopened. */
static Result kbd_retry_bounce_t(SwkbdConfig *kbd, char *out, size_t n, Result rc, u64 t0) {
  u64 ms = armTicksToNs(armGetSystemTick() - t0) / 1000000;
  if (rc == MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit) && ms < 400) {
    debugPrintf("[menus] keyboard bounced after %llu ms, retrying\n", (unsigned long long)ms);
    rc = swkbdShow(kbd, out, n);
  }
  return rc;
}
static int rep_dir, rep_next;
/* -1 up, +1 down, with hold-to-repeat (D-pad or left stick) */
static int vertical_step(const InputFrame *in) {
  int dir = 0;
  if (in->held & HidNpadButton_Up) dir = -1;
  if (in->held & HidNpadButton_Down) dir = 1;
  if (!dir && abs(in->ls.y) > abs(in->ls.x) && abs(in->ls.y) > 16000) dir = in->ls.y > 0 ? -1 : 1;
  int step = 0;
  if (dir != rep_dir) { if (dir) step = dir; rep_next = (int)frame_no + 22; }
  else if (dir && (int)frame_no >= rep_next) { step = dir; rep_next = (int)frame_no + 7; }
  rep_dir = dir;
  return step;
}
static int hrep_dir, hrep_next;
static int horizontal_step(const InputFrame *in) {
  int dir = 0;
  if (in->held & HidNpadButton_Left) dir = -1;
  if (in->held & HidNpadButton_Right) dir = 1;
  if (!dir && abs(in->ls.x) > abs(in->ls.y) && abs(in->ls.x) > 16000) dir = in->ls.x > 0 ? 1 : -1;
  int step = 0;
  if (dir != hrep_dir) { if (dir) step = dir; hrep_next = (int)frame_no + 22; }
  else if (dir && (int)frame_no >= hrep_next) { step = dir; hrep_next = (int)frame_no + 5; }
  hrep_dir = dir;
  return step;
}
static void strip(InputFrame *in, u64 keys) {
  in->down &= ~keys;
  in->held &= ~keys;
  in->ls.x = in->ls.y = in->rs.x = in->rs.y = 0;
}
static int touch_started(const InputFrame *in, int i) {
  int id = in->t[i].id;
  return id >= 0 && id < 16 && (in->touch_started & (1u << id));
}
static const u64 NAV = HidNpadButton_A | HidNpadButton_X | HidNpadButton_Y | HidNpadButton_Up | HidNpadButton_Down |
                       HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_L | HidNpadButton_R |
                       HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_StickLUp | HidNpadButton_StickLDown |
                       HidNpadButton_StickLLeft | HidNpadButton_StickLRight |
                       HidNpadButton_StickRUp | HidNpadButton_StickRDown |
                       HidNpadButton_StickRLeft | HidNpadButton_StickRRight;

/* Drawing helpers shared by both screens. */
static void panel(float x0, float y0, float x1, float y1) {
  sui_color(0.12f, 0.06f, 0.02f, 1); sui_rect(x0, y0, x1, y1);
  sui_color(0.78f, 0.58f, 0.26f, 1); sui_frame(x0, y0, x1, y1, 3);
}
static void gold_frame(float x0, float y0, float x1, float y1) {
  sui_color(1.0f, 0.84f, 0.40f, 1); sui_rect(x0, y0, x1, y1);
  sui_color(0.45f, 0.24f, 0.05f, 1); sui_frame(x0, y0, x1, y1, 3);
}

/* ================================================================== pause menu */
static int in_lottery_level(void) {
  static void *(*lm_inst)(int);
  static int (*lm_is_level)(void *);
  if (!lm_inst) {
    lm_inst = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI10LotteryMgrE11GetInstanceEb");
    lm_is_level = (void *)so_symbol(port_engine_module(), "_ZNK10LotteryMgr14IsLotteryLevelEv");
  }
  void *lm = lm_inst ? lm_inst(0) : NULL;
  return lm && lm_is_level && (lm_is_level(lm) & 1);
}
typedef struct { uint8_t *btn; float r[4]; const char *label; } pitem;
enum { MAXP = 12 };
static pitem pitems[MAXP];
static int npitems, psel, pause_on;

static int read_pause_items(uint8_t *menu) {
  uint8_t *page = *(uint8_t **)(menu + 0x8);
  void **pb = page ? *(void ***)(page + 0x68) : NULL, **pe = page ? *(void ***)(page + 0x70) : NULL;
  int n = 0;
  for (void **it = pb; pb && pe > pb && pe - pb < 2000 && it < pe && n < MAXP; it++) {
    uint8_t *o = *it;
    if (!o || !strstr(cls(o), "LayerButton") || !vis(o)) continue;
    float *r = (float *)(o + 0xc4), cx = (r[0] + r[2]) / 2, w = r[2] - r[0], h = r[3] - r[1];
    if (cx < 200 || cx > 380 || w < 100 || h < 12 || h > 50) continue; /* the menu column only */
    if (r[1] < 0 || r[3] > 320) continue; /* off screen: e.g. SLOT MACHINE (text 699) parked at y -118 */
    const char *l = str(btn_text_id(o));
    if (!l) continue;
    /* In the shooting lottery the game ignores LOTTERY (867) and MAIN MENU (65): you leave with the
     * picker's X after a round. Not listed there (pressing them did nothing, owner 2026-11-14). */
    if (in_lottery_level() && (btn_text_id(o) == 867 || btn_text_id(o) == 65)) continue;
    int dup = 0;
    for (int k = 0; k < n; k++) dup |= fabsf(pitems[k].r[1] - r[1]) < 4;
    if (dup) continue;
    pitems[n].btn = o;
    memcpy(pitems[n].r, r, sizeof(pitems[n].r));
    pitems[n].label = l;
    n++;
  }
  for (int i = 1; i < n; i++) /* top to bottom */
    for (int j = i; j > 0 && pitems[j].r[1] < pitems[j - 1].r[1]; j--) { pitem t = pitems[j]; pitems[j] = pitems[j - 1]; pitems[j - 1] = t; }
  return n;
}

enum { P_ROW_H = 62, P_W = 520, P_VIS = 7 };
static int pause_first(void) { return psel >= P_VIS ? psel - P_VIS + 1 : 0; }
static float p_x0, p_y0;
static void pause_layout(int n) {
  float h = 90 + (n < P_VIS ? n : P_VIS) * P_ROW_H + 56;
  p_x0 = 640 - P_W / 2;
  p_y0 = 360 - h / 2 + 20;
}

static void pause_input(InputFrame *in, uint8_t *menu) {
  npitems = read_pause_items(menu);
  if (!npitems) { strip(in, NAV); in->ntouch = 0; return; }
  if (psel >= npitems) psel = npitems - 1;
  pause_layout(npitems);
  int step = vertical_step(in);
  if (step) psel = (psel + step + npitems) % npitems; /* wraps */
  if (in->down & HidNpadButton_A) {
    pitem *p = &pitems[psel];
    debugPrintf("[menus] pause: %s\n", p->label);
    tap((p->r[0] + p->r[2]) / 2, (p->r[1] + p->r[3]) / 2);
  }
  /* touch on a row: that item */
  for (int i = 0; i < in->ntouch; i++) {
    int x = in->t[i].x, y = in->t[i].y;
    float bottom = p_y0 + 90 + (npitems < P_VIS ? npitems : P_VIS) * P_ROW_H + 56;
    if (touch_started(in, i) && x >= p_x0 + P_W - 180 && x < p_x0 + P_W && y >= bottom - 48 && y < bottom) {
      in->down |= HidNpadButton_B; continue;
    }
    if (x < p_x0 || x > p_x0 + P_W) continue;
    int r = (int)((y - (p_y0 + 90)) / P_ROW_H);
    if (y < p_y0 + 90 || r < 0 || r >= P_VIS || r + pause_first() >= npitems) continue;
    if (touch_started(in, i)) {
      psel = r + pause_first();
      pitem *p = &pitems[psel];
      debugPrintf("[menus] pause (touch): %s\n", p->label);
      tap((p->r[0] + p->r[2]) / 2, (p->r[1] + p->r[3]) / 2);
    }
    in->t[i] = in->t[--in->ntouch];
    i--;
  }
  strip(in, NAV);
  in->ntouch = 0; /* hidden engine buttons must not see taps outside our rows either */
}

static void pause_draw(void) {
  if (!npitems) return;
  /* dim the game */
  sui_color(0, 0, 0, 0.55f); sui_rect(0, 0, SCREEN_W, SCREEN_H);
  int visible = npitems < P_VIS ? npitems : P_VIS, first = pause_first();
  float x0 = p_x0, x1 = p_x0 + P_W, y1 = p_y0 + 90 + visible * P_ROW_H + 56;
  panel(x0, p_y0, x1, y1);
  const char *title = str(0x2a8) ? NULL : NULL; /* (the game's "In-Game Menu" title is an image) */
  (void)title;
  sui_color(1.0f, 0.86f, 0.45f, 1);
  const char *t = "Paused";
  sui_text_shadow(640 - sui_text_w(t, 40) / 2, p_y0 + 60, t, 40, 0);
  for (int i = first; i < npitems && i < first + P_VIS; i++) {
    float ry0 = p_y0 + 90 + (i - first) * P_ROW_H + 4, ry1 = ry0 + P_ROW_H - 8;
    if (i == psel) gold_frame(x0 + 24, ry0, x1 - 24, ry1);
    else { sui_color(0.95f, 0.87f, 0.69f, 1); sui_rect(x0 + 24, ry0, x1 - 24, ry1); }
    sui_color(0.22f, 0.12f, 0.04f, 1);
    const char *l = pitems[i].label;
    float lw = fminf(sui_text_w(l, 30), P_W - 80);
    sui_text(640 - lw / 2, ry0 + (ry1 - ry0) / 2 + 11, l, 30, P_W - 80);
  }
  if (in_lottery_level()) {
    sui_color(1.0f, 0.86f, 0.45f, 1);
    const char *n = "Lottery: finish the round, then the X on the chest board leaves";
    sui_text_shadow(640 - sui_text_w(n, 20) / 2, y1 + 34, n, 20, 0);
  }
  sui_color(0.95f, 0.88f, 0.70f, 1);
  sui_text_shadow(x0 + 26, y1 - 18, G_A " Select", 24, 0);
  float bw = sui_text_w(G_B " Resume", 24);
  sui_text_shadow(x1 - 26 - bw, y1 - 18, G_B " Resume", 24, 0);
}

static void pause_render_hook(void *menu, int a1, int a2) {
  if (!enabled || !gp_inst || !gp_render || !sui_ready()) { orig_pause_render(menu, a1, a2); return; }
  void *gp = gp_inst(0);
  if (gp) gp_render(gp);
  sui_begin();
  pause_draw();
  sui_end();
  void *pc = pc_inst ? pc_inst(-1) : NULL;
  if (pc && pc_render_info) pc_render_info(pc, 1); /* the money bar, as the game's own menu showed it */
}

/* ================================================================== options */
static const char *const lang_names[15] = {
  "English", "Fran\xc3\xa7" "ais", "Deutsch", "Italiano", "Espa\xc3\xb1ol", "Portugu\xc3\xaas (Brasil)", "Japanese",
  "Chinese (Simplified)", "Korean", "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9", "T\xc3\xbcrk\xc3\xa7" "e",
  "Arabic", "Thai", "Chinese (Traditional)", "Bahasa Indonesia",
};
enum { O_MUSIC, O_SFX, O_AIM, O_CAM, O_LANG, O_CTRL, O_ACH, O_ROWS };
static const char *const o_labels[O_ROWS] = { "Music", "Sound FX", "Aim assist", "Camera speed", "Language", "Controls", "Achievements" };
static int osub, ach_scroll; /* 1 controls, 2 achievements: a panel over the options (owner: Info menu removed) */
static int ach_finger = -1, ach_y, ach_scroll_start, ach_moved;
static int osel, opts_on, lang_open, lang_sel;
static int slider_finger = -1;
static uint8_t *lang_pending_item;
static int lang_pending_frames;

static uint8_t *opage(uint8_t *menu) { return *(uint8_t **)(menu + 0x8); }
/* the page's own slider objects: value slider and its on/off toggle */
static void *o_slider(uint8_t *menu, int row) {
  static const int ids[3] = { 0x12, 0x1f, 0x2c };
  return row <= O_AIM ? get_iobj(opage(menu), ids[row]) : NULL;
}
static void *o_toggle(uint8_t *menu, int row) {
  static const int ids[3] = { 0x9, 0x16, 0x23 };
  return row <= O_AIM ? get_iobj(opage(menu), ids[row]) : NULL;
}
/* In the slider's own percent (LayerSlideBar +0x64, what SetValue takes); GetValue maps it onto the
 * slider's range (+0x44..+0x48, e.g. 50..100 for music), which the game then applies. */
/* The slider recomputes its percent from the knob position, a few points off what was set (90 came
 * back as 95), so the screen keeps the chosen values itself: read once when Options opens (rounded
 * to steps of 10), then only written. */
static int ovals[3] = { -1, -1, -1 };
static int o_value(uint8_t *menu, int row) {
  if (row > O_AIM) return 0;
  if (ovals[row] < 0) {
    uint8_t *s = o_slider(menu, row);
    (void)slider_get;
    int v = s ? (int)(*(float *)(s + 0x64) + 0.5f) : 0;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    ovals[row] = (v + 5) / 10 * 10;
  }
  return ovals[row];
}
/* after an aim assist change: the game's own value once its UpdateUI has applied the slider */
static int aim_check;
static void aim_check_frame(void) {
  if (!aim_check || --aim_check) return;
  static void *(*gi)(int);
  static float (*scale)(void *);
  static int (*on)(void *);
  if (!gi) {
    gi = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI4GameE11GetInstanceEb");
    scale = (void *)so_symbol(port_engine_module(), "_ZN4Game22OptionsGetAutoAimScaleEv");
    on = (void *)so_symbol(port_engine_module(), "_ZN4Game23OptionsIsAutoAimEnabledEv");
  }
  void *g = gi ? gi(0) : NULL;
  if (g && scale && on) debugPrintf("[menus] game auto-aim now: %s, scale %.2f\n",on(g) & 1 ? "on" : "off", scale(g));
}
static void o_set(uint8_t *menu, int row, int v) {
  void *s = o_slider(menu, row), *t = o_toggle(menu, row);
  if (!s || !t) return;
  v = v < 0 ? 0 : v > 100 ? 100 : v;
  slider_set(t, v > 0 ? 100.0f : 0.0f); /* keep the toggle in step, or UpdateSlideButton zeroes the value */
  slider_set(s, (float)v);
  ovals[row] = v;
  debugPrintf("[menus] options: %s %d\n", o_labels[row], v);
  aim_check = row == O_AIM ? 20 : 0;
}
static int cur_lang(void) {
  void *sm = str_inst ? str_inst(0) : NULL;
  int l = sm && loaded_lang ? loaded_lang(sm) : 0;
  return l < 0 || l > 14 ? 0 : l;
}
/* Picking a language: its own Page_LanguageButtonItem (grid object 0x43, +0x90 = language) gets its
 * button released before its Update, which runs the game's whole language switch. The grid only
 * updates cells inside its visible area: the item is scrolled into view first. */
static void choose_language(uint8_t *menu, int lang) {
  uint8_t *grid = get_iobj(opage(menu), 0x43);
  void **b = grid ? *(void ***)(grid + 0xb8) : NULL, **e = grid ? *(void ***)(grid + 0xc0) : NULL;
  for (void **it = b; b && e > b && e - b < 64 && it < e; it++) {
    uint8_t *item = *it;
    if (!item || !strstr(cls(item), "LanguageButtonItem") || *(int32_t *)(item + 0x90) != lang) continue;
    grid_center(grid, item);
    if (*(int32_t *)(grid + 0x108) == 0) *(float *)(grid + 0x110) = *(float *)(grid + 0x10c);
    else *(float *)(grid + 0x118) = *(float *)(grid + 0x114);
    lang_pending_item = item;
    lang_pending_frames = 0;
    debugPrintf("[menus] options: language %d (%s)\n", lang, lang_names[lang]);
    return;
  }
  debugPrintf("[menus] options: language %d has no button\n", lang);
}
static void lang_update_hook(void *item, int a1, int a2, int a3) {
  if (item == lang_pending_item && a1 > 0) {
    void *btn = get_iobj(item, 2);
    if (btn) add_released(btn);
    lang_pending_item = NULL;
  }
  orig_lang_update(item, a1, a2, a3);
}

enum { O_W = 860, O_ROW_H = 66 };
static float o_x0, o_y0;
static void opts_layout(void) {
  float h = 100 + O_ROWS * O_ROW_H + 60;
  o_x0 = 640 - O_W / 2;
  o_y0 = 360 - h / 2;
}

static void opts_change(uint8_t *menu, int row, int dir) {
  if (row <= O_AIM) o_set(menu, row, o_value(menu, row) + dir * 10);
  else if (row == O_CAM) camera_level_set(camera_level_get() + dir);
}

static void opts_input(InputFrame *in, uint8_t *menu) {
  opts_layout();
  if (lang_pending_item && ++lang_pending_frames > 30) lang_pending_item = NULL;
  aim_check_frame();
  if (osub) {
    slider_finger = -1;
    int step = vertical_step(in);
    if (osub == 2 && step) ach_scroll += step;
    if (in->down & (HidNpadButton_B | HidNpadButton_A)) { osub = 0; strip(in, HidNpadButton_B | HidNpadButton_A); }
    if (osub == 2) {
      int seen = 0;
      for (int i = 0; i < in->ntouch; i++) {
        if (ach_finger < 0 && touch_started(in, i)) {
          ach_finger = in->t[i].id; ach_y = in->t[i].y; ach_scroll_start = ach_scroll; ach_moved = 0;
        }
        if (in->t[i].id != ach_finger) continue;
        seen = 1;
        int dy = in->t[i].y - ach_y;
        if (abs(dy) > 16) ach_moved = 1;
        if (ach_moved) { ach_scroll = ach_scroll_start - dy / 76; if (ach_scroll < 0) ach_scroll = 0; }
      }
      if (ach_finger >= 0 && !seen) { if (!ach_moved) osub = 0; ach_finger = -1; }
    } else {
      ach_finger = -1;
      for (int i = 0; i < in->ntouch; i++) if (touch_started(in, i)) osub = 0;
    }
    strip(in, NAV);
    in->ntouch = 0;
    return;
  }
  if (lang_open) {
    slider_finger = -1;
    int step = vertical_step(in);
    if (step) lang_sel = (lang_sel + step + 15) % 15;
    int confirm = (in->down & HidNpadButton_A) != 0;
    for (int i = 0; i < in->ntouch; i++) if (touch_started(in, i)) {
      float lx0 = o_x0 + O_W - 420, lx1 = o_x0 + O_W - 40, top = o_y0 + 50;
      int x = in->t[i].x, y = in->t[i].y;
      int row = (int)((y - top) / 36);
      if (x >= lx0 + 8 && x < lx1 - 8 && y >= top && row >= 0 && row < 15) {
        lang_sel = row; confirm = 1;
      } else lang_open = 0; /* outside tap dismisses without changing language */
      break;
    }
    if (in->down & HidNpadButton_B) { lang_open = 0; confirm = 0; strip(in, HidNpadButton_B); }
    if (confirm && lang_open) { lang_open = 0; if (lang_sel != cur_lang()) choose_language(menu, lang_sel); }
    strip(in, NAV);
    in->ntouch = 0;
    return;
  }
  int step = vertical_step(in);
  if (step) osel = (osel + step + O_ROWS) % O_ROWS;
  int h = horizontal_step(in);
  if (h) opts_change(menu, osel, h);
  if (in->down & HidNpadButton_ZL) opts_change(menu, O_CAM, -1);
  if (in->down & HidNpadButton_ZR) opts_change(menu, O_CAM, 1);
  if (in->down & HidNpadButton_A) {
    if (osel == O_LANG) { lang_open = 1; lang_sel = cur_lang(); }
    else if (osel == O_CTRL) { osub = 1; debugPrintf("[menus] options: controls\n"); }
    else if (osel == O_ACH) { osub = 2; ach_scroll = 0; debugPrintf("[menus] options: achievements\n"); }
  }
  /* touch: a row selects it; on a value bar it sets the value; on Language / Help it opens them */
  int slider_seen = 0;
  for (int i = 0; i < in->ntouch; i++) {
    int x = in->t[i].x, y = in->t[i].y;
    float bottom = o_y0 + 100 + O_ROWS * O_ROW_H + 60;
    if (touch_started(in, i) && x >= o_x0 + O_W - 150 && x < o_x0 + O_W && y >= bottom - 48 && y < bottom)
      in->down |= HidNpadButton_B;
    int r = (int)((y - (o_y0 + 100)) / O_ROW_H);
    if (x >= o_x0 && x <= o_x0 + O_W && y >= o_y0 + 100 && r >= 0 && r < O_ROWS) {
      osel = r;
      float bx0 = o_x0 + 380, bx1 = o_x0 + O_W - 110;
      int bar_touch = r <= O_CAM && x >= bx0 - 20 && x <= bx1 + 20;
      if (bar_touch && touch_started(in, i)) slider_finger = in->t[i].id;
      int drag = bar_touch && slider_finger == in->t[i].id;
      if (drag) slider_seen = 1;
      if (r <= O_AIM && drag) {
        float t = (x - bx0) / (bx1 - bx0);
        int v = (int)((t < 0 ? 0 : t > 1 ? 1 : t) * 10 + 0.5f) * 10;
        if (v != o_value(menu, r)) o_set(menu, r, v);
      } else if (r == O_CAM && drag) {
        float t = (x - bx0) / (bx1 - bx0);
        int l = 1 + (int)((t < 0 ? 0 : t > 1 ? 1 : t) * 9 + 0.5f);
        if (l != camera_level_get()) camera_level_set(l);
      } else {
        if (touch_started(in, i)) {
          if (r == O_LANG) { lang_open = 1; lang_sel = cur_lang(); }
          else if (r == O_CTRL) osub = 1;
          else if (r == O_ACH) { osub = 2; ach_scroll = 0; }
        }
      }
    }
    in->t[i] = in->t[--in->ntouch];
    i--;
  }
  if (!slider_seen) slider_finger = -1;
  strip(in, NAV);
}

static void bar(float x0, float x1, float y, float t, const char *val) {
  sui_color(0.45f, 0.30f, 0.12f, 1); sui_rect(x0, y - 3, x1, y + 3);
  float kx = x0 + (x1 - x0) * t;
  sui_color(0.20f, 0.12f, 0.04f, 1); sui_rect(kx - 22, y - 18, kx + 22, y + 18);
  sui_color(0.10f, 0.62f, 0.12f, 1); sui_rect(kx - 19, y - 15, kx + 19, y + 15);
  sui_color(0.45f, 0.85f, 0.35f, 1); sui_rect(kx - 16, y - 12, kx + 16, y - 4);
  sui_color(0.25f, 0.14f, 0.05f, 1);
  sui_text(x1 + 30, y + 9, val, 26, 0);
}

static void opts_draw(uint8_t *menu) {
  sui_color(0.07f, 0.04f, 0.02f, 1); sui_rect(0, 0, SCREEN_W, SCREEN_H);
  float x0 = o_x0, x1 = o_x0 + O_W, y1 = o_y0 + 100 + O_ROWS * O_ROW_H + 60;
  panel(x0, o_y0, x1, y1);
  sui_color(1.0f, 0.86f, 0.45f, 1);
  const char *t = "Options";
  sui_text_shadow(640 - sui_text_w(t, 44) / 2, o_y0 + 66, t, 44, 0);
  for (int i = 0; i < O_ROWS; i++) {
    float ry0 = o_y0 + 100 + i * O_ROW_H + 4, ry1 = ry0 + O_ROW_H - 8, cy = (ry0 + ry1) / 2;
    if (i == osel && !lang_open) gold_frame(x0 + 24, ry0, x1 - 24, ry1);
    else { sui_color(0.95f, 0.87f, 0.69f, 1); sui_rect(x0 + 24, ry0, x1 - 24, ry1); }
    sui_color(0.22f, 0.12f, 0.04f, 1);
    sui_text(x0 + 50, cy + 11, o_labels[i], 30, 300);
    float bx0 = x0 + 380, bx1 = x1 - 110;
    char v[48];
    if (i <= O_AIM) {
      int val = o_value(menu, i);
      if (val == 0) snprintf(v, sizeof(v), "Off");
      else snprintf(v, sizeof(v), "%d", val);
      bar(bx0, bx1, cy, val / 100.0f, v);
    } else if (i == O_CAM) {
      int l = camera_level_get();
      snprintf(v, sizeof(v), "%d", l);
      bar(bx0, bx1, cy, (l - 1) / 9.0f, v);
    } else if (i == O_LANG) {
      snprintf(v, sizeof(v), "%s  \xe2\x96\xbc", lang_names[cur_lang()]); /* ▼ */
      sui_color(0.22f, 0.12f, 0.04f, 1);
      sui_text(x1 - 60 - sui_text_w(v, 28), cy + 10, v, 28, 0);
    } else {
      sui_color(0.22f, 0.12f, 0.04f, 1);
      sui_text(x1 - 60 - sui_text_w("\xe2\x96\xb6", 26), cy + 9, "\xe2\x96\xb6", 26, 0); /* ▶ */
    }
  }
  sui_color(0.95f, 0.88f, 0.70f, 1);
  const char *hint = "\xee\x83\x81 Choose   \xee\x83\x82 Change   " G_A " Select";
  (void)hint;
  sui_text_shadow(x0 + 30, y1 - 20, "Up/Down Choose    Left/Right Change    " G_A " Select", 24, 0);
  float bw = sui_text_w(G_B " Back", 24);
  sui_text_shadow(x1 - 30 - bw, y1 - 20, G_B " Back", 24, 0);

  if (lang_open) { /* dropdown */
    float lx0 = x1 - 420, lx1 = x1 - 40, rh = 36;
    float ly0 = o_y0 + 40, ly1 = ly0 + 15 * rh + 20;
    panel(lx0, ly0, lx1, ly1);
    for (int k = 0; k < 15; k++) {
      float r0 = ly0 + 10 + k * rh, r1 = r0 + rh - 2;
      if (k == lang_sel) gold_frame(lx0 + 8, r0, lx1 - 8, r1);
      sui_color(k == lang_sel ? 0.22f : 0.95f, k == lang_sel ? 0.12f : 0.88f, k == lang_sel ? 0.04f : 0.70f, 1);
      sui_text(lx0 + 24, r0 + rh / 2 + 8, lang_names[k], 24, lx1 - lx0 - 60);
      if (k == cur_lang()) sui_text(lx1 - 24 - sui_text_w("current", 18), r0 + rh / 2 + 7, "current", 18, 0);
    }
  }
}


/* Controls: one flat page (the game's Help pages scrolled like credits). */
#define G_X "\xee\x83\xa2"
#define G_Y "\xee\x83\xa3"
#define G_L "\xee\x83\xa4"
#define G_R "\xee\x83\xa5"
#define G_ZL "\xee\x83\xa6"
#define G_ZR "\xee\x83\xa7"
#define G_UP "\xee\x83\xab"
#define G_DN "\xee\x83\xac"
#define G_LT "\xee\x83\xad"
#define G_RT "\xee\x83\xae"
#define G_PLUS "\xee\x83\xaf"
#define G_MINUS "\xee\x83\xb0"
#define G_LS "\xee\x83\x81"
#define G_RS "\xee\x83\x82"
typedef struct { const char *key, *what; } ctl;
static const ctl ctl_left[] = {
  { 0, "On foot" },
  { G_LS, "Move" }, { G_RS, "Look" }, { G_ZL, "Aim" }, { G_ZR, "Shoot" }, { G_Y, "Reload" },
  { G_X, "Next weapon" }, { G_R, "Tap: throw dynamite" }, { G_R " + " G_UP G_DN, "Dynamite type" },
  { G_L " + " G_LT G_UP G_RT, "Use an elixir" }, { "L3", "Take / leave cover" }, { G_B, "Roll" },
  { "R3", "Turn around" }, { G_A, "Interact" }, { G_LT, "Call horse" }, { G_PLUS, "Pause menu" }, { G_MINUS, "Map" },
};
static const ctl ctl_right[] = {
  { 0, "On a horse" }, { G_A, "Spur" }, { G_LT, "Dismount" },
  { 0, "Sniper scope" }, { G_LS " " G_UP G_DN, "Zoom" },
  { 0, "Menus" }, { G_LS " / D-pad", "Move the highlight" }, { G_A, "Select" }, { G_B, "Back" },
  { G_L " " G_R, "Switch tabs" }, { G_RS, "Free pointer" },
  { 0, "Cutscenes and comics" }, { "Hold " G_A, "Skip" },
};
static void ctl_column(const ctl *c, int n, float x, float y) {
  for (int i = 0; i < n; i++, y += 31) {
    if (!c[i].key) {
      if (i) y += 8;
      sui_color(1.0f, 0.86f, 0.45f, 1);
      sui_text_shadow(x, y + 22, c[i].what, 26, 0);
      continue;
    }
    sui_color(0.95f, 0.88f, 0.70f, 1);
    sui_text(x + 10, y + 22, c[i].key, 24, 150);
    sui_text(x + 170, y + 22, c[i].what, 24, 330);
  }
}
static void controls_draw(void) {
  sui_color(0.07f, 0.04f, 0.02f, 1); sui_rect(0, 0, SCREEN_W, SCREEN_H);
  panel(60, 30, 1220, 690);
  sui_color(1.0f, 0.86f, 0.45f, 1);
  sui_text_shadow(640 - sui_text_w("Controls", 40) / 2, 80, "Controls", 40, 0);
  ctl_column(ctl_left, sizeof(ctl_left) / sizeof(*ctl_left), 100, 100);
  ctl_column(ctl_right, sizeof(ctl_right) / sizeof(*ctl_right), 680, 100);
  sui_color(0.95f, 0.88f, 0.70f, 1);
  sui_text_shadow(1220 - 30 - sui_text_w(G_B " Back", 24), 670, G_B " Back", 24, 0);
}

/* Achievements: AchivementMgr's own table (Get(i), 0x70 bytes: +0x28 name, +0x2c description text
 * ids, +0x38 unlocked, +0x48 listed; deprecated ones skipped), as Menu_AchievementBrowser reads it. */
static void *(*ach_inst)(int);
static uint8_t *(*ach_get)(void *, unsigned);
static int (*ach_depr)(unsigned);
static void achievements_draw(void) {
  sui_color(0.07f, 0.04f, 0.02f, 1); sui_rect(0, 0, SCREEN_W, SCREEN_H);
  panel(120, 30, 1160, 690);
  void *am = ach_inst ? ach_inst(0) : NULL;
  int ids[130], n = 0, got = 0;
  for (unsigned i = 0; am && ach_get && i < 130; i++) {
    uint8_t *a = ach_get(am, i);
    if (!a || !a[0x48] || (ach_depr && (ach_depr(i) & 1))) continue;
    ids[n++] = (int)i;
    got += a[0x38] != 0;
  }
  char t[64];
  snprintf(t, sizeof(t), "Achievements   %d / %d", got, n);
  sui_color(1.0f, 0.86f, 0.45f, 1);
  sui_text_shadow(640 - sui_text_w(t, 36) / 2, 78, t, 36, 0);
  enum { VIS = 7, RH = 76 };
  if (ach_scroll > n - VIS) ach_scroll = n - VIS;
  if (ach_scroll < 0) ach_scroll = 0;
  for (int k = 0; k < VIS && ach_scroll + k < n; k++) {
    uint8_t *a = ach_get(am, (unsigned)ids[ach_scroll + k]);
    float y0 = 100 + k * RH, y1 = y0 + RH - 8;
    int on = a[0x38] != 0;
    if (on) sui_color(0.98f, 0.90f, 0.66f, 1); else sui_color(0.62f, 0.55f, 0.44f, 1);
    sui_rect(150, y0, 1130, y1);
    sui_color(0.22f, 0.12f, 0.04f, 1);
    const char *nm = str(*(int *)(a + 0x28)), *ds = str(*(int *)(a + 0x2c));
    sui_text(176, y0 + 30, nm ? nm : "?", 26, 820);
    sui_color(0.30f, 0.20f, 0.10f, 1);
    sui_text(176, y0 + 58, ds ? ds : "", 20, 880);
    if (on) { /* The shared font has no check glyph; draw one rather than a missing-glyph box. */
      sui_color(0.10f, 0.50f, 0.12f, 1);
      for (int j = 0; j < 6; j++) sui_rect(1080 + j * 2, y0 + 30 + j * 2, 1085 + j * 2, y0 + 35 + j * 2);
      for (int j = 0; j < 12; j++) sui_rect(1090 + j * 2, y0 + 40 - j * 2, 1095 + j * 2, y0 + 45 - j * 2);
    }
  }
  sui_color(0.95f, 0.88f, 0.70f, 1);
  sui_text_shadow(150, 670, "Up/Down or swipe to scroll", 24, 0);
  sui_text_shadow(1160 - 30 - sui_text_w(G_B " Back", 24), 670, G_B " Back", 24, 0);
}

static void opts_render_hook(void *menu, int a1, int a2) {
  if (!enabled || !sui_ready()) { orig_opts_render(menu, a1, a2); return; }
  opts_layout();
  sui_begin();
  if (osub == 1) controls_draw();
  else if (osub == 2) achievements_draw();
  else opts_draw(menu);
  sui_end();
}

/* ================================================================== loading screen */
static void ls_render_hook(void *menu, int a1, int a2) {
  uint8_t *m = menu;
  if (!enabled || m[0x79] || m[0x7a] || !ls_inst || !ls_render_bg) { orig_ls_render(menu, a1, a2); return; }
  void *ls = ls_inst(0);
  if (!ls) { orig_ls_render(menu, a1, a2); return; }
  ls_render_bg(ls); /* backdrop and spinner */
  /* the "press a button" prompt (LayerText +0x40), when the level is ready */
  uint8_t *prompt = *(uint8_t **)(m + 0x40);
  if (prompt && ls_active && (ls_active(ls) & 1) && vis(prompt))
    (*(void (**)(void *, int, int))(*(uint8_t **)prompt + 0x28))(prompt, a1, a2);
}

/* ================================================================== multiplayer lobby */
/* Menu_MP_CreateHost (menu id 0x1d): mode buttons (+0x38 CTF -> mode 0, +0x40 TDM -> 2, +0x48 any -> 3;
 * Menu_MP_CreateHost::SelectMapMode stores it at +0xb4 and in the room info), a LayerGrid of map pages
 * at +0xa0 (page 0 = any map, pages 1..4 = MPMapManager maps 0..3; UpdateUI notices a page change and
 * calls onMapSelected, which stores the map at +0xb0, 4 = any), PLAY at +0x28 (quick match with those
 * settings; on the LAN this searches the network, then hosts). The invite-friends button (+0x30) is
 * online-only and not offered. */
static void (*mp_select_mode)(void *, int);
static int (*grid_get_page)(void *);
static void (*grid_set_page)(void *, int);
static void *(*mpmap_inst)(void);
static uint8_t *(*mpmap_info)(void *, int);
static void (*orig_mp_render)(void *, int, int);
static const int mp_modes[3] = { 3, 0, 2 }; /* any, capture the flag, team deathmatch */
static const int mp_mode_str[3] = { 0x8e7, 0x8c9, 0x8cb };
enum { M_NAME, M_MODE, M_MAP, M_SERVERS, M_PLAY, M_ROWS };
static int msel, mp_on;
enum { M_ROW_H = 92, M_W = 760 };
static float m_x0, m_y0;
static void mp_layout(void) {
  float h = 110 + M_ROWS * M_ROW_H + 70;
  m_x0 = 640 - M_W / 2;
  m_y0 = 360 - h / 2 + 10;
}
static int mp_mode_index(uint8_t *menu) {
  int m = *(int *)(menu + 0xb4);
  for (int i = 0; i < 3; i++) if (mp_modes[i] == m) return i;
  return 0;
}
static void *mp_grid(uint8_t *menu) { return *(void **)(menu + 0xa0); }
static int mp_page(uint8_t *menu) { void *g = mp_grid(menu); return g ? grid_get_page(g) : 0; }
/* custom maps from mods (custom_maps.c): lobby page 5 + n is map index 5 + n */
int custom_maps_count(void);
const char *custom_map_name(int index);
int custom_map_installed(int index);
static const char *mp_map_name(int page) {
  if (page <= 0) return str(0x8e8) ? str(0x8e8) : "Any map";
  if (page >= 5) { const char *c = custom_map_name(page); return c ? c : "?"; }
  void *mm = mpmap_inst ? mpmap_inst() : NULL;
  uint8_t *info = mm ? mpmap_info(mm, page - 1) : NULL;
  const char *s = info ? str(*(int *)(info + 8)) : NULL;
  return s ? s : "?";
}
/* The player's multiplayer name: the game's guest name (Game+0xda8, a libc++ string, saved in
 * save_common; GetUniversalUserName hands it to the anonymous sign-in). Owner: a local name screen
 * instead of the old guest login page. Set with the Switch keyboard, no server involved. */
static void *(*game_inst)(int);
static void (*game_set_guest)(void *, const void *);
static void (*game_save_common)(void *);
static const char *mp_name(void) {
  static char buf[64];
  uint8_t *g = game_inst ? game_inst(0) : NULL;
  if (!g) return "?";
  uint8_t *str = g + 0xda8;
  if (!(str[0] & 1)) snprintf(buf, sizeof(buf), "%.*s", str[0] >> 1, (const char *)str + 1);
  else snprintf(buf, sizeof(buf), "%.*s", (int)*(uint64_t *)(str + 8), *(const char **)(str + 0x10));
  return buf[0] ? buf : "(none)";
}
static void mp_edit_name(void) {
  void *g = game_inst ? game_inst(0) : NULL;
  if (!g || !game_set_guest) return;
  SwkbdConfig kbd;
  char out[64] = {0};
  if (R_FAILED(swkbdCreate(&kbd, 0))) return;
  swkbdConfigMakePresetDefault(&kbd);
  swkbdConfigSetHeaderText(&kbd, "Multiplayer name");
  swkbdConfigSetInitialText(&kbd, mp_name());
  swkbdConfigSetStringLenMax(&kbd, 16);
  u64 t0 = armGetSystemTick();
  Result rc = swkbdShow(&kbd, out, sizeof(out));
  rc = kbd_retry_bounce_t(&kbd, out, sizeof(out), rc, t0);
  swkbdClose(&kbd);
  if (R_FAILED(rc)) { debugPrintf("[menus] multiplayer name unchanged\n"); return; }
  out[sizeof(out)-1] = 0;
  size_t n = strlen(out);
  while (n && out[n - 1] == ' ') out[--n] = 0;
  if (!n) { debugPrintf("[menus] multiplayer name unchanged\n"); return; }
  uint8_t sstr[24] = { 0 }; /* libc++ short string: size << 1, then the characters */
  if (n > 22) { n = 22; while (n && ((unsigned char)out[n] & 0xc0) == 0x80) n--; }
  sstr[0] = (uint8_t)(n << 1);
  memcpy(sstr + 1, out, n);
  game_set_guest(g, sstr);
  if (game_save_common) game_save_common(g);
  debugPrintf("[menus] multiplayer name set to \"%s\"\n", mp_name());
}
/* Server browser (net.c): the hosts on the network that answer the port's query, plus "add a server by
 * address" (Switch keyboard; saved in mp_servers.txt). Picking a host makes it a direct peer and presses
 * PLAY: the game's search then reaches it without broadcasts. */
int net_add_peer(uint32_t ip, int save);
void net_browser_set(int on);
int net_browser_count(void);
int net_browser_get(int i, uint32_t *ip, int *mode, int *map, const char **name);
static int sb_open, sb_sel;
static uint32_t sb_selected_ip;
/* Seven rows fit above the footer; keep the selected row visible even at capacity. */
static int sb_first_row(int count) {
  if (sb_sel > count) sb_sel = count;
  if (sb_sel < 0) sb_sel = 0;
  return sb_sel >= 7 ? sb_sel - 6 : 0;
}
static void mp_play(uint8_t *menu);
static const char *mode_name(int m) {
  int id = m == 0 ? 0x8c9 : m == 1 ? 0x8ca : m == 2 ? 0x8cb : 0x8e7;
  return str(id) ? str(id) : "?";
}
static const char *map_name_idx(int m) {
  if (m >= 5) {
    static char b[48];
    const char *c = custom_map_name(m);
    if (c) return c;
    snprintf(b, sizeof(b), "Custom map %d (not installed)", m - 4);
    return b;
  }
  return mp_map_name(m >= 0 && m < 4 ? m + 1 : 0);
}
static char sb_msg[96];
static u64 sb_msg_until;
static void sb_add_address(void) {
  SwkbdConfig kbd;
  char out[64] = {0};
  if (R_FAILED(swkbdCreate(&kbd, 0))) return;
  swkbdConfigMakePresetDefault(&kbd);
  swkbdConfigSetHeaderText(&kbd, "Server address (e.g. 192.168.1.20)");
  swkbdConfigSetType(&kbd, SwkbdType_NumPad);
  swkbdConfigSetLeftOptionalSymbolKey(&kbd, ".");
  swkbdConfigSetStringLenMax(&kbd, 15);
  u64 t0 = armGetSystemTick();
  Result rc = swkbdShow(&kbd, out, sizeof(out));
  rc = kbd_retry_bounce_t(&kbd, out, sizeof(out), rc, t0);
  swkbdClose(&kbd);
  struct in_addr a;
  if (R_SUCCEEDED(rc) && inet_aton(out, &a)) {
    net_add_peer(a.s_addr, 1);
    debugPrintf("[menus] server address added: %s\n", out);
  } else debugPrintf("[menus] no server address entered\n");
}
static void sb_input(InputFrame *in, uint8_t *menu) {
  int n = net_browser_count(), rows = n + 1; /* the servers, then "add by address" */
  int lost_selection = 0;
  if (sb_selected_ip) {
    int found = 0;
    for (int i = 0; i < n; i++) {
      uint32_t ip; int mode, map; const char *name;
      if (net_browser_get(i, &ip, &mode, &map, &name) && ip == sb_selected_ip) { sb_sel = i; found = 1; break; }
    }
    if (!found) { sb_sel = n; lost_selection = 1; debugPrintf("[menus] selected server expired\n"); }
  }
  sb_first_row(n);
  int step = vertical_step(in);
  if (step) sb_sel = (sb_sel + step + rows) % rows;
  if (sb_sel >= rows) sb_sel = rows - 1;
  int confirm = !lost_selection && (in->down & HidNpadButton_A) != 0;
  int back = (in->down & HidNpadButton_B) != 0;
  int first = sb_first_row(n);
  for (int i = 0; i < in->ntouch; i++) if (touch_started(in, i)) {
    int x = in->t[i].x, y = in->t[i].y;
    int row = (y - 182) / 62;
    if (x >= 128 && x < 1152 && y >= 182 && row >= 0 && row < 7 &&
        (y - 182) % 62 < 54 && first + row <= n) {
      sb_sel = first + row; confirm = 1;
    } else if (x >= 1040 && x < 1180 && y >= 612 && y < 650) back = 1;
    else if (x >= 100 && x < 380 && y >= 612 && y < 650) confirm = !lost_selection;
    break;
  }
  sb_selected_ip = 0;
  if (sb_sel < n) { int mode, map; const char *name; net_browser_get(sb_sel, &sb_selected_ip, &mode, &map, &name); }
  if (back) { sb_open = 0; net_browser_set(0); strip(in, HidNpadButton_B); }
  else if (confirm) {
    if (sb_sel == n) sb_add_address();
    else {
      uint32_t ip; int mode, map; const char *nm;
      if (net_browser_get(sb_sel, &ip, &mode, &map, &nm) && map >= 5 && !custom_map_installed(map)) {
        /* the host plays a custom map from a mod this console does not have: joining would load a level
         * that is not here (custom_maps.c also hides such rooms from the game's own search) */
        snprintf(sb_msg, sizeof(sb_msg), "This match plays a custom map you don't have installed.");
        sb_msg_until = armGetSystemTick() + armGetSystemTickFreq() * 4;
        debugPrintf("[menus] not joining %s: custom map %d not installed\n", nm, map);
      } else if (net_browser_get(sb_sel, &ip, &mode, &map, &nm)) {
        struct in_addr a = { .s_addr = ip };
        debugPrintf("[menus] joining %s (%s)\n", nm, inet_ntoa(a));
        net_add_peer(ip, 0);
        sb_open = 0;
        net_browser_set(0);
        mp_play(menu);
      }
    }
  }
  strip(in, NAV);
  in->ntouch = 0;
}
static void sb_draw(void) {
  float x0 = 100, x1 = 1180, y0 = 70, y1 = 650;
  sui_color(0, 0, 0, 0.85f); sui_rect(0, 0, SCREEN_W, SCREEN_H);
  panel(x0, y0, x1, y1);
  sui_color(1.0f, 0.86f, 0.45f, 1);
  sui_text_shadow(640 - sui_text_w("Servers", 40) / 2, y0 + 58, "Servers", 40, 0);
  sui_color(0.85f, 0.75f, 0.55f, 1);
  const char *sub = "Matches hosted on your network (searching...)";
  sui_text(640 - sui_text_w(sub, 22) / 2, y0 + 92, sub, 22, 0);
  int n = net_browser_count();
  int first = sb_first_row(n);
  float ry = y0 + 112;
  for (int i = first; i <= n && i < first + 7; i++, ry += 62) {
    float r0 = ry, r1 = ry + 54, cy = (r0 + r1) / 2;
    if (i == sb_sel) gold_frame(x0 + 28, r0, x1 - 28, r1);
    else { sui_color(0.95f, 0.87f, 0.69f, 1); sui_rect(x0 + 28, r0, x1 - 28, r1); }
    sui_color(0.22f, 0.12f, 0.04f, 1);
    if (i == n) { sui_text(x0 + 56, cy + 10, "+ Add a server by address", 28, 0); continue; }
    uint32_t ip; int mode, map; const char *nm;
    net_browser_get(i, &ip, &mode, &map, &nm);
    char b[96];
    struct in_addr a = { .s_addr = ip };
    sui_text(x0 + 56, cy + 10, nm[0] ? nm : "Host", 28, 280);
    snprintf(b, sizeof(b), "%s  \xc2\xb7  %s", mode_name(mode), map_name_idx(map));
    sui_text(x0 + 360, cy + 10, b, 24, 500);
    sui_color(0.45f, 0.30f, 0.12f, 1);
    sui_text(x1 - 60 - sui_text_w(inet_ntoa(a), 20), cy + 9, inet_ntoa(a), 20, 0);
  }
  if (!n) {
    sui_color(0.85f, 0.75f, 0.55f, 1);
    sui_text(x0 + 56, ry + 40, "No servers found yet. PLAY hosts your own; others can then join it.", 22, 0);
  }
  if (sb_msg[0] && armGetSystemTick() < sb_msg_until) {
    sui_color(1.0f, 0.55f, 0.40f, 1);
    sui_text_shadow(640 - sui_text_w(sb_msg, 22) / 2, y1 - 56, sb_msg, 22, 0);
  }
  sui_color(0.95f, 0.88f, 0.70f, 1);
  sui_text_shadow(x0 + 30, y1 - 22, sb_sel == n ? G_A " Add address" : G_A " Join", 24, 0);
  sui_text_shadow(x1 - 30 - sui_text_w(G_B " Back", 24), y1 - 22, G_B " Back", 24, 0);
}

static void mp_change(uint8_t *menu, int row, int dir) {
  if (row == M_MODE) {
    int i = (mp_mode_index(menu) + dir + 3) % 3;
    debugPrintf("[menus] multiplayer: mode %d\n", mp_modes[i]);
    mp_select_mode(menu, mp_modes[i]);
  } else if (row == M_MAP) {
    void *g = mp_grid(menu);
    if (!g) return;
    int pages = 5 + custom_maps_count(); /* any, the four maps, then the custom maps */
    int p = (mp_page(menu) + dir + pages) % pages;
    debugPrintf("[menus] multiplayer: map page %d\n", p);
    grid_set_page(g, p);
  }
}
/* The lobby after a match: the game moves the map page to the played map's index about 90 ms after the
 * lobby opens, one page short (pages are map + 1: page 0 is "any map"), so a Canyon match came back on
 * Bloodwine and a second PLAY started the wrong map (emulator 2026-11-15). The port remembers the map
 * chosen at PLAY (menu +0xb0: 0..3, 4 = any, 5+ custom) and puts the page back to it for the first
 * second after the lobby opens; the player's own presses end that at once. */
static int mp_chosen = -1;
static unsigned long mp_restore_until;
static int mp_play_pending;
static void *mp_play_menu;
static void mp_play(uint8_t *menu) {
  uint8_t *b = *(uint8_t **)(menu + 0x28);
  if (!b) return;
  float *r = (float *)(b + 0xc4);
  debugPrintf("[menus] multiplayer: PLAY (mode %d, map %d), button at %.0f,%.0f\n", *(int *)(menu + 0xb4), *(int *)(menu + 0xb0),
              (r[0] + r[2]) / 2, (r[1] + r[3]) / 2);
  mp_play_menu = menu;
  mp_play_pending = 1; /* released into the lobby's own UpdateUI (mp_update_hook) */
}
static int (*orig_mp_update)(void *, int);
static void *(*mp_inst)(void), *(*mp_ctx)(void *), *(*gs_current)(void);
static uint8_t *(*mp_room)(void *);
static void (*mp_quick)(void *, int);
/* PLAY, as the lobby's own UpdateUI does it for its PLAY button (a released button injected before
 * that UpdateUI was lost: Menu_Base::UpdateUI runs the page first): room info +0x24 mode, +0x28 map,
 * then GS_InitialMultiplayerBase::QuickMatchMultiplayerGame(true). Run after the lobby's UpdateUI,
 * outside its own logic. */
static int mp_update_hook(void *menu, int a1) {
  int r = orig_mp_update(menu, a1);
  if (mp_play_pending) {
    mp_play_pending = 0;
    void *requested = mp_play_menu; mp_play_menu = NULL;
    if (requested != menu || current_menu() != menu) return r;
    uint8_t *m = menu;
    void *mpi = mp_inst ? mp_inst() : NULL, *ctx = mpi ? mp_ctx(mpi) : NULL;
    uint8_t *room = ctx ? mp_room(ctx) : NULL;
    void *gs = gs_current ? gs_current() : NULL;
    if (room && gs && mp_quick && !popup_up()) {
      *(int *)(room + 0x24) = *(int *)(m + 0xb4);
      *(int *)(room + 0x28) = *(int *)(m + 0xb0);
      debugPrintf("[menus] multiplayer: quick match (mode %d, map %d)\n",*(int *)(room + 0x24), *(int *)(room + 0x28));
      mp_chosen = *(int *)(m + 0xb0);
      mp_quick(gs, 1);
    }
  }
  return r;
}
static void mp_input(InputFrame *in, uint8_t *menu) {
  mp_layout();
  if (mp_restore_until) {
    if (frame_no > mp_restore_until || (in->down & (HidNpadButton_L | HidNpadButton_R | HidNpadButton_Left | HidNpadButton_Right)) ||
        in->ntouch) {
      mp_restore_until = 0;
    } else {
      int want = mp_chosen == 4 ? 0 : mp_chosen < 4 ? mp_chosen + 1 : mp_chosen; /* custom map index = its page */
      void *g = mp_grid(menu);
      if (g && want >= 0 && want < 5 + custom_maps_count() && (mp_page(menu) != want || *(int *)(menu + 0xb0) != mp_chosen)) {
        grid_set_page(g, want);
        static void (*on_map_selected)(void *, int);
        if (!on_map_selected) on_map_selected = (void *)so_symbol(port_engine_module(), "_ZN18Menu_MP_CreateHost13onMapSelectedEi");
        if (on_map_selected) on_map_selected(menu, want); /* the menu's map (+0xb0) and the room's follow the page */
        debugPrintf("[menus] lobby back on the map chosen before the match (page %d)\n", want);
      }
    }
  }
  if (sb_open) { sb_input(in, menu); return; }
  /* Footer touches must become actions before the page consumes its touch list. */
  for (int i=0;i<in->ntouch;i++) if (touch_started(in,i)) {
    int x=in->t[i].x,y=in->t[i].y;
    float bottom=m_y0+110+M_ROWS*M_ROW_H+70;
    if(y>=bottom-58 && y<bottom && x>=m_x0 && x<m_x0+M_W) {
      if(x>m_x0+M_W-160) in->down|=HidNpadButton_B;
      else if(x<m_x0+330) in->down|=HidNpadButton_A;
    }
  }
  if (in->down & HidNpadButton_B) { mp_play_pending=0; mp_play_menu=NULL; in->ntouch=0; strip(in,NAV & ~HidNpadButton_B); return; }
  int step = vertical_step(in);
  if (step) msel = (msel + step + M_ROWS) % M_ROWS;
  int h = horizontal_step(in);
  if (h) mp_change(menu, msel, h);
  if (in->down & HidNpadButton_L) mp_change(menu, M_MAP, -1);
  if (in->down & HidNpadButton_R) mp_change(menu, M_MAP, 1);
  if (in->down & HidNpadButton_A) {
    if (msel == M_PLAY) mp_play(menu);
    else if (msel == M_NAME) mp_edit_name();
    else if (msel == M_SERVERS) { sb_open = 1; sb_sel = 0; sb_selected_ip = 0; net_browser_set(1); }
    else mp_change(menu, msel, 1);
  }
  for (int i = 0; i < in->ntouch; i++) {
    int x = in->t[i].x, y = in->t[i].y;
    int r = (int)((y - (m_y0 + 110)) / M_ROW_H);
    if (x >= m_x0 && x <= m_x0 + M_W && y >= m_y0 + 110 && r >= 0 && r < M_ROWS) {
      if (touch_started(in, i)) {
        msel = r;
        if (r == M_PLAY) mp_play(menu);
        else if (r == M_NAME) mp_edit_name();
        else if (r == M_SERVERS) { sb_open = 1; sb_sel = 0; sb_selected_ip = 0; net_browser_set(1); }
        else mp_change(menu, r, x < m_x0 + M_W / 2 + 60 ? -1 : 1); /* left / right half */
      }
    }
    in->t[i] = in->t[--in->ntouch];
    i--;
  }
  strip(in, NAV);
}
static void arrow(float cx, float cy, int dir) {
  for (int k = 0; k < 12; k++) {
    float w = 12 - k;
    float x = cx + dir * (k - 6) * 1.5f;
    sui_rect(x - 1, cy - w, x + 1, cy + w);
  }
}
static void mp_draw(uint8_t *menu) {
  sui_color(0.07f, 0.04f, 0.02f, 1); sui_rect(0, 0, SCREEN_W, SCREEN_H);
  float x0 = m_x0, x1 = m_x0 + M_W, y1 = m_y0 + 110 + M_ROWS * M_ROW_H + 70;
  panel(x0, m_y0, x1, y1);
  sui_color(1.0f, 0.86f, 0.45f, 1);
  const char *t = "Multiplayer";
  sui_text_shadow(640 - sui_text_w(t, 44) / 2, m_y0 + 62, t, 44, 0);
  sui_color(0.85f, 0.75f, 0.55f, 1);
  const char *sub = "Local network: joins a match with these settings, or hosts one";
  sui_text(640 - sui_text_w(sub, 22) / 2, m_y0 + 96, sub, 22, 0);
  const char *mode_l = str(0x8cc) ? str(0x8cc) : "Mode";
  for (int i = 0; i < M_ROWS; i++) {
    float ry0 = m_y0 + 110 + i * M_ROW_H + 8, ry1 = ry0 + M_ROW_H - 16, cy = (ry0 + ry1) / 2;
    if (i == msel) gold_frame(x0 + 28, ry0, x1 - 28, ry1);
    else { sui_color(0.95f, 0.87f, 0.69f, 1); sui_rect(x0 + 28, ry0, x1 - 28, ry1); }
    sui_color(0.22f, 0.12f, 0.04f, 1);
    if (i == M_PLAY) {
      const char *p = "PLAY";
      sui_text(640 - sui_text_w(p, 36) / 2, cy + 13, p, 36, 0);
      continue;
    }
    if (i == M_SERVERS) {
      sui_text(x0 + 56, cy + 11, "Servers", 30, 200);
      const char *v = "Browse the network";
      float vx0 = x0 + 290, vx1 = x1 - 70, vc = (vx0 + vx1) / 2;
      sui_text(vc - sui_text_w(v, 28) / 2, cy + 10, v, 28, vx1 - vx0 - 60);
      continue;
    }
    if (i == M_NAME) {
      sui_text(x0 + 56, cy + 11, "Name", 30, 200);
      const char *nm = mp_name();
      float vx0 = x0 + 290, vx1 = x1 - 70, vc = (vx0 + vx1) / 2;
      sui_text(vc - sui_text_w(nm, 30) / 2, cy + 11, nm, 30, vx1 - vx0 - 60);
      sui_color(0.45f, 0.30f, 0.12f, 1);
      sui_text(vx1 - sui_text_w(G_A " Change", 20) + 30, cy + 9, G_A " Change", 20, 0);
      continue;
    }
    sui_text(x0 + 56, cy + 11, i == M_MODE ? mode_l : "Map", 30, 200);
    const char *v = i == M_MODE ? str(mp_mode_str[mp_mode_index(menu)]) : mp_map_name(mp_page(menu));
    if (!v) v = "?";
    float vx0 = x0 + 290, vx1 = x1 - 70, vc = (vx0 + vx1) / 2;
    sui_text(vc - sui_text_w(v, 30) / 2, cy + 11, v, 30, vx1 - vx0 - 60);
    arrow(vx0, cy, -1);
    arrow(vx1, cy, 1);
    if (i == M_MAP) { /* page dots */
      int pg = mp_page(menu);
      int np = 5 + custom_maps_count(); /* any, the four maps, the custom maps */
      for (int k = 0; k < np; k++) {
        float dx = vc - (np - 1) * 10 + k * 20;
        if (k == pg) sui_color(0.45f, 0.24f, 0.05f, 1); else sui_color(0.70f, 0.58f, 0.40f, 1);
        sui_rect(dx - 4, ry1 - 12, dx + 4, ry1 - 4);
      }
    }
  }
  sui_color(0.95f, 0.88f, 0.70f, 1);
  sui_text_shadow(x0 + 30, y1 - 22, G_A " Select   Left/Right Change", 24, 0);
  float bw = sui_text_w(G_B " Back", 24);
  sui_text_shadow(x1 - 30 - bw, y1 - 22, G_B " Back", 24, 0);
}
/* Under the lobby the menu stack holds Menu_TabScroll (Play / Leagues / Gangs sidebar, black content
 * backdrop), rendered first. Drawn from the lobby's RenderUI the panel stayed under that page's
 * batched sprites; drawn after the frame it covered the game's popups, which then showed the old
 * page under them (owner, hardware 2026-11-14). Now TabScroll draws nothing while the lobby is the
 * current menu, and the lobby's RenderUI draws the panel (dimmed under a popup) and the money bar;
 * the popups are drawn after it. */
static void (*orig_ts_render)(void *, int, int);
static void ts_render_hook(void *menu, int a1, int a2) {
  extern const char *current_menu_class;
  if (enabled && mp_select_mode && sui_ready() && !strcmp(current_menu_class, "18Menu_MP_CreateHost")) return;
  orig_ts_render(menu, a1, a2);
}
/* The multiplayer state renders the menus, then TabScroll, then the popup, then flushes the sprite
 * batch: anything the port draws during the lobby's RenderUI ends up under all of that. So the lobby's
 * RenderUI only notes the frame; the panel is drawn after the engine's frame (menus_ui_overlay). While a
 * popup is up (PLEASE WAIT / CANCEL while searching) the panel is not drawn and TabScroll is hidden, so
 * the popup shows on a plain background instead of over the old page (re-rendering the popup after
 * the frame blacked out the screen). */
static uint8_t *mp_draw_menu;
static unsigned long mp_draw_frame;
/* The lobby's deleting destructor: when the game tears the menus down (joining a LAN room), the
 * current-menu pointer still named the freed lobby for a frame and the port read its map grid from freed
 * memory (crash of the joining emulator, 2026-11-14). The port never touches a lobby once deleted. */
static void *mp_dead;
static unsigned long mp_dead_frame;
/* mp_dead only has to cover the frames in which the current-menu pointer still names the freed lobby.
 * Forget it once the current menu moved on (or after a few frames): otherwise a later menu that the
 * allocator places in the freed chunk (pause, options, map) was treated as dead for good and lost its
 * controller input. */
/* A port screen "opens" when its engine menu becomes current (or is replaced by a new object), not when
 * a popup over it goes away: dismissing a popup used to reset focus to the first item (pause after "No"
 * on MAIN MENU, Options row and language list, lobby back to PLAY). Returns 1 once per real opening, on
 * the first frame the screen is drawn without a popup. */
struct screen_open { int raw_prev, pending; void *obj; };
static int screen_opened(struct screen_open *so, int raw, void *menu, int shown) {
  if (raw && (!so->raw_prev || menu != so->obj)) so->pending = 1;
  so->raw_prev = raw;
  so->obj = raw ? menu : NULL;
  if (!raw) so->pending = 0;
  if (shown && so->pending) { so->pending = 0; return 1; }
  return 0;
}
static void *dead_lobby(void) {
  if (mp_dead && (current_menu() != (uint8_t *)mp_dead || frame_no > mp_dead_frame + 8)) mp_dead = NULL;
  return mp_dead;
}
/* The lobby the game rendered last: the port's input (run before the game's update and render) only
 * touches a lobby rendered in the previous frame, i.e. alive after the game's last update. Joining a room
 * frees the menus while the current-menu pointer still names the lobby; reading it crashed both
 * emulators (2026-11-14). */
static void *mp_seen_menu;
static unsigned long mp_seen_frame;
static void (*orig_mp_dtor)(void *);
static void mp_dtor_hook(void *menu) {
  mp_dead = menu; mp_dead_frame = frame_no;
  debugPrintf("[menus] multiplayer lobby deleted\n");
  orig_mp_dtor(menu);
}
static void mp_render_hook(void *menu, int a1, int a2) {
  if (menu == mp_dead) mp_dead = NULL; /* a new lobby at the same address */
  mp_seen_menu = menu; mp_seen_frame = frame_no;
  if (!enabled || !mp_select_mode || !sui_ready()) { orig_mp_render(menu, a1, a2); return; }
  mp_draw_menu = menu;
  mp_draw_frame = frame_no;
}
void menus_ui_overlay(void) {
  uint8_t *m = mp_draw_menu;
  mp_draw_menu = NULL;
  if (!m || m == mp_dead || mp_draw_frame != frame_no || current_menu() != m || !mp_grid(m)) return;
  if (popup_up()) return; /* the game's popup (PLEASE WAIT / CANCEL) alone; TabScroll's old page is hidden */
  mp_layout();
  sui_begin();
  mp_draw(m);
  if (sb_open) sb_draw();
  sui_end();
}


/* ================================================================== multiplayer sign-in */
/* Menu_MP_Login ("Connect": guest name, Log in, Log in with Facebook) only shows until the first guest
 * sign-in; after that the game signs in by itself (Game::IsAnonymousWasAlreadyLogin). The owner got
 * it with Facebook on the first visit (hardware 2026-11-14). The port presses its Log in once
 * (Menu_MP_Login::TriggerAnonymous, as the button does) and draws a plain panel instead of the page. */
static void (*orig_login_update)(void *, int), (*orig_login_render)(void *, int, int);
static void (*login_trigger)(void *), (*login_exit)(void *);
static unsigned long mp_left_frame; /* the frame the lobby was left */
static void *login_menu;
static unsigned long login_frame;
static int login_update_hook(void *menu, int a1) {
  int r = ((int (*)(void *, int))orig_login_update)(menu, a1);
  if (menu != login_menu || frame_no > login_frame + 30) { /* a new visit */
    login_menu = menu;
    if (popup_up()) login_menu = NULL; /* try again next frame */
    else if (frame_no < mp_left_frame + 30 && login_exit) {
      /* back from the lobby (B): this page again would sign in again or raise its name keyboard
       * (owner, hardware 2026-11-14); leave multiplayer the way its own back does */
      debugPrintf("[menus] multiplayer: back from the lobby, leaving multiplayer\n");
      login_exit(menu);
    } else if (login_trigger) {
      debugPrintf("[menus] multiplayer sign-in: guest login\n");
      login_trigger(menu);
    }
  }
  login_frame = frame_no;
  return r;
}
static void login_render_hook(void *menu, int a1, int a2) {
  if (!enabled || !sui_ready()) { orig_login_render(menu, a1, a2); return; }
  sui_begin();
  sui_color(0.07f, 0.04f, 0.02f, 1); sui_rect(0, 0, SCREEN_W, SCREEN_H);
  panel(340, 260, 940, 460);
  sui_color(1.0f, 0.86f, 0.45f, 1);
  const char *t = "Multiplayer";
  sui_text_shadow(640 - sui_text_w(t, 40) / 2, 320, t, 40, 0);
  sui_color(0.85f, 0.75f, 0.55f, 1);
  const char *m = "Signing in as a guest...";
  sui_text(640 - sui_text_w(m, 26) / 2, 380, m, 26, 0);
  sui_color(0.95f, 0.88f, 0.70f, 1);
  sui_text_shadow(940 - 30 - sui_text_w(G_B " Back", 24), 440, G_B " Back", 24, 0);
  sui_end();
}


/* ================================================================== multiplayer death screen */
/* "Spawn now" costs one multiplayer star; with none the game opened its in-app store (dead in the
 * port) and nothing happened (owner, hardware 2026-11-14). On a LAN with no store it is free: the
 * screen's "already paid for this death" flag (+0x58; while set the revive is not charged) is set
 * before each update. */
static int (*orig_ds_update)(void *, int);
static int ds_update_hook(void *menu, int a1) {
  ((uint8_t *)menu)[0x58] = 1;
  return orig_ds_update(menu, a1);
}

/* ================================================================== journal */
/* Menu_IGM_Quests (the Journal): the game's page is a set of collapsible LayerLists filled from
 * QuestLinker::GetQuestLinkerArray (CURRENT story quest, COLLECTIONS, COMPLETED); the focus frame
 * reached empty list slots and headers (owner, hardware 2026-11-14). The port lists the same quests in
 * the same three groups (Menu_IGM_Quests::SetState's filters), shows the selected one through the
 * game's own SetQuestForDescription (it also decides the story / stop buttons), and draws name,
 * description, objectives and progress itself. X plays the story comic as the page's button does
 * (new GS_Comics(id); Init; GameState::SetState), R part 2 where there is one (page object 0x1c),
 * Y "stop quest" presses the page's own button 0x16 (its question popup), B is the game's Back. */
typedef struct { void **b, **e; } vec_t;
static vec_t *(*ql_array)(void);
static int (*ql_started)(void *), (*ql_type)(void *), (*ql_showed)(void *), (*ql_iter)(void *), (*ql_finished)(void *);
static int (*ql_name)(void *), (*ql_desc)(void *), (*ql_comics)(void *), (*ql_winmsg)(void *), (*ql_total)(void *), (*ql_step)(void *);
static void *(*ql_quest)(void *), *(*q_story)(void), *(*q_linker)(void *);
static vec_t *(*q_steps)(void *);
static int (*qs_show)(void *), (*qs_text)(void *), (*qs_success)(void *), (*qs_active)(void *);
static void (*jq_set_desc)(void *, void *);
static void **jq_desc_var;
static void (*orig_jq_render)(void *, int, int);
static void (*comics_ctor)(void *, int), (*comics_init)(void *), (*state_set)(void *);
static void *(*op_new)(size_t);

enum { J_MAX = 96 };
typedef struct { int head; int text; void *ql; int cat; } jrow;
static jrow jrows[J_MAX];
static int njrows, jsel = -1, jscroll, journal_on;
static void *jshown; /* the quest whose description is set */
static float j_detail_scroll, j_detail_max;
static int j_detail_finger=-1, j_detail_y;
static float j_detail_start;
/* Optional emulator fixture: exercise overflow/localization without editing game assets or saves. */
static const char *journal_fixture(void) {
  static int checked;
  static char fixture[8193];
  if(!checked) {
    checked=1;
    FILE *em=fopen(PORT_ROOT "/emulator_mode","r");
    if(em) {
      fclose(em);FILE *f=fopen(PORT_ROOT "/test_journal_text","rb");
      if(f) {size_t n=fread(fixture,1,sizeof(fixture)-1,f);fixture[n]=0;fclose(f);
        debugPrintf("[menus] journal text fixture: %zu bytes (emulator only)\n",n);}
    }
  }
  return *fixture ? fixture:NULL;
}

static int j_is_side(void *q) { int t = ql_type(q); return t < 0 || t > 6 || !((1u << t) & 0x61); } /* not type 0, 5, 6 */
static void j_build(void) {
  njrows = 0;
  vec_t *v = ql_array ? ql_array() : NULL;
  if (!v) return;
  void *sq = q_story ? q_story() : NULL, *sl = sq && q_linker ? q_linker(sq) : NULL;
  if (sl && njrows < J_MAX - 2) {
    jrows[njrows++] = (jrow){ 1, 0x241, NULL, 0x241 };
    jrows[njrows++] = (jrow){ 0, ql_name(sl), sl, 0x241 };
  }
  int any = 0;
  for (void **it = v->b; it < v->e; it++) any |= *it && (ql_started(*it) & 1) && j_is_side(*it) && (ql_showed(*it) & 1);
  if (any && njrows < J_MAX) {
    jrows[njrows++] = (jrow){ 1, 0x242, NULL, 0x242 };
    for (void **it = v->b; it < v->e && njrows < J_MAX; it++)
      if (*it && (ql_started(*it) & 1) && j_is_side(*it) && (ql_showed(*it) & 1)) jrows[njrows++] = (jrow){ 0, ql_name(*it), *it, 0x242 };
  }
  any = 0;
  for (void **it = v->b; it < v->e; it++) any |= *it && (ql_iter(*it) > 0 || (ql_finished(*it) & 1)) && (ql_showed(*it) & 1);
  if (any && njrows < J_MAX) {
    jrows[njrows++] = (jrow){ 1, 0x234, NULL, 0x234 };
    for (void **it = v->b; it < v->e && njrows < J_MAX; it++)
      if (*it && (ql_iter(*it) > 0 || (ql_finished(*it) & 1)) && (ql_showed(*it) & 1)) jrows[njrows++] = (jrow){ 0, ql_name(*it), *it, 0x234 };
  }
}
static void j_show(uint8_t *menu, int i) {
  if (i < 0 || i >= njrows || jrows[i].head) return;
  jsel = i;
  if (jshown != jrows[i].ql || *(int *)(menu + 0x28) != jrows[i].cat) {
    j_detail_scroll=j_detail_max=0;
    jshown = jrows[i].ql;
    *(int *)(menu + 0x28) = jrows[i].cat; /* the group decides objectives vs. completion text */
    jq_set_desc(menu, jrows[i].ql);
  }
}
static int j_next(int from, int dir) {
  for (int i = from + dir; i >= 0 && i < njrows; i += dir) if (!jrows[i].head) return i;
  return from;
}
static void *j_page_obj(uint8_t *menu, int id) { uint8_t *page = *(uint8_t **)(menu + 0x8); return page ? get_iobj(page, id) : NULL; }
static void j_comic(int id) {
  if (id <= 0 || !op_new || !comics_ctor || !comics_init || !state_set) return;
  debugPrintf("[menus] journal: story comic %d\n", id);
  void *c = op_new(0x70);
  if (!c) return;
  comics_ctor(c, id);
  comics_init(c);
  state_set(c);
}
enum { J_RH = 40, J_LX0 = 60, J_LX1 = 470, J_TOP = 110, J_VIS = 13 };
static void journal_input(InputFrame *in, uint8_t *menu) {
  void *selected = jsel >= 0 && jsel < njrows ? jrows[jsel].ql : NULL;
  int selected_cat = jsel >= 0 && jsel < njrows ? jrows[jsel].cat : 0;
  j_build();
  jsel = -1;
  for (int i = 0; i < njrows; i++)
    if (!jrows[i].head && jrows[i].ql == selected && jrows[i].cat == selected_cat) { jsel = i; break; }
  if (!njrows) { jshown = NULL; jscroll = 0; strip(in, NAV); in->ntouch = 0; return; }
  if (jsel < 0 || jsel >= njrows || jrows[jsel].head) {
    int pick = -1;
    void *cur = jq_desc_var ? *jq_desc_var : NULL;
    for (int i = 0; i < njrows; i++) if (!jrows[i].head && (pick < 0 || jrows[i].ql == cur)) pick = i;
    jshown = cur;
    if (pick >= 0) j_show(menu, pick);
  }
  int step = vertical_step(in);
  if (step) j_show(menu, j_next(jsel, step));
  if (jsel >= 0) j_show(menu, jsel);
  if (jsel < jscroll + 1) jscroll = jsel - 1;
  if (jsel > jscroll + J_VIS - 2) jscroll = jsel - J_VIS + 2;
  if (jscroll > njrows - J_VIS) jscroll = njrows - J_VIS;
  if (jscroll < 0) jscroll = 0;
  if (in->down & HidNpadButton_ZR) j_detail_scroll+=240;
  if (in->down & HidNpadButton_ZL) j_detail_scroll-=240;
  if (abs(in->rs.y)>12000) j_detail_scroll-=in->rs.y/32767.0f*9;
  int detail_seen=0;
  for (int i=0;i<in->ntouch;i++) {
    int x=in->t[i].x,y=in->t[i].y;
    if (touch_started(in,i)) {
      if (y>=644 && y<690) {
        if (x>=1100) in->down|=HidNpadButton_B;
        else if (x>=890 && x<1060) in->down|=HidNpadButton_Y;
        else if (x>=700 && x<870) in->down|=HidNpadButton_R;
        else if (x>=550 && x<690) in->down|=HidNpadButton_X;
      } else if (x>=512 && x<1230 && y>=110 && y<600 && j_detail_finger<0) {
        j_detail_finger=in->t[i].id; j_detail_y=y; j_detail_start=j_detail_scroll;
      }
    }
    if (in->t[i].id==j_detail_finger) { detail_seen=1; j_detail_scroll=j_detail_start+j_detail_y-y; }
  }
  if (!detail_seen) j_detail_finger=-1;
  j_detail_scroll=fminf(j_detail_max,fmaxf(0,j_detail_scroll));
  if(in->down & (HidNpadButton_ZL|HidNpadButton_ZR))
    debugPrintf("[menus] journal details %.0f / %.0f\n",j_detail_scroll,j_detail_max);
  int comics = jshown && ql_comics ? ql_comics(jshown) : 0;
  if ((in->down & HidNpadButton_X) && comics) j_comic(comics);
  if ((in->down & HidNpadButton_R) && comics && vis(j_page_obj(menu, 0x1c))) j_comic(comics + 1);
  if (in->down & HidNpadButton_Y) {
    uint8_t *stop = j_page_obj(menu, 0x16);
    if (vis(stop)) { float *r = (float *)(stop + 0xc4); debugPrintf("[menus] journal: stop quest\n"); tap((r[0] + r[2]) / 2, (r[1] + r[3]) / 2); }
  }
  for (int i = 0; i < in->ntouch; i++) {
    int x = in->t[i].x, y = in->t[i].y;
    int r = jscroll + (int)((y - J_TOP) / J_RH);
    if (x >= J_LX0 && x <= J_LX1 && y >= J_TOP && r >= 0 && r < njrows && r < jscroll + J_VIS) j_show(menu, r);
    in->t[i] = in->t[--in->ntouch];
    i--;
  }
  strip(in, NAV & ~(u64)HidNpadButton_A);
  in->down &= ~(u64)HidNpadButton_A; in->held &= ~(u64)HidNpadButton_A;
}
/* word-wrapped text; returns the y after the last line */
static float j_wrap(float x,float y,float w,const char *s,int px,float lh) {
  return sui_wrap(x,y,w,s,px,lh,124,584);
}
static void journal_draw(uint8_t *menu) {
  sui_color(0.07f, 0.04f, 0.02f, 1); sui_rect(0, 0, SCREEN_W, SCREEN_H);
  panel(40, 30, 1240, 690);
  sui_color(1.0f, 0.86f, 0.45f, 1);
  const char *title = "Journal";
  sui_text_shadow(640 - sui_text_w(title, 40) / 2, 82, title, 40, 0);
  sui_color(0.45f, 0.30f, 0.12f, 1); sui_rect(484, 104, 488, 640); /* divider */
  if (!njrows) {
    sui_color(0.95f, 0.88f, 0.70f, 1);
    sui_text(J_LX0 + 10, J_TOP + 30, "No quests yet", 26, 0);
  }
  for (int k = 0; k < J_VIS && jscroll + k < njrows; k++) {
    jrow *r = &jrows[jscroll + k];
    float y0 = J_TOP + k * J_RH, y1 = y0 + J_RH - 4;
    const char *t = str(r->text);
    if (r->head) {
      sui_color(1.0f, 0.86f, 0.45f, 1);
      sui_text_shadow(J_LX0 + 4, y1 - 8, t ? t : "?", 24, J_LX1 - J_LX0);
      continue;
    }
    if (jscroll + k == jsel) gold_frame(J_LX0, y0, J_LX1, y1);
    else { sui_color(0.95f, 0.87f, 0.69f, 1); sui_rect(J_LX0, y0, J_LX1, y1); }
    sui_color(0.22f, 0.12f, 0.04f, 1);
    sui_text(J_LX0 + 14, y1 - 10, t ? t : "?", 24, J_LX1 - J_LX0 - 24);
  }
  /* description of the selected quest */
  void *q = jshown;
  int cat = jsel >= 0 && jsel < njrows ? jrows[jsel].cat : 0;
  float x = 512, w = 700, y = 140-j_detail_scroll;
  if (q) {
    const char *nm = str(ql_name(q)), *ds = str(ql_desc(q));
    const char *fixture=journal_fixture();if(fixture) ds=fixture;
    sui_color(1.0f, 0.86f, 0.45f, 1);
    if (y>=124 && y<=584) sui_text_shadow(x, y, nm ? nm : "", 32, w);
    y += 44;
    sui_color(0.95f, 0.88f, 0.70f, 1);
    if (ds) y = j_wrap(x, y, w, ds, 24, 30);
    y += 14;
    void *quest = ql_quest ? ql_quest(q) : NULL;
    if (quest && (cat == 0x241 || cat == 0x242) && q_steps) {
      vec_t *st = q_steps(quest);
      for (int pass = 0; st && pass < 2; pass++)
        for (void **it = st->b; it < st->e; it++) {
          if (!*it || !(qs_show(*it) & 1)) continue;
          int done = qs_success(*it) & 1, act = qs_active(*it) & 1;
          if ((pass == 0 && !done) || (pass == 1 && (!act || done))) continue;
          const char *o = str(qs_text(*it));
          if (!o) continue;
          char b[400];
          snprintf(b, sizeof(b), "%s  %s", done ? "[done]" : "[ ]", o);
          if (done) sui_color(0.55f, 0.85f, 0.45f, 1); else sui_color(0.95f, 0.88f, 0.70f, 1);
          y = j_wrap(x, y, w, b, 24, 30);
        }
    } else if (cat == 0x234 && ql_winmsg) {
      const char *m = str(ql_winmsg(q));
      sui_color(0.95f, 0.88f, 0.70f, 1);
      if (m) y = j_wrap(x, y, w, m, 24, 30);
    }
    j_detail_max=fmaxf(0,y+j_detail_scroll-584);
    if(j_detail_max>0) {
      float h=fmaxf(24,460*460/(460+j_detail_max));
      float top=124+(460-h)*fminf(1,j_detail_scroll/j_detail_max);
      sui_color(.35f,.24f,.10f,1);sui_rect(1220,124,1224,584);
      sui_color(.95f,.75f,.35f,1);sui_rect(1220,top,1224,top+h);
    }
    int total = ql_total ? ql_total(q) : 0, t = ql_type(q);
    if (total > 1 && (t == 0 || t == 5 || t == 6 || t == 1 || t == 2)) {
      int cur = ql_step(q) + (cat == 0x241 && (t == 0 || t == 5 || t == 6) ? 1 : 0);
      char b[64];
      snprintf(b, sizeof(b), "Progress: %d / %d", cur > total ? total : cur, total);
      sui_color(1.0f, 0.86f, 0.45f, 1);
      sui_text(x, 620, b, 26, 0);
    }
  }
  /* buttons */
  int comics = q && ql_comics ? ql_comics(q) : 0;
  sui_color(0.95f, 0.88f, 0.70f, 1);
  sui_text_shadow(70,672,"D-pad Quests   ZL/ZR Details",22,460);
  if (comics) sui_text_shadow(550,672,G_X " Story",22,140);
  if (comics && vis(j_page_obj(menu,0x1c))) sui_text_shadow(700,672,G_R " Part 2",22,170);
  if (vis(j_page_obj(menu,0x16))) sui_text_shadow(890,672,G_Y " Stop quest",22,170);
  sui_text_shadow(1240 - 30 - sui_text_w(G_B " Back", 24), 672, G_B " Back", 24, 0);
}
static void jq_render_hook(void *menu, int a1, int a2) {
  if (!enabled || !sui_ready() || !ql_array) { orig_jq_render(menu, a1, a2); return; } /* popups are drawn after it */
  sui_begin();
  journal_draw(menu);
  sui_end();
}

/* ================================================================== per frame */
int menus_ui_pause_active(void) { return pause_on; }
int menus_ui_options_active(void) { return opts_on; }

/* Before the pointer and the engine see the input (main.c, after shop_ui_input). */
void menus_ui_input(InputFrame *in) {
  frame_no++;
  tap_frame();
  if (!enabled) return;
  extern const char *current_menu_class;
  uint8_t *menu = current_menu();
  /* social.c's class name is sampled before the engine renders; a queued pop can replace the
   * object during that render. Never cast the new object using the previous frame's class. */
  const char *menu_class = menu && menu != (uint8_t *)dead_lobby() ? cls(menu) : "";
  int p = !strcmp(menu_class, "13Menu_IGM_Main") && !popup_up();
  int o = !strcmp(menu_class, "16Menu_IGM_Options") && !popup_up();
  static struct screen_open p_open, o_open, mp_open, jq_open;
  if (screen_opened(&p_open, !strcmp(menu_class, "13Menu_IGM_Main"), menu, p)) {
    psel = 0; rep_dir = 0; /* always opens on the first item (SHOP) */
    static int logged;
    if (!logged++) {
      int n = read_pause_items(menu);
      for (int i = 0; i < n; i++)
        debugPrintf("[menus] pause item %d: text %d \"%s\" at %.0f,%.0f\n", i, btn_text_id(pitems[i].btn), pitems[i].label,
                    pitems[i].r[0], pitems[i].r[1]);
    }
  }
  if (screen_opened(&o_open, !strcmp(menu_class, "16Menu_IGM_Options"), menu, o)) {
    rep_dir = hrep_dir = 0;
    osel = 0; lang_open = 0; osub = 0;
    ach_finger = -1;
    slider_finger = -1;
    lang_pending_item = NULL;
    ovals[0] = ovals[1] = ovals[2] = -1; /* read the current settings again */
    for (int r = 0; r <= O_AIM; r++) {
      uint8_t *sl = o_slider(menu, r);
      if (sl) debugPrintf("[menus] options slider %d: %.0f%% of %.0f..%.0f\n", r, *(float *)(sl + 0x64), *(float *)(sl + 0x44),
                          *(float *)(sl + 0x48));
    }
  }
  if (o && !opts_on) ovals[0] = ovals[1] = ovals[2] = -1; /* a popup may have changed a setting */
  int mp = !strcmp(menu_class, "18Menu_MP_CreateHost") && menu && menu != (uint8_t *)mp_dead && !popup_up() && mp_select_mode &&
           menu == (uint8_t *)mp_seen_menu && mp_seen_frame + 1 >= frame_no;
  if (screen_opened(&mp_open, !strcmp(menu_class, "18Menu_MP_CreateHost"), menu, mp)) {
    rep_dir = hrep_dir = 0;
    static int dumped;
    void *mm = mpmap_inst ? mpmap_inst() : NULL;
    for (int k = 0; mm && !dumped && k < 5; k++) {
      int *q = (int *)mpmap_info(mm, k);
      if (q) debugPrintf("[menus] mp map %d: %s | %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", k, str(q[2]) ? str(q[2]) : "?", q[0], q[1], q[2], q[3],
                         q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13]);
    }
    dumped = 1;
    msel = M_PLAY;debugPrintf("[menus] multiplayer lobby: mode %d, map page %d\n", *(int *)(menu + 0xb4), mp_page(menu));
    if (mp_chosen >= 0) mp_restore_until = frame_no + 60; }
  extern int cutscene_comic; /* a story comic plays over the Journal: its hold-A skip needs A */
  int jq = !strcmp(menu_class, "15Menu_IGM_Quests") && menu && !popup_up() && ql_array && !cutscene_comic;
  if (screen_opened(&jq_open, !strcmp(menu_class, "15Menu_IGM_Quests"), menu, jq)) { jsel = -1; jshown = NULL; jscroll = 0; rep_dir = hrep_dir = 0; }
  journal_on = jq;
  if (jq) journal_input(in, menu);
  if (mp_on && !mp) { mp_left_frame = frame_no; if (sb_open) { sb_open = 0; net_browser_set(0); } }
  mp_on = mp;
  if (mp) mp_input(in, menu);
  if (p != pause_on || o != opts_on) debugPrintf("[menus] pause %d options %d\n", p, o);
  pause_on = p;
  opts_on = o;
  if (!o) lang_pending_item = NULL; /* a queued language button belongs to this Options page */
  if (p) pause_input(in, menu);
  if (o) opts_input(in, menu);
}

static int patch_slot(so_module *mod, const char *vt_name, const char *fn_name, void *hook, void **orig) {
  uintptr_t vt = so_symbol(mod, vt_name), fn = so_symbol(mod, fn_name);
  if (!vt || !fn || !mod->image) return 0;
  uintptr_t *slots = (uintptr_t *)(mod->image + (vt - mod->base));
  int n = 0;
  for (int i = 2; i < 40; i++) if (slots[i] == fn) { slots[i] = (uintptr_t)hook; n++; }
  if (n) *orig = (void *)fn;
  return n;
}

int menus_ui_install(so_module *mod) {
  FILE *f = fopen(PORT_ROOT "/old_menus", "r");
  if (f) { fclose(f); debugPrintf("[menus] off (old_menus marker)\n"); return -1; }
#define S(v, n) if (!(*(void **)&v = (void *)so_symbol(mod, n))) { debugPrintf("[menus] missing %s, off\n", n); return -1; }
  S(menus_inst, "_ZN9SingletonI9MGR_MenusE11GetInstanceEb");
  S(cur_menu, "_ZNK9MGR_Menus21GetCurrentPlayingMenuEv");
  S(get_iobj, "_ZN9BasicPage15GetInterfaceObjEi");
  S(btn_text_id, "_ZN11LayerButton9GetTextIDEv");
  S(str_inst, "_ZN9SingletonI9StringMgrE11GetInstanceEb");
  S(get_string, "_ZN9StringMgr9GetStringEi");
  S(loaded_lang, "_ZNK9StringMgr17GetLoadedLanguageEv");
  S(popup_inst, "_ZN9SingletonI5PopupE11GetInstanceEb");
  S(popup_enabled, "_ZN5Popup9IsEnabledEv");
  S(gp_inst, "_ZN9SingletonI11GS_GamePlayE11GetInstanceEb");
  S(gp_render, "_ZN11GS_GamePlay14RenderGameplayEv");
  S(pc_inst, "_ZN10PlayerCtrl11GetInstanceEi");
  S(pc_render_info, "_ZN10PlayerCtrl16RenderPlayerInfoEb");
  S(slider_set, "_ZN13LayerSlideBar8SetValueEf");
  S(slider_get, "_ZN13LayerSlideBar8GetValueEv");
  S(push_menu, "_ZN9MGR_Menus8PushMenuEi");
  S(grid_center, "_ZN9LayerGrid12CenterScrollEP15InterfaceObject");
  S(add_released, "_ZN15ButtonActionMgr17AddReleasedButtonEP11LayerButton");
  S(ls_inst, "_ZN9SingletonI13LoadingScreenE11GetInstanceEb");
  S(ls_render_bg, "_ZN13LoadingScreen6RenderEv");
  S(ls_active, "_ZNK13LoadingScreen13IsActivePhaseEv");
#undef S
  /* the multiplayer lobby is optional: without these the game's own screen stays */
  mp_select_mode = (void *)so_symbol(mod, "_ZN18Menu_MP_CreateHost13SelectMapModeEi");
  mp_inst = (void *)so_symbol(mod, "_ZN11Multiplayer11GetInstanceEv");
  mp_ctx = (void *)so_symbol(mod, "_ZN11Multiplayer10GetContextEv");
  mp_room = (void *)so_symbol(mod, "_ZN18MultiplayerContext11GetRoomInfoEv");
  gs_current = (void *)so_symbol(mod, "_ZN9GameState15GetCurrentStateEv");
  mp_quick = (void *)so_symbol(mod, "_ZN25GS_InitialMultiplayerBase25QuickMatchMultiplayerGameEb");
  if (!mp_inst || !mp_ctx || !mp_room || !gs_current || !mp_quick) mp_select_mode = NULL;
  grid_get_page = (void *)so_symbol(mod, "_ZN9LayerGrid14GetCurrentPageEv");
  grid_set_page = (void *)so_symbol(mod, "_ZN9LayerGrid14SetCurrentPageEi");
  mpmap_inst = (void *)so_symbol(mod, "_ZN12MPMapManager4InstEv");
  mpmap_info = (void *)so_symbol(mod, "_ZNK12MPMapManager10GetMapInfoEi");
  if (!mp_select_mode || !grid_get_page || !grid_set_page || !mpmap_inst || !mpmap_info ||
      !patch_slot(mod, "_ZTV14Menu_TabScroll", "_ZN14Menu_TabScroll8RenderUIEii", ts_render_hook, (void **)&orig_ts_render) ||
      !patch_slot(mod, "_ZTV18Menu_MP_CreateHost", "_ZN18Menu_MP_CreateHost8RenderUIEii", mp_render_hook, (void **)&orig_mp_render) ||
      !patch_slot(mod, "_ZTV18Menu_MP_CreateHost", "_ZN18Menu_MP_CreateHost8UpdateUIEi", mp_update_hook, (void **)&orig_mp_update))
    mp_select_mode = NULL;
  if (mp_select_mode && !patch_slot(mod, "_ZTV18Menu_MP_CreateHost", "_ZN18Menu_MP_CreateHostD0Ev", (void *)mp_dtor_hook, (void **)&orig_mp_dtor))
    debugPrintf("[menus] lobby destructor not found\n");
  debugPrintf("[menus] multiplayer lobby %s\n", mp_select_mode ? "replaced" : "kept");
  login_trigger = (void *)so_symbol(mod, "_ZN13Menu_MP_Login16TriggerAnonymousEv");
  login_exit = (void *)so_symbol(mod, "_ZN13Menu_MP_Login4ExitEv");
  game_inst = (void *)so_symbol(mod, "_ZN9SingletonI4GameE11GetInstanceEb");
  game_set_guest = (void *)so_symbol(mod, "_ZN4Game16SetGuestUserNameERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE");
  game_save_common = (void *)so_symbol(mod, "_ZN4Game10SaveCommonEv");
  {
#define J(v, n) *(void **)&v = (void *)so_symbol(mod, n); ok &= v != NULL;
    int ok = 1;
    J(ql_array, "_ZN11QuestLinker19GetQuestLinkerArrayEv");
    J(ql_started, "_ZN11QuestLinker9IsStartedEv");
    J(ql_type, "_ZN11QuestLinker7GetTypeEv");
    J(ql_showed, "_ZNK11QuestLinker17IsShowedInJournalEv");
    J(ql_iter, "_ZN11QuestLinker24GetCurrentIterationLevelEv");
    J(ql_finished, "_ZN11QuestLinker10IsFinishedEv");
    J(ql_name, "_ZN11QuestLinker13GetNameTextIDEv");
    J(ql_desc, "_ZN11QuestLinker20GetDescriptionTextIDEv");
    J(ql_quest, "_ZN11QuestLinker8GetQuestEv");
    J(ql_comics, "_ZNK11QuestLinker19getAssociatedComicsEv");
    J(ql_winmsg, "_ZN11QuestLinker25GetCustomWinMessageTextIDEv");
    J(ql_total, "_ZN11QuestLinker13GetTotalStepsEv");
    J(ql_step, "_ZN11QuestLinker14GetCurrentStepEv");
    J(q_story, "_ZN5Quest20GetCurrentStoryQuestEv");
    J(q_linker, "_ZN5Quest9GetLinkerEv");
    J(q_steps, "_ZN5Quest13GetQuestStepsEv");
    J(qs_show, "_ZN9QuestStep26ShowObjetiveInQuestJournalEv");
    J(qs_text, "_ZN9QuestStep18GetObjectiveTextIDEv");
    J(qs_success, "_ZN9QuestStep9IsSuccessEv");
    J(qs_active, "_ZN9QuestStep8IsActiveEv");
    J(jq_set_desc, "_ZN15Menu_IGM_Quests22SetQuestForDescriptionEP11QuestLinker");
    J(jq_desc_var, "_ZN15Menu_IGM_Quests18s_questDescriptionE");
    J(comics_ctor, "_ZN9GS_ComicsC1Ei");
    J(comics_init, "_ZN9GS_Comics4InitEv");
    J(state_set, "_ZN9GameState8SetStateEPS_");
    J(op_new, "_Znwm");
#undef J
    if (!ok || !patch_slot(mod, "_ZTV15Menu_IGM_Quests", "_ZN15Menu_IGM_Quests8RenderUIEii", jq_render_hook, (void **)&orig_jq_render)) {
      ql_array = NULL;
      debugPrintf("[menus] journal kept (symbols missing)\n");
    } else debugPrintf("[menus] journal replaced\n");
  }
  ach_inst = (void *)so_symbol(mod, "_ZN9SingletonI13AchivementMgrE11GetInstanceEb");
  ach_get = (void *)so_symbol(mod, "_ZNK13AchivementMgr3GetEj");
  ach_depr = (void *)so_symbol(mod, "_ZN13AchivementMgr12IsDeprecatedEj");
  if (patch_slot(mod, "_ZTV19Menu_MP_DeathScreen", "_ZN19Menu_MP_DeathScreen8UpdateUIEi", (void *)ds_update_hook, (void **)&orig_ds_update))
    debugPrintf("[menus] multiplayer death screen: spawn now is free\n");
  if (login_trigger &&
      patch_slot(mod, "_ZTV13Menu_MP_Login", "_ZN13Menu_MP_Login8UpdateUIEi", (void *)login_update_hook, (void **)&orig_login_update) &&
      patch_slot(mod, "_ZTV13Menu_MP_Login", "_ZN13Menu_MP_Login8RenderUIEii", login_render_hook, (void **)&orig_login_render))
    debugPrintf("[menus] multiplayer sign-in page replaced\n");
  int a = patch_slot(mod, "_ZTV13Menu_IGM_Main", "_ZN13Menu_IGM_Main8RenderUIEii", pause_render_hook, (void **)&orig_pause_render);
  int b = patch_slot(mod, "_ZTV16Menu_IGM_Options", "_ZN16Menu_IGM_Options8RenderUIEii", opts_render_hook, (void **)&orig_opts_render);
  int c = patch_slot(mod, "_ZTV18Menu_LoadingScreen", "_ZN18Menu_LoadingScreen8RenderUIEii", ls_render_hook, (void **)&orig_ls_render);
  int d = patch_slot(mod, "_ZTV23Page_LanguageButtonItem", "_ZN23Page_LanguageButtonItem6UpdateEiii", lang_update_hook, (void **)&orig_lang_update);
  debugPrintf("[menus] pause %d, options %d, loading %d, language %d slot(s) replaced\n", a, b, c, d);
  if (!a || !b || !c || !d) { debugPrintf("[menus] a slot is missing: off\n"); return -1; }
  enabled = 1;
  return 0;
}
int menus_ui_enabled(void) { return enabled; }
int menus_ui_mp_enabled(void) { return enabled && mp_select_mode != NULL; }
int menus_ui_mp_active(void) { return mp_on; }
int menus_ui_journal_active(void) { return journal_on; }
int port_map_popup_up(void) { return popup_up(); }
void *menus_ui_live_menu(void) { void *m=current_menu(); return m && m==dead_lobby() ? NULL:m; }

/* For net.c's browser: whether this game is hosting a match, and what. */
int menus_mp_host_info(int *mode, int *map, char *name, int n) {
  static void *(*mpi)(void);
  static int (*is_host)(void *), (*is_enabled)(void *);
  if (!mpi) {
    mpi = (void *)so_symbol(port_engine_module(), "_ZN11Multiplayer11GetInstanceEv");
    is_host = (void *)so_symbol(port_engine_module(), "_ZN11Multiplayer6IsHostEv");
    is_enabled = (void *)so_symbol(port_engine_module(), "_ZN11Multiplayer9IsEnabledEv");
  }
  void *m = mpi ? mpi() : NULL;
  if (!m || !is_host || !is_enabled || !(is_enabled(m) & 1) || !(is_host(m) & 1)) return 0;
  {
    /* Answer the server browser only once the host is in the match itself: a join while the host was still
     * loading crashed it (owner, 2026-11-15; net.c free_cam_hook also guards the crash itself). */
    extern const char *current_menu_class;
    static void *(*cur_state)(void), *(*gp_inst)(int), *(*lvl)(int), *(*lvl_player)(void *);
    if (!cur_state) {
      cur_state = (void *)so_symbol(port_engine_module(), "_ZN9GameState15GetCurrentStateEv");
      gp_inst = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI11GS_GamePlayE11GetInstanceEb");
      lvl = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI9GameLevelE11GetInstanceEb");
      lvl_player = (void *)so_symbol(port_engine_module(), "_ZN9GameLevel9GetPlayerEv");
    }
    void *gp = gp_inst ? gp_inst(0) : NULL, *level = lvl ? lvl(0) : NULL;
    if (!gp || !cur_state || cur_state() != gp || !level || !lvl_player || !lvl_player(level) ||
        strstr(current_menu_class, "LoadingScreen"))
      return 0;
  }
  void *ctx = mp_ctx ? mp_ctx(m) : NULL;
  uint8_t *room = ctx && mp_room ? mp_room(ctx) : NULL;
  *mode = room ? *(int *)(room + 0x24) : 3;
  *map = room ? *(int *)(room + 0x28) : 4;
  snprintf(name, n, "%s", mp_name());
  return 1;
}

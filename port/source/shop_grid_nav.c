/* Shop D-pad paging: the item the page flips to gets selected.
 *
 * AndroidController::GridWidget::right/left (the shop's item grid under the Android controller
 * scheme) move to the neighbouring widget. When that neighbour is outside the grid's area they
 * scroll by faking a swipe (TouchSystem press/drag/release one cell wide) and only set it as the
 * current widget: unlike ControllableMenuSystem::toRight they never call its highlight(true),
 * which is what selects an item (Widget::highlight taps the widget's centre). So after a page flip
 * nothing was selected (no frame, old description), and the next press moved on from the unseen
 * first item: "flipping pages skips the first gun" (owner, hardware 2026-11-13). On the last page
 * the item was never selected at all.
 *
 * The port wraps both vtable slots (GridWidget and GridWidgetImp): after the original, if the new
 * current widget lies outside the grid (the engine's own swipe condition), it waits until the
 * scroll has brought it inside the grid and its position is steady, then calls
 * GridWidget::highlight(true) (tap on the current widget), as the normal move does.
 * Marker `no_shop_page_fix` keeps the original behaviour. */
#include <stdint.h>
#include <stdio.h>
#include "config.h"
#include "so_util.h"
#include "util.h"

typedef int (*int_fn)(void *);
static void (*orig_right)(void *), (*orig_left)(void *);
static void (*grid_highlight)(void *, int);
static uint8_t *pending;
static int pending_frames, steady, last_x = -100000;
static unsigned long fixes;

static void *current_widget(uint8_t *gw) { return *(void **)(gw + 0xa8); } /* CMS at +0xa0, weak_ptr px at +8 */
/* Engine image range: a vtable and the method it names must both lie inside it. The weapon upgrade
 * view's grid holds widgets whose slot +0x40 is empty: D-pad Right there jumped to address 0
 * (Switch crash 2026-11-15: ControllableMenuSystem::toRight -> GridWidget right hook -> after_move). */
static uintptr_t eng_lo, eng_hi;
static int in_engine(const void *p) { return (uintptr_t)p >= eng_lo && (uintptr_t)p < eng_hi; }
static int vcall_ok(void *o, int off, int *out) {
  if (!o || !eng_lo) return 0;
  uint8_t *vt = *(uint8_t **)o;
  if (!in_engine(vt)) return 0;
  int_fn f = *(int_fn *)(vt + off);
  if (!in_engine((void *)f)) return 0;
  *out = f(o);
  return 1;
}
#define X 0x40
#define Y 0x48
#define H 0x50
#define W 0x58

/* 1 outside, 0 inside, -1 not a positioned widget (leave it to the engine) */
static int outside(uint8_t *gw, void *cw) {
  int gx, gwid, cx, cwid;
  if (!vcall_ok(gw, X, &gx) || !vcall_ok(gw, W, &gwid) || !vcall_ok(cw, X, &cx) || !vcall_ok(cw, W, &cwid)) {
    static int logged;
    if (!logged++) debugPrintf("[shop] page fix: focused widget has no position methods (upgrade view?); left to the engine\n");
    return -1;
  }
  return cx + cwid > gx + gwid || cx < gx;
}

static void after_move(uint8_t *gw) {
  void *cw = current_widget(gw);
  if (!cw || !grid_highlight) return;
  if (outside(gw, cw) == 1) {
    pending = gw;
    pending_frames = 0;
    steady = 0;
    last_x = -100000;
  }
}

static void right_hook(void *gw) { orig_right(gw); after_move(gw); }
static void left_hook(void *gw) { orig_left(gw); after_move(gw); }

/* Main loop, once per frame after the engine's frame. */
void shop_grid_nav_frame(void) {
  if (!pending) return;
  extern int shop_menu_active(void);
  if (!shop_menu_active() || ++pending_frames > 120) { pending = NULL; return; }
  void *cw = current_widget(pending);
  if (!cw) { pending = NULL; return; }
  int x;
  if (!vcall_ok(cw, X, &x)) { pending = NULL; return; }
  steady = (x == last_x) ? steady + 1 : 0;
  last_x = x;
  if (steady >= 8 && outside(pending, cw) == 0) {
    grid_highlight(pending, 1);
    if (fixes++ < 20) debugPrintf("[shop] page flip: selected the first item of the new page (x %d)\n", x);
    pending = NULL;
  }
}

/* Tab rebuilds may free the grid's widgets: forget a pending selection. */
void shop_grid_nav_reset(void) { pending = NULL; }

static int patch_vtable(so_module *mod, const char *vt_name, uintptr_t fn, void *hook) {
  uintptr_t vt = so_symbol(mod, vt_name);
  if (!vt) return 0;
  uintptr_t *slots = (uintptr_t *)(mod->image + (vt - mod->base));
  for (int i = 2; i < 40; i++) {
    if (slots[i] == fn) { slots[i] = (uintptr_t)hook; return 1; }
  }
  return 0;
}

int shop_grid_nav_install(so_module *mod) {
  FILE *f = fopen(PORT_ROOT "/no_shop_page_fix", "r");
  if (f) { fclose(f); return -1; }
  uintptr_t r = so_symbol(mod, "_ZN17AndroidController10GridWidget5rightEv");
  uintptr_t l = so_symbol(mod, "_ZN17AndroidController10GridWidget4leftEv");
  grid_highlight = (void *)so_symbol(mod, "_ZN17AndroidController10GridWidget9highlightEb");
  if (!r || !l || !grid_highlight) { debugPrintf("[shop] page fix: engine functions missing\n"); return -1; }
  eng_lo = mod->base; eng_hi = mod->base + mod->image_size;
  orig_right = (void *)r;
  orig_left = (void *)l;
  int n = 0;
  static const char *const vts[] = { "_ZTVN17AndroidController10GridWidgetE", "_ZTVN17AndroidController13GridWidgetImpE" };
  for (int k = 0; k < 2; k++) {
    n += patch_vtable(mod, vts[k], r, (void *)right_hook);
    n += patch_vtable(mod, vts[k], l, (void *)left_hook);
  }
  debugPrintf("[shop] page fix: %d grid vtable slots wrapped\n", n);
  return n ? 0 : -1;
}

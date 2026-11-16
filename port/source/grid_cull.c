/* Shop speed-up: LayerGrid::Render draws only the cells inside its visible area.
 *
 * The shop's item list is a LayerGrid (Menu_IGM_Inventory::RenderUI -> LayerGrid::Render -> one
 * BasicPage per item). The original drew every cell of the tab each frame and relied on the clip
 * rectangle (Sprite::EnableClip of the grid's area, +0xd0..+0xe0 widened by the margins
 * +0x15c/+0x160) to hide the rest, so each frame paid for every item's sprites and text.
 * Hardware 2026-11-13 (build 09d814d6, profile): with a long tab open the shop ran at a steady 40 ms
 * per frame, all of it sprite painting (ASprite::PaintAFrame, GetFrameRect, Sprite::Clip ...).
 *
 * The replacement keeps the original function for everything (clip, cell order, the "page x/y"
 * text after the loop): it lists the cells for which the engine's own LayerGrid::
 * IsObjectInVisibleArea is true (same rectangle as the clip; InterfaceObject::IsInRect is an
 * overlap test, so partly visible cells are kept), points the grid's cell vector (+0xb8 begin,
 * +0xc0 end) at that list, runs the original through a trampoline (grid_hooks.s), and restores
 * the vector. A cell that is skipped would have been clipped away completely anyway.
 * Only when clipping is on (+0xd8); otherwise the original runs unchanged.
 * Marker `no_grid_cull` keeps the original. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "so_util.h"
#include "util.h"

extern void grid_orig_render(void *grid, int x, int y);
extern const uint32_t grid_orig_render_head[4];
uintptr_t grid_orig_render_cont;

static int (*in_visible_area)(void *grid, void *obj);
static unsigned long drawn_total, skipped_total, frames_logged;

void grid_render(uint8_t *grid, int x, int y) {
  { extern int shop_ui_grid_render(uint8_t *); if (shop_ui_grid_render(grid)) return; } /* shop list (shop_ui.c) */
  void ***pb = (void ***)(grid + 0xb8), ***pe = (void ***)(grid + 0xc0);
  void **b = *pb, **e = *pe;
  size_t n = (size_t)(e - b);
  enum { MAX_CELLS = 1024 };
  if (!grid[0xd8] || !in_visible_area || n == 0 || n > MAX_CELLS) { grid_orig_render(grid, x, y); return; }
  void *vis[MAX_CELLS];
  size_t k = 0;
  for (size_t i = 0; i < n; i++)
    if (in_visible_area(grid, b[i]) & 1) vis[k++] = b[i];
  if (k == n) { grid_orig_render(grid, x, y); drawn_total += n; return; }
  *pb = vis;
  *pe = vis + k;
  grid_orig_render(grid, x, y);
  *pb = b;
  *pe = e;
  drawn_total += k;
  skipped_total += n - k;
  if (frames_logged < 3 && n - k > 0) {
    frames_logged++;
    debugPrintf("[grid_cull] grid %p: %zu of %zu cells drawn\n", (void *)grid, k, n);
  }
}

int grid_cull_install(so_module *mod) {
  FILE *f = fopen(PORT_ROOT "/no_grid_cull", "r");
  if (f) { fclose(f); return -1; }
  uintptr_t a = so_symbol(mod, "_ZN9LayerGrid6RenderEii");
  in_visible_area = (void *)so_symbol(mod, "_ZN9LayerGrid21IsObjectInVisibleAreaEP15InterfaceObject");
  if (!a || !in_visible_area || !mod->image) return -1;
  if (memcmp(mod->image + (a - mod->base), grid_orig_render_head, 16) != 0) {
    debugPrintf("[grid_cull] LayerGrid::Render does not start as expected, off\n");
    return -1;
  }
  grid_orig_render_cont = a + 16;
  return so_hook_function(mod, "_ZN9LayerGrid6RenderEii", (void *)grid_render);
}

void grid_cull_stats(void) {
  if (drawn_total || skipped_total)
    debugPrintf("[grid_cull] cells drawn %lu, skipped %lu\n", drawn_total, skipped_total);
}

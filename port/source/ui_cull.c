/* Shop / scrolling-list speed-up: LayerScrollPanel::Render drew every page
 * of the panel each frame (the shop's item grid: all pages of a tab, e.g. 13
 * pages of weapons) and relied on scissoring and per-sprite CPU clipping
 * (Sprite::Clip) to hide all but the one on screen. This replacement (hooked
 * in tweaks.c) does the same calls but skips pages whose rectangle lies
 * entirely outside the panel's clip area.
 *
 * Original (engine +0xbb8560), reproduced:
 *   if (clip @+0x78) Sprite::EnableClip((int)x@+0x70, (int)y@+0x74, (int)w@+0x7c, (int)h@+0x80);
 *   for each page in vector @+0x58..+0x60: page->Render(ox, oy)   (vtable +0x28)
 *   if (clip) Sprite::DisableClip();
 * Page geometry: virtual GetPosX/GetPosY (vtable +0x60/+0x68) and
 * GetWidth/GetHeight (+0xc0/+0xc8); BasicPage::Render draws its children at
 * page position + (ox, oy). A page with no size (w or h <= 0) is always drawn.
 * Marker `no_ui_cull` restores the original behaviour (all pages drawn). */
#include <stdint.h>
#include <stdio.h>
#include "config.h"
#include "so_util.h"
#include "util.h"

so_module *port_engine_module(void);

typedef float (*fget)(void *);
static void (*enable_clip)(int, int, int, int);
static void (*disable_clip)(void);
static unsigned long drawn_total, skipped_total;

void ui_scroll_panel_render(uint8_t *self, int ox, int oy) {
  if (!enable_clip) {
    so_module *mod = port_engine_module();
    enable_clip = (void *)so_symbol(mod, "_ZN6Sprite10EnableClipEiiii");
    disable_clip = (void *)so_symbol(mod, "_ZN6Sprite11DisableClipEv");
  }
  int clip = self[0x78];
  float cx = *(float *)(self + 0x70), cy = *(float *)(self + 0x74);
  float cw = *(float *)(self + 0x7c), ch = *(float *)(self + 0x80);
  if (clip && enable_clip) enable_clip((int)cx, (int)cy, (int)cw, (int)ch);
  void **it = *(void ***)(self + 0x58), **end = *(void ***)(self + 0x60);
  unsigned drawn = 0, skipped = 0;
  for (; it != end; it++) {
    void *page = *it;
    void **vt = *(void ***)page;
    if (clip && cw > 0 && ch > 0) {
      float px = ((fget)vt[0x60 / 8])(page) + ox, py = ((fget)vt[0x68 / 8])(page) + oy;
      float pw = ((fget)vt[0xc0 / 8])(page), ph = ((fget)vt[0xc8 / 8])(page);
      const float m = 8.0f;
      if (pw > 0 && ph > 0 &&
          (px + pw < cx - m || px > cx + cw + m || py + ph < cy - m || py > cy + ch + m)) {
        skipped++;
        continue;
      }
    }
    ((void (*)(void *, int, int))vt[0x28 / 8])(page, ox, oy);
    drawn++;
  }
  if (clip && disable_clip) disable_clip();
  drawn_total += drawn;
  skipped_total += skipped;
  static unsigned logged;
  if (skipped && logged < 5) {
    logged++;
    debugPrintf("[ui] scroll panel: drew %u page(s), skipped %u off-screen (clip %.0f,%.0f %.0fx%.0f)\n", drawn,
                skipped, cx, cy, cw, ch);
  }
}

int ui_cull_enabled(void) {
  FILE *f = fopen(PORT_ROOT "/no_ui_cull", "r");
  if (f) { fclose(f); return 0; }
  return 1;
}

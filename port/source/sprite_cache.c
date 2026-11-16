/* Shop / UI speed-up: cached SpriteMgr::getNewScale / getNewOffset.
 *
 * Every Sprite::PaintFrame (and LayerText::PaintTextArea) asks SpriteMgr whether the frame needs a
 * per-device scale or offset. The engine answers by copying the sprite's name (std::string) and
 * comparing it with up to 14 names of SpriteMgr::spriteMap (strcmp chains in needScale/needOffset),
 * then copying it again. Hardware profile 2026-11-12 (build 2a7bec59): ~25% of the shop's main-thread
 * time (Menu_IGM_Inventory: strcmp <- needScale/needOffset <- getNewScale/getNewOffset <- PaintFrame
 * <- LayerText::RenderButton / LayerButton::Render).
 *
 * The answers depend only on (name, frame) and on globals fixed after start-up, so the replacements
 * call the engine's own needScale/needOffset once per (name, frame), remember the result, and redo
 * the float math of the originals exactly (engine +0xd0776c / +0xd079e0, translated from the
 * disassembly; constants read from the engine image). No string copies on the hit path.
 *
 * Calling convention of the originals: x8 = result (TVector3D<float> / TVector2D<float>),
 * x1 = const std::string *name (libc++), w2 = frame, getNewOffset also w3, w4; x0 unused.
 * The entry stubs in sprite_hooks.s move x8 into x0, so the C functions see (out, name, frame, ...).
 * Marker `no_sprite_cache` restores the originals. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "so_util.h"
#include "util.h"

typedef int (*need_fn)(void *self, const void *name, int frame);
static need_fn need_scale, need_offset;
static const int *g_custom_w, *g_custom_h, *g_w, *g_h;
static const uint8_t *is_xoom;
static float k_16_9, k_219_5, k_0_1;

enum { SLOTS = 4096, NAME_MAX_LEN = 46 };
typedef struct {
  uint32_t hash;
  int16_t frame;
  uint8_t len;
  uint8_t flags; /* 1 used, 2 scale known, 4 scale, 8 offset known, 16 offset */
  char name[NAME_MAX_LEN];
} entry_t;
static entry_t table[SLOTS];
static unsigned used, misses, uncached;

/* libc++ std::string (short: byte0 = len<<1, data at +1; long: byte0 bit0 set, size +8, data +0x10) */
static inline const char *str_data(const uint8_t *s, size_t *len) {
  if (!(s[0] & 1)) { *len = s[0] >> 1; return (const char *)s + 1; }
  *len = *(const size_t *)(s + 8);
  return *(const char *const *)(s + 0x10);
}

static int lookup(const void *name, int frame, int want_offset) {
  size_t len;
  const char *d = str_data(name, &len);
  if (len >= NAME_MAX_LEN || frame < -32768 || frame > 32767) {
    uncached++;
    return want_offset ? need_offset(NULL, name, frame) & 1 : need_scale(NULL, name, frame) & 1;
  }
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)d[i]) * 16777619u;
  h ^= (uint32_t)frame * 0x9e3779b1u;
  unsigned i = h & (SLOTS - 1);
  entry_t *e;
  for (;; i = (i + 1) & (SLOTS - 1)) {
    e = &table[i];
    if (!(e->flags & 1)) {
      if (used >= SLOTS * 3 / 4) { /* full: answer without caching */
        uncached++;
        return want_offset ? need_offset(NULL, name, frame) & 1 : need_scale(NULL, name, frame) & 1;
      }
      e->hash = h; e->frame = (int16_t)frame; e->len = (uint8_t)len; e->flags = 1;
      memcpy(e->name, d, len);
      used++;
      break;
    }
    if (e->hash == h && e->frame == frame && e->len == len && !memcmp(e->name, d, len)) break;
  }
  if (want_offset) {
    if (!(e->flags & 8)) { misses++; e->flags |= 8 | ((need_offset(NULL, name, frame) & 1) ? 16 : 0); }
    return (e->flags >> 4) & 1;
  }
  if (!(e->flags & 2)) { misses++; e->flags |= 2 | ((need_scale(NULL, name, frame) & 1) ? 4 : 0); }
  return (e->flags >> 2) & 1;
}

/* SpriteMgr::getNewScale(std::string, int) -> TVector3D<float> */
void sprite_get_new_scale(float *out, const void *name, int frame) {
  float x = 1.0f, y = 1.0f;
  if (lookup(name, frame, 0)) {
    float ch = (float)*g_custom_h, h = (float)*g_h, cw = (float)*g_custom_w, w = (float)*g_w;
    float r = (ch / h) / (cw / w);
    if ((frame | 4) != 0xd) x = r;
    else if (cw / ch == k_16_9 && !*is_xoom) y = r;
  }
  out[0] = x; out[1] = y; out[2] = 1.0f;
}

/* SpriteMgr::getNewOffset(std::string, int, int, int) -> TVector2D<float> */
void sprite_get_new_offset(float *out, const void *name, int frame, int p5, int p6) {
  float ch = (float)*g_custom_h, cw = (float)*g_custom_w;
  float s8 = ch / (float)*g_h, s9 = cw / (float)*g_w;
  float x = 0.0f, y = 0.0f;
  int need = lookup(name, frame, 1);
  if (need && (frame | 4) != 0xd) {
    x = ((float)p5 * 0.5f) * (s9 - s8);
  } else if (need && cw / ch == k_16_9 && !*is_xoom) {
    float a = (frame | 4) == 0xd ? k_219_5 : (float)p6 * 0.5f;
    a = a * (s9 - s8);
    y = fmaf((float)*g_h - (float)*g_custom_h, k_0_1, a);
  }
  out[0] = x; out[1] = y;
}

int sprite_cache_install(so_module *mod) {
  FILE *f = fopen(PORT_ROOT "/no_sprite_cache", "r");
  if (f) { fclose(f); return -1; }
  uintptr_t ns = so_symbol(mod, "_ZN9SpriteMgr9needScaleENSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEEi");
  uintptr_t no = so_symbol(mod, "_ZN9SpriteMgr10needOffsetENSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEEi");
  uintptr_t cw = so_symbol(mod, "g_customWidth"), chh = so_symbol(mod, "g_customHeight");
  uintptr_t w = so_symbol(mod, "g_width"), h = so_symbol(mod, "g_height"), xo = so_symbol(mod, "IS_XOOM");
  if (!ns || !no || !cw || !chh || !w || !h || !xo) { debugPrintf("[sprite_cache] symbols missing, off\n"); return -1; }
  if (!mod->image || ns - mod->base != 0xd0747c) { debugPrintf("[sprite_cache] unexpected layout, off\n"); return -1; }
  need_scale = (need_fn)ns; need_offset = (need_fn)no;
  g_custom_w = (const int *)cw; g_custom_h = (const int *)chh; g_w = (const int *)w; g_h = (const int *)h;
  is_xoom = (const uint8_t *)xo;
  /* Read constants from the loaded image, not from mod->base: on hardware the engine is only mapped
   * at mod->base by so_finalize, after the hooks (reading base here crashed at start-up on the
   * Switch, 2026-11-13; the emulator runs from the image itself so it didn't show there). */
  k_16_9 = *(const float *)(mod->image + 0x1404178);
  k_219_5 = *(const float *)(mod->image + 0x140417c);
  k_0_1 = *(const float *)(mod->image + 0x13cd664);
  if (k_219_5 != 219.5f || fabsf(k_16_9 - 16.0f / 9.0f) > 1e-5f || fabsf(k_0_1 - 0.1f) > 1e-6f) {
    debugPrintf("[sprite_cache] unexpected constants (%f %f %f), off\n", k_16_9, k_219_5, k_0_1);
    return -1;
  }
  extern void sprite_get_new_scale_hook(void), sprite_get_new_offset_hook(void);
  if (so_hook_function(mod, "_ZN9SpriteMgr11getNewScaleENSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEEi",
                       (void *)sprite_get_new_scale_hook) != 0 ||
      so_hook_function(mod, "_ZN9SpriteMgr12getNewOffsetENSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEEiii",
                       (void *)sprite_get_new_offset_hook) != 0) {
    debugPrintf("[sprite_cache] hook failed\n");
    return -1;
  }
  return 0;
}

void sprite_cache_stats(void) {
  debugPrintf("[sprite_cache] %u entries, %u engine lookups, %u uncached calls\n", used, misses, uncached);
}

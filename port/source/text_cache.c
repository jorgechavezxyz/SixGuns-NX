/* Text layout speed-up: memoised Font::MapChar and bidi::get_char_type.
 *
 * Every character measured or drawn goes through Font::MapChar (character -> glyph index, a binary
 * search over static tables picked by the font's type) and bidi::get_char_type (Unicode bidi class,
 * another binary search), several times per character (GetCharWidth, is_non_space_mark, has_space,
 * has_space_between). Shop tab switches rebuild every item's text (LayerText::SetPos ->
 * FontMgr::SetPageInfoData), and ~22% of the shop's slow frames were this text layout (hardware
 * profile 2026-11-12, Menu_IGM_Inventory hitches).
 *
 * Both functions are pure: MapChar depends only on the font's type (int at +0x38, 0..8), its flag
 * (byte at +0x3c, read by type 0) and the character; get_char_type only on the character. The
 * replacements remember each answer in a table. A miss runs the engine's own code through a
 * trampoline (text_hooks.s: the original's first four instructions, which the hook overwrites and
 * which are position-independent, then a jump to original+16), so every answer is the engine's.
 * The trampoline's copied instructions are compared with the image before hooking.
 * Marker `no_text_cache` keeps the originals. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "so_util.h"
#include "util.h"

enum { FONT_TYPES = 9, BMP = 0x10000, MAP_UNKNOWN = 0xffff, BIDI_UNKNOWN = 0xff };

/* Trampolines (text_hooks.s) and the addresses they continue at. */
extern long text_orig_map_char(const void *font, int c);
extern int text_orig_bidi_char_type(int c);
extern const uint32_t text_orig_map_char_head[4], text_orig_bidi_head[4];
uintptr_t text_orig_map_char_cont, text_orig_bidi_cont;

static uint16_t *map_tab[FONT_TYPES][2];
static uint8_t bidi_tab[BMP];
static unsigned long map_miss, bidi_miss;

long text_map_char(const uint8_t *font, int c) {
  unsigned type = *(const uint32_t *)(font + 0x38);
  if (type >= FONT_TYPES || (unsigned)c >= BMP) return text_orig_map_char(font, c);
  int flag = type == 0 && font[0x3c] != 0;
  uint16_t *t = map_tab[type][flag];
  if (!t) {
    t = malloc(BMP * sizeof(uint16_t));
    if (!t) return text_orig_map_char(font, c);
    memset(t, 0xff, BMP * sizeof(uint16_t));
    map_tab[type][flag] = t;
  }
  uint16_t v = t[c];
  if (v != MAP_UNKNOWN) return v;
  long r = text_orig_map_char(font, c);
  map_miss++;
  if (r >= 0 && r < MAP_UNKNOWN) t[c] = (uint16_t)r;
  return r;
}

int text_bidi_char_type(int c) {
  if ((unsigned)c >= BMP) return text_orig_bidi_char_type(c);
  uint8_t v = bidi_tab[c];
  if (v != BIDI_UNKNOWN) return v;
  int r = text_orig_bidi_char_type(c);
  bidi_miss++;
  if (r >= 0 && r < BIDI_UNKNOWN) bidi_tab[c] = (uint8_t)r;
  return r;
}

static int head_matches(so_module *mod, const char *name, const uint32_t head[4], uintptr_t *cont) {
  uintptr_t a = so_symbol(mod, name);
  if (!a || !mod->image) return 0;
  uintptr_t off = a - mod->base;
  if (memcmp(mod->image + off, head, 16) != 0) {
    debugPrintf("[text_cache] %s does not start as expected, off\n", name);
    return 0;
  }
  *cont = a + 16; /* final (mapped) address: so_symbol returns base-relative addresses */
  return 1;
}

int text_cache_install(so_module *mod) {
  FILE *f = fopen(PORT_ROOT "/no_text_cache", "r");
  if (f) { fclose(f); return -1; }
  memset(bidi_tab, 0xff, sizeof(bidi_tab));
  if (!head_matches(mod, "_ZN4Font7MapCharEi", text_orig_map_char_head, &text_orig_map_char_cont) ||
      !head_matches(mod, "_ZN4bidi13get_char_typeEi", text_orig_bidi_head, &text_orig_bidi_cont))
    return -1;
  if (so_hook_function(mod, "_ZN4Font7MapCharEi", (void *)text_map_char) != 0) return -1;
  if (so_hook_function(mod, "_ZN4bidi13get_char_typeEi", (void *)text_bidi_char_type) != 0) {
    debugPrintf("[text_cache] bidi hook failed (MapChar cache stays on)\n");
    return 0;
  }
  return 0;
}

void text_cache_stats(void) {
  debugPrintf("[text_cache] engine lookups so far: MapChar %lu, get_char_type %lu\n", map_miss, bidi_miss);
}

/* Shop tab bar: no tab slots built from memory past the end of the tab list.
 *
 * Menu_IGM_Inventory keeps two tables: 7 "physical" slots of the tab bar (+0x760..+0x768, 0x18
 * bytes each: normal and selected layout ids, ..., button pointer at +0x10), built once by
 * MakePhysicalTabNodeTable, and the "logical" tabs (+0x748..+0x750, 0x1c bytes each) rebuilt by
 * MakeLogicalTabNodeTable on every ResetInventory. The loop that creates the tab buttons
 * (engine +0xb78b00 area) walks all physical slots and reads logical[scroll + slot] without a
 * bounds check, and the right-scroll arrow (object 0x70) is shown when
 * scroll < logical - physical computed unsigned. With all 9 tabs that was fine; the port removes
 * four offline tabs (tweaks.c shop_tabs_trim), leaving 5 logical tabs for 7 slots, so slots 6 and 7
 * were made from whatever followed the vector (the empty slots with big gold rings) and the
 * arrow showed (the stray round button) -- the glitch reproduced on 2026-11-12.
 *
 * Fix: MakeLogicalTabNodeTable is wrapped (shop_tabs_hooks.s: original via trampoline, then
 * shop_tabs_fixup with the returned vector); the fixup sets the shop menu's physical end to
 * min(7, logical count) slots. The storage for 7 stays allocated (entries are plain data), and the
 * full count comes back when a later table is longer (marker `all_shop_tabs`).
 * Marker `no_shop_tab_fix` keeps the original. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "so_util.h"
#include "util.h"
#include <switch.h>

enum { PHYS_FULL = 7, PHYS_SIZE = 0x18, LOG_SIZE = 0x1c };

extern const uint32_t shop_tabs_orig_head[4];
uintptr_t shop_tabs_orig_cont;

static void *(*menus_instance)(int);
static uint8_t *shop_resetting_menu; /* Menu_IGM_Inventory inside ResetInventory, else NULL */

void shop_tabs_fixup(uint8_t **vec) {
  if (!vec || !vec[0] || !menus_instance) return;
  size_t nlog = (size_t)(vec[1] - vec[0]) / LOG_SIZE;
  /* Only the menu whose ResetInventory is running (set by shop_reset_inventory). The first
   * MakeLogicalTabNodeTable call comes from the Menu_IGM_Inventory constructor while
   * MGR_Menus is still being built, when MGR_Menus+0x68 is not yet valid. */
  uint8_t *menu = shop_resetting_menu;
  if (!menu) return;
  uint8_t **phys = (uint8_t **)(menu + 0x760); /* begin, end, capacity */
  if (!phys[0] || (size_t)(phys[2] - phys[0]) < PHYS_FULL * PHYS_SIZE) return; /* not built yet */
  size_t n = nlog < PHYS_FULL ? nlog : PHYS_FULL;
  uint8_t *want = phys[0] + n * PHYS_SIZE;
  if (phys[1] != want) {
    debugPrintf("[shop_tabs] %zu tabs: tab bar uses %zu of %d slots\n", nlog, n, PHYS_FULL);
    phys[1] = want;
  }
}

int shop_tabs_install(so_module *mod) {
  FILE *f = fopen(PORT_ROOT "/no_shop_tab_fix", "r");
  if (f) { fclose(f); return -1; }
  const char *name = "_ZN18Menu_IGM_Inventory23MakeLogicalTabNodeTableENS_23k_makeLogicalTabsOptionE";
  uintptr_t a = so_symbol(mod, name);
  menus_instance = (void *)so_symbol(mod, "_ZN9SingletonI9MGR_MenusE11GetInstanceEb");
  if (!a || !menus_instance || !mod->image) return -1;
  if (memcmp(mod->image + (a - mod->base), shop_tabs_orig_head, 16) != 0) {
    debugPrintf("[shop_tabs] MakeLogicalTabNodeTable does not start as expected, off\n");
    return -1;
  }
  shop_tabs_orig_cont = a + 16;
  extern void shop_tabs_make_logical_hook(void);
  return so_hook_function(mod, name, (void *)shop_tabs_make_logical_hook);
}

/* Tab rebuild timing: Menu_IGM_Inventory::ResetInventory (the whole grid of a tab is torn down and
 * rebuilt on every tab switch; ~150 ms per switch on hardware, 2026-11-13) runs through this
 * wrapper, which logs its duration and the tab type (+0x44). */
extern const uint32_t shop_reset_orig_head[4];
extern void shop_reset_orig(void *menu);
uintptr_t shop_reset_orig_cont;

static uintptr_t engine_base;
/* Weapons tab: Menu_IGM_Inventory::UpdateUI rebuilds the grid when the tab is selected
 * (call returning to +0xb778d4), then looks up the equipped weapon, SetItemSelected()s it and
 * rebuilds the whole grid again (+0xb77acc) to highlight it -- two full rebuilds in one update
 * (Ryubing: 35 + 46 ms; hardware ~2x). The first one is deferred: if the second follows in the same
 * update only that one runs; otherwise shop_tabs_frame() (after the engine frame) runs it. In
 * between, UpdateUI only reads the player/inventory and SetItemSelected writes +0x38/+0x3c.
 * Marker `no_shop_tab_fix` turns this off with the tab-bar fix. */
static uint8_t *pending;
static int defer_on;

void shop_reset_inventory(uint8_t *menu) {
  { extern void shop_grid_nav_reset(void); shop_grid_nav_reset(); }
  { extern void shop_ui_reset(void); shop_ui_reset(); }
  { extern void shop_ui_unlock_league_items(void); shop_ui_unlock_league_items(); }
  uintptr_t lr = (uintptr_t)__builtin_return_address(0) - engine_base;
  if (defer_on && lr == 0xb778d4 && *(int32_t *)(menu + 0x44) == 4) {
    pending = menu;
    return;
  }
  pending = NULL;
  u64 t0 = armGetSystemTick();
  shop_resetting_menu = menu;
  shop_reset_orig(menu);
  shop_resetting_menu = NULL;
  double ms = armTicksToNs(armGetSystemTick() - t0) / 1e6;
  debugPrintf("[shop] tab %d rebuilt in %.1f ms (from engine +0x%lx)\n", *(int32_t *)(menu + 0x44), ms, (unsigned long)lr);
}

int shop_reset_timing_install(so_module *mod) {
  const char *name = "_ZN18Menu_IGM_Inventory14ResetInventoryEv";
  uintptr_t a = so_symbol(mod, name);
  if (!a || !mod->image || memcmp(mod->image + (a - mod->base), shop_reset_orig_head, 16) != 0) return -1;
  shop_reset_orig_cont = a + 16;
  engine_base = mod->base;
  FILE *f = fopen(PORT_ROOT "/no_shop_tab_fix", "r");
  if (f) fclose(f);
  else defer_on = 1;
  return so_hook_function(mod, name, (void *)shop_reset_inventory);
}

/* After each engine frame: a deferred weapons-tab rebuild whose follow-up never came. */
void shop_tabs_frame(void) {
  if (!pending) return;
  uint8_t *menu = pending;
  pending = NULL;
  uint8_t *mgr = menus_instance ? menus_instance(0) : NULL;
  if (!mgr || *(uint8_t **)(mgr + 0x68) != menu) return; /* shop gone */
  debugPrintf("[shop] deferred weapons-tab rebuild\n");
  shop_reset_inventory(menu);
}

/* Custom multiplayer maps from mods (2026-11-15).
 *
 * A mod zip in switch/sixguns/mods/ that contains `custom_map.txt` adds a map to the multiplayer lobby:
 *     name=Stadium
 *     base=2                                         one of the game's maps 0..3 (2 = Canyon)
 *     level=custommaps/stadium/stadium.bclara        the replacement level inside the mod zip; it is
 *                                                    found by file name, so the name must be unique
 * A custom map is an overlay on its base map. MPMapManager (the table g_mapsInfo: 4 maps, 0x38 bytes each,
 * +0x8 name string id, +0x10 level path, +0x2c/+0x30 lobby picture) is unchanged; custom maps get map
 * indices 5, 6, ... (4 is "any map" in the room info) in name order, so consoles with the same mods agree.
 *   - MPMapManager::GetMapInfo(5 + n) returns a copy of the base map's entry: every engine check that a
 *     level is a multiplayer map (GS_LoadNextLevel::IsMultiplayerMap, ... compare level paths against the
 *     four maps) sees the base map's path. GetMapsCount stays 4, so "any map" never picks a custom map.
 *   - ZipStreamFactory::CreateStream (how GameLevel opens levels.pak entries): while the room's map index (MultiplayerContext room info +0x28, also on a
 *     client: GameRoomInfoData from the host's player info) is a custom map, opening the base map's level
 *     opens the custom level instead. The custom level names its own model/collision files.
 *   - Other files: a zip entry named "<map name>__<file>", all lower case, (custommaps/stadium/stadium__a12_tdm_forest_minimap.tga)
 *     replaces <file> during that map's matches only (zip factories look entries up by file name).
 *   - Lobby (Menu_MP_CreateHost): InitUI adds one map page per custom map (AddMapGUIElement with the base
 *     map's picture); onMapSelected maps page 5 + n to map index 5 + n (the original turns page p into
 *     map p - 1 and page 0 into 4, "any").
 *   - CMatchingLocal::AddServer: a LAN room whose map (room attribute 2) is a custom map this console does
 *     not have is not listed, so a quick match never joins it (the player would load a level they lack).
 * ui_menus.c draws the names (custom_map_name) and the server browser refuses such rooms with a message. */
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>
#include "config.h"
#include "so_util.h"
#include "util.h"

#define MAX_CM 8
#define CM_FIRST 5
#define MAX_OVR 16
typedef struct {
  char name[32], level[160], zip[96];
  char ovr[MAX_OVR][64]; /* "<map>__<game file name>" entries of the mod zip */
  int base, novr;
  void *factory; /* the mod zip's ZipStreamFactory, seen when the custom level is found through it */
  uint8_t info[0x38];
  int info_ok;
} custom_map;
static custom_map cm[MAX_CM];
static int ncm;

static uint8_t *g_maps;
static int *g_maps_count;
static void (*str_ctor)(void *, const char *), (*str_dtor)(void *);
static const char *(*str_cstr)(const void *);
static void **mp_single; /* Multiplayer::s_Multiplayer: GetInstance would create it on first use */
static void *(*mp_ctx)(void *);
static void *mp_inst(void) { return mp_single ? *mp_single : NULL; }
static uint8_t *(*mp_room)(void *);
static int (*attr_int)(void *, int);
static void (*add_map_page)(void *, int, int);
static void (*text_set)(void *, const char *);

int custom_maps_count(void) { return ncm; }
int custom_map_first(void) { return CM_FIRST; }
const char *custom_map_name(int index) {
  int i = index - CM_FIRST;
  return i >= 0 && i < ncm ? cm[i].name : NULL;
}
int custom_map_installed(int index) { return custom_map_name(index) != NULL; }

/* ---------------------------------------------------------------- reading custom_map.txt from a zip */
/* Reads the stored entry `want` into out (0 = found) and lists the zip's per-map override files: entries
 * whose file name contains "__" ("stadium__a12_tdm_forest_minimap.tga") go to ovr[]/novr. */
static int read_zip_entry(const char *zpath, const char *want, char *out, size_t outn, char ovr[][64], int *novr) {
  FILE *f = fopen(zpath, "rb");
  if (!f) return -1;
  int ok = -1;
  uint8_t tail[1024 + 22];
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  long tn = size < (long)sizeof(tail) ? size : (long)sizeof(tail);
  fseek(f, size - tn, SEEK_SET);
  if (fread(tail, 1, tn, f) != (size_t)tn) goto done;
  long e = -1;
  for (long i = tn - 22; i >= 0; i--)
    if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6) { e = i; break; }
  if (e < 0) goto done;
  uint32_t cd_size, cd_off;
  uint16_t count;
  memcpy(&count, tail + e + 10, 2);
  memcpy(&cd_size, tail + e + 12, 4);
  memcpy(&cd_off, tail + e + 16, 4);
  if (cd_size > (1u << 20)) goto done;
  uint8_t *cd = malloc(cd_size);
  if (!cd) goto done;
  fseek(f, cd_off, SEEK_SET);
  if (fread(cd, 1, cd_size, f) == cd_size) {
    size_t p = 0, wl = strlen(want);
    for (unsigned k = 0; k < count && p + 46 <= cd_size; k++) {
      uint16_t method, nl, xl, cl;
      uint32_t csize, lofs;
      memcpy(&method, cd + p + 10, 2);
      memcpy(&csize, cd + p + 20, 4);
      memcpy(&nl, cd + p + 28, 2);
      memcpy(&xl, cd + p + 30, 2);
      memcpy(&cl, cd + p + 32, 2);
      memcpy(&lofs, cd + p + 42, 4);
      if (novr && p + 46 + nl <= cd_size && *novr < MAX_OVR) {
        char nm[160];
        size_t k = nl < sizeof(nm) - 1 ? nl : sizeof(nm) - 1;
        memcpy(nm, cd + p + 46, k);
        nm[k] = 0;
        const char *bn = strrchr(nm, '/') ? strrchr(nm, '/') + 1 : nm;
        if (strstr(bn, "__") && strlen(bn) < 64) strcpy(ovr[(*novr)++], bn);
      }
      if (ok != 0 && nl == wl && p + 46 + nl <= cd_size && !memcmp(cd + p + 46, want, wl) && method == 0 && csize < outn) {
        uint8_t lh[30];
        fseek(f, lofs, SEEK_SET);
        if (fread(lh, 1, 30, f) == 30) {
          uint16_t lnl, lxl;
          memcpy(&lnl, lh + 26, 2);
          memcpy(&lxl, lh + 28, 2);
          fseek(f, lofs + 30 + lnl + lxl, SEEK_SET);
          if (fread(out, 1, csize, f) == csize) { out[csize] = 0; ok = 0; }
        }
      }
      p += 46 + nl + xl + cl;
    }
  }
  free(cd);
done:
  fclose(f);
  return ok;
}

static void parse_custom_map(const char *zipname, char *txt, char ovr[][64], int novr) {
  if (ncm >= MAX_CM) return;
  custom_map m = { .base = -1 };
  for (char *line = strtok(txt, "\r\n"); line; line = strtok(NULL, "\r\n")) {
    if (line[0] == '#') continue;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    const char *k = line, *v = eq + 1;
    if (!strcmp(k, "name")) snprintf(m.name, sizeof(m.name), "%s", v);
    else if (!strcmp(k, "base")) m.base = atoi(v);
    else if (!strcmp(k, "level")) snprintf(m.level, sizeof(m.level), "%s", v);
  }
  if (!m.name[0] || !m.level[0] || m.base < 0 || m.base > 3) {
    debugPrintf("[maps] %s: custom_map.txt incomplete (name, base 0..3, level), ignored\n", zipname);
    return;
  }
  for (int i = 0; i < ncm; i++)
    if (!strcasecmp(cm[i].name, m.name)) { debugPrintf("[maps] %s: a map named %s is already installed, ignored\n", zipname, m.name); return; }
  snprintf(m.zip, sizeof(m.zip), "%s", zipname);
  /* override files for this map: "<name>__<file>", name compared without case */
  size_t nl = strlen(m.name);
  for (int i = 0; i < novr; i++)
    if (!strncasecmp(ovr[i], m.name, nl) && ovr[i][nl] == '_' && ovr[i][nl + 1] == '_' && m.novr < MAX_OVR) strcpy(m.ovr[m.novr++], ovr[i]);
  cm[ncm++] = m;
}

static int cmp_cm(const void *a, const void *b) { return strcasecmp(((const custom_map *)a)->name, ((const custom_map *)b)->name); }

static void scan_mods(void) {
  DIR *d = opendir(PORT_ROOT "/mods");
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    size_t l = strlen(e->d_name);
    if (l < 5 || strcasecmp(e->d_name + l - 4, ".zip")) continue;
    char path[512], txt[2048], ovr[MAX_OVR][64];
    int novr = 0;
    snprintf(path, sizeof(path), PORT_ROOT "/mods/%s", e->d_name);
    if (read_zip_entry(path, "custom_map.txt", txt, sizeof(txt), ovr, &novr) == 0) parse_custom_map(e->d_name, txt, ovr, novr);
  }
  closedir(d);
  qsort(cm, ncm, sizeof(*cm), cmp_cm);
  for (int i = 0; i < ncm; i++)
    debugPrintf("[maps] custom map %d: %s (base map %d, level %s, %d file override(s), from %s)\n", CM_FIRST + i, cm[i].name, cm[i].base,
                cm[i].level, cm[i].novr, cm[i].zip);
}

/* ---------------------------------------------------------------- engine hooks */
static uint8_t *base_info(int i) { return g_maps && g_maps_count && i >= 0 && i < *g_maps_count ? g_maps + i * 0x38 : NULL; }

static uint8_t *get_map_info_hook(void *self, int i) {
  (void)self;
  uint8_t *b = base_info(i);
  if (b) return b;
  int k = i - CM_FIRST;
  if (k < 0 || k >= ncm) return NULL;
  if (!cm[k].info_ok) {
    uint8_t *src = base_info(cm[k].base);
    if (!src) return NULL;
    memcpy(cm[k].info, src, 0x38);
    cm[k].info_ok = 1;
  }
  return cm[k].info;
}

/* the custom map the current room plays, or -1 */
static int room_custom_map(void) {
  void *mp = mp_inst(), *ctx = mp ? mp_ctx(mp) : NULL;
  uint8_t *room = ctx ? mp_room(ctx) : NULL;
  if (!room) return -1;
  int k = *(int *)(room + 0x28) - CM_FIRST;
  return k >= 0 && k < ncm ? k : -1;
}

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

/* GameLevel opens a level straight from the levels.pak factory (StreamMgr::GetZipStreamFactory(0) ->
 * CreateStream(file name)), not through StreamMgr::CreateStream. While the room plays a custom map, the
 * base map's level is fetched with StreamMgr::CreateStream(custom level path), which finds it in the mod
 * zip (registered before the OBBs, mods.c); `redirecting` keeps that lookup from coming back here. */
extern void *cm_zip_orig(void *factory, const void *name);
extern const uint32_t cm_zip_orig_head[4];
uintptr_t cm_zip_orig_cont;
static void *(*stream_mgr)(void), *(*stream_create)(void *, const void *);
static int redirecting, cm_any_ovr;
static void *zip_hook(void *factory, const void *name) {
  if (redirecting) {
    /* StreamMgr asking each factory for the custom level: remember which one is the mod's zip */
    void *r = cm_zip_orig(factory, name);
    if (r && redirecting > 0) cm[redirecting - 1].factory = factory;
    return r;
  }
  const char *n = str_cstr(name);
  size_t l = n ? strlen(n) : 0;
  if (l > 7 && !strcasecmp(n + l - 7, ".bclara")) {
    int k = room_custom_map();
    uint8_t *b = k >= 0 ? base_info(cm[k].base) : NULL;
    const char *bl = b ? *(const char **)(b + 0x10) : NULL;
    if (bl && !strcasecmp(base_name(n), base_name(bl))) {
      _Alignas(16) uint8_t s[64];
      str_ctor(s, base_name(cm[k].level)); /* zip factories find entries by file name, not by folder path */
      redirecting = k + 1;
      void *r = stream_create(stream_mgr(), s);
      redirecting = 0;
      str_dtor(s);
      debugPrintf("[maps] level %s -> %s: %s\n", n, cm[k].level, r ? "custom map loaded" : "NOT FOUND in the mod, base map used");
      if (r) { extern int cm_return_k; extern u64 cm_return_t; cm_return_k = k; cm_return_t = 0; }
      if (r) return r;
    }
  }
  if (l && strstr(n, "minimap")) { static int lg; if (lg++ < 12) debugPrintf("[maps] open %s (room custom map %d)\n", n, room_custom_map()); }
  if (l) {
    /* per-map files: while the room plays custom map k, "<game file>" opens "<map>__<game file>" (the
     * minimap image, ...). Base map files only change for the custom map's matches. */
    int k = cm_any_ovr ? room_custom_map() : -1;
    if (k >= 0 && cm[k].novr) {
      const char *bn = base_name(n);
      size_t ml = strlen(cm[k].name);
      for (int i = 0; i < cm[k].novr; i++)
        if (!strcasecmp(cm[k].ovr[i] + ml + 2, bn)) {
          /* StreamMgr hands a name to the one factory that has it (the game's OBB here) and calling
           * StreamMgr again from inside its call hung the game: ask the mod's own zip factory, known
           * from loading this map's level just before */
          if (!cm[k].factory) break;
          char lower[64];
          for (int c = 0;; c++) { lower[c] = (char)tolower((unsigned char)cm[k].ovr[i][c]); if (!lower[c]) break; }
          _Alignas(16) uint8_t s[64];
          str_ctor(s, lower); /* zip entry names are looked up in lower case */
          void *r = cm_zip_orig(cm[k].factory, s);
          str_dtor(s);
          if (r) { debugPrintf("[maps] %s -> %s\n", n, lower); return r; }
          { static int lg; if (lg++ < 8) debugPrintf("[maps] %s: %s not found in the mod\n", n, lower); }
          break;
        }
    }
  }
  return cm_zip_orig(factory, name);
}

extern void cm_addserver_orig(void *self, int a, void *netid, int b, void *attrs);
extern const uint32_t cm_addserver_orig_head[4];
uintptr_t cm_addserver_orig_cont;
static void addserver_hook(void *self, int a, void *netid, int b, void *attrs) {
  int map = attr_int(attrs, 2);
  if (map >= CM_FIRST && !custom_map_installed(map)) {
    static int logged;
    if (logged++ < 5) debugPrintf("[maps] LAN room skipped: it plays custom map %d, which is not installed here\n", map);
    return;
  }
  cm_addserver_orig(self, a, netid, b, attrs);
}

extern void cm_initui_orig(void *menu);
extern const uint32_t cm_initui_orig_head[4];
uintptr_t cm_initui_orig_cont;
static void initui_hook(void *menu) {
  cm_initui_orig(menu);
  for (int k = 0; k < ncm; k++) {
    uint8_t *b = base_info(cm[k].base);
    if (b) add_map_page(menu, *(int *)(b + 0x2c), *(int *)(b + 0x30));
  }
  if (ncm) debugPrintf("[maps] lobby: %d custom map page(s) added\n", ncm);
}

/* Back in the lobby after a custom map's match, the game puts the room's map back to the map whose
 * level path it played (the base map: the custom map borrows its path) and the lobby opens on the
 * base map's page (2026-11-15: Stadium -> Canyon). The first selection of the base map's page within
 * a few seconds of the first lobby selection after that match goes to the custom map's page instead. */
int cm_return_k = -1;
u64 cm_return_t;
static void (*grid_set_page)(void *, int);
extern void cm_mapsel_orig(void *menu, int page);
extern const uint32_t cm_mapsel_orig_head[4];
uintptr_t cm_mapsel_orig_cont;
static void mapsel_hook(void *menu, int page) {
  if (cm_return_k >= 0) {
    u64 now = armGetSystemTick();
    if (!cm_return_t) cm_return_t = now; /* the lobby's first selection after the custom match */
    int k = cm_return_k;
    if (armTicksToNs(now - cm_return_t) > 3000000000ULL) cm_return_k = -1;
    else if (page == cm[k].base + 1) {
      cm_return_k = -1;
      void *grid = *(void **)((uint8_t *)menu + 0xa0);
      if (grid && grid_set_page) {
        grid_set_page(grid, CM_FIRST + k);
        page = CM_FIRST + k;
        debugPrintf("[maps] lobby back on %s after its match\n", cm[k].name);
      }
    }
  }
  cm_mapsel_orig(menu, page);
  int k = page - CM_FIRST;
  if (k < 0 || k >= ncm) return;
  uint8_t *m = menu;
  *(int *)(m + 0xb0) = CM_FIRST + k;
  void *mp = mp_inst(), *ctx = mp ? mp_ctx(mp) : NULL;
  uint8_t *room = ctx ? mp_room(ctx) : NULL;
  if (room) *(int *)(room + 0x28) = CM_FIRST + k;
  void *label = *(void **)(m + 0x90);
  if (label && text_set) text_set(label, cm[k].name);
}

static int hook_tramp(so_module *mod, const char *sym, const uint32_t *head, uintptr_t *cont, void *fn) {
  uintptr_t a = so_symbol(mod, sym);
  if (!a || !mod->image || memcmp(mod->image + (a - mod->base), head, 16) != 0) {
    const uint32_t *w = a && mod->image ? (const uint32_t *)(mod->image + (a - mod->base)) : NULL;
    debugPrintf("[maps] %s: start differs (%08x %08x %08x %08x)\n", sym, w ? w[0] : 0, w ? w[1] : 0, w ? w[2] : 0, w ? w[3] : 0);
    return -1;
  }
  *cont = a + 16;
  return so_hook_function(mod, sym, fn);
}

void custom_maps_install(so_module *mod) {
  scan_mods();
  if (!ncm) return;
  g_maps = (uint8_t *)so_symbol(mod, "g_mapsInfo");
  g_maps_count = (int *)so_symbol(mod, "g_mapsInfoCount");
  str_ctor = (void *)so_symbol(mod, "_ZN3pig6StringC2EPKc");
  str_dtor = (void *)so_symbol(mod, "_ZN3pig6StringD2Ev");
  str_cstr = (void *)so_symbol(mod, "_ZNK3pig6String5c_strEv");
  mp_single = (void **)so_symbol(mod, "_ZN11Multiplayer13s_MultiplayerE");
  mp_ctx = (void *)so_symbol(mod, "_ZN11Multiplayer10GetContextEv");
  mp_room = (void *)so_symbol(mod, "_ZN18MultiplayerContext11GetRoomInfoEv");
  attr_int = (void *)so_symbol(mod, "_ZN15CRoomAttributes15GetAttributeIntE20tROOM_ATTRIBUTES_INT");
  add_map_page = (void *)so_symbol(mod, "_ZN18Menu_MP_CreateHost16AddMapGUIElementEii");
  text_set = (void *)so_symbol(mod, "_ZN9LayerText6SetStrEPKc");
  grid_set_page = (void *)so_symbol(mod, "_ZN9LayerGrid14SetCurrentPageEi");
  stream_mgr = (void *)so_symbol(mod, "_ZN3pig6stream9StreamMgr11GetInstanceEv");
  stream_create = (void *)so_symbol(mod, "_ZN3pig6stream9StreamMgr12CreateStreamERKNS_6StringE");
  if (!g_maps || !g_maps_count || !str_ctor || !str_dtor || !str_cstr || !mp_single || !mp_ctx || !mp_room || !attr_int ||
      !add_map_page || !stream_mgr || !stream_create) {
    debugPrintf("[maps] engine functions missing: custom maps off\n");
    ncm = 0;
    return;
  }
  int ok = so_hook_function(mod, "_ZNK12MPMapManager10GetMapInfoEi", (void *)get_map_info_hook) == 0;
  ok = ok && hook_tramp(mod, "_ZN3pig6stream16ZipStreamFactory12CreateStreamERKNS_6StringE", cm_zip_orig_head, &cm_zip_orig_cont,
                        (void *)zip_hook) == 0;
  ok = ok && hook_tramp(mod, "_ZN14CMatchingLocal9AddServerEiR10CNetworkIdiR15CRoomAttributes", cm_addserver_orig_head,
                        &cm_addserver_orig_cont, (void *)addserver_hook) == 0;
  ok = ok && hook_tramp(mod, "_ZN18Menu_MP_CreateHost6InitUIEv", cm_initui_orig_head, &cm_initui_orig_cont, (void *)initui_hook) == 0;
  ok = ok && hook_tramp(mod, "_ZN18Menu_MP_CreateHost13onMapSelectedEi", cm_mapsel_orig_head, &cm_mapsel_orig_cont,
                        (void *)mapsel_hook) == 0;
  if (!ok) {
    /* GetMapInfo may already answer for custom indices; without the lobby page nothing can select one */
    debugPrintf("[maps] hooks NOT applied (engine code differs): custom maps off\n");
    ncm = 0;
    return;
  }
  for (int k = 0; k < ncm; k++) cm_any_ovr |= cm[k].novr > 0;
  debugPrintf("[maps] %d custom map(s) in the multiplayer lobby\n", ncm);
}

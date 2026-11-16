/* Mod folder: files in switch/sixguns/mods/ replace the game's own files.
 *
 * The engine opens every data file through pig::stream::StreamMgr::CreateStream,
 * which asks its registered stream factories in order and takes the first that
 * has the file. Game::Init registers the patch OBB, then the main OBB, with
 * StreamMgr::AddPath (that is how the patch overrides the main OBB). AddPath on
 * a .zip creates a ZipStreamFactory, the same kind as the OBBs. Registering
 * each mod zip before Game::Init puts them first: a file inside a mod zip is
 * used instead of the OBB's copy, with no change to any loader.
 *
 * Inside a mod zip, paths are the OBB paths: texts, textures/foo.tga,
 * soundpack, levels.pak (whole archive), ... Readable from tools/decomp_all
 * (pig.c: StreamMgr, CreateStreamFactoryFromPath, ZipStreamFactory,
 * LibStreamLoader2). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "config.h"
#include "so_util.h"
#include "util.h"

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }

void mods_register(so_module *mod) {
  DIR *d = opendir(PORT_ROOT "/mods");
  if (!d) return;
  /* Each mods/<name>.zip is one mod. Loose files are not used: some loaders
   * (pig::stream::LibStreamLoader2: texts, interface, gui, popup) need the
   * memory-mapped stream a zip factory gives for stored entries, and crash on
   * the plain FileStream a folder factory returns. Zips must be "stored"
   * (no compression), like the OBBs: tools/scripts/pack_mod.py makes them. */
  char *names[64];
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) && n < 64) {
    size_t l = strlen(e->d_name);
    if (l > 4 && !strcasecmp(e->d_name + l - 4, ".zip")) names[n++] = strdup(e->d_name);
  }
  closedir(d);
  if (!n) { debugPrintf("[mods] mods/ has no .zip mods\n"); return; }
  /* Factories are asked in registration order and the first registered wins,
   * so mods are registered in reverse name order: with 10_base.zip and
   * 20_override.zip, 20_override wins where both have a file. */
  qsort(names, n, sizeof(*names), cmp_names);

  void *(*get_mgr)(void) = (void *)so_symbol(mod, "_ZN3pig6stream9StreamMgr11GetInstanceEv");
  void *(*add_path)(void *, void *, void *) = (void *)so_symbol(mod, "_ZN3pig6stream9StreamMgr7AddPathERKNS_6StringES4_");
  void (*str_ctor)(void *, const char *) = (void *)so_symbol(mod, "_ZN3pig6StringC2EPKc");
  void (*str_dtor)(void *) = (void *)so_symbol(mod, "_ZN3pig6StringD2Ev");
  if (!get_mgr || !add_path || !str_ctor || !str_dtor) {
    debugPrintf("[mods] engine stream functions not found: mods disabled\n");
    return;
  }
  for (int i = 0; i < n; i++) {
    char full[512];
    snprintf(full, sizeof(full), PORT_ROOT "/mods/%s", names[i]);
    _Alignas(16) unsigned char path[64], ext[64]; /* pig::String: 32 bytes; leave room */
    str_ctor(path, full);
    str_ctor(ext, "*");
    void *f = add_path(get_mgr(), path, ext);
    str_dtor(ext);
    str_dtor(path);
    debugPrintf("[mods] %s %s\n", names[i], f ? "loaded (overrides the game's files it contains)" : "NOT loaded (AddPath failed)");
    free(names[i]);
  }
}

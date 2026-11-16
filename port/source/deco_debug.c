/* Level-model diagnostics for custom maps (marker `deco_debug`, 2026-11-15).
 *
 * Built for the stadium map mod (tools/modding/maps/stadium): its model replaces
 * models/tdm_statickmesh_buildings.pig and its collision works, but nothing was drawn. Once a second
 * during gameplay this logs, for the Deco entities named in PORT_ROOT/deco_debug (one per line;
 * default TDM_StaticMesh_Buildings), whether the entity is visible, the model it holds
 * (Deco +0x130, set by Deco::Init from its "Model" param), the model's resource name, the engine's
 * own visible / culled flags and the bounding sphere the culling uses. Read-only. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "so_util.h"
#include "util.h"

extern so_module *port_engine_module(void);

typedef struct { float x, y, z; } vec3;
static int on = -1, nnames;
static char names[4][96];
static void *(*level_inst)(int), *(*level_entities)(void *), *(*find_by_name)(void *, void *);
static void (*str_ctor)(void *, const char *), (*str_dtor)(void *);
static const char *(*str_cstr)(void *);
static int (*ent_visible)(void *), (*rend_visible)(void *), (*rend_culled)(void *);
static void *(*model_res_name)(void *);
static const vec3 *(*sphere_center)(void *);
static float (*sphere_radius)(void *);
static const float *(*get_bbox)(void *);
/* TVector3D has a constructor: returned through x8 like any non-trivial C++ object; a 20-byte struct makes
 * the C compiler pass a result pointer in x8 too (only the first 12 bytes are written) */
typedef struct { float x, y, z, pad[2]; } vec3_ret;
static void *(*scene_mgr)(void), *(*scene_camera)(void *);
static vec3_ret (*node_abs_pos)(void *);
static float (*dist_to_cam)(void *);
static void *(*gp_inst)(int);

void deco_debug_frame(void) {
  if (on < 0) {
    FILE *f = fopen(PORT_ROOT "/deco_debug", "r");
    on = f != NULL;
    if (!f) return;
    char line[96];
    while (nnames < 4 && fgets(line, sizeof(line), f)) {
      line[strcspn(line, "\r\n")] = 0;
      if (line[0]) strcpy(names[nnames++], line);
    }
    fclose(f);
    if (!nnames) { strcpy(names[0], "TDM_StaticMesh_Buildings"); nnames = 1; }
    so_module *m = port_engine_module();
    level_inst = (void *)so_symbol(m, "_ZN9SingletonI9GameLevelE11GetInstanceEb");
    level_entities = (void *)so_symbol(m, "_ZN5clara9ClaraFile11GetEntitiesEv");
    find_by_name = (void *)so_symbol(m, "_ZN5clara11TSearchableINS_6EntityEE10FindByNameERKN3pig6StringE");
    str_ctor = (void *)so_symbol(m, "_ZN3pig6StringC2EPKc");
    str_dtor = (void *)so_symbol(m, "_ZN3pig6StringD2Ev");
    str_cstr = (void *)so_symbol(m, "_ZNK3pig6String5c_strEv");
    ent_visible = (void *)so_symbol(m, "_ZNK5clara6Entity9IsVisibleEv");
    rend_visible = (void *)so_symbol(m, "_ZNK3pig5scene10Renderable9IsVisibleEv");
    rend_culled = (void *)so_symbol(m, "_ZNK3pig5scene10Renderable8IsCulledEv");
    model_res_name = (void *)so_symbol(m, "_ZNK3pig5scene5Model15GetResourceNameEv");
    sphere_center = (void *)so_symbol(m, "_ZNK3pig5scene10Renderable23GetBoundingSphereCenterEv");
    sphere_radius = (void *)so_symbol(m, "_ZNK3pig5scene10Renderable23GetBoundingSphereRadiusEv");
    gp_inst = (void *)so_symbol(m, "_ZN9SingletonI11GS_GamePlayE11GetInstanceEb");
    get_bbox = (void *)so_symbol(m, "_ZN3pig5scene10Renderable14GetBoundingBoxEv");
    scene_mgr = (void *)so_symbol(m, "_ZN3pig5scene8SceneMgr11GetSceneMgrEv");
    scene_camera = (void *)so_symbol(m, "_ZNK3pig5scene8SceneMgr9GetCameraEv");
    node_abs_pos = (void *)so_symbol(m, "_ZNK3pig5scene4Node19GetAbsolutePositionEv");
    dist_to_cam = (void *)so_symbol(m, "_ZNK3pig5scene10Renderable19GetDistanceToCameraEv");
    if (!level_inst || !level_entities || !find_by_name || !str_ctor || !str_dtor || !ent_visible || !rend_visible ||
        !rend_culled || !sphere_center || !sphere_radius || !gp_inst) {
      debugPrintf("[deco] deco_debug: engine functions missing, off\n");
      on = 0;
      return;
    }
    debugPrintf("[deco] deco_debug on (%d name(s))\n", nnames);
  }
  if (!on) return;
  static unsigned frame;
  if (++frame % 60) return;
  if (!gp_inst(0)) return;
  void *lvl = level_inst(0);
  if (!lvl) return;
  void *ents = level_entities(lvl);
  if (!ents) return;
  if (scene_mgr && scene_camera && node_abs_pos) {
    void *sm = scene_mgr(), *cam = sm ? scene_camera(sm) : NULL;
    if (cam) { vec3_ret p = node_abs_pos(cam); debugPrintf("[deco] camera at (%.1f %.1f %.1f)\n", p.x, p.y, p.z); }
  }
  for (int i = 0; i < nnames; i++) {
    _Alignas(16) unsigned char s[64];
    str_ctor(s, names[i]);
    uint8_t *e = find_by_name(ents, s);
    str_dtor(s);
    if (!e) { debugPrintf("[deco] %s: no such entity\n", names[i]); continue; }
    void *mdl = *(void **)(e + 0x130);
    if (!mdl) { debugPrintf("[deco] %s: entity visible %d, NO MODEL\n", names[i], ent_visible(e) & 1); continue; }
    const char *rn = "?";
    if (model_res_name && str_cstr) { void *r = model_res_name(mdl); if (r) rn = str_cstr(r); }
    vec3 c = *sphere_center(mdl);
    debugPrintf("[deco] %s: entity visible %d, model %s, renderable visible %d culled %d, sphere (%.1f %.1f %.1f) r %.1f\n",
                names[i], ent_visible(e) & 1, rn, rend_visible(mdl) & 1, rend_culled(mdl) & 1, c.x, c.y, c.z, sphere_radius(mdl));
    /* SceneMgr::Cull: a sphere that intersects the frustum is tested again with this box (Frustum::CullAABB) */
    const float *b = get_bbox ? get_bbox(mdl) : NULL;
    if (dist_to_cam) debugPrintf("[deco] %s: distance to camera %.1f\n", names[i], dist_to_cam(mdl));
    if (b) debugPrintf("[deco] %s: bounding box (%.1f %.1f %.1f) - (%.1f %.1f %.1f)\n", names[i], b[0], b[1], b[2], b[3], b[4], b[5]);
  }
}

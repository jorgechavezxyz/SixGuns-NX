/* Graphics settings: a button next to the title screen's settings gear opens a menu of the engine's own
 * quality switches, with Low / Medium / High presets (2026-11-15, owner's request).
 *
 * Medium is exactly how the port drew before this menu (2x anisotropy, RGB565 textures, the levels' own
 * draw distance and LOD distances, all particles, screen effects and blob shadows, 60 fps), so a card
 * without graphics.txt plays as before. Settings live in switch/sixguns/graphics.txt (key=value; an old
 * file holding just a number is read as the anisotropy), never in the save.
 *
 *   anisotropy      graphics_quality.c (GL_EXT_texture_filter_anisotropic on mipmapped textures),
 *                   applied to already-loaded textures at once
 *   textures        Standard: ETC1 decoded to RGB565 (38-41 dB against the exact decode, ETC1 holds only
 *                   4-5 bits per channel); Compact: transcoded to BC1 (gl_debug.c etc1_bc1_rows: a quarter
 *                   of the GPU memory traffic, ~35 dB). Chosen per texture upload: after a restart
 *   draw_distance   SceneMgr::Render reads the cutoff (SceneMgr +0xf8, set by LevelDef/FogMgr through
 *                   SceneMgr::SetCutoffDistance, too small to hook); it is scaled for the call only
 *   model_detail    Model::PreRender picks LOD 1..5 from the switch distances at Model +0x80..+0x8c
 *                   (30/50/70/90 m by default; 532 meshes in the OBB have 2-4 LODs, mostly characters);
 *                   they are scaled for the call only
 *   particles       ps::ParticleMgr +0xf0 (GetMaxParticleCount, 6000)
 *   screen_effects  GS_GamePlay::UpdatePostFx starts the shockwave, depth of field and heat distortion
 *                   post effects; skipped = no effect started (Driver::EndPostEffect runs every frame
 *                   and draws nothing when none was started, as in most frames)
 *   shadows         ShadowRenderable::Render (the characters' blob shadows)
 *   frame_rate      main.c: swap interval 1 (60 fps) or 2 (30 fps, battery); loading screens stay unsynced
 * docs/re/subsystems/render_scene.md 5-6 lists where these values come from. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "config.h"
#include "input.h"
#include "so_util.h"
#include "util.h"

#define ENGINE_SCALE (SCREEN_H / 320.0f)
int sui_ready(void);
void sui_begin(void);
void sui_end(void);
void sui_color(float r, float g, float b, float a);
void sui_rect(float x0, float y0, float x1, float y1);
void sui_frame(float x0, float y0, float x1, float y1, float t);
float sui_text(float x, float y, const char *s, int px, float maxw);
void sui_text_shadow(float x, float y, const char *s, int px, float maxw);
float sui_text_w(const char *s, int px);
#define G_A "\xee\x83\xa0"
#define G_B "\xee\x83\xa1"

/* ------------------------------------------------------------------ settings */
enum { S_PRESET, S_ANISO, S_TEX, S_DRAW, S_LOD, S_PART, S_POST, S_SHADOW, S_FPS, S_COUNT };
typedef struct {
  const char *label, *key, *help;
  int n;
  const char *names[5], *values[5];
} setting_def;
static const setting_def defs[S_COUNT] = {
  { "Preset", "preset", "Low favours speed and battery, High favours detail. Medium is the original look.", 4,
    { "Low", "Medium", "High", "Custom" }, { "low", "medium", "high", "custom" } },
  { "Texture filtering", "anisotropy", "Anisotropic filtering: sharper ground and walls at an angle.", 5,
    { "Off", "2x", "4x", "8x", "16x" }, { "1", "2", "4", "8", "16" } },
  { "Texture quality", "textures", "Compact uses less GPU memory, slightly softer. Applies after restarting the game.", 2,
    { "Compact", "Standard" }, { "compact", "standard" } },
  { "Draw distance", "draw_distance", "How far away scenery and objects are still drawn.", 3,
    { "Near", "Normal", "Far" }, { "near", "normal", "far" } },
  { "Model detail", "model_detail", "How far characters and objects keep their detailed models.", 3,
    { "Low", "Normal", "High" }, { "low", "normal", "high" } },
  { "Particle effects", "particles", "Smoke, fire, sparks and blood. Reduced caps how many are shown at once.", 2,
    { "Reduced", "Full" }, { "reduced", "full" } },
  { "Screen effects", "screen_effects", "Shockwaves, depth of field and heat distortion.", 2,
    { "Off", "On" }, { "off", "on" } },
  { "Shadows", "shadows", "Shadows under characters.", 2, { "Off", "On" }, { "off", "on" } },
  { "Frame rate", "frame_rate", "30 fps saves battery. Not part of the presets.", 2, { "60 fps", "30 fps" }, { "60", "30" } },
};
/* presets for S_ANISO..S_SHADOW (frame rate is the player's own choice) */
static const int presets[3][S_COUNT] = {
  /*        preset aniso tex draw lod part post shadow fps */
  /* low */ { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
  /* med */ { 1, 1, 1, 1, 1, 1, 1, 1, 0 },
  /* high */{ 2, 3, 1, 2, 2, 1, 1, 1, 0 },
};
static int cur[S_COUNT];
static int loaded;
static int tex_at_start = -1; /* the texture mode in use (fixed at the first upload) */

static void apply_preset(int p) {
  for (int i = S_ANISO; i <= S_SHADOW; i++) cur[i] = presets[p][i];
  cur[S_PRESET] = p;
}
static void match_preset(void) {
  for (int p = 0; p < 3; p++) {
    int same = 1;
    for (int i = S_ANISO; i <= S_SHADOW; i++) same &= cur[i] == presets[p][i];
    if (same) { cur[S_PRESET] = p; return; }
  }
  cur[S_PRESET] = 3;
}

void gfx_settings_load(void) {
  if (loaded) return;
  loaded = 1;
  apply_preset(1);
  cur[S_FPS] = 0;
  FILE *f = fopen(PORT_ROOT "/graphics.txt", "r");
  if (!f) { debugPrintf("[gfx] no graphics.txt: Medium (the original look)\n"); return; }
  char line[128];
  int any = 0;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0] || line[0] == '#') continue;
    char *eq = strchr(line, '=');
    if (!eq) { /* old graphics.txt: a single anisotropy number */
      float v = strtof(line, NULL);
      for (int k = 0; k < 5; k++) if (fabsf(v - (float)(1 << k)) < 0.5f) cur[S_ANISO] = k;
      any = 1;
      continue;
    }
    *eq = 0;
    for (int i = 0; i < S_COUNT; i++)
      if (!strcmp(line, defs[i].key))
        for (int k = 0; k < defs[i].n; k++)
          if (!strcmp(eq + 1, defs[i].values[k])) { cur[i] = k; any = 1; }
  }
  fclose(f);
  if (cur[S_PRESET] < 3) {
    /* a named preset stands for its values; a file edited by hand says "custom" (or nothing) */
    int p = cur[S_PRESET];
    int same = 1;
    for (int i = S_ANISO; i <= S_SHADOW; i++) same &= cur[i] == presets[p][i];
    if (!same) match_preset();
  }
  debugPrintf("[gfx] graphics.txt%s: preset %s, anisotropy %s, textures %s, draw %s, models %s, particles %s, effects %s, shadows %s, %s\n",
              any ? "" : " (empty)", defs[0].names[cur[0]], defs[1].names[cur[1]], defs[2].names[cur[2]], defs[3].names[cur[3]],
              defs[4].names[cur[4]], defs[5].names[cur[5]], defs[6].names[cur[6]], defs[7].names[cur[7]], defs[8].names[cur[8]]);
}

static void save(void) {
  FILE *f = fopen(PORT_ROOT "/graphics.txt", "w");
  if (!f) { debugPrintf("[gfx] graphics.txt NOT written\n"); return; }
  fprintf(f, "# Six Guns graphics settings: the button next to the settings gear on the title screen\n");
  for (int i = 0; i < S_COUNT; i++) fprintf(f, "%s=%s\n", defs[i].key, defs[i].values[cur[i]]);
  fclose(f);
  debugPrintf("[gfx] graphics.txt saved (preset %s)\n", defs[0].names[cur[0]]);
}

/* for graphics_quality.c, gl_debug.c and main.c */
float gfx_anisotropy(void) { gfx_settings_load(); return (float)(1 << cur[S_ANISO]); }
int gfx_textures_compact(void) {
  gfx_settings_load();
  if (tex_at_start < 0) tex_at_start = cur[S_TEX];
  return tex_at_start == 0;
}
int gfx_swap_interval(void) { gfx_settings_load(); return cur[S_FPS] ? 2 : 1; }

static float draw_scale(void) { static const float v[3] = { 0.7f, 1.0f, 1.6f }; return v[cur[S_DRAW]]; }
static float lod_scale(void) { static const float v[3] = { 0.5f, 1.0f, 2.5f }; return v[cur[S_LOD]]; }

/* ------------------------------------------------------------------ engine hooks */
extern void gfx_scene_orig(void *, void *, void *, void *);
extern const uint32_t gfx_scene_orig_head[4];
uintptr_t gfx_scene_orig_cont;
static void scene_render_hook(uint8_t *sm, void *a, void *b, void *c) {
  float s = draw_scale();
  if (s == 1.0f || !sm) { gfx_scene_orig(sm, a, b, c); return; }
  float *cut = (float *)(sm + 0xf8), keep = *cut;
  *cut = keep * s;
  gfx_scene_orig(sm, a, b, c);
  *cut = keep;
}

extern void gfx_prerender_orig(void *);
extern const uint32_t gfx_prerender_orig_head[4];
uintptr_t gfx_prerender_orig_cont;
static void prerender_hook(uint8_t *model) {
  float s = lod_scale();
  if (s == 1.0f) { gfx_prerender_orig(model); return; }
  float *d = (float *)(model + 0x80), keep[4];
  memcpy(keep, d, sizeof(keep));
  for (int i = 0; i < 4; i++) d[i] = keep[i] * s;
  gfx_prerender_orig(model);
  memcpy(d, keep, sizeof(keep));
}

extern long gfx_postfx_orig(void *);
extern const uint32_t gfx_postfx_orig_head[4];
uintptr_t gfx_postfx_orig_cont;
static long postfx_hook(void *gp) { return cur[S_POST] ? gfx_postfx_orig(gp) : 1; }

extern long gfx_shadow_orig(void *);
extern const uint32_t gfx_shadow_orig_head[4];
uintptr_t gfx_shadow_orig_cont;
static long shadow_hook(void *s) { return cur[S_SHADOW] ? gfx_shadow_orig(s) : 0; }

static uint8_t **particle_mgr; /* Singleton<ps::ParticleMgr>::s_instance */

static int hook_tramp(so_module *mod, const char *sym, const uint32_t *head, uintptr_t *cont, void *fn) {
  uintptr_t a = so_symbol(mod, sym);
  if (!a || !mod->image || memcmp(mod->image + (a - mod->base), head, 16) != 0) {
    debugPrintf("[gfx] %s: start differs, setting not available\n", sym);
    return 0;
  }
  *cont = a + 16;
  return so_hook_function(mod, sym, fn) == 0;
}

static int hooks_ok[S_COUNT];
static void *(*popup_inst)(int);
static int (*popup_enabled)(void *);

/* before so_finalize (main.c) */
void gfx_settings_install(so_module *mod) {
  gfx_settings_load();
  hooks_ok[S_DRAW] = hook_tramp(mod, "_ZN3pig5scene8SceneMgr6RenderEv", gfx_scene_orig_head, &gfx_scene_orig_cont,
                                (void *)scene_render_hook);
  hooks_ok[S_LOD] = hook_tramp(mod, "_ZN3pig5scene5Model9PreRenderEv", gfx_prerender_orig_head, &gfx_prerender_orig_cont,
                               (void *)prerender_hook);
  hooks_ok[S_POST] = hook_tramp(mod, "_ZN11GS_GamePlay12UpdatePostFxEv", gfx_postfx_orig_head, &gfx_postfx_orig_cont,
                                (void *)postfx_hook);
  hooks_ok[S_SHADOW] = hook_tramp(mod, "_ZN16ShadowRenderable6RenderEv", gfx_shadow_orig_head, &gfx_shadow_orig_cont,
                                  (void *)shadow_hook);
  particle_mgr = (uint8_t **)so_symbol(mod, "_ZN9SingletonIN2ps11ParticleMgrEE10s_instanceE");
  hooks_ok[S_PART] = particle_mgr != NULL;
  hooks_ok[S_PRESET] = hooks_ok[S_ANISO] = hooks_ok[S_TEX] = hooks_ok[S_FPS] = 1;
  popup_inst = (void *)so_symbol(mod, "_ZN9SingletonI5PopupE11GetInstanceEb");
  popup_enabled = (void *)so_symbol(mod, "_ZN5Popup9IsEnabledEv");
  debugPrintf("[gfx] graphics settings: draw distance %s, model detail %s, screen effects %s, shadows %s, particles %s\n",
              hooks_ok[S_DRAW] ? "ok" : "off", hooks_ok[S_LOD] ? "ok" : "off", hooks_ok[S_POST] ? "ok" : "off",
              hooks_ok[S_SHADOW] ? "ok" : "off", hooks_ok[S_PART] ? "ok" : "off");
}

/* ------------------------------------------------------------------ title button and menu */
static int open, sel;
static unsigned long frame_no;
static int rep_dir, rep_next, hrep_dir, hrep_next;
static int aniso_dirty;

/* the button: right of the settings gear (gear at engine units 4..50 x 271..317, social.c) */
static const float BTN[4] = { 54, 271, 100, 317 }; /* engine units */
static int on_title(void) {
  extern const char *current_menu_class;
  if (strcmp(current_menu_class, "15Menu_MainMenu3D")) return 0;
  void *p = popup_inst ? popup_inst(0) : NULL;
  return !(p && popup_enabled && (popup_enabled(p) & 1));
}
/* for pointer.c: the button as a focus target while the title shows it */
int gfx_title_button(float r[4]) {
  if (open || !on_title()) return 0;
  memcpy(r, BTN, sizeof(BTN));
  return 1;
}
int gfx_menu_open(void) { return open; }

static int vstep(const InputFrame *in) {
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
static int hstep(const InputFrame *in) {
  int dir = 0;
  if (in->held & HidNpadButton_Left) dir = -1;
  if (in->held & HidNpadButton_Right) dir = 1;
  if (!dir && abs(in->ls.x) > abs(in->ls.y) && abs(in->ls.x) > 16000) dir = in->ls.x > 0 ? 1 : -1;
  int step = 0;
  if (dir != hrep_dir) { if (dir) step = dir; hrep_next = (int)frame_no + 22; }
  else if (dir && (int)frame_no >= hrep_next) { step = dir; hrep_next = (int)frame_no + 8; }
  hrep_dir = dir;
  return step;
}

enum { ROWS = S_COUNT + 1, ROW_DONE = S_COUNT, ROW_H = 50, PW = 820 };
static float px0, py0;
static void layout(void) {
  float h = 120 + ROWS * ROW_H + 96;
  px0 = 640 - PW / 2;
  py0 = 360 - h / 2;
}
static void change(int row, int dir) {
  if (row < 0 || row >= S_COUNT || !hooks_ok[row]) return;
  int n = row == S_PRESET ? 3 : defs[row].n; /* "Custom" is never picked by hand */
  int v = cur[row];
  if (row == S_PRESET && v == 3) v = dir > 0 ? -1 : 3;
  v = (v + dir + n) % n;
  if (row == S_PRESET) apply_preset(v);
  else { cur[row] = v; if (row != S_FPS) match_preset(); }
  aniso_dirty = 1;
  debugPrintf("[gfx] %s -> %s (preset %s)\n", defs[row].label, defs[row].names[cur[row]], defs[0].names[cur[0]]);
}
static void close_menu(void) {
  open = 0;
  save();
}
static void eat(InputFrame *in) {
  in->down = in->held = in->up = 0;
  in->ls.x = in->ls.y = in->rs.x = in->rs.y = 0;
  in->ntouch = 0;
  in->touch_started = 0;
}

/* main.c, before the other UI layers and the pointer: the menu takes all input while open */
void gfx_menu_input(InputFrame *in) {
  frame_no++;
  if (!open) {
    if (!on_title()) return;
    int go = 0;
    int pointer_debug_state(float r[4]);
    float r[4];
    if ((in->down & HidNpadButton_A) && pointer_debug_state(r) == 1 && fabsf(r[0] - BTN[0]) < 2 && fabsf(r[1] - BTN[1]) < 2) go = 1;
    for (int i = 0; i < in->ntouch; i++) {
      int id = in->t[i].id;
      if (id < 0 || id >= 16 || !(in->touch_started & (1u << id))) continue;
      float ex = in->t[i].x / ENGINE_SCALE, ey = in->t[i].y / ENGINE_SCALE;
      if (ex >= BTN[0] && ex < BTN[2] && ey >= BTN[1] && ey < BTN[3]) go = 1;
    }
    if (!go) return;
    open = 1;
    sel = 0;
    rep_dir = hrep_dir = 0;
    debugPrintf("[gfx] graphics menu opened\n");
    eat(in);
    return;
  }
  if (!on_title()) { close_menu(); return; } /* the title went away underneath */
  layout();
  int step = vstep(in);
  if (step) sel = (sel + step + ROWS) % ROWS;
  int h = hstep(in);
  if (h && sel < S_COUNT) change(sel, h);
  if (in->down & HidNpadButton_A) {
    if (sel == ROW_DONE) close_menu();
    else change(sel, 1);
  }
  if (in->down & HidNpadButton_B) close_menu();
  for (int i = 0; i < in->ntouch; i++) {
    int id = in->t[i].id;
    if (id < 0 || id >= 16 || !(in->touch_started & (1u << id))) continue;
    int x = in->t[i].x, y = in->t[i].y;
    int row = (int)((y - (py0 + 110)) / ROW_H);
    if (x >= px0 && x < px0 + PW && y >= py0 + 110 && row >= 0 && row < ROWS) {
      sel = row;
      if (row == ROW_DONE) close_menu();
      else if (x > px0 + PW / 2) change(row, x < px0 + PW / 2 + 60 ? -1 : 1);
    }
    break;
  }
  eat(in);
}

static void arrow(float cx, float cy, int dir) {
  for (int k = 0; k < 10; k++) {
    float w = 10 - k, x = cx + dir * (k - 5) * 1.5f;
    sui_rect(x - 1, cy - w, x + 1, cy + w);
  }
}
static void draw_button(void) {
  float x0 = BTN[0] * ENGINE_SCALE, y0 = BTN[1] * ENGINE_SCALE + 2, x1 = BTN[2] * ENGINE_SCALE, y1 = BTN[3] * ENGINE_SCALE - 6;
  sui_color(0.30f, 0.22f, 0.08f, 1); sui_rect(x0, y0, x1, y1);
  sui_color(0.93f, 0.72f, 0.18f, 1); sui_rect(x0 + 3, y0 + 3, x1 - 3, y1 - 3);
  sui_color(1.0f, 0.84f, 0.36f, 1); sui_rect(x0 + 3, y0 + 3, x1 - 3, (y0 + y1) / 2);
  /* icon: three sliders */
  float ix0 = x0 + 20, ix1 = x1 - 20, cy = (y0 + y1) / 2;
  static const float knob[3] = { 0.30f, 0.70f, 0.45f };
  for (int k = 0; k < 3; k++) {
    float yy = cy - 24 + k * 24;
    sui_color(0.28f, 0.18f, 0.05f, 0.55f); sui_rect(ix0 + 2, yy - 2, ix1 + 2, yy + 5);
    sui_color(1, 1, 1, 1); sui_rect(ix0, yy - 4, ix1, yy + 3);
    float kx = ix0 + (ix1 - ix0) * knob[k];
    sui_color(0.28f, 0.18f, 0.05f, 0.55f); sui_rect(kx - 6, yy - 9, kx + 10, yy + 12);
    sui_color(1, 1, 1, 1); sui_rect(kx - 8, yy - 11, kx + 8, yy + 9);
  }
}

static void draw_menu(void) {
  layout();
  sui_color(0, 0, 0, 0.72f); sui_rect(0, 0, SCREEN_W, SCREEN_H);
  float x1 = px0 + PW, y1 = py0 + 110 + ROWS * ROW_H + 96;
  sui_color(0.12f, 0.06f, 0.02f, 1); sui_rect(px0, py0, x1, y1);
  sui_color(0.78f, 0.58f, 0.26f, 1); sui_frame(px0, py0, x1, y1, 3);
  sui_color(1.0f, 0.86f, 0.45f, 1);
  sui_text_shadow(640 - sui_text_w("Graphics", 42) / 2, py0 + 58, "Graphics", 42, 0);
  sui_color(0.85f, 0.75f, 0.55f, 1);
  const char *sub = "Changes apply right away and are saved on the SD card";
  sui_text(640 - sui_text_w(sub, 21) / 2, py0 + 92, sub, 21, 0);
  for (int i = 0; i < ROWS; i++) {
    float r0 = py0 + 110 + i * ROW_H + 4, r1 = r0 + ROW_H - 8, cy = (r0 + r1) / 2;
    if (i == sel) { sui_color(1.0f, 0.84f, 0.40f, 1); sui_rect(px0 + 24, r0, x1 - 24, r1);
                    sui_color(0.45f, 0.24f, 0.05f, 1); sui_frame(px0 + 24, r0, x1 - 24, r1, 3); }
    else { sui_color(0.95f, 0.87f, 0.69f, 1); sui_rect(px0 + 24, r0, x1 - 24, r1); }
    if (i == ROW_DONE) {
      sui_color(0.22f, 0.12f, 0.04f, 1);
      sui_text(640 - sui_text_w("Done", 26) / 2, cy + 9, "Done", 26, 0);
      continue;
    }
    int avail = hooks_ok[i];
    sui_color(0.22f, 0.12f, 0.04f, avail ? 1.0f : 0.45f);
    sui_text(px0 + 50, cy + 9, defs[i].label, 26, 300);
    const char *v = avail ? defs[i].names[cur[i]] : "Not available";
    char vb[48];
    if (i == S_TEX && tex_at_start >= 0 && cur[S_TEX] != tex_at_start) { snprintf(vb, sizeof(vb), "%s (after restart)", v); v = vb; }
    float vc = px0 + PW * 0.72f;
    sui_text(vc - sui_text_w(v, 24) / 2, cy + 8, v, 24, 0);
    if (avail) { arrow(px0 + PW / 2 + 40, cy, -1); arrow(x1 - 64, cy, 1); }
  }
  sui_color(0.85f, 0.75f, 0.55f, 1);
  if (sel < S_COUNT) sui_text(px0 + 40, y1 - 58, defs[sel].help, 20, PW - 80);
  sui_color(0.95f, 0.88f, 0.70f, 1);
  sui_text_shadow(px0 + 30, y1 - 18, G_A " Change   Left/Right Change", 22, 0);
  sui_text_shadow(x1 - 30 - sui_text_w(G_B " Back", 22), y1 - 18, G_B " Back", 22, 0);
}

/* main.c, after the engine drew the frame */
void gfx_overlay(void) {
  if (aniso_dirty) {
    aniso_dirty = 0;
    extern void graphics_quality_set(float);
    graphics_quality_set(gfx_anisotropy());
  }
  if (particle_mgr && *particle_mgr) {
    int cap = cur[S_PART] ? 6000 : 2000;
    int *p = (int *)(*particle_mgr + 0xf0);
    if (*p != cap && *p > 0) *p = cap;
  }
  if (!sui_ready()) return;
  int title = on_title();
  if (!title && !open) return;
  sui_begin();
  if (open) draw_menu();
  else draw_button();
  sui_end();
}

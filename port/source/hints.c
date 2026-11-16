/* Button-hint sidebar: a small list of controls on the right side of the
 * screen during gameplay, like the control lists in console open-world games.
 *
 * Text is rendered with FreeType from the Switch's own system fonts (pl:u
 * shared fonts: "Standard" for the labels, "NintendoExt" for the button
 * glyphs), so no font files are shipped. Every row the sidebar can show is
 * rendered once into one alpha texture (atlas) at start; drawing is then a
 * handful of textured quads after the engine's frame, with the GL state saved
 * and restored like pointer.c does.
 *
 * Shown only in gameplay (no menu, popup, cutscene); the on-foot and
 * on-horse sets differ (Player::IsDriving). Marker `no_button_hints` turns it
 * off. If a button glyph is missing from the system font, the row shows a
 * text label like "[ZL]" instead. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "config.h"
#include "so_util.h"
#include "util.h"

so_module *port_engine_module(void);

enum { ATLAS_W = 512, ROW_H = 23, GLYPH_PX = 21, TEXT_PX = 16, ICON_W = 27 };

/* Nintendo extended font code points (filled button icons). */
#define G_A 0xE0E0
#define G_B 0xE0E1
#define G_X 0xE0E2
#define G_Y 0xE0E3
#define G_L 0xE0E4
#define G_R 0xE0E5
#define G_ZL 0xE0E6
#define G_ZR 0xE0E7
#define G_LEFT 0xE0ED
#define G_DOWN 0xE0EC
#define G_UP 0xE0EB
#define G_PLUS 0xE0EF

typedef struct { unsigned glyph; const char *fallback, *label; int w; } row;
static row rows[] = {
  /* 0..10 on foot */
  { G_ZL, "ZL", "Aim     Shoot" },
  { G_Y, "Y", "Reload" },
  { G_X, "X", "Next weapon" },
  { G_R, "R", "Tap: throw dynamite" },
  { G_R, "R", "Hold + : dynamite type" },
  { G_L, "L", "Hold + : use elixir" },
  { 0, "L3", "Take / leave cover" },
  { G_B, "B", "Roll   (R3: turn around)" },
  { G_A, "A", "Interact" },
  { G_LEFT, "<", "Call horse" },
  { G_PLUS, "+", "Menu     Map" },
  /* 11.. on horse */
  { G_A, "A", "Spur horse" },
  { G_LEFT, "<", "Dismount" },
  /* 13..15: elixir key (glyph only), 16..26: "0".."9", ":" (cooldown timer) */
  { G_LEFT, "<", "" }, { G_UP, "^", "" }, { 0xE0EE, ">", "" },
  { 0, "", "0" }, { 0, "", "1" }, { 0, "", "2" }, { 0, "", "3" }, { 0, "", "4" }, { 0, "", "5" },
  { 0, "", "6" }, { 0, "", "7" }, { 0, "", "8" }, { 0, "", "9" }, { 0, "", ":" },
  /* 27: cutscene skip */
  { G_A, "A", "Hold to skip" },
  /* 28: D-pad Down glyph for the HUD shortcut (buy the offered item) */
  { G_DOWN, "v", "" },
};
enum { ROW_SKIP = 27, ROW_KEY_DOWN = 28 };
enum { ROW_KEY = 13, ROW_DIGIT = 16 };
#define N_ROWS (sizeof(rows) / sizeof(*rows))
static const int foot_rows[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
static const int horse_rows[] = { 11, 0, 1, 3, 4, 12, 10 };

static int state; /* 0 untried, 1 ready, -1 off/failed */
static GLuint tex, prog;
static GLint u_screen, u_color, u_tex;
static int (*player_is_driving)(void *);
static void *(*level_instance)(int);
static void *(*level_get_player)(void *);
static void *(*gp_inst)(int);
static int (*gp_cinematic)(void *);

static struct {
  typeof(&glGetIntegerv) GetIntegerv; typeof(&glIsEnabled) IsEnabled; typeof(&glEnable) Enable;
  typeof(&glDisable) Disable; typeof(&glBlendFuncSeparate) BlendFuncSeparate;
  typeof(&glBlendEquationSeparate) BlendEquationSeparate; typeof(&glColorMask) ColorMask;
  typeof(&glGetBooleanv) GetBooleanv; typeof(&glUseProgram) UseProgram; typeof(&glBindBuffer) BindBuffer;
  typeof(&glBindFramebuffer) BindFramebuffer; typeof(&glViewport) Viewport;
  typeof(&glGetVertexAttribiv) GetVertexAttribiv; typeof(&glGetVertexAttribPointerv) GetVertexAttribPointerv;
  typeof(&glVertexAttribPointer) VertexAttribPointer; typeof(&glEnableVertexAttribArray) EnableVertexAttribArray;
  typeof(&glDisableVertexAttribArray) DisableVertexAttribArray; typeof(&glDrawArrays) DrawArrays;
  typeof(&glCreateShader) CreateShader; typeof(&glShaderSource) ShaderSource; typeof(&glCompileShader) CompileShader;
  typeof(&glCreateProgram) CreateProgram; typeof(&glAttachShader) AttachShader;
  typeof(&glBindAttribLocation) BindAttribLocation; typeof(&glLinkProgram) LinkProgram;
  typeof(&glGetUniformLocation) GetUniformLocation; typeof(&glUniform2f) Uniform2f; typeof(&glUniform4f) Uniform4f;
  typeof(&glUniform1i) Uniform1i; typeof(&glGetProgramiv) GetProgramiv; typeof(&glGenTextures) GenTextures;
  typeof(&glBindTexture) BindTexture; typeof(&glTexImage2D) TexImage2D; typeof(&glTexParameteri) TexParameteri;
  typeof(&glActiveTexture) ActiveTexture; typeof(&glPixelStorei) PixelStorei;
} G;

/* Draws one UTF-32 code point into the atlas with its pen at (*pen, baseline). */
static void blit_char(FT_Face f, unsigned cp, unsigned char *atlas, int *pen, int baseline, int y0, int y1) {
  FT_UInt gi = FT_Get_Char_Index(f, cp);
  if (FT_Load_Glyph(f, gi, FT_LOAD_RENDER)) return;
  FT_GlyphSlot g = f->glyph;
  for (unsigned r = 0; r < g->bitmap.rows; r++) {
    int y = baseline - g->bitmap_top + (int)r;
    if (y < y0 || y >= y1) continue;
    for (unsigned c = 0; c < g->bitmap.width; c++) {
      int x = *pen + g->bitmap_left + (int)c;
      if (x < 0 || x >= ATLAS_W) continue;
      unsigned char v = g->bitmap.buffer[r * g->bitmap.pitch + c];
      if (v > atlas[y * ATLAS_W + x]) atlas[y * ATLAS_W + x] = v;
    }
  }
  *pen += (int)(g->advance.x >> 6);
}

/* UTF-8 text; code points in the Nintendo private-use range (button icons)
 * come from the extended font `fe` at the icon size. */
static FT_Face ext_face;
static void blit_str(FT_Face f, const char *s, unsigned char *atlas, int *pen, int baseline, int y0, int y1) {
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    unsigned cp = *p++;
    if (cp >= 0xe0 && p[0] && p[1]) { cp = ((cp & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f); p += 2; }
    else if (cp >= 0xc0 && p[0]) { cp = ((cp & 0x1f) << 6) | (p[0] & 0x3f); p += 1; }
    if (cp >= 0xE000 && cp < 0xF900 && ext_face && FT_Get_Char_Index(ext_face, cp)) {
      FT_Set_Pixel_Sizes(ext_face, 0, GLYPH_PX - 3);
      blit_char(ext_face, cp, atlas, pen, baseline + 2, y0, y1);
      FT_Set_Pixel_Sizes(f, 0, TEXT_PX);
      continue;
    }
    blit_char(f, cp, atlas, pen, baseline, y0, y1);
  }
}

static int build_atlas(void) {
  Result rc = plInitialize(PlServiceType_User);
  if (R_FAILED(rc)) { debugPrintf("[hints] pl:u unavailable (0x%x)\n", rc); return -1; }
  PlFontData std, ext;
  int have_ext = 0;
  if (R_FAILED(plGetSharedFontByType(&std, PlSharedFontType_Standard))) {
    debugPrintf("[hints] no system font\n");
    plExit();
    return -1;
  }
  have_ext = R_SUCCEEDED(plGetSharedFontByType(&ext, PlSharedFontType_NintendoExt));
  FT_Library lib;
  FT_Face fs = NULL, fe = NULL;
  if (FT_Init_FreeType(&lib)) { plExit(); return -1; }
  if (FT_New_Memory_Face(lib, std.address, std.size, 0, &fs)) { FT_Done_FreeType(lib); plExit(); return -1; }
  if (have_ext && FT_New_Memory_Face(lib, ext.address, ext.size, 0, &fe)) fe = NULL;
  ext_face = fe;

  int h = (int)N_ROWS * ROW_H;
  static unsigned char atlas[ATLAS_W * N_ROWS * ROW_H];
  memset(atlas, 0, sizeof(atlas));
  int missing = 0;
  for (size_t i = 0; i < N_ROWS; i++) {
    int y0 = (int)i * ROW_H, y1 = y0 + ROW_H, base = y0 + ROW_H / 2 + TEXT_PX * 7 / 20;
    int pen = 2;
    if (fe && rows[i].glyph && FT_Get_Char_Index(fe, rows[i].glyph)) {
      FT_Set_Pixel_Sizes(fe, 0, GLYPH_PX);
      blit_char(fe, rows[i].glyph, atlas, &pen, y0 + ROW_H / 2 + GLYPH_PX * 2 / 5, y0, y1);
    } else if (!rows[i].fallback[0]) {
      /* text-only row (digits): no icon column */
    } else {
      missing++;
      FT_Set_Pixel_Sizes(fs, 0, TEXT_PX);
      blit_str(fs, "[", atlas, &pen, base, y0, y1);
      blit_str(fs, rows[i].fallback, atlas, &pen, base, y0, y1);
      blit_str(fs, "]", atlas, &pen, base, y0, y1);
    }
    if (rows[i].fallback[0] && rows[i].label[0]) pen = pen < ICON_W ? ICON_W : pen + 6;
    FT_Set_Pixel_Sizes(fs, 0, TEXT_PX);
    blit_str(fs, rows[i].label, atlas, &pen, base, y0, y1);
    rows[i].w = pen + 2;
  }
  ext_face = NULL;
  if (fe) FT_Done_Face(fe);
  FT_Done_Face(fs);
  FT_Done_FreeType(lib);
  plExit();

  GLint prev_tex = 0, prev_unit = 0, prev_align = 4;
  G.GetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
  G.ActiveTexture(GL_TEXTURE0);
  G.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  G.GetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
  G.GenTextures(1, &tex);
  G.BindTexture(GL_TEXTURE_2D, tex);
  G.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
  G.TexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS_W, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, atlas);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  G.PixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
  G.BindTexture(GL_TEXTURE_2D, prev_tex);
  G.ActiveTexture(prev_unit);
  debugPrintf("[hints] button-hint sidebar ready (%d rows, %d button glyph(s) missing -> text labels)\n", (int)N_ROWS,
              missing);
  return 0;
}

static int setup(void) {
  if (state) return state > 0;
  state = -1;
  FILE *f = fopen(PORT_ROOT "/no_button_hints", "r");
  if (f) { fclose(f); debugPrintf("[hints] off (no_button_hints marker)\n"); return 0; }
#define L(n) if (!(G.n = (void *)eglGetProcAddress("gl" #n))) { debugPrintf("[hints] no gl" #n "\n"); return 0; }
  L(GetIntegerv) L(IsEnabled) L(Enable) L(Disable) L(BlendFuncSeparate) L(BlendEquationSeparate) L(ColorMask)
  L(GetBooleanv) L(UseProgram) L(BindBuffer) L(BindFramebuffer) L(Viewport) L(GetVertexAttribiv)
  L(GetVertexAttribPointerv) L(VertexAttribPointer) L(EnableVertexAttribArray) L(DisableVertexAttribArray)
  L(DrawArrays) L(CreateShader) L(ShaderSource) L(CompileShader) L(CreateProgram) L(AttachShader)
  L(BindAttribLocation) L(LinkProgram) L(GetUniformLocation) L(Uniform2f) L(Uniform4f) L(Uniform1i) L(GetProgramiv)
  L(GenTextures) L(BindTexture) L(TexImage2D) L(TexParameteri) L(ActiveTexture) L(PixelStorei)
#undef L

  static const char *vs =
      "attribute vec4 a; uniform vec2 u_screen; varying vec2 uv;\n"
      "void main() { uv = a.zw; gl_Position = vec4(a.x / u_screen.x * 2.0 - 1.0, 1.0 - a.y / u_screen.y * 2.0, 0.0, 1.0); }\n";
  static const char *fs =
      "precision mediump float; uniform vec4 u_color; uniform sampler2D u_tex; varying vec2 uv;\n"
      "void main() { gl_FragColor = vec4(u_color.rgb, u_color.a * (uv.x < 0.0 ? 1.0 : texture2D(u_tex, uv).a)); }\n";
  GLint prev_prog = 0;
  G.GetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  GLuint v = G.CreateShader(GL_VERTEX_SHADER), fr = G.CreateShader(GL_FRAGMENT_SHADER);
  G.ShaderSource(v, 1, &vs, NULL); G.CompileShader(v);
  G.ShaderSource(fr, 1, &fs, NULL); G.CompileShader(fr);
  prog = G.CreateProgram();
  G.AttachShader(prog, v); G.AttachShader(prog, fr);
  G.BindAttribLocation(prog, 0, "a");
  G.LinkProgram(prog);
  GLint ok = 0;
  G.GetProgramiv(prog, GL_LINK_STATUS, &ok);
  G.UseProgram(prev_prog);
  if (!ok) { debugPrintf("[hints] shader link failed\n"); return 0; }
  u_screen = G.GetUniformLocation(prog, "u_screen");
  u_color = G.GetUniformLocation(prog, "u_color");
  u_tex = G.GetUniformLocation(prog, "u_tex");
  if (build_atlas() < 0) return 0;
  state = 1;
  return 1;
}

/* x0,y0-x1,y1 on screen; u0,v0-u1,v1 in the atlas (u0 < 0: solid colour). */
static void tquad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float r, float g,
                  float b, float a) {
  GLfloat q[16] = { x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y1, u1, v1 };
  G.Uniform4f(u_color, r, g, b, a);
  G.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, q);
  G.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Which set to show now: 0 none, 1 on foot, 2 on horse. */
static int current_set(void) {
  static int resolved;
  if (!resolved) {
    resolved = 1;
    so_module *mod = port_engine_module();
    player_is_driving = (void *)so_symbol(mod, "_ZN6Player9IsDrivingEv");
    level_instance = (void *)so_symbol(mod, "_ZN9SingletonI9GameLevelE11GetInstanceEb");
    level_get_player = (void *)so_symbol(mod, "_ZN9GameLevel9GetPlayerEv");
    gp_inst = (void *)so_symbol(mod, "_ZN9SingletonI11GS_GamePlayE11GetInstanceEb");
    gp_cinematic = (void *)so_symbol(mod, "_ZN11GS_GamePlay18IsCinematicEnabledEv");
  }
  extern int game_menu_active(void);
  extern int input_route(void);
  if (game_menu_active() || input_route() == 0 /* ROUTE_UI */) return 0;
  {
    /* not in the shooting lottery: none of the on-foot controls apply there (pointer.c runs it) */
    static void *(*lm_inst)(int);
    static int (*lm_is_level)(void *);
    if (!lm_inst) {
      lm_inst = (void *)so_symbol(port_engine_module(), "_ZN9SingletonI10LotteryMgrE11GetInstanceEb");
      lm_is_level = (void *)so_symbol(port_engine_module(), "_ZNK10LotteryMgr14IsLotteryLevelEv");
    }
    void *lm = lm_inst ? lm_inst(0) : NULL;
    if (lm && lm_is_level && (lm_is_level(lm) & 1)) return 0;
  }
  if (gp_inst && gp_cinematic) {
    void *gp = gp_inst(0);
    if (gp && (gp_cinematic(gp) & 1)) return 0;
  }
  void *level = level_instance ? level_instance(0) : NULL;
  void *player = level && level_get_player ? level_get_player(level) : NULL;
  if (!player) return 0;
  return player_is_driving && (player_is_driving(player) & 1) ? 2 : 1;
}

/* Called after the engine has drawn the frame, before the buffer swap. */
void hints_draw(void) {
  if (state < 0) return;
  int set = current_set();
  static int last_set = -1, shown_frames;
  if (set != last_set) {
    extern int game_menu_active(void);
    extern int input_route(void);
    debugPrintf("[hints] set %d (menu %d, route %d)\n", set, game_menu_active(), input_route());
    shown_frames = 0;
    last_set = set;
  }
  extern int cutscene_active, cutscene_hold;
  extern int cutscene_hold_frames;
  extern int cutscene_skippable;
  int cut = cutscene_active && cutscene_skippable;
  {
    static int dbg = -1; /* marker debug_cutscene_ui: always draw the skip indicator (testing) */
    if (dbg < 0) { FILE *m = fopen(PORT_ROOT "/debug_cutscene_ui", "r"); dbg = m != NULL; if (m) fclose(m); }
    if (dbg) cut = 1;
  }
  if (!set && !cut) return;
  if (!setup()) return;
  shown_frames++;
  const int *list = set == 2 ? horse_rows : foot_rows;
  int n = set == 2 ? (int)(sizeof(horse_rows) / sizeof(*horse_rows)) : (int)(sizeof(foot_rows) / sizeof(*foot_rows));
  /* fade in over ~0.25 s when gameplay resumes */
  float fade = shown_frames >= 15 ? 1.0f : shown_frames / 15.0f;

  GLint prog0, abuf, fbo, vp[4], bsrc, bdst, basrc, badst, beq, beqa, unit0, tex0;
  GLboolean cmask[4];
  G.GetIntegerv(GL_CURRENT_PROGRAM, &prog0);
  G.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
  G.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  G.GetIntegerv(GL_VIEWPORT, vp);
  G.GetIntegerv(GL_BLEND_SRC_RGB, &bsrc); G.GetIntegerv(GL_BLEND_DST_RGB, &bdst);
  G.GetIntegerv(GL_BLEND_SRC_ALPHA, &basrc); G.GetIntegerv(GL_BLEND_DST_ALPHA, &badst);
  G.GetIntegerv(GL_BLEND_EQUATION_RGB, &beq); G.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &beqa);
  G.GetBooleanv(GL_COLOR_WRITEMASK, cmask);
  G.GetIntegerv(GL_ACTIVE_TEXTURE, &unit0);
  G.ActiveTexture(GL_TEXTURE0);
  G.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
  static const GLenum caps[] = { GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST };
  GLboolean on[5];
  for (int i = 0; i < 5; i++) on[i] = G.IsEnabled(caps[i]);
  enum { MAXA = 16 };
  GLint enabled[MAXA] = {0}, maxa = 0;
  G.GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxa);
  if (maxa > MAXA) maxa = MAXA;
  for (int i = 0; i < maxa; i++) G.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
  GLint a0_size, a0_type, a0_norm, a0_stride, a0_buf;
  void *a0_ptr = NULL;
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a0_size);
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a0_type);
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a0_norm);
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a0_stride);
  G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a0_buf);
  G.GetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a0_ptr);

  G.BindFramebuffer(GL_FRAMEBUFFER, 0);
  G.Viewport(0, 0, SCREEN_W, SCREEN_H);
  for (int i = 1; i < 5; i++) G.Disable(caps[i]);
  G.Enable(GL_BLEND);
  G.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  G.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  G.ColorMask(1, 1, 1, 1);
  for (int i = 1; i < maxa; i++) if (enabled[i]) G.DisableVertexAttribArray(i);
  G.EnableVertexAttribArray(0);
  G.BindBuffer(GL_ARRAY_BUFFER, 0);
  G.UseProgram(prog);
  G.Uniform2f(u_screen, SCREEN_W, SCREEN_H);
  G.Uniform1i(u_tex, 0);
  G.BindTexture(GL_TEXTURE_2D, tex);

  float th = (float)(N_ROWS * ROW_H);
  /* Cutscene: "(A) Hold to skip" with a bar that fills while A is held (5 s). */
  if (cut) {
    const row *r = &rows[ROW_SKIP];
    float w = (float)r->w, bx1 = SCREEN_W - 16, bx0 = bx1 - w - 16, by1 = SCREEN_H - 16, by0 = by1 - ROW_H - 18;
    float v0 = ROW_SKIP * ROW_H / th, v1 = (ROW_SKIP + 1) * ROW_H / th;
    tquad(bx0, by0, bx1, by1, -1, 0, -1, 0, 0.05f, 0.03f, 0.02f, 0.6f);
    tquad(bx0 + 9, by0 + 5, bx0 + 9 + w, by0 + 5 + ROW_H, 0, v0, w / ATLAS_W, v1, 0.98f, 0.94f, 0.84f, 1.0f);
    float p = cutscene_hold_frames > 0 ? (float)cutscene_hold / cutscene_hold_frames : 0;
    if (p > 1) p = 1;
    tquad(bx0 + 8, by1 - 9, bx1 - 8, by1 - 5, -1, 0, -1, 0, 0.3f, 0.3f, 0.3f, 0.8f);
    if (p > 0) tquad(bx0 + 8, by1 - 9, bx0 + 8 + (bx1 - bx0 - 16) * p, by1 - 5, -1, 0, -1, 0, 1.0f, 0.82f, 0.25f, 1.0f);
  }
  if (!set) goto restore;
  /* Right edge, below the minimap and compass, above the bottom-right corner. */
  int wmax = 0;
  for (int k = 0; k < n; k++) if (rows[list[k]].w > wmax) wmax = rows[list[k]].w;
  const float pad = 7, right = SCREEN_W - 8, bottom = SCREEN_H - 10;
  float x0 = right - wmax - pad * 2, y0 = bottom - n * ROW_H - pad * 2;
  tquad(x0, y0, right, bottom, -1, 0, -1, 0, 0.05f, 0.03f, 0.02f, 0.5f * fade);  /* backing panel */
  for (int k = 0; k < n; k++) {
    const row *r = &rows[list[k]];
    float rx = x0 + pad, ry = y0 + pad + k * ROW_H;
    float v0 = list[k] * ROW_H / th, v1 = (list[k] + 1) * ROW_H / th, u1 = (float)r->w / ATLAS_W;
    tquad(rx + 1.5f, ry + 1.5f, rx + 1.5f + r->w, ry + 1.5f + ROW_H, 0, v0, u1, v1, 0, 0, 0, 0.8f * fade); /* shadow */
    tquad(rx, ry, rx + r->w, ry + ROW_H, 0, v0, u1, v1, 0.98f, 0.94f, 0.84f, 0.95f * fade);
  }

  /* Elixir key while L is held: D-pad icon above each of the three elixirs on
   * the bottom bar (centres measured at 1280x720), and the cooldown left. */
  {
    extern int elixir_key_shown;
    extern int elixir_cooldown_s(int k);
    static int force = -1;
    if (force < 0) {
      FILE *m = fopen(PORT_ROOT "/debug_elixir_key", "r");
      force = m != NULL;
      if (m) fclose(m);
    }
    if (elixir_key_shown || force) {
      static const float cx[3] = { 572, 679, 786 };
      const float ky = 604, iy = 672; /* arrow row; elixir icon centre */
      for (int k = 0; k < 3; k++) {
        const row *g = &rows[ROW_KEY + k];
        float x = cx[k] - g->w / 2.0f;
        float v0 = (ROW_KEY + k) * ROW_H / th, v1 = (ROW_KEY + k + 1) * ROW_H / th;
        /* small box for the arrow only, just above the icon */
        tquad(cx[k] - 15, ky, cx[k] + 15, ky + ROW_H, -1, 0, -1, 0, 0.05f, 0.03f, 0.02f, 0.55f * fade);
        tquad(x + 1.5f, ky + 1.5f, x + 1.5f + g->w, ky + 1.5f + ROW_H, 0, v0, (float)g->w / ATLAS_W, v1, 0, 0, 0, 0.8f * fade);
        tquad(x, ky, x + g->w, ky + ROW_H, 0, v0, (float)g->w / ATLAS_W, v1, 0.98f, 0.94f, 0.84f, fade);
        int left = elixir_cooldown_s(k);
        char t[8];
        if (left) snprintf(t, sizeof(t), "%d:%02d", left / 60, left % 60);
        else snprintf(t, sizeof(t), "%s", "");
        float tw = 0;
        for (const char *c = t; *c; c++) tw += rows[ROW_DIGIT + (*c == ':' ? 10 : *c - '0')].w - 4;
        /* cooldown time on the elixir icon itself */
        float tx = cx[k] - tw / 2.0f, ty = iy - ROW_H / 2.0f;
        if (tw > 0) tquad(tx - 5, ty + 1, tx + tw + 7, ty + ROW_H - 1, -1, 0, -1, 0, 0.05f, 0.03f, 0.02f, 0.7f * fade);
        for (const char *c = t; *c; c++) {
          int r = ROW_DIGIT + (*c == ':' ? 10 : *c - '0');
          float w = (float)rows[r].w, a0 = r * ROW_H / th, a1 = (r + 1) * ROW_H / th;
          tquad(tx + 1, ty + 1, tx + 1 + w, ty + 1 + ROW_H, 0, a0, w / ATLAS_W, a1, 0, 0, 0, 0.8f * fade);
          tquad(tx, ty, tx + w, ty + ROW_H, 0, a0, w / ATLAS_W, a1, 1.0f, 0.55f, 0.35f, fade);
          tx += w - 4;
        }
      }
    }
  }

  /* HUD shortcuts (controller.c): a D-pad arrow just left of the "out of item" offer (Down) and of the quest
   * teleport button under the minimap (Right) while they are shown. */
  {
    extern int hud_oii_visible, hud_tp_visible;
    extern float hud_oii_rect[4], hud_tp_rect[4];
    for (int which = 0; which < 2; which++) {
      if (!(which ? hud_tp_visible : hud_oii_visible)) continue;
      const float *r = which ? hud_tp_rect : hud_oii_rect;
      int ri = which ? ROW_KEY + 2 : ROW_KEY_DOWN;
      const row *g = &rows[ri];
      float cy = (r[1] + r[3]) / 2, bx1 = r[0] - 4, bx0 = bx1 - 30, by0 = cy - ROW_H / 2.0f;
      if (!which || bx0 < 2) { bx0 = r[2] + 6; bx1 = bx0 + 30; } /* the offer has its item icon on the left */
      float gx = (bx0 + bx1) / 2 - g->w / 2.0f;
      float v0 = ri * ROW_H / th, v1 = (ri + 1) * ROW_H / th;
      tquad(bx0, by0, bx1, by0 + ROW_H, -1, 0, -1, 0, 0.05f, 0.03f, 0.02f, 0.6f);
      tquad(gx + 1.5f, by0 + 1.5f, gx + 1.5f + g->w, by0 + 1.5f + ROW_H, 0, v0, (float)g->w / ATLAS_W, v1, 0, 0, 0, 0.8f);
      tquad(gx, by0, gx + g->w, by0 + ROW_H, 0, v0, (float)g->w / ATLAS_W, v1, 0.98f, 0.94f, 0.84f, 1.0f);
    }
  }

restore:
  G.BindTexture(GL_TEXTURE_2D, tex0);
  G.ActiveTexture(unit0);
  G.BindBuffer(GL_ARRAY_BUFFER, a0_buf);
  G.VertexAttribPointer(0, a0_size, a0_type, a0_norm, a0_stride, a0_ptr);
  G.BindBuffer(GL_ARRAY_BUFFER, abuf);
  if (!enabled[0]) G.DisableVertexAttribArray(0);
  for (int i = 1; i < maxa; i++) if (enabled[i]) G.EnableVertexAttribArray(i);
  G.UseProgram(prog0);
  G.ColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
  G.BlendEquationSeparate(beq, beqa);
  G.BlendFuncSeparate(bsrc, bdst, basrc, badst);
  for (int i = 0; i < 5; i++) { if (on[i]) G.Enable(caps[i]); else G.Disable(caps[i]); }
  G.Viewport(vp[0], vp[1], vp[2], vp[3]);
  G.BindFramebuffer(GL_FRAMEBUFFER, fbo);
}

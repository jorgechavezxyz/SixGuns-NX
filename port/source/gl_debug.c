/* Thin wrappers around a few GL entry points the engine imports, to see what
 * it actually draws: draw counts per framebuffer, viewport, clears, and any
 * shader compile/link failures. Summaries are logged by gl_debug_frame(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "util.h"

#define REAL(name) static typeof(&name) real_##name
REAL(glDrawElements); REAL(glDrawArrays); REAL(glBindFramebuffer); REAL(glViewport);
REAL(glClear); REAL(glClearColor); REAL(glCompileShader); REAL(glLinkProgram);
REAL(glGetShaderiv); REAL(glGetShaderInfoLog); REAL(glGetProgramiv); REAL(glGetProgramInfoLog);
REAL(glGetError); REAL(glUseProgram); REAL(glCheckFramebufferStatus);

static struct {
  unsigned draws_default, draws_fbo, clears_default, clears_fbo, fbo_binds;
  int cur_fbo, vp[4];
  float clear[4];
  unsigned prog_zero_draws;
  int cur_prog;
} st;

static unsigned shader_fail, link_fail;

/* Which thread is allowed to touch GL (the one that owns the EGL context). */
#include <switch.h>
static Thread *gl_thread;
static unsigned offthread_calls;
static void check_thread(const char *fn) {
  if (gl_thread && threadGetSelf() != gl_thread && offthread_calls++ < 20)
    debugPrintf("[gl] WARNING: %s called from non-GL thread %p\n", fn, (void *)threadGetSelf());
}
#define CHECK() check_thread(__func__ + 2)

static void w_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) {
  CHECK();
  if (st.cur_fbo) st.draws_fbo++; else st.draws_default++;
  if (!st.cur_prog) st.prog_zero_draws++;
  real_glDrawElements(m, c, t, i);
}
static void w_glDrawArrays(GLenum m, GLint f, GLsizei c) {
  CHECK();
  if (st.cur_fbo) st.draws_fbo++; else st.draws_default++;
  if (!st.cur_prog) st.prog_zero_draws++;
  real_glDrawArrays(m, f, c);
}
static void w_glBindFramebuffer(GLenum t, GLuint fb) {
  CHECK();
  st.cur_fbo = fb;
  st.fbo_binds++;
  real_glBindFramebuffer(t, fb);
}
static void w_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  CHECK();
  if (!st.cur_fbo) { st.vp[0] = x; st.vp[1] = y; st.vp[2] = w; st.vp[3] = h; }
  real_glViewport(x, y, w, h);
}
static void w_glClear(GLbitfield b) {
  CHECK();
  if (st.cur_fbo) st.clears_fbo++; else st.clears_default++;
  real_glClear(b);
}
static void w_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  CHECK();
  st.clear[0] = r; st.clear[1] = g; st.clear[2] = b; st.clear[3] = a;
  real_glClearColor(r, g, b, a);
}
static void w_glUseProgram(GLuint p) { st.cur_prog = p; real_glUseProgram(p); }

static void w_glCompileShader(GLuint s) {
  CHECK();
  real_glCompileShader(s);
  GLint ok = 0;
  real_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok && shader_fail++ < 8) {
    char log[1024] = "";
    real_glGetShaderInfoLog(s, sizeof(log), NULL, log);
    debugPrintf("[gl] shader %u compile FAILED: %s\n", s, log);
  }
}
/* Shader pre-compilation. Mesa's nouveau driver compiles a program's GPU code
 * on its first draw, not at glLinkProgram, so every new effect or menu hitched
 * the frame it first appeared (hardware only; the emulator compiles on the
 * host). Right after a successful link we draw once with the program into a
 * hidden 1x1 renderbuffer, which makes the driver compile it now (the engine
 * links during loading). All GL state touched is restored, since the engine
 * caches bindings. Mesa's on-disk shader cache is compiled out of
 * devkitPro's switch-mesa, so this is the way to move the cost. */
static struct {
  typeof(&glGetIntegerv) GetIntegerv;
  typeof(&glGenRenderbuffers) GenRenderbuffers;
  typeof(&glBindRenderbuffer) BindRenderbuffer;
  typeof(&glRenderbufferStorage) RenderbufferStorage;
  typeof(&glGenFramebuffers) GenFramebuffers;
  typeof(&glBindFramebuffer) BindFramebuffer;
  typeof(&glFramebufferRenderbuffer) FramebufferRenderbuffer;
  typeof(&glViewport) Viewport;
  typeof(&glGetVertexAttribiv) GetVertexAttribiv;
  typeof(&glDisableVertexAttribArray) DisableVertexAttribArray;
  typeof(&glEnableVertexAttribArray) EnableVertexAttribArray;
  typeof(&glUseProgram) UseProgram;
  typeof(&glDrawArrays) DrawArrays;
} G;
static int warm_load(void) {
  static int state; /* 0 untried, 1 ok, -1 missing */
  if (state) return state > 0;
  G.GetIntegerv = (void *)eglGetProcAddress("glGetIntegerv");
  G.GenRenderbuffers = (void *)eglGetProcAddress("glGenRenderbuffers");
  G.BindRenderbuffer = (void *)eglGetProcAddress("glBindRenderbuffer");
  G.RenderbufferStorage = (void *)eglGetProcAddress("glRenderbufferStorage");
  G.GenFramebuffers = (void *)eglGetProcAddress("glGenFramebuffers");
  G.BindFramebuffer = (void *)eglGetProcAddress("glBindFramebuffer");
  G.FramebufferRenderbuffer = (void *)eglGetProcAddress("glFramebufferRenderbuffer");
  G.Viewport = (void *)eglGetProcAddress("glViewport");
  G.GetVertexAttribiv = (void *)eglGetProcAddress("glGetVertexAttribiv");
  G.DisableVertexAttribArray = (void *)eglGetProcAddress("glDisableVertexAttribArray");
  G.EnableVertexAttribArray = (void *)eglGetProcAddress("glEnableVertexAttribArray");
  G.UseProgram = (void *)eglGetProcAddress("glUseProgram");
  G.DrawArrays = (void *)eglGetProcAddress("glDrawArrays");
  state = 1;
  if (!G.GetIntegerv) state = -1;
  if (!G.GenRenderbuffers) state = -1;
  if (!G.BindRenderbuffer) state = -1;
  if (!G.RenderbufferStorage) state = -1;
  if (!G.GenFramebuffers) state = -1;
  if (!G.BindFramebuffer) state = -1;
  if (!G.FramebufferRenderbuffer) state = -1;
  if (!G.Viewport) state = -1;
  if (!G.GetVertexAttribiv) state = -1;
  if (!G.DisableVertexAttribArray) state = -1;
  if (!G.EnableVertexAttribArray) state = -1;
  if (!G.UseProgram) state = -1;
  if (!G.DrawArrays) state = -1;
  if (state < 0) debugPrintf("[gl] shader pre-compile disabled (GL entry points missing)\n");
  return state > 0;
}

static GLuint warm_fbo, warm_rb;
static unsigned warmed;
static u64 warm_ns;

static void prewarm_program(GLuint p) {
  if (!warm_load()) return;
  u64 t0 = armGetSystemTick();
  GLint prev_fbo = 0, prev_rb = 0, vp[4];
  G.GetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
  G.GetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
  G.GetIntegerv(GL_VIEWPORT, vp);
  if (!warm_fbo) {
    G.GenRenderbuffers(1, &warm_rb);
    G.BindRenderbuffer(GL_RENDERBUFFER, warm_rb);
    G.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, 1, 1);
    G.GenFramebuffers(1, &warm_fbo);
    G.BindFramebuffer(GL_FRAMEBUFFER, warm_fbo);
    G.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, warm_rb);
    G.BindRenderbuffer(GL_RENDERBUFFER, prev_rb);
  }
  G.BindFramebuffer(GL_FRAMEBUFFER, warm_fbo);
  G.Viewport(0, 0, 1, 1);
  /* No vertex arrays: the draw must not read the engine's buffers. */
  enum { MAXA = 16 };
  GLint enabled[MAXA] = {0}, maxa = 0;
  G.GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxa);
  if (maxa > MAXA) maxa = MAXA;
  for (int i = 0; i < maxa; i++) {
    G.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
    if (enabled[i]) G.DisableVertexAttribArray(i);
  }
  G.UseProgram(p);
  G.DrawArrays(GL_TRIANGLES, 0, 3);
  G.UseProgram(st.cur_prog);
  for (int i = 0; i < maxa; i++) if (enabled[i]) G.EnableVertexAttribArray(i);
  G.Viewport(vp[0], vp[1], vp[2], vp[3]);
  G.BindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
  warmed++;
  warm_ns += armTicksToNs(armGetSystemTick() - t0);
}

extern unsigned long gl_link_n, gl_link_ns;
static void w_glLinkProgram(GLuint p) {
  CHECK();
  u64 lt0 = armGetSystemTick();
  real_glLinkProgram(p);
  GLint ok = 0;
  real_glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok && link_fail++ < 8) {
    char log[1024] = "";
    real_glGetProgramInfoLog(p, sizeof(log), NULL, log);
    debugPrintf("[gl] program %u link FAILED: %s\n", p, log);
  }
  if (ok) {
    prewarm_program(p);
    gl_link_n++; gl_link_ns += armTicksToNs(armGetSystemTick() - lt0);
    if (warmed % 25 == 1)
      debugPrintf("[gl] pre-compiled %u programs so far (%.1f ms total)\n", warmed, warm_ns / 1e6);
  }
}
static GLenum w_glCheckFramebufferStatus(GLenum t) {
  CHECK();
  GLenum r = real_glCheckFramebufferStatus(t);
  if (r != GL_FRAMEBUFFER_COMPLETE) debugPrintf("[gl] framebuffer %d incomplete: 0x%x\n", st.cur_fbo, r);
  return r;
}

REAL(glTexImage2D); REAL(glCompressedTexImage2D); REAL(glTexSubImage2D); REAL(glGenTextures);
REAL(glBindTexture); REAL(glBufferData); REAL(glGenBuffers); REAL(glShaderSource); REAL(glDeleteTextures);
REAL(glActiveTexture); REAL(glTexParameteri); REAL(glTexParameterf);
/* Texture upload timing (per frame, reported with [hitch] lines) and the
 * compressed formats seen: some phone formats are decompressed on the CPU by
 * the Switch's Mesa driver. */
unsigned long gl_tex_n, gl_tex_kb, gl_tex_ns, gl_link_n, gl_link_ns;
static struct { GLenum fmt; unsigned long n, kb, ns; } fmts[16];
static void count_fmt(GLenum f, unsigned long kb, unsigned long ns) {
  for (int i = 0; i < 16; i++) {
    if (fmts[i].fmt == f || !fmts[i].fmt) {
      if (!fmts[i].fmt) { fmts[i].fmt = f; debugPrintf("[gl] first texture upload in format 0x%x\n", f); }
      fmts[i].n++; fmts[i].kb += kb; fmts[i].ns += ns;
      return;
    }
  }
}
void gl_tex_stats(void) {
  extern void graphics_quality_stats(void); graphics_quality_stats();
  extern void sui_stats(void); sui_stats();
  extern void gpu_profile_stats(void); gpu_profile_stats();
  extern void memfile_stats(void); memfile_stats();
  for (int i = 0; i < 16 && fmts[i].fmt; i++)
    debugPrintf("[perf] textures 0x%x: %lu uploads, %lu KB, %.1f ms\n", fmts[i].fmt, fmts[i].n, fmts[i].kb,
                fmts[i].ns / 1e6);
  extern unsigned long etc_decode_ns;
  debugPrintf("[perf] ETC1 decode part of that: %.1f ms\n", etc_decode_ns / 1e6);
}
static void w_glTexImage2D(GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const void *i) {
  CHECK();
  u64 t0 = armGetSystemTick();
  real_glTexImage2D(a, b, c, d, e, f, g, h, i);
  unsigned long ns = armTicksToNs(armGetSystemTick() - t0), kb = (unsigned long)d * e * 4 / 1024;
  gl_tex_n++; gl_tex_kb += kb; gl_tex_ns += ns;
  count_fmt(g == GL_RGBA ? (h == GL_UNSIGNED_BYTE ? 0x1908 : 0x10000 | h) : g, kb, ns);
}
/* debug_tex_callers marker: for big compressed uploads, log who asked for them
 * (engine frames from the frame-pointer chain), to find textures that are
 * reloaded over and over (e.g. every time a menu opens). */
#include "so_util.h"
static void log_tex_callers(int w, int h, int kb) {
  static int enabled = -1;
  if (enabled < 0) {
    FILE *f = fopen("sdmc:/switch/sixguns/debug_tex_callers", "r");
    enabled = f != NULL;
    if (f) fclose(f);
  }
  if (!enabled) return;
  extern so_module *port_engine_module(void);
  so_module *m = port_engine_module();
  char line[600];
  int n = snprintf(line, sizeof(line), "[gl] upload %dx%d %d KB from", w, h, kb);
  uintptr_t *fp = __builtin_frame_address(0);
  for (int i = 0; i < 9 && fp && n < (int)sizeof(line) - 90; i++) {
    uintptr_t lr = fp[1];
    uintptr_t off = 0;
    const char *s = (lr >= m->base && lr < m->base + m->image_size) ? so_symbol_name(m, lr, &off) : NULL;
    if (s) n += snprintf(line + n, sizeof(line) - n, " | %.60s+0x%lx", s, (unsigned long)off);
    uintptr_t *next = (uintptr_t *)fp[0];
    if (next <= fp) break;
    fp = next;
  }
  debugPrintf("%s\n", line);
}
/* ETC1 -> RGB565 at upload (default; marker `etc1_mesa` turns it off; 2026-11-11).
 * The profiler showed Mesa's software etc1_fetch_texel on the main thread: this
 * Mesa (switch-mesa 20.1, nouveau) decodes ETC1 *and* ETC2 on the CPU, texel by
 * texel, into RGBA8 (tried uploading ETC1 as ETC2: same, etc2_rgb8_fetch_texel,
 * no gain). A block decoder writing RGB565 is much cheaper and halves the GPU
 * memory of the RGBA8 Mesa makes (ETC1 colours are 4-5 bits per channel). */
static int etc1_565 = -1;
static uint16_t *etc_buf;
static size_t etc_buf_px;

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Decodes a w x h ETC1 image (blocks of 8 bytes, 4x4 texels, big-endian) to RGB565. */
/* ETC1 -> RGB565. Per block the 8 possible colours (2 sub-blocks x 4
 * modifiers) are computed once, as 565 words; each texel is then a table
 * pick. Full blocks skip the edge checks. Rows of blocks [by0, by1). */
static void etc1_rows(const uint8_t *src, int w, int h, uint16_t *dst, int by0, int by1) {
  static const int mod[8][4] = { { 2, 8, -2, -8 }, { 5, 17, -5, -17 }, { 9, 29, -9, -29 }, { 13, 42, -13, -42 },
                                 { 18, 60, -18, -60 }, { 24, 80, -24, -80 }, { 33, 106, -33, -106 },
                                 { 47, 183, -47, -183 } };
  int bw = (w + 3) / 4;
  src += (size_t)by0 * bw * 8;
  for (int by = by0; by < by1; by++)
    for (int bx = 0; bx < bw; bx++, src += 8) {
      uint32_t hi = (uint32_t)src[0] << 24 | src[1] << 16 | src[2] << 8 | src[3];
      uint32_t lo = (uint32_t)src[4] << 24 | src[5] << 16 | src[6] << 8 | src[7];
      int flip = hi & 1;
      int r[2], g[2], b[2];
      if (hi & 2) {
        int r5 = hi >> 27, g5 = (hi >> 19) & 31, b5 = (hi >> 11) & 31;
        int dr = (int)((hi >> 24) & 7), dg = (int)((hi >> 16) & 7), db = (int)((hi >> 8) & 7);
        dr = dr >= 4 ? dr - 8 : dr; dg = dg >= 4 ? dg - 8 : dg; db = db >= 4 ? db - 8 : db;
        int r52 = (r5 + dr) & 31, g52 = (g5 + dg) & 31, b52 = (b5 + db) & 31;
        r[0] = r5 << 3 | r5 >> 2; g[0] = g5 << 3 | g5 >> 2; b[0] = b5 << 3 | b5 >> 2;
        r[1] = r52 << 3 | r52 >> 2; g[1] = g52 << 3 | g52 >> 2; b[1] = b52 << 3 | b52 >> 2;
      } else {
        r[0] = (hi >> 28) * 17; r[1] = ((hi >> 24) & 15) * 17;
        g[0] = ((hi >> 20) & 15) * 17; g[1] = ((hi >> 16) & 15) * 17;
        b[0] = ((hi >> 12) & 15) * 17; b[1] = ((hi >> 8) & 15) * 17;
      }
      uint16_t pal[2][4];
      for (int sb = 0; sb < 2; sb++) {
        const int *mt = mod[sb ? (hi >> 2) & 7 : (hi >> 5) & 7];
        for (int k = 0; k < 4; k++) {
          int R = clamp255(r[sb] + mt[k]), G = clamp255(g[sb] + mt[k]), B = clamp255(b[sb] + mt[k]);
          pal[sb][k] = (uint16_t)((R >> 3) << 11 | (G >> 2) << 5 | (B >> 3));
        }
      }
      int px0 = bx * 4, py0 = by * 4;
      uint16_t *o = dst + (size_t)py0 * w + px0;
      if (px0 + 4 <= w && py0 + 4 <= h) {
        for (int x = 0; x < 4; x++)
          for (int y = 0; y < 4; y++) {
            int i = x * 4 + y; /* texel index runs down columns */
            int sub = flip ? (y >> 1) : (x >> 1);
            o[y * w + x] = pal[sub][((lo >> (i + 15)) & 2) | ((lo >> i) & 1)];
          }
      } else {
        for (int x = 0; x < 4; x++)
          for (int y = 0; y < 4; y++) {
            if (px0 + x >= w || py0 + y >= h) continue;
            int i = x * 4 + y;
            int sub = flip ? (y >> 1) : (x >> 1);
            o[y * w + x] = pal[sub][((lo >> (i + 15)) & 2) | ((lo >> i) & 1)];
          }
      }
    }
}

/* ETC1 -> BC1 (DXT1), block for block (marker `etc1_bc1`, 2026-11-15). Both formats are 8 bytes per
 * 4x4 block, so the upload keeps the game's 4 bits per texel instead of RGB565's 16: a quarter of the
 * GPU memory traffic for every textured draw, and a quarter of the upload. The Switch GPU samples BC1
 * natively (GL_COMPRESSED_RGB_S3TC_DXT1_EXT is in the driver's list on hardware). Encoding: the block's
 * darkest and brightest texels (luma) are the endpoints, each texel takes the nearest of BC1's four
 * colours. ETC1's own colours are 4-5 bits per channel, BC1's endpoints 5:6:5. */
static void etc1_bc1_rows(const uint8_t *src, int bw, uint8_t *dst, int by0, int by1) {
  static const int mod[8][4] = { { 2, 8, -2, -8 }, { 5, 17, -5, -17 }, { 9, 29, -9, -29 }, { 13, 42, -13, -42 },
                                 { 18, 60, -18, -60 }, { 24, 80, -24, -80 }, { 33, 106, -33, -106 },
                                 { 47, 183, -47, -183 } };
  size_t first = (size_t)by0 * bw, last = (size_t)by1 * bw;
  for (size_t blk = first; blk < last; blk++) {
    const uint8_t *b8 = src + blk * 8;
    uint32_t hi = (uint32_t)b8[0] << 24 | b8[1] << 16 | b8[2] << 8 | b8[3];
    uint32_t lo = (uint32_t)b8[4] << 24 | b8[5] << 16 | b8[6] << 8 | b8[7];
    int flip = hi & 1, r[2], g[2], b[2];
    if (hi & 2) {
      int r5 = hi >> 27, g5 = (hi >> 19) & 31, b5 = (hi >> 11) & 31;
      int dr = (int)((hi >> 24) & 7), dg = (int)((hi >> 16) & 7), db = (int)((hi >> 8) & 7);
      dr = dr >= 4 ? dr - 8 : dr; dg = dg >= 4 ? dg - 8 : dg; db = db >= 4 ? db - 8 : db;
      int r52 = (r5 + dr) & 31, g52 = (g5 + dg) & 31, b52 = (b5 + db) & 31;
      r[0] = r5 << 3 | r5 >> 2; g[0] = g5 << 3 | g5 >> 2; b[0] = b5 << 3 | b5 >> 2;
      r[1] = r52 << 3 | r52 >> 2; g[1] = g52 << 3 | g52 >> 2; b[1] = b52 << 3 | b52 >> 2;
    } else {
      r[0] = (hi >> 28) * 17; r[1] = ((hi >> 24) & 15) * 17;
      g[0] = ((hi >> 20) & 15) * 17; g[1] = ((hi >> 16) & 15) * 17;
      b[0] = ((hi >> 12) & 15) * 17; b[1] = ((hi >> 8) & 15) * 17;
    }
    int pr[2][4], pg[2][4], pb[2][4];
    for (int sb = 0; sb < 2; sb++) {
      const int *mt = mod[sb ? (hi >> 2) & 7 : (hi >> 5) & 7];
      for (int k = 0; k < 4; k++) { pr[sb][k] = clamp255(r[sb] + mt[k]); pg[sb][k] = clamp255(g[sb] + mt[k]); pb[sb][k] = clamp255(b[sb] + mt[k]); }
    }
    /* texels in BC1 order (row-major); ETC1 indexes down columns */
    int tr[16], tg[16], tb[16], lmin = 1 << 30, lmax = -1, imin = 0, imax = 0;
    for (int x = 0; x < 4; x++)
      for (int y = 0; y < 4; y++) {
        int i = x * 4 + y, sub = flip ? (y >> 1) : (x >> 1), k = ((lo >> (i + 15)) & 2) | ((lo >> i) & 1), t = y * 4 + x;
        tr[t] = pr[sub][k]; tg[t] = pg[sub][k]; tb[t] = pb[sub][k];
        int l = 2 * tr[t] + 5 * tg[t] + tb[t];
        if (l < lmin) { lmin = l; imin = t; }
        if (l > lmax) { lmax = l; imax = t; }
      }
    uint16_t c0 = (uint16_t)((tr[imax] >> 3) << 11 | (tg[imax] >> 2) << 5 | (tb[imax] >> 3));
    uint16_t c1 = (uint16_t)((tr[imin] >> 3) << 11 | (tg[imin] >> 2) << 5 | (tb[imin] >> 3));
    uint32_t idx = 0;
    for (int pass = 0; pass < 2; pass++) {
    idx = 0;
    if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    if (c0 != c1) {
      /* the four colours as the GPU expands them */
      int er[4], eg[4], eb[4];
      er[0] = (c0 >> 11) * 255 / 31; eg[0] = ((c0 >> 5) & 63) * 255 / 63; eb[0] = (c0 & 31) * 255 / 31;
      er[1] = (c1 >> 11) * 255 / 31; eg[1] = ((c1 >> 5) & 63) * 255 / 63; eb[1] = (c1 & 31) * 255 / 31;
      er[2] = (2 * er[0] + er[1]) / 3; eg[2] = (2 * eg[0] + eg[1]) / 3; eb[2] = (2 * eb[0] + eb[1]) / 3;
      er[3] = (er[0] + 2 * er[1]) / 3; eg[3] = (eg[0] + 2 * eg[1]) / 3; eb[3] = (eb[0] + 2 * eb[1]) / 3;
      for (int t = 0; t < 16; t++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 4; k++) {
          int d0 = tr[t] - er[k], d1 = tg[t] - eg[k], d2 = tb[t] - eb[k], d = 2 * d0 * d0 + 4 * d1 * d1 + d2 * d2;
          if (d < bd) { bd = d; best = k; }
        }
        idx |= (uint32_t)best << (2 * t);
      }
    }
    if (pass || c0 == c1) break;
    /* Least-squares endpoints for these indices (weights 1, 0, 2/3, 1/3 of c0), then pick indices again:
     * the luma extremes alone left visible blocks on gradients. */
    static const int w3[4] = { 3, 0, 2, 1 }; /* weight of c0, in thirds */
    long aa = 0, ab = 0, bb = 0, at[3] = { 0, 0, 0 }, bt[3] = { 0, 0, 0 };
    for (int t = 0; t < 16; t++) {
      int a = w3[(idx >> (2 * t)) & 3], bw3 = 3 - a;
      aa += a * a; ab += a * bw3; bb += bw3 * bw3;
      at[0] += a * tr[t]; at[1] += a * tg[t]; at[2] += a * tb[t];
      bt[0] += bw3 * tr[t]; bt[1] += bw3 * tg[t]; bt[2] += bw3 * tb[t];
    }
    long det = aa * bb - ab * ab;
    if (!det) break;
    int e0[3], e1[3];
    for (int ch = 0; ch < 3; ch++) {
      /* solve in thirds: [aa ab; ab bb] [x0 x1] = 3 * [at bt] */
      long x0 = (3 * (at[ch] * bb - bt[ch] * ab) + det / 2) / det, x1 = (3 * (bt[ch] * aa - at[ch] * ab) + det / 2) / det;
      e0[ch] = clamp255((int)x0); e1[ch] = clamp255((int)x1);
    }
    c0 = (uint16_t)((e0[0] * 31 + 127) / 255 << 11 | (e0[1] * 63 + 127) / 255 << 5 | (e0[2] * 31 + 127) / 255);
    c1 = (uint16_t)((e1[0] * 31 + 127) / 255 << 11 | (e1[1] * 63 + 127) / 255 << 5 | (e1[2] * 31 + 127) / 255);
    }
    uint8_t *o = dst + blk * 8;
    o[0] = c0 & 0xff; o[1] = c0 >> 8; o[2] = c1 & 0xff; o[3] = c1 >> 8;
    o[4] = idx & 0xff; o[5] = (idx >> 8) & 0xff; o[6] = (idx >> 16) & 0xff; o[7] = idx >> 24;
  }
}

/* Big textures are decoded by the caller (main thread) and two helper threads
 * (cores 1 and 2) together, in chunks of ETC_CHUNK block rows taken from a shared
 * counter. The caller keeps taking chunks until none are left, so it only waits
 * for chunks a helper is decoding right now -- never for a helper that the
 * scheduler has not run yet (the old fixed thirds made the main thread idle in
 * semaphoreWait while a busy core delayed a helper: seen in the profiler,
 * etc1_to_565 <- GLES20Texture::Load <- GameEntity::PreLoadTextures, 2026-11-12).
 *
 * Protocol: a helper first increments `active`, then reads the job pointer; the
 * caller unpublishes the job and waits for active == 0 before returning, so a job
 * (on the caller's stack) is never touched after it ends. Same output as before:
 * every chunk goes through etc1_rows. */
#include <stdatomic.h>
enum { ETC_CHUNK = 8 };
typedef struct { const uint8_t *src; int w, h, bh; uint16_t *dst; atomic_int next; uint8_t *bc1; } etc_job_t;
static _Atomic(etc_job_t *) etc_job;
static atomic_int etc_active;
static Semaphore etc_go[2];
static Thread etc_thr[2];
static int etc_workers = -1;

static void etc_run(etc_job_t *j) {
  for (;;) {
    int by0 = atomic_fetch_add(&j->next, ETC_CHUNK);
    if (by0 >= j->bh) break;
    int by1 = by0 + ETC_CHUNK < j->bh ? by0 + ETC_CHUNK : j->bh;
    if (j->bc1) etc1_bc1_rows(j->src, (j->w + 3) / 4, j->bc1, by0, by1);
    else etc1_rows(j->src, j->w, j->h, j->dst, by0, by1);
  }
}

/* Helpers poll for work for ETC_LINGER_NS after their last job (texture loads
 * come in bursts on loading screens), then sleep on their semaphore. The caller
 * signals a helper only when it is asleep (etc_sleeping), so during a burst the
 * main thread takes no lock at all: signalling a semaphore locks its mutex, and a
 * helper that was still waking up held it, which showed up as the main thread
 * blocked in semaphoreSignal <- etc1_to_565 (profiler, 2026-11-12). */
enum { ETC_LINGER_NS = 30000000, ETC_POLL_NS = 50000 };
static atomic_int etc_sleeping[2];
static void etc_worker(void *arg) {
  int k = (int)(intptr_t)arg;
  for (;;) {
    u64 last = armGetSystemTick();
    while (armTicksToNs(armGetSystemTick() - last) < ETC_LINGER_NS) {
      atomic_fetch_add(&etc_active, 1);
      etc_job_t *j = atomic_load(&etc_job);
      if (j) { etc_run(j); last = armGetSystemTick(); }
      atomic_fetch_sub(&etc_active, 1);
      if (!j) svcSleepThread(ETC_POLL_NS);
    }
    atomic_store(&etc_sleeping[k], 1);
    if (!atomic_load(&etc_job)) semaphoreWait(&etc_go[k]); /* a job published before this check is picked up without waiting */
    atomic_store(&etc_sleeping[k], 0);
  }
}
unsigned long etc_decode_ns;
static void etc1_convert(const uint8_t *src, int w, int h, uint16_t *dst, uint8_t *bc1) {
  u64 t0 = armGetSystemTick();
  int bh = (h + 3) / 4;
  if (etc_workers < 0) {
    etc_workers = 0;
    for (int k = 0; k < 2; k++) {
      semaphoreInit(&etc_go[k], 0);
      Result rc=threadCreate(&etc_thr[k],etc_worker,(void *)(intptr_t)k,NULL,0x4000,0x2b,k+1);
      if(R_SUCCEEDED(rc)) {rc=threadStart(&etc_thr[k]);if(R_FAILED(rc)) threadClose(&etc_thr[k]);}
      if (R_SUCCEEDED(rc))
        etc_workers++;
      else
        break;
    }
    debugPrintf("[gl] ETC1 decode: %d helper thread(s), shared chunks of %d block rows\n", etc_workers, ETC_CHUNK);
  }
  if (etc_workers > 0 && bh >= 4 * ETC_CHUNK) {
    etc_job_t j = { src, w, h, bh, dst, 0, bc1 };
    atomic_store(&etc_job, &j);
    for (int k = 0; k < etc_workers; k++)
      if (atomic_exchange(&etc_sleeping[k], 0)) semaphoreSignal(&etc_go[k]); /* only wake a sleeping helper */
    etc_run(&j);
    atomic_store(&etc_job, NULL);
    while (atomic_load(&etc_active)) svcSleepThread(0); /* only chunks in progress remain */
  } else if (bc1) {
    etc1_bc1_rows(src, (w + 3) / 4, bc1, 0, bh);
  } else {
    etc1_rows(src, w, h, dst, 0, bh);
  }
  etc_decode_ns += armTicksToNs(armGetSystemTick() - t0);
}

static void w_glCompressedTexImage2D(GLenum a, GLint b, GLenum c, GLsizei d, GLsizei e, GLint f, GLsizei g, const void *h) {
  CHECK();
  if (etc1_565 < 0) {
    /* default on since 2026-11-11 (checked in Ryubing); marker etc1_mesa = Mesa's own decode */
    FILE *fm = fopen("sdmc:/switch/sixguns/etc1_mesa", "r");
    etc1_565 = fm == NULL;
    if (fm) fclose(fm);
    FILE *fb = etc1_565 ? fopen("sdmc:/switch/sixguns/etc1_bc1", "r") : NULL;
    if (fb) { fclose(fb); etc1_565 = 2; }
    extern int gfx_textures_compact(void); /* graphics menu: Texture quality Compact */
    if (etc1_565 == 1 && gfx_textures_compact()) etc1_565 = 2;
    debugPrintf("[gl] ETC1 textures: %s\n", etc1_565 == 2 ? "transcoded by the port to BC1/DXT1 (Compact textures or etc1_bc1 marker)"
                : etc1_565 ? "decoded by the port to RGB565" : "Mesa's decode (etc1_mesa marker)");
  }
  if (etc1_565 == 2 && c == 0x8D64 /* GL_ETC1_RGB8_OES */ && h && d > 0 && e > 0 && d <= 0x7ffffffc && e <= 0x7ffffffc &&
      (size_t)g == (size_t)((d + 3) / 4) * ((e + 3) / 4) * 8) {
    static uint8_t *bc_buf;
    static size_t bc_cap;
    if ((size_t)g > bc_cap) { void *next = realloc(bc_buf, g); if (next) { bc_buf = next; bc_cap = g; } }
    if (bc_buf && (size_t)g <= bc_cap) {
      u64 t0 = armGetSystemTick();
      etc1_convert(h, d, e, NULL, bc_buf);
      real_glCompressedTexImage2D(a, b, 0x83F0 /* GL_COMPRESSED_RGB_S3TC_DXT1_EXT */, d, e, f, g, bc_buf);
      unsigned long ns = armTicksToNs(armGetSystemTick() - t0);
      gl_tex_n++; gl_tex_kb += g / 1024; gl_tex_ns += ns;
      count_fmt(0x83F0, g / 1024, ns);
      return;
    }
  }
  if (etc1_565 && c == 0x8D64 /* GL_ETC1_RGB8_OES */ && h && d > 0 && e > 0 &&
      d<=0x7ffffffc && e<=0x7ffffffc && g>=0 &&
      (size_t)g >= (size_t)((d + 3) / 4) * ((e + 3) / 4) * 8) {
    u64 t0 = armGetSystemTick();
    size_t px = (size_t)d * e;
    if (px > etc_buf_px) { void *next=realloc(etc_buf,px*2);if(next) {etc_buf=next;etc_buf_px=px;} }
    if (etc_buf && px<=etc_buf_px) {
      etc1_convert(h, d, e, etc_buf, NULL);
      static void (*get_iv)(GLenum, GLint *);
      static void (*pixel_store)(GLenum, GLint);
      if (!get_iv) get_iv = (void *)eglGetProcAddress("glGetIntegerv");
      if (!pixel_store) pixel_store = (void *)eglGetProcAddress("glPixelStorei");
      GLint align = 4;
      if (get_iv) get_iv(GL_UNPACK_ALIGNMENT, &align);
      int fix = pixel_store && align != 2 && align != 1;
      if (fix) pixel_store(GL_UNPACK_ALIGNMENT, 2);
      real_glTexImage2D(a, b, GL_RGB, d, e, f, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, etc_buf);
      if (fix) pixel_store(GL_UNPACK_ALIGNMENT, align);
      unsigned long ns = armTicksToNs(armGetSystemTick() - t0);
      gl_tex_n++; gl_tex_kb += g / 1024; gl_tex_ns += ns;
      count_fmt(0x8363 /* reported as GL_UNSIGNED_SHORT_5_6_5 */, g / 1024, ns);
      return;
    }
  }
  u64 t0 = armGetSystemTick();
  real_glCompressedTexImage2D(a, b, c, d, e, f, g, h);
  unsigned long ns = armTicksToNs(armGetSystemTick() - t0);
  gl_tex_n++; gl_tex_kb += g / 1024; gl_tex_ns += ns;
  count_fmt(c, g / 1024, ns);
  if (g >= 256 * 1024 && b == 0) log_tex_callers(d, e, g / 1024);
}
static void w_glTexSubImage2D(GLenum a, GLint b, GLint c, GLint d, GLsizei e, GLsizei f, GLenum g, GLenum h, const void *i) {
  CHECK();
  u64 t0 = armGetSystemTick();
  real_glTexSubImage2D(a, b, c, d, e, f, g, h, i);
  unsigned long ns = armTicksToNs(armGetSystemTick() - t0);
  gl_tex_n++; gl_tex_kb += (unsigned long)e * f * 4 / 1024; gl_tex_ns += ns;
}
static void w_glGenTextures(GLsizei n, GLuint *t) { CHECK(); real_glGenTextures(n, t); }
static void w_glBindTexture(GLenum a, GLuint t) { CHECK(); real_glBindTexture(a, t);
  extern void graphics_quality_bind(GLenum,GLuint);graphics_quality_bind(a,t); }
static void w_glActiveTexture(GLenum t) { CHECK(); real_glActiveTexture(t);
  extern void graphics_quality_active(GLenum);graphics_quality_active(t); }
static void w_glTexParameteri(GLenum a,GLenum p,GLint v) { CHECK();real_glTexParameteri(a,p,v);
  extern void graphics_quality_filter(GLenum,GLenum,GLfloat);graphics_quality_filter(a,p,(GLfloat)v); }
static void w_glTexParameterf(GLenum a,GLenum p,GLfloat v) { CHECK();real_glTexParameterf(a,p,v);
  extern void graphics_quality_filter(GLenum,GLenum,GLfloat);graphics_quality_filter(a,p,v); }
static void w_glBufferData(GLenum a, GLsizeiptr b, const void *c, GLenum d) { CHECK(); real_glBufferData(a, b, c, d); }
static void w_glGenBuffers(GLsizei n, GLuint *b) { CHECK(); real_glGenBuffers(n, b); }
static void w_glShaderSource(GLuint a, GLsizei b, const GLchar *const *c, const GLint *d) { CHECK(); real_glShaderSource(a, b, c, d); }
static void w_glDeleteTextures(GLsizei n, const GLuint *t) { CHECK();
  extern void graphics_quality_delete(GLsizei,const GLuint *);graphics_quality_delete(n,t); real_glDeleteTextures(n, t); }

#define WRAP(name) { #name, (void **)&real_##name, (void *)w_##name }
static const struct { const char *name; void **real; void *wrapper; } wraps[] = {
  WRAP(glDrawElements), WRAP(glDrawArrays), WRAP(glBindFramebuffer), WRAP(glViewport),
  WRAP(glClear), WRAP(glClearColor), WRAP(glCompileShader), WRAP(glLinkProgram),
  WRAP(glUseProgram), WRAP(glCheckFramebufferStatus), WRAP(glTexImage2D), WRAP(glCompressedTexImage2D),
  WRAP(glTexSubImage2D), WRAP(glGenTextures), WRAP(glBindTexture), WRAP(glBufferData), WRAP(glGenBuffers),
  WRAP(glShaderSource), WRAP(glDeleteTextures),
  WRAP(glActiveTexture), WRAP(glTexParameteri), WRAP(glTexParameterf),
};

void *gl_debug_wrap(const char *name, void *real) {
  for (size_t i = 0; i < sizeof(wraps) / sizeof(*wraps); i++) {
    if (strcmp(wraps[i].name, name) == 0) {
      *wraps[i].real = real;
      return wraps[i].wrapper;
    }
  }
  return real;
}

void gl_debug_init(void) {
  gl_thread = threadGetSelf();
  real_glGetShaderiv = (void *)eglGetProcAddress("glGetShaderiv");
  real_glGetShaderInfoLog = (void *)eglGetProcAddress("glGetShaderInfoLog");
  real_glGetProgramiv = (void *)eglGetProcAddress("glGetProgramiv");
  real_glGetProgramInfoLog = (void *)eglGetProcAddress("glGetProgramInfoLog");
  real_glGetError = (void *)eglGetProcAddress("glGetError");
}

/* Called once per frame; logs a summary every `every` frames. */
void gl_debug_frame(unsigned long frame, unsigned every) {
  GLenum err = real_glGetError ? real_glGetError() : 0;
  if (frame % every == 0 || frame < 3) {
    debugPrintf("[gl] frame %lu: draws default=%u fbo=%u (no-program %u), clears default=%u fbo=%u, "
                "fbo binds %u, viewport %d,%d %dx%d, clear (%.2f %.2f %.2f %.2f), glError 0x%x, "
                "shader fails %u, link fails %u\n",
                frame, st.draws_default, st.draws_fbo, st.prog_zero_draws, st.clears_default, st.clears_fbo,
                st.fbo_binds, st.vp[0], st.vp[1], st.vp[2], st.vp[3], st.clear[0], st.clear[1], st.clear[2],
                st.clear[3], err, shader_fail, link_fail);
  }
  st.draws_default = st.draws_fbo = st.clears_default = st.clears_fbo = st.fbo_binds = st.prog_zero_draws = 0;
}

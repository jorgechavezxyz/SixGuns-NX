/* In-game SCREENSHOT button (replaces the dead SHARE button on achievement
 * popups and other share prompts).
 *
 * The engine's share buttons end in Popup::ShowSharePopup(...) (Facebook/
 * Twitter share sheet, dead). That function is redirected here
 * (so_hook_function in tweaks.c): pressing the button requests a capture, and
 * the next finished frame -- with the popup still on screen, before the port's
 * own overlays are drawn -- is read back and saved to the Switch album through
 * caps:su (capssuSaveScreenShot, RGBA8 1280x720; the system shows its usual
 * "screenshot saved" notice). If the album refuses (no permission for the
 * running title, emulator without caps:su), the picture is saved as a PNG in
 * switch/sixguns/photos/ instead.
 *
 * The button label is changed from SHARE to SCREENSHOT in social.c. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <png.h>
#include "config.h"
#include "util.h"
#include "so_util.h"

so_module *port_engine_module(void);

/* The achievement popup's OK path ends with closePopup-like cleanup
 * (AchivementMgr+8 = 0, AchivementMgr::achievementPopupClosed = 1); its SHARE
 * path doesn't, because the share sheet used to take over. Without it the
 * popup flag stayed set after SCREENSHOT: the popup was gone from the screen
 * but the port still saw a popup, so input stayed in menu mode and the hint
 * sidebar stayed hidden. Done here after the capture. */
static void close_achievement_popup(void) {
  so_module *mod = port_engine_module();
  void *(*inst)(int) = (void *)so_symbol(mod, "_ZN9SingletonI13AchivementMgrE11GetInstanceEb");
  uint8_t *closed = (uint8_t *)so_symbol(mod, "_ZN13AchivementMgr22achievementPopupClosedE");
  uint8_t *am = inst ? inst(0) : NULL;
  if (!am || !am[8]) return;
  am[8] = 0;
  if (closed) *closed = 1;
  debugPrintf("[shot] achievement popup closed\n");
}

static volatile int requested;

/* Called by the engine instead of Popup::ShowSharePopup. Returns false (no
 * share sheet shown), like the stub it replaces. */
int screenshot_share_hook(void) {
  /* Who pressed SHARE decides what it does: the generic Popup (purchase
   * "congratulations", gifts; Popup::Update) shows CLOSE (social.c) and is
   * closed like B; everything else (achievement popup, level-up, quest
   * summary) takes a screenshot. Decided by the caller's function, since the
   * achievement manager's flag can be set while a purchase popup is up. */
  so_module *mod = port_engine_module();
  uintptr_t lr = (uintptr_t)__builtin_return_address(0), off = 0;
  const char *caller = (lr >= mod->base && lr < mod->base + mod->image_size) ? so_symbol_name(mod, lr, &off) : NULL;
  if (caller && !strcmp(caller, "_ZN5Popup6UpdateEv")) {
    /* Only the popup: a Back press here also closed the shop behind a purchase popup (owner,
     * 2026-11-13). Popup::HidePopup is what Back does to a popup; it runs on the next frame,
     * not from inside Popup::Update. */
    extern void port_request_popup_close(void);
    {
      /* The lottery win dialog (Popup type 19): its own close also runs LotteryMgr::ResetLottery.
       * Without it the lottery stayed in its "won" state 8: the dialog came back at every boot and
       * Back was refused everywhere ("You cannot go back at this stage", emulator 2026-11-14). */
      void *(*pinst)(int) = (void *)so_symbol(mod, "_ZN9SingletonI5PopupE11GetInstanceEb");
      int (*ptype)(void *) = (void *)so_symbol(mod, "_ZNK5Popup7GetTypeEv");
      void *(*linst)(int) = (void *)so_symbol(mod, "_ZN9SingletonI10LotteryMgrE11GetInstanceEb");
      void (*lreset)(void *) = (void *)so_symbol(mod, "_ZN10LotteryMgr12ResetLotteryEv");
      void *p = pinst ? pinst(0) : NULL, *lm = linst ? linst(0) : NULL;
      if (p && ptype && ptype(p) == 19 && lm && lreset) {
        lreset(lm);
        debugPrintf("[shot] lottery win dialog closed: lottery reset\n");
      }
    }
    port_request_popup_close();
    debugPrintf("[shot] popup SHARE pressed: closing the popup (CLOSE button)\n");
    return 0;
  }
  requested = 1;
  debugPrintf("[shot] share button pressed (%s): taking a screenshot\n", caller ? caller : "?");
  return 0;
}

static int save_png(const char *path, const uint8_t *rgba) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop info = png ? png_create_info_struct(png) : NULL;
  if (!png || !info || setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    fclose(f);
    return -1;
  }
  png_init_io(png, f);
  png_set_IHDR(png, info, SCREEN_W, SCREEN_H, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_set_compression_level(png, 3);
  png_write_info(png, info);
  for (int y = 0; y < SCREEN_H; y++) png_write_row(png, (png_const_bytep)(rgba + (size_t)y * SCREEN_W * 4));
  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  fclose(f);
  return 0;
}

/* Called after the engine has drawn the frame, before the port's overlays. */
void screenshot_frame(void);
/* Marker `debug_capture_l`: one capture WITH the port's overlays after L has
 * been held for a second in gameplay (checks the elixir key on screen). */
void screenshot_frame_post(void) {
  static int marker = -1, frames, done;
  if (marker < 0) {
    FILE *m = fopen(PORT_ROOT "/debug_capture_l", "r");
    marker = m != NULL;
    if (m) fclose(m);
  }
  extern int elixir_key_shown;
  if (!marker || done) return;
  frames = elixir_key_shown ? frames + 1 : 0;
  if (frames == 60) { done = 1; requested = 1; debugPrintf("[shot] debug_capture_l: capturing with overlays\n"); screenshot_frame(); }
}

void screenshot_frame(void) {
  /* Marker `debug_screenshot`: one capture 20 s after start (tests this path). */
  static unsigned long n;
  if (++n == 1200) {
    FILE *m = fopen(PORT_ROOT "/debug_screenshot", "r");
    if (m) { fclose(m); requested = 1; debugPrintf("[shot] debug_screenshot marker: capturing now\n"); }
  }
  if (!requested) return;
  requested = 0;
  static uint8_t px[SCREEN_W * SCREEN_H * 4];
  static uint8_t strip[320 * SCREEN_H * 4];
  void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) =
      (void *)eglGetProcAddress("glReadPixels");
  void (*finish)(void) = (void *)eglGetProcAddress("glFinish");
  void (*bind_fb)(GLenum, GLuint) = (void *)eglGetProcAddress("glBindFramebuffer");
  void (*get_iv)(GLenum, GLint *) = (void *)eglGetProcAddress("glGetIntegerv");
  void (*pixel_store)(GLenum, GLint) = (void *)eglGetProcAddress("glPixelStorei");
  if (!read_pixels || !finish || !bind_fb || !get_iv || !pixel_store) return;
  GLint fbo = 0, pack = 4;
  get_iv(GL_FRAMEBUFFER_BINDING, &fbo);
  get_iv(GL_PACK_ALIGNMENT, &pack);
  bind_fb(GL_FRAMEBUFFER, 0);
  pixel_store(GL_PACK_ALIGNMENT, 4);
  finish();
  /* 320-pixel strips (Ryujinx only copies back part of a full-width read);
   * GL rows are bottom-up, the album wants top-down. Alpha forced opaque. */
  for (int sx = 0; sx < SCREEN_W; sx += 320) {
    read_pixels(sx, 0, 320, SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, strip);
    for (int y = 0; y < SCREEN_H; y++) {
      uint8_t *dst = px + ((size_t)(SCREEN_H - 1 - y) * SCREEN_W + sx) * 4;
      memcpy(dst, strip + (size_t)y * 320 * 4, 320 * 4);
      for (int i = 3; i < 320 * 4; i += 4) dst[i] = 255;
    }
  }
  pixel_store(GL_PACK_ALIGNMENT, pack);
  bind_fb(GL_FRAMEBUFFER, fbo);

  Result rc = capssuInitialize();
  if (R_SUCCEEDED(rc)) {
    rc = capssuSaveScreenShot(px, sizeof(px), AlbumReportOption_Enable, AlbumImageOrientation_Unknown0, NULL);
    capssuExit();
  }
  close_achievement_popup();
  if (R_SUCCEEDED(rc)) {
    debugPrintf("[shot] saved to the Switch album\n");
    return;
  }
  mkdir(PORT_ROOT "/photos", 0777);
  char path[160];
  time_t t = time(NULL);
  struct tm tm0 = { 0 }, *tm = localtime(&t);
  if (!tm) tm = &tm0;
  snprintf(path, sizeof(path), PORT_ROOT "/photos/sixguns_%04d%02d%02d_%02d%02d%02d.png", tm->tm_year + 1900,
           tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
  int ok = save_png(path, px) == 0;
  debugPrintf("[shot] album refused (0x%x); %s %s\n", rc, ok ? "saved" : "could not save", path);
}

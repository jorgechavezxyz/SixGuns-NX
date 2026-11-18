/* First-launch setup on the Switch: turns the player's own official game files
 * (the APK and the two OBBs, copied next to sixguns.nro) into the install the
 * PC SD Builder (publish/tools/build_sd.py) makes:
 *   libAndroid.so  <- lib/arm64-v8a/libAndroid.so from the APK
 *   assets/        <- every assets/ entry of the APK
 *   base.apk       <- the APK
 *   obb/           <- the two OBBs
 *   lan            <- LAN multiplayer marker
 *   setup.complete <- written last
 *
 * Only Gameloft's official Six Guns 2.9.9a files are accepted (SHA-256, same
 * values as publish/tools/known_files.json). Everything is written under
 * temporary names (libAndroid.so.part, assets.part/) and renamed at the end,
 * so a setup that is interrupted (power off, B, crash) starts over cleanly on
 * the next launch. save/, prefs.txt, device_id.txt, save_backups/ and mods/
 * are never touched. An install that already has libAndroid.so (made by the
 * SD Builder or an earlier setup) is left exactly as it is.
 *
 * Not done here: the Stadium demo map (mods/30_stadium.zip) still needs the PC
 * builder (tools/modding/maps/stadium/build_stadium.py). */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/iosupport.h>
#include <zlib.h>
#include <switch.h>
#include "config.h"
#include "util.h"

#define SETUP_DONE   PORT_ROOT "/setup.complete"
#define LIB_PART     PORT_ROOT "/libAndroid.so.part"
#define ASSETS_PART  PORT_ROOT "/assets.part"
#define PKG_DIR      PORT_ROOT "/" PACKAGE_NAME
#define ANDROID_OBB  PORT_ROOT "/Android/obb/" PACKAGE_NAME
#define LIB_IN_APK   "lib/arm64-v8a/libAndroid.so"

static const char APK_SHA[]   = "224f2f7d9e4e3f49ffbc40094747cb85cae798ce2dc95c14e7026c23d8840f37";
static const char LIB_SHA[]   = "502fd831bcd6fe24d7e79f4ef41c3a36c7b6f02515df507c56618b4fd421d08e";
static const char MAIN_SHA[]  = "07ea412d5fed53c1050031e7f5b491d36539cf509a0eb29012b05403da502212";
static const char PATCH_SHA[] = "9480370d7c9caad7500fbc3c3bb5a308e05d6cc89f25fb6929b3fe3aa3cfc7a8";

#define BUF_SIZE (1 << 20)
#define PATH_MAX_FR 512

enum { FR_OK = 0, FR_CANCEL = -1, FR_FAIL = -2 };

static PadState pad;
/* stdout/stderr as they were before consoleInit: restored after consoleExit
 * (debugPrintf mirrors every log line to stdout, and a stdout still bound to
 * the closed console crashes in the framebuffer). */
static const devoptab_t *saved_out, *saved_err;

/* Log line for log.txt only, not drawn on the setup screen. */
#define slog(...) do { const devoptab_t *cur_ = devoptab_list[STD_OUT];     devoptab_list[STD_OUT] = saved_out; debugPrintf(__VA_ARGS__); devoptab_list[STD_OUT] = cur_; } while (0)

static void console_open(void) {
  saved_out = devoptab_list[STD_OUT];
  saved_err = devoptab_list[STD_ERR];
  consoleInit(NULL);
}

static void console_close(void) {
  consoleExit(NULL);
  devoptab_list[STD_OUT] = saved_out;
  devoptab_list[STD_ERR] = saved_err;
}
static u64 last_draw;
static char fail_msg[1024];

/* ---------- screen ---------- */

static int cancel_pressed(void) {
  padUpdate(&pad);
  return (padGetButtonsDown(&pad) & HidNpadButton_B) != 0;
}

/* Redraws at most ~15 times a second (consoleUpdate flushes the framebuffer). */
static void draw(int force) {
  u64 now = armGetSystemTick();
  if (!force && armTicksToNs(now - last_draw) < 66000000ULL) return;
  last_draw = now;
  consoleUpdate(NULL);
}

static void progress(const char *what, u64 done, u64 total) {
  int pct = total ? (int)(done * 100 / total) : 100, bars = pct * 30 / 100;
  char bar[31];
  for (int i = 0; i < 30; i++) bar[i] = i < bars ? '#' : '.';
  bar[30] = 0;
  printf("\r  %-26.26s [%s] %3d%%", what, bar, pct);
  draw(0);
}

static void header(void) {
  printf("\x1b[2J\x1b[1;1H");
  printf("\n  Preparing Six Guns...\n  =====================\n\n");
  printf("  First start: setting up the game from your APK and OBB files.\n");
  printf("  This takes a few minutes. Press B to cancel.\n\n");
}

/* Shows a message and waits for + (the caller then quits back to the menu). */
static void wait_exit(const char *title, const char *msg) {
  { extern void log_flush(void); log_flush(); } /* the result is in log.txt even if the console is switched off here */
  printf("\x1b[2J\x1b[1;1H\n  %s\n  ", title);
  for (const char *p = title; *p; p++) putchar('=');
  printf("\n\n%s\n\n  Press + to exit.\n", msg);
  draw(1);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    svcSleepThread(16000000ULL);
  }
}

static int refuse(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int refuse(const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  vsnprintf(fail_msg, sizeof(fail_msg), fmt, va);
  va_end(va);
  slog("[setup] refused: %s\n", fail_msg);
  return FR_FAIL;
}

/* ---------- files ---------- */

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static void rm_rf(const char *path) {
  if (!is_dir(path)) { remove(path); return; }
  DIR *d = opendir(path);
  if (d) {
    struct dirent *e;
    char sub[PATH_MAX_FR];
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
      rm_rf(sub);
    }
    closedir(d);
  }
  rmdir(path);
}

static int mkdirs_for(const char *file) {
  char tmp[PATH_MAX_FR];
  snprintf(tmp, sizeof(tmp), "%s", file);
  for (char *p = tmp + strlen("sdmc:/"); *p; p++) {
    if (*p != '/') continue;
    *p = 0;
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST && !is_dir(tmp)) return -1;
    *p = '/';
  }
  return 0;
}

static void hex(const u8 *h, char *out) {
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", h[i]);
}

/* Hardware SHA-256 of a file, with a progress line. */
static int sha256_file(const char *path, const char *label, char out[65], u8 *buf) {
  FILE *f = fopen(path, "rb");
  if (!f) return refuse("  Could not open %s", path);
  setvbuf(f, NULL, _IONBF, 0);
  fseek(f, 0, SEEK_END);
  u64 size = ftell(f), done = 0;
  fseek(f, 0, SEEK_SET);
  Sha256Context ctx;
  sha256ContextCreate(&ctx);
  size_t n;
  while ((n = fread(buf, 1, BUF_SIZE, f)) > 0) {
    sha256ContextUpdate(&ctx, buf, n);
    done += n;
    progress(label, done, size);
    if (cancel_pressed()) { fclose(f); return FR_CANCEL; }
  }
  int err = ferror(f);
  fclose(f);
  if (err) return refuse("  Read error in %s (check the SD card).", path);
  u8 h[32];
  sha256ContextGetHash(&ctx, h);
  hex(h, out);
  progress(label, size, size);
  printf("\n");
  return FR_OK;
}

static int ends_with_ci(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && !strcasecmp(s + a - b, suf);
}

/* ---------- locating the player's files ---------- */

static const char *const search_dirs[] = { PORT_ROOT, OBB_ROOT, PKG_DIR, ANDROID_OBB };

#define MAX_APKS 8
static char apks[MAX_APKS][PATH_MAX_FR];
static int n_apks;
static char main_obb[PATH_MAX_FR], patch_obb[PATH_MAX_FR], other_obb[PATH_MAX_FR];

static void scan_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    const char *n = e->d_name;
    char full[PATH_MAX_FR];
    if (n[0] == '.') continue; /* ., .., macOS ._ AppleDouble files */
    snprintf(full, sizeof(full), "%s/%s", dir, n);
    if (is_dir(full)) continue;
    if (ends_with_ci(n, ".apk")) {
      /* base.apk last: a loose APK the player just copied wins over an old base.apk */
      if (n_apks < MAX_APKS) snprintf(apks[n_apks++], PATH_MAX_FR, "%s", full);
    } else if (!strcmp(n, MAIN_OBB)) {
      if (!main_obb[0]) snprintf(main_obb, sizeof(main_obb), "%s", full);
    } else if (!strcmp(n, PATCH_OBB)) {
      if (!patch_obb[0]) snprintf(patch_obb, sizeof(patch_obb), "%s", full);
    } else if (ends_with_ci(n, "." PACKAGE_NAME ".obb")) {
      snprintf(other_obb, sizeof(other_obb), "%s", n);
    }
  }
  closedir(d);
}

static void find_files(void) {
  n_apks = 0;
  main_obb[0] = patch_obb[0] = other_obb[0] = 0;
  for (size_t i = 0; i < sizeof(search_dirs) / sizeof(*search_dirs); i++) scan_dir(search_dirs[i]);
  /* base.apk (a setup interrupted after moving the APK) is tried last */
  for (int i = 0; i < n_apks - 1; i++)
    if (!strcmp(apks[i], APK_PATH)) {
      char t[PATH_MAX_FR];
      memcpy(t, apks[i], PATH_MAX_FR);
      memmove(apks[i], apks[i + 1], (size_t)(n_apks - 1 - i) * PATH_MAX_FR);
      memcpy(apks[n_apks - 1], t, PATH_MAX_FR);
      break;
    }
}

static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

/* ---------- zip (APK) ---------- */

typedef struct {
  char name[256];
  u16 method;
  u32 crc, csize, usize, local_off;
} ZipEntry;

static ZipEntry *zents;
static int n_zents;

static u16 rd16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

static int zip_open(FILE *f) {
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  long tail = size < 65557 ? size : 65557;
  u8 *t = malloc(tail);
  if (!t) return -1;
  fseek(f, size - tail, SEEK_SET);
  if (fread(t, 1, tail, f) != (size_t)tail) { free(t); return -1; }
  long eocd = -1;
  for (long i = tail - 22; i >= 0; i--)
    if (rd32(t + i) == 0x06054b50) { eocd = i; break; }
  if (eocd < 0) { free(t); return -1; }
  u32 count = rd16(t + eocd + 10), cd_size = rd32(t + eocd + 12), cd_off = rd32(t + eocd + 16);
  free(t);
  if ((long)cd_off + (long)cd_size > size) return -1;
  u8 *cd = malloc(cd_size);
  zents = calloc(count ? count : 1, sizeof(ZipEntry));
  if (!cd || !zents) { free(cd); return -1; }
  fseek(f, cd_off, SEEK_SET);
  if (fread(cd, 1, cd_size, f) != cd_size) { free(cd); return -1; }
  n_zents = 0;
  u32 p = 0;
  for (u32 i = 0; i < count; i++) {
    if (p + 46 > cd_size || rd32(cd + p) != 0x02014b50) { free(cd); return -1; }
    u32 nl = rd16(cd + p + 28), xl = rd16(cd + p + 30), cl = rd16(cd + p + 32);
    if (p + 46 + nl > cd_size) { free(cd); return -1; }
    ZipEntry *e = &zents[n_zents++];
    e->method = rd16(cd + p + 10);
    e->crc = rd32(cd + p + 16);
    e->csize = rd32(cd + p + 20);
    e->usize = rd32(cd + p + 24);
    e->local_off = rd32(cd + p + 42);
    if (nl >= sizeof(e->name)) { free(cd); return -1; }
    memcpy(e->name, cd + p + 46, nl);
    e->name[nl] = 0;
    p += 46 + nl + xl + cl;
  }
  free(cd);
  return 0;
}

/* Entry names come from a hash-checked APK, but are still checked before they
 * become paths: no absolute paths, no "..", no backslashes, drive letters or
 * control characters, no empty components. */
static int safe_name(const char *n) {
  if (!n[0] || n[0] == '/') return 0;
  for (const char *p = n; *p; p++)
    if (*p == '\\' || *p == ':' || (unsigned char)*p < 0x20) return 0;
  const char *c = n;
  while (*c) {
    const char *s = strchr(c, '/');
    size_t len = s ? (size_t)(s - c) : strlen(c);
    if (len == 0 || (len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.')) return 0;
    if (!s) break;
    c = s + 1;
  }
  return 1;
}

/* Extracts one entry to dest; checks its CRC, optionally hashes it. */
static int zip_extract(FILE *f, const ZipEntry *e, const char *dest, u8 *inbuf, u8 *outbuf,
                       u64 *done, u64 total, char sha_out[65]) {
  u8 lh[30];
  fseek(f, e->local_off, SEEK_SET);
  if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) return refuse("  The APK is damaged (%s).", e->name);
  fseek(f, e->local_off + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
  if (e->method != 0 && e->method != 8) return refuse("  Unsupported compression in the APK (%s).", e->name);
  if (mkdirs_for(dest) != 0) return refuse("  Could not create the folder for %s (SD card full or read-only?).", dest);
  FILE *o = fopen(dest, "wb");
  if (!o) return refuse("  Could not write %s (SD card full or read-only?).", dest);
  setvbuf(o, NULL, _IOFBF, 256 * 1024);

  Sha256Context ctx;
  if (sha_out) sha256ContextCreate(&ctx);
  uLong crc = crc32(0L, Z_NULL, 0);
  u32 left = e->csize, written = 0;
  int rc = FR_OK;
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (e->method == 8 && inflateInit2(&zs, -MAX_WBITS) != Z_OK) { fclose(o); return refuse("  zlib init failed."); }

  int zret = Z_OK;
  while (rc == FR_OK && (left > 0 || (e->method == 8 && zret != Z_STREAM_END))) {
    size_t want = left < BUF_SIZE ? left : BUF_SIZE;
    size_t got = want ? fread(inbuf, 1, want, f) : 0;
    if (got != want) { rc = refuse("  Read error in the APK (%s).", e->name); break; }
    left -= got;
    if (e->method == 0) {
      if (fwrite(inbuf, 1, got, o) != got) { rc = refuse("  Write error (SD card full?)."); break; }
      crc = crc32(crc, inbuf, got);
      if (sha_out) sha256ContextUpdate(&ctx, inbuf, got);
      written += got;
    } else {
      zs.next_in = inbuf;
      zs.avail_in = got;
      do {
        zs.next_out = outbuf;
        zs.avail_out = BUF_SIZE;
        zret = inflate(&zs, Z_NO_FLUSH);
        if (zret == Z_BUF_ERROR) break; /* needs more input */
        if (zret != Z_OK && zret != Z_STREAM_END) {
          rc = refuse("  The APK is damaged (%s).", e->name);
          break;
        }
        size_t n = BUF_SIZE - zs.avail_out;
        if (n && fwrite(outbuf, 1, n, o) != n) { rc = refuse("  Write error (SD card full?)."); break; }
        crc = crc32(crc, outbuf, n);
        if (sha_out) sha256ContextUpdate(&ctx, outbuf, n);
        written += n;
        *done += n;
        if (zret == Z_STREAM_END) break;
      } while (zs.avail_out == 0);
      if (got == 0 && zret != Z_STREAM_END && rc == FR_OK) rc = refuse("  The APK is damaged (%s).", e->name);
    }
    if (e->method == 0) *done += got;
    progress("Extracting game files", *done, total);
    if (rc == FR_OK && cancel_pressed()) rc = FR_CANCEL;
  }
  if (e->method == 8) inflateEnd(&zs);
  if (fclose(o) != 0 && rc == FR_OK) rc = refuse("  Write error (SD card full?).");
  if (rc != FR_OK) return rc;
  if (written != e->usize || (u32)crc != e->crc) return refuse("  The APK is damaged (%s: checksum).", e->name);
  if (sha_out) {
    u8 h[32];
    sha256ContextGetHash(&ctx, h);
    hex(h, sha_out);
  }
  return FR_OK;
}

/* ---------- setup ---------- */

static int move_file(const char *from, const char *to) {
  if (!strcmp(from, to)) return 0;
  remove(to);
  if (rename(from, to) == 0) return 0;
  slog("[setup] rename %s -> %s failed (errno %d)\n", from, to, errno);
  return -1;
}

static int run_setup(u8 *buf, u8 *buf2) {
  header();
  printf("  Checking your files...\n\n");
  draw(1);

  /* 1. The official APK (any file name). */
  const char *apk = NULL;
  char sha[65];
  for (int i = 0; i < n_apks && !apk; i++) {
    char label[64];
    snprintf(label, sizeof(label), "%.60s", base(apks[i]));
    int r = sha256_file(apks[i], label, sha, buf);
    if (r != FR_OK) return r;
    slog("[setup] %s sha256 %s\n", apks[i], sha);
    if (!strcmp(sha, APK_SHA)) apk = apks[i];
  }
  if (!apk) {
    char names[400] = "";
    for (int i = 0; i < n_apks; i++) {
      size_t l = strlen(names);
      snprintf(names + l, sizeof(names) - l, "%s%.60s", i ? ", " : "", base(apks[i]));
    }
    return refuse("  Wrong APK: %s\n  is not the official Six Guns 2.9.9a APK (it is modified, re-signed,\n"
                  "  repackaged or a different version).\n\n"
                  "  Use Gameloft's official APK: com.gameloft.android.ANMP.GloftSXHM,\n"
                  "  version 2.9.9a (version code 29923). Delete the wrong file, copy the\n"
                  "  official one into switch/sixguns/ and start Six Guns again.", names);
  }

  /* 2. The two OBBs, byte-identical to Gameloft's. */
  const struct { const char *path, *name, *sha; } obbs[2] = {
    { main_obb, MAIN_OBB, MAIN_SHA }, { patch_obb, PATCH_OBB, PATCH_SHA } };
  for (int i = 0; i < 2; i++) {
    int r = sha256_file(obbs[i].path, i ? "patch OBB" : "main OBB", sha, buf);
    if (r != FR_OK) return r;
    slog("[setup] %s sha256 %s\n", obbs[i].path, sha);
    if (strcmp(sha, obbs[i].sha))
      return refuse("  Wrong OBB: %s\n  is not the original file (it differs from Gameloft's).\n\n"
                    "  Copy it again from the phone's Android/obb/%s/ folder\n"
                    "  (Six Guns 2.9.9a) and start Six Guns again.", obbs[i].name, PACKAGE_NAME);
  }
  printf("\n  All files are the official Six Guns 2.9.9a.\n\n");
  draw(1);

  /* 3. Clear what an interrupted setup left behind. assets/ without libAndroid.so
   *    can only be an incomplete copy of the APK's assets; it is rebuilt. */
  remove(LIB_PART);
  rm_rf(ASSETS_PART);
  rm_rf(ASSETS_ROOT);

  /* 4. Read the APK's file list and check the free space. */
  FILE *f = fopen(apk, "rb");
  if (!f) return refuse("  Could not open %s", apk);
  setvbuf(f, NULL, _IONBF, 0);
  if (zip_open(f) != 0) { fclose(f); return refuse("  %s could not be read as an APK (damaged?).", base(apk)); }
  const ZipEntry *lib = NULL;
  u64 total = 0;
  int n_assets = 0;
  for (int i = 0; i < n_zents; i++) {
    const ZipEntry *e = &zents[i];
    size_t nl = strlen(e->name);
    if (!strcmp(e->name, LIB_IN_APK)) { lib = e; total += e->usize; }
    else if (!strncmp(e->name, "assets/", 7) && nl > 7 && e->name[nl - 1] != '/') {
      if (!safe_name(e->name)) { fclose(f); return refuse("  Unsafe file name in the APK: %s", e->name); }
      total += e->usize;
      n_assets++;
    }
  }
  if (!lib) { fclose(f); return refuse("  The APK has no arm64 engine (%s).", LIB_IN_APK); }
  s64 free_b = 0;
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (fs && R_SUCCEEDED(fsFsGetFreeSpace(fs, "/", &free_b))) {
    u64 need = total + (64ULL << 20);
    slog("[setup] need %llu MB, free %lld MB\n", (unsigned long long)(need >> 20), (long long)(free_b >> 20));
    if ((u64)free_b < need) {
      fclose(f);
      return refuse("  Not enough free space on the SD card: setup needs %llu MB more,\n"
                    "  the card has %lld MB free. Free some space and start Six Guns again.",
                    (unsigned long long)(need >> 20), (long long)(free_b >> 20));
    }
  }

  /* 5. Engine + assets, under temporary names. */
  printf("  Extracting the engine and %d asset files...\n\n", n_assets);
  draw(1);
  u64 done = 0;
  int r = zip_extract(f, lib, LIB_PART, buf, buf2, &done, total, sha);
  if (r == FR_OK && strcmp(sha, LIB_SHA)) r = refuse("  The engine inside the APK is not Gameloft's 2.9.9a engine.");
  char dest[PATH_MAX_FR];
  for (int i = 0; r == FR_OK && i < n_zents; i++) {
    const ZipEntry *e = &zents[i];
    size_t nl = strlen(e->name);
    if (strncmp(e->name, "assets/", 7) || nl <= 7 || e->name[nl - 1] == '/') continue;
    snprintf(dest, sizeof(dest), "%s/%s", ASSETS_PART, e->name + 7);
    r = zip_extract(f, e, dest, buf, buf2, &done, total, NULL);
  }
  fclose(f);
  if (r != FR_OK) return r;
  progress("Extracting game files", total, total);
  printf("\n\n  Finishing...\n");
  draw(1);

  /* 6. Put everything in place; libAndroid.so and setup.complete last. */
  mkdir(OBB_ROOT, 0777);
  if (move_file(main_obb, OBB_ROOT "/" MAIN_OBB) || move_file(patch_obb, OBB_ROOT "/" PATCH_OBB))
    return refuse("  Could not move the OBB files into switch/sixguns/obb/.");
  if (move_file(apk, APK_PATH)) return refuse("  Could not rename the APK to base.apk.");
  if (rename(ASSETS_PART, ASSETS_ROOT) != 0) return refuse("  Could not rename assets.part to assets.");
  FILE *m = fopen(PORT_ROOT "/lan", "a");
  if (m) fclose(m);
  if (rename(LIB_PART, SO_PATH) != 0) return refuse("  Could not rename libAndroid.so.part.");
  m = fopen(SETUP_DONE, "w");
  if (m) {
    fprintf(m, "Six Guns NX first-launch setup: official 2.9.9a APK + OBBs verified, %d asset files.\n", n_assets);
    fclose(m);
  }
  /* The emptied copy of the phone's folder (fails harmlessly if anything is left in it). */
  rmdir(PKG_DIR);
  rmdir(ANDROID_OBB);
  slog("[setup] complete: %d assets, %llu bytes\n", n_assets, (unsigned long long)total);
  return FR_OK;
}

/* Returns 0 to start the game, nonzero to quit back to the homebrew menu. */
int first_run(void) {
  if (exists(SETUP_DONE) || exists(SO_PATH)) return 0; /* set up already (here or by the SD Builder) */

  console_open();
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  find_files();
  slog("[setup] first launch: %d apk(s), main obb %s, patch obb %s\n", n_apks,
              main_obb[0] ? main_obb : "-", patch_obb[0] ? patch_obb : "-");

  if (!n_apks || !main_obb[0] || !patch_obb[0]) {
    char msg[1200];
    int l = snprintf(msg, sizeof(msg),
        "  Six Guns needs your own copy of the game. Copy these files into the\n"
        "  SD card's switch/sixguns/ folder (next to sixguns.nro):\n\n"
        "    - the official Six Guns 2.9.9a APK (any file name ending in .apk)\n"
        "    - " MAIN_OBB "\n"
        "    - " PATCH_OBB "\n"
        "      (both OBBs are in the phone's Android/obb/" PACKAGE_NAME "/\n"
        "      folder; copying that whole folder works too)\n\n"
        "  Missing:%s%s%s\n",
        n_apks ? "" : " the APK", main_obb[0] ? "" : " the main OBB", patch_obb[0] ? "" : " the patch OBB");
    if (other_obb[0] && l > 0 && (size_t)l < sizeof(msg))
      snprintf(msg + l, sizeof(msg) - l, "\n  Found %s: that is a different game version.\n"
               "  Six Guns NX needs version 2.9.9a.\n", other_obb);
    slog("[setup] files missing\n");
    wait_exit("Six Guns: game files needed", msg);
    console_close();
    return 1;
  }

  u8 *buf = malloc(BUF_SIZE), *buf2 = malloc(BUF_SIZE);
  int r = buf && buf2 ? run_setup(buf, buf2) : refuse("  Out of memory.");
  free(buf);
  free(buf2);
  free(zents);
  zents = NULL;

  if (r == FR_OK) {
    printf("\n  Done. Starting Six Guns...\n");
    draw(1);
    svcSleepThread(1000000000ULL);
    console_close();
    return 0;
  }
  if (r == FR_CANCEL) {
    slog("[setup] cancelled with B\n");
    wait_exit("Setup cancelled",
              "  Nothing was installed and your saves were not touched.\n"
              "  Start Six Guns again to finish setting up.");
  } else {
    wait_exit("Six Guns: setup stopped", fail_msg);
  }
  console_close();
  return 1;
}

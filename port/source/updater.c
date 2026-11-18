/* Online update check at startup (Switch).
 *
 * Asks GitHub for the newest SixGuns-NX release (prereleases included) and
 * compares the SHA-256 of its sixguns.nro asset (GitHub's own "digest" field)
 * with the SHA-256 of the running sixguns.nro. When they differ and the release
 * was published after this build was made (a newer local build is never
 * "updated" back), the player is asked:
 *   A  update now   - download, check size + SHA-256 + NRO header, swap in
 *                     (previous launchable copy retained), restart
 *   B  not now      - start the game; ask again next time
 *   X  skip version - never ask about this release again (update_skip.txt)
 * The check is quick (short timeouts) and silent when offline, up to date or
 * anything fails; the game always starts. Never runs in the desktop launcher
 * (emulator_mode: the launcher updates its own copy) or with the
 * `no_update_check` marker. Saves and other files are never touched.
 *
 * Test-only: with emulator_mode, the marker `update_check_in_emulator` turns
 * the check on; if it contains "accept" the prompt is answered with A. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/iosupport.h>
#include <curl/curl.h>
#include <switch.h>
#include "config.h"
#include "util.h"

#ifndef BUILD_EPOCH
#define BUILD_EPOCH 0
#endif

#define RELEASES_URL "https://api.github.com/repos/jorgechavezxyz/SixGuns-NX/releases?per_page=1"
#define CACHE_PATH   PORT_ROOT "/update_check.txt"
#define CHECK_EVERY  (6 * 3600) /* seconds between online checks (GitHub allows 60 anonymous requests an hour per IP) */
#define USER_AGENT   "SixGuns-NX-updater"
#define DL_PATH      PORT_ROOT "/sixguns.nro.download"
#define BOOT_PATH    PORT_ROOT "/sixguns.nro"
#define SLOT_PENDING PORT_ROOT "/update-slot.part"
#define SKIP_PATH    PORT_ROOT "/update_skip.txt"
#define MAX_NRO      (64u << 20)

static PadState pad;
static int net_up(void);
static const devoptab_t *saved_out, *saved_err;
static int console_on, auto_accept;
static const char *const slot_nro[] = { PORT_ROOT "/sixguns-update-a.nro", PORT_ROOT "/sixguns-update-b.nro" };
static const char *const slot_stamp[] = { PORT_ROOT "/update-a.complete", PORT_ROOT "/update-b.complete" };

#define ulog(...) do { const devoptab_t *cur_ = devoptab_list[STD_OUT]; \
    if (console_on) { devoptab_list[STD_OUT] = saved_out; } debugPrintf(__VA_ARGS__); devoptab_list[STD_OUT] = cur_; } while (0)

static void console_open(void) {
  if (console_on) return;
  saved_out = devoptab_list[STD_OUT];
  saved_err = devoptab_list[STD_ERR];
  consoleInit(NULL);
  console_on = 1;
}

static void console_close(void) {
  if (!console_on) return;
  consoleExit(NULL);
  devoptab_list[STD_OUT] = saved_out;
  devoptab_list[STD_ERR] = saved_err;
  console_on = 0;
}

static void screen(const char *title) {
  printf("\x1b[2J\x1b[1;1H\n  %s\n  ", title);
  for (const char *p = title; *p; p++) putchar('=');
  printf("\n\n");
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* ---------- HTTP ---------- */

typedef struct { char *data; size_t len, cap; } Buf;

static size_t buf_write(char *p, size_t sz, size_t n, void *ud) {
  Buf *b = ud;
  size_t add = sz * n;
  if (b->len + add + 1 > 1u << 20) return 0; /* the release list is small; refuse anything huge */
  if (b->len + add + 1 > b->cap) {
    size_t cap = (b->len + add + 1) * 2;
    char *d = realloc(b->data, cap);
    if (!d) return 0;
    b->data = d;
    b->cap = cap;
  }
  memcpy(b->data + b->len, p, add);
  b->len += add;
  b->data[b->len] = 0;
  return add;
}

static CURL *new_curl(const char *url) {
  CURL *c = curl_easy_init();
  if (!c) return NULL;
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); /* gzip: the release list shrinks ~5x */
  return c;
}

/* ---------- minimal JSON scanning (GitHub's release list) ---------- */

/* End of the JSON value (object/array/string/literal) starting at p. */
static const char *skip_value(const char *p) {
  if (*p == '"') {
    for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
    return *p ? p + 1 : p;
  }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    for (; *p; p++) {
      if (*p == '"') { p = skip_value(p) - 1; continue; }
      if (*p == '{' || *p == '[') depth++;
      else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
    }
    return p;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']') p++;
  return p;
}

static const char *ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }

/* Value of a top-level key of the object at obj (pointer to its '{'), or NULL. */
static const char *member(const char *obj, const char *key) {
  size_t kl = strlen(key);
  const char *p = ws(obj + 1);
  while (*p == '"') {
    const char *k = p + 1, *ke = skip_value(p) - 1;
    p = ws(skip_value(p));
    if (*p != ':') return NULL;
    p = ws(p + 1);
    if ((size_t)(ke - k) == kl && !memcmp(k, key, kl)) return p;
    p = ws(skip_value(p));
    if (*p == ',') p = ws(p + 1);
  }
  return NULL;
}

/* Copies a JSON string value (no escapes expected in the fields we read). */
static int str_value(const char *v, char *out, size_t n) {
  if (!v || *v != '"') return 0;
  const char *e = skip_value(v) - 1;
  size_t l = (size_t)(e - v - 1);
  if (l >= n || memchr(v + 1, '\\', l)) return 0;
  memcpy(out, v + 1, l);
  out[l] = 0;
  return 1;
}

typedef struct {
  char tag[64], published[32], digest[80], url[512];
  long long size;
} Release;

/* Newest non-draft release with a sixguns.nro asset that has a sha256 digest. */
static int pick_release(const char *json, Release *r) {
  const char *p = ws(json);
  if (*p != '[') return -1; /* not a release list (error page) */
  p = ws(p + 1);
  while (*p == '{') {
    const char *obj = p;
    const char *draft = member(obj, "draft");
    if (draft && !strncmp(draft, "false", 5) &&
        str_value(member(obj, "tag_name"), r->tag, sizeof(r->tag)) &&
        str_value(member(obj, "published_at"), r->published, sizeof(r->published))) {
      const char *a = member(obj, "assets");
      if (a && *a == '[') {
        const char *q = ws(a + 1);
        while (*q == '{') {
          char name[64];
          if (str_value(member(q, "name"), name, sizeof(name)) && !strcmp(name, "sixguns.nro") &&
              str_value(member(q, "digest"), r->digest, sizeof(r->digest)) &&
              !strncmp(r->digest, "sha256:", 7) && strlen(r->digest) == 7 + 64 &&
              str_value(member(q, "browser_download_url"), r->url, sizeof(r->url))) {
            const char *s = member(q, "size");
            r->size = s ? strtoll(s, NULL, 10) : 0;
            return 1;
          }
          q = ws(skip_value(q));
          if (*q == ',') q = ws(q + 1);
        }
      }
    }
    p = ws(skip_value(p));
    if (*p == ',') p = ws(p + 1);
  }
  return 0;
}

/* "2026-11-17T15:19:20Z" -> seconds since 1970 (UTC). */
static long long iso_epoch(const char *s) {
  int y, mo, d, h, mi, se;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  y -= mo <= 2;
  long long era = (y >= 0 ? y : y - 399) / 400;
  long long yoe = y - era * 400;
  long long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 + se;
}

/* ---------- hashing / download ---------- */

static void hex(const u8 *h, char *out) { for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", h[i]); }

static int sha256_path(const char *path, char out[65]) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  setvbuf(f, NULL, _IONBF, 0);
  u8 *buf = malloc(1 << 20);
  if (!buf) { fclose(f); return 0; }
  Sha256Context ctx;
  sha256ContextCreate(&ctx);
  size_t n;
  while ((n = fread(buf, 1, 1 << 20, f)) > 0) sha256ContextUpdate(&ctx, buf, n);
  int ok = !ferror(f);
  fclose(f);
  free(buf);
  u8 h[32];
  sha256ContextGetHash(&ctx, h);
  hex(h, out);
  return ok;
}

typedef struct { FILE *f; Sha256Context sha; u64 got, total, last_draw; int cancelled, failed, finished; } Dl;

static size_t dl_write(char *p, size_t sz, size_t n, void *ud) {
  Dl *d = ud;
  size_t len = sz * n;
  if (d->got + len > MAX_NRO) { d->failed = 1; return 0; }
  if (fwrite(p, 1, len, d->f) != len) { d->failed = 1; return 0; }
  sha256ContextUpdate(&d->sha, p, len);
  d->got += len;
  return len;
}

static int dl_progress(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ut, curl_off_t un) {
  Dl *d = ud;
  (void)ut; (void)un;
  if (!d->finished) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_B) { d->cancelled = 1; return 1; }
  }
  u64 now = armGetSystemTick();
  if (armTicksToNs(now - d->last_draw) > 100000000ULL) {
    d->last_draw = now;
    (void)dltotal; (void)dlnow; /* per request; d->got counts across resumed requests */
    u64 total = d->total;
    int pct = total ? (int)(d->got * 100 / total) : 0, bars = pct * 30 / 100;
    if (pct > 100) pct = 100, bars = 30;
    char bar[31];
    for (int i = 0; i < 30; i++) bar[i] = i < bars ? '#' : '.';
    bar[30] = 0;
    printf("\r  Downloading [%s] %3d%%  (%llu KB)", bar, pct, (unsigned long long)(d->got / 1024));
    consoleUpdate(NULL);
  }
  return 0;
}

/* NRO header: "NRO0" at 0x10, declared size at 0x18 must fit in the file. */
static int nro_ok(const char *path, u64 file_size) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  u8 h[0x20];
  int ok = fread(h, 1, sizeof(h), f) == sizeof(h);
  fclose(f);
  if (!ok || memcmp(h + 0x10, "NRO0", 4)) return 0;
  u32 size = h[0x18] | h[0x19] << 8 | h[0x1a] << 16 | (u32)h[0x1b] << 24;
  return size >= 0x80 && size <= file_size;
}

/* The entry point stays in place. Each slot is eligible only after its stamp is
 * published, and updating the inactive slot never touches the selected one. */
static int boot_slot(long long *published) {
  int best = -1;
  *published = 0;
  for (int i = 0; i < 2; i++) {
    FILE *f = fopen(slot_stamp[i], "r");
    if (!f) continue;
    long long epoch = 0;
    int ok = fscanf(f, "%lld", &epoch) == 1;
    fclose(f);
    struct stat st;
    if (ok && epoch > *published && stat(slot_nro[i], &st) == 0 &&
        st.st_size > 0 && nro_ok(slot_nro[i], (u64)st.st_size)) {
      best = i;
      *published = epoch;
    }
  }
  return best;
}

static int commit_slot(const char *self, long long published) {
  long long previous_epoch;
  int current = boot_slot(&previous_epoch);
  if (published <= previous_epoch) return -1;
  /* Also preserve the running slot if it was opened directly in hbmenu. */
  for (int i = 0; i < 2; i++) {
    if (!strcmp(self, slot_nro[i])) {
      if (current >= 0 && current != i) return -1;
      current = i;
    }
  }
  int next = current == 0 ? 1 : 0;
  if ((exists(slot_stamp[next]) && remove(slot_stamp[next]) != 0) ||
      (exists(slot_nro[next]) && remove(slot_nro[next]) != 0) ||
      rename(DL_PATH, slot_nro[next]) != 0)
    return -1;

  FILE *stamp = fopen(SLOT_PENDING, "w");
  if (!stamp) return -1;
  int ok = fprintf(stamp, "%lld\n", published) > 0 && fflush(stamp) == 0 && fsync(fileno(stamp)) == 0;
  if (fclose(stamp) != 0) ok = 0;
  if (ok && rename(SLOT_PENDING, slot_stamp[next]) == 0) return next;
  remove(SLOT_PENDING);
  return -1;
}

static void wait_plus(void) {
  { extern void log_flush(void); log_flush(); } /* the result is in log.txt even if the console is switched off here */
  printf("\n  Press + to continue.\n");
  consoleUpdate(NULL);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    svcSleepThread(16000000ULL);
  }
}

/* Returns 1 when the new version was installed and will be started instead. */
static int download_and_install(const Release *r, const char *self) {
  screen("Updating Six Guns");
  if (!net_up()) { printf("  No internet connection. Your current version is unchanged.\n"); wait_plus(); return 0; }
  printf("  %s\n\n  Press B to cancel.\n\n", r->tag);
  consoleUpdate(NULL);
  remove(DL_PATH);
  Dl d = {0};
  d.total = r->size > 0 ? (u64)r->size : 0;
  d.f = fopen(DL_PATH, "wb");
  if (!d.f) { printf("  Could not write to the SD card.\n"); wait_plus(); return 0; }
  setvbuf(d.f, NULL, _IOFBF, 256 * 1024);
  sha256ContextCreate(&d.sha);
  /* A stalled transfer (flaky Wi-Fi; in Ryubing the last TLS record of a response
   * can stay buffered unseen) is resumed with a Range request from where it stopped. */
  CURLcode rc = CURLE_FAILED_INIT;
  for (int attempt = 0; attempt < 6; attempt++) {
    CURL *c = new_curl(r->url);
    if (!c) break;
    u64 before = d.got;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dl_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &d);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 8L);
    if (d.got) curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)d.got);
    rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (before && code != 206 && d.got > before) { rc = CURLE_RANGE_ERROR; break; } /* resume ignored: data would be appended twice */
    if (d.cancelled || d.failed) break;
    if (rc == CURLE_OK && (!d.total || d.got >= d.total)) break;
    if (d.total && d.got >= d.total) { rc = CURLE_OK; break; }
    if (rc != CURLE_OPERATION_TIMEDOUT && rc != CURLE_PARTIAL_FILE && rc != CURLE_RECV_ERROR &&
        rc != CURLE_GOT_NOTHING && rc != CURLE_OK) break;
    ulog("[update] transfer stopped at %llu bytes (%s), resuming\n", (unsigned long long)d.got, curl_easy_strerror(rc));
  }
  int write_err = d.failed || fflush(d.f) != 0 || fsync(fileno(d.f)) != 0;
  if (fclose(d.f) != 0) write_err = 1;
  u8 h[32];
  char sha[65];
  sha256ContextGetHash(&d.sha, h);
  hex(h, sha);
  d.last_draw = 0;
  d.finished = 1;
  dl_progress(&d, 0, 0, 0, 0); /* final state of the bar (draws are throttled) */
  printf("\n\n");
  if (d.cancelled) {
    remove(DL_PATH);
    ulog("[update] download cancelled\n");
    printf("  Update cancelled. Starting the current version.\n");
    consoleUpdate(NULL);
    svcSleepThread(1500000000ULL);
    return 0;
  }
  const char *why = NULL;
  if (rc != CURLE_OK) why = curl_easy_strerror(rc);
  else if (write_err) why = "could not write the file (SD card full?)";
  else if (r->size > 0 && d.got != (u64)r->size) why = "the download is incomplete";
  else if (strcmp(sha, r->digest + 7)) why = "the download does not match the release's SHA-256";
  else if (!nro_ok(DL_PATH, d.got)) why = "the download is not a valid NRO";
  if (why) {
    remove(DL_PATH);
    ulog("[update] failed: %s (got %llu bytes, sha256 %s)\n", why, (unsigned long long)d.got, sha);
    printf("  Update failed: %s.\n  Your current version is unchanged.\n", why);
    wait_plus();
    return 0;
  }
  int slot = commit_slot(self, iso_epoch(r->published));
  if (slot < 0) {
    remove(DL_PATH);
    ulog("[update] could not commit update slot\n");
    printf("  Could not install the update. Your current version is unchanged.\n");
    wait_plus();
    return 0;
  }
  remove(SKIP_PATH);
  ulog("[update] installed %s (sha256 %s) to %s\n", r->tag, sha, slot_nro[slot]);
  if (envHasNextLoad() && R_SUCCEEDED(envSetNextLoad(slot_nro[slot], slot_nro[slot]))) {
    printf("  Updated to %s. Restarting...\n", r->tag);
    consoleUpdate(NULL);
    svcSleepThread(1500000000ULL);
    return 1;
  }
  printf("  Updated to %s. Start %s from the homebrew menu.\n", r->tag, slot_nro[slot]);
  wait_plus();
  return 1;
}

/* ---------- entry ---------- */

static int read_marker(const char *path, char *out, size_t n) {
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  if (!fgets(out, n, f)) out[0] = 0;
  out[strcspn(out, "\r\n")] = 0;
  fclose(f);
  return 1;
}

static int net_ready;

/* Sockets + curl, set up only when the network is really used. */
static int net_up(void) {
  if (net_ready) return net_ready > 0;
  net_ready = -1;
  NifmInternetConnectionStatus st;
  if (R_FAILED(nifmInitialize(NifmServiceType_User))) return 0;
  Result nrc = nifmGetInternetConnectionStatus(NULL, NULL, &st);
  nifmExit();
  if (R_FAILED(nrc) || st != NifmInternetConnectionStatus_Connected) { ulog("[update] offline, no check\n"); return 0; }
  if (R_FAILED(socketInitializeDefault())) return 0;
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { socketExit(); return 0; }
  net_ready = 1;
  return 1;
}

static void net_down(void) {
  if (net_ready > 0) { curl_global_cleanup(); socketExit(); }
  net_ready = 0;
}

/* update_check.txt: time of the last online check, then the release found
 * (tag, published, digest, url, size) or "none". */
static int cache_read(Release *r, long long *checked) {
  FILE *f = fopen(CACHE_PATH, "r");
  if (!f) return -1;
  char line[600];
  int n = 0, ok = 0;
  *checked = 0;
  while (fgets(line, sizeof(line), f) && n < 6) {
    line[strcspn(line, "\r\n")] = 0;
    switch (n++) {
      case 0: *checked = strtoll(line, NULL, 10); break;
      case 1: if (!strcmp(line, "none")) { fclose(f); return 0; } snprintf(r->tag, sizeof(r->tag), "%.63s", line); break;
      case 2: snprintf(r->published, sizeof(r->published), "%.31s", line); break;
      case 3: snprintf(r->digest, sizeof(r->digest), "%.79s", line); break;
      case 4: snprintf(r->url, sizeof(r->url), "%.511s", line); break;
      case 5: r->size = strtoll(line, NULL, 10); ok = 1; break;
    }
  }
  fclose(f);
  return ok && !strncmp(r->digest, "sha256:", 7) && strlen(r->digest) == 71 && !strncmp(r->url, "https://", 8) ? 1 : -1;
}

static void cache_write(const Release *r, long long now) {
  FILE *f = fopen(CACHE_PATH, "w");
  if (!f) return;
  if (r) fprintf(f, "%lld\n%s\n%s\n%s\n%s\n%lld\n", now, r->tag, r->published, r->digest, r->url, r->size);
  else fprintf(f, "%lld\nnone\n", now);
  fclose(f);
}

/* 1: release in r, 0: no release, -1: could not find out. */
static int fetch_release(Release *r) {
  if (!net_up()) return -1;
  Buf b = {0};
  CURL *c = new_curl(RELEASES_URL);
  if (!c) return -1;
  struct curl_slist *hdr = curl_slist_append(NULL, "Accept: application/vnd.github+json");
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buf_write);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 4L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
  u64 t0 = armGetSystemTick();
  CURLcode rc = curl_easy_perform(c);
  curl_easy_cleanup(c);
  curl_slist_free_all(hdr);
  ulog("[update] release check: %s, %zu bytes, %llu ms\n", curl_easy_strerror(rc), b.len,
       (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000));
  int res = rc == CURLE_OK && b.data ? pick_release(b.data, r) : -1;
  free(b.data);
  return res;
}

static int check(const char *self) {
  Release r;
  long long checked = 0, now = (long long)time(NULL);
  int have = cache_read(&r, &checked);
  if (have < 0 || now < checked || now - checked >= CHECK_EVERY || exists(PORT_ROOT "/update_check_now")) {
    Release fresh;
    int got = fetch_release(&fresh);
    remove(PORT_ROOT "/update_check_now");
    if (got >= 0) { cache_write(got ? &fresh : NULL, now); have = got; if (got) r = fresh; }
  } else ulog("[update] last check %lld s ago, using it\n", now - checked);
  if (have <= 0) return 0;

  /* Cheap checks first; the running nro (8 MB) is hashed only when an offer is possible. */
  ulog("[update] newest release %s (%s) nro %.23s..., built %lld\n", r.tag, r.published, r.digest, (long long)BUILD_EPOCH);
  long long pub = iso_epoch(r.published);
  long long installed;
  boot_slot(&installed);
  if (pub <= installed) return 0;
  if (BUILD_EPOCH && pub <= (long long)BUILD_EPOCH) { ulog("[update] this build is newer than the release, no offer\n"); return 0; }
  char skip[96];
  if (read_marker(SKIP_PATH, skip, sizeof(skip)) && !strcmp(skip, r.digest)) { ulog("[update] %s skipped by the player\n", r.tag); return 0; }
  char own[65];
  if (!sha256_path(self, own)) return 0;
  if (!strcmp(own, r.digest + 7)) { ulog("[update] up to date (running %.16s...)\n", own); return 0; }
  ulog("[update] running %.16s..., offering %s\n", own, r.tag);

  console_open();
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  screen("Six Guns update available");
  printf("  A new version of the Six Guns port is out: %s\n", r.tag);
  printf("  (%lld KB, from github.com/jorgechavezxyz/SixGuns-NX).\n\n", r.size / 1024);
  printf("  Your saves and settings stay as they are. In LAN matches every\n");
  printf("  player needs the same version.\n\n");
  printf("    A  Update now\n    B  Not now\n    X  Skip this version\n");
  consoleUpdate(NULL);
  u64 btn = 0;
  while (appletMainLoop()) {
    if (auto_accept) { btn = HidNpadButton_A; break; }
    padUpdate(&pad);
    btn = padGetButtonsDown(&pad) & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X);
    if (btn) break;
    svcSleepThread(16000000ULL);
  }
  if (btn & HidNpadButton_A) return download_and_install(&r, self);
  if (btn & HidNpadButton_X) {
    FILE *f = fopen(SKIP_PATH, "w");
    if (f) { fprintf(f, "%s\n%s\n", r.digest, r.tag); fclose(f); }
    ulog("[update] player skipped %s\n", r.tag);
  } else ulog("[update] player chose not now\n");
  return 0;
}

/* Returns nonzero when an update was installed: main should exit so the new
 * version starts (hbloader next-load) instead of continuing. */
int updater_run(const char *argv0) {
  remove(DL_PATH); /* an interrupted download */
  if (exists(PORT_ROOT "/emulator_mode")) {
    char m[32];
    if (!read_marker(PORT_ROOT "/update_check_in_emulator", m, sizeof(m))) return 0;
    auto_accept = !strcmp(m, "accept");
  }
  char self[256];
  snprintf(self, sizeof(self), "%s", argv0 && !strncmp(argv0, "sdmc:/", 6) && exists(argv0) ? argv0 : BOOT_PATH);
  if (!exists(self)) return 0;

  if (!strcmp(self, BOOT_PATH) && envHasNextLoad()) {
    long long published;
    int slot = boot_slot(&published);
    if (slot >= 0 && published > (long long)BUILD_EPOCH &&
        R_SUCCEEDED(envSetNextLoad(slot_nro[slot], slot_nro[slot])))
      return 1;
  }
  if (exists(PORT_ROOT "/no_update_check")) return 0;

  int r = check(self);
  net_down();
  console_close();
  return r;
}

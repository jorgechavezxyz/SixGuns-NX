/* Stand-ins for Android NDK APIs: assets, native window, sensors, looper,
 * logging, OpenSL ES. */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "util.h"

/* ---------------------------------------------------------------- AAsset
 * The APK's assets/ folder is copied to ASSETS_ROOT on the SD card. */

typedef struct {
  FILE *f;
  long len;
  void *buf;
} AAsset;

typedef struct {
  DIR *d;
  char name[256];
} AAssetDir;

static int fake_asset_manager;

void *AAssetManager_fromJava(void *env, void *obj) { return &fake_asset_manager; }

AAsset *AAssetManager_open(void *mgr, const char *name, int mode) {
  char path[1024];
  snprintf(path, sizeof(path), ASSETS_ROOT "/%s", name);
  FILE *f = fopen(path, "rb");
  if (!f) {
    debugPrintf("[asset] missing: %s\n", name);
    return NULL;
  }
  AAsset *a = calloc(1, sizeof(*a));
  if (!a) { fclose(f); return NULL; }
  a->f = f;
  fseek(f, 0, SEEK_END);
  a->len = ftell(f);
  fseek(f, 0, SEEK_SET);
  return a;
}

int AAsset_read(AAsset *a, void *buf, size_t n) { return fread(buf, 1, n, a->f); }

long AAsset_seek(AAsset *a, long off, int whence) {
  if (fseek(a->f, off, whence) != 0) return -1;
  return ftell(a->f);
}

long AAsset_getLength(AAsset *a) { return a->len; }

const void *AAsset_getBuffer(AAsset *a) {
  if (!a->buf) {
    a->buf = malloc(a->len > 0 ? a->len : 1);
    if (!a->buf) return NULL;
    long pos = ftell(a->f);
    fseek(a->f, 0, SEEK_SET);
    fread(a->buf, 1, a->len, a->f);
    fseek(a->f, pos, SEEK_SET);
  }
  return a->buf;
}

void AAsset_close(AAsset *a) {
  fclose(a->f);
  free(a->buf);
  free(a);
}

AAssetDir *AAssetManager_openDir(void *mgr, const char *dir) {
  char path[1024];
  snprintf(path, sizeof(path), ASSETS_ROOT "/%s", dir);
  DIR *d = opendir(path);
  if (!d) return NULL;
  AAssetDir *ad = calloc(1, sizeof(*ad));
  if (!ad) { closedir(d); return NULL; }
  ad->d = d;
  return ad;
}

const char *AAssetDir_getNextFileName(AAssetDir *ad) {
  struct dirent *e;
  while ((e = readdir(ad->d))) {
    if (e->d_type == DT_DIR) continue; /* Android only lists files here */
    strncpy(ad->name, e->d_name, sizeof(ad->name) - 1);
    return ad->name;
  }
  return NULL;
}

void AAssetDir_rewind(AAssetDir *ad) { rewinddir(ad->d); }
void AAssetDir_close(AAssetDir *ad) { closedir(ad->d); free(ad); }

/* ---------------------------------------------------------------- window, looper, sensors */

static int fake_window, fake_looper;
void *ANativeWindow_fromSurface(void *env, void *surface) { return &fake_window; }
void ANativeWindow_release(void *w) {}
void *ALooper_forThread(void) { return &fake_looper; }
void *ALooper_prepare(int opts) { return &fake_looper; }

/* No sensors: the game falls back to touch/gamepad aiming. */
void *ASensorManager_getInstance(void) { return NULL; }
void *ASensorManager_getDefaultSensor(void *m, int type) { return NULL; }
void *ASensorManager_createEventQueue(void *m, void *l, int id, void *cb, void *d) { return NULL; }
int ASensorEventQueue_enableSensor(void *q, void *s) { return -1; }
int ASensorEventQueue_disableSensor(void *q, void *s) { return -1; }
int ASensorEventQueue_setEventRate(void *q, void *s, int us) { return -1; }
long ASensorEventQueue_getEvents(void *q, void *ev, size_t n) { return 0; }

/* ---------------------------------------------------------------- log */

static const char prio_chars[] = "??VDIWEF";

/* Android priorities: 2 verbose, 3 debug, 4 info, 5 warn, 6 error. Verbose and
 * debug are dropped unless PORT_ROOT/verbose_log exists (some engine code logs
 * every frame at debug level). */
static int log_verbose = -1;

int __android_log_write(int prio, const char *tag, const char *msg) {
  if (log_verbose < 0) {
    FILE *f = fopen(PORT_ROOT "/verbose_log", "r");
    log_verbose = f != NULL;
    if (f) fclose(f);
  }
  if (prio <= 3 && !log_verbose) return 0;
  debugPrintf("[%c/%s] %s\n", prio_chars[prio & 7], tag ? tag : "", msg ? msg : "");
  return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list va) {
  if (prio <= 3 && log_verbose == 0) return 0; /* skip formatting dropped messages */
  char buf[2048];
  vsnprintf(buf, sizeof(buf), fmt, va);
  return __android_log_write(prio, tag, buf);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int r = __android_log_vprint(prio, tag, fmt, va);
  va_end(va);
  return r;
}

/* ---------------------------------------------------------------- OpenSL ES
 * Not implemented yet. The engine also has an AudioTrack (Java) path, which
 * jni.c routes to SDL audio. If the log shows the game insisting on OpenSL,
 * this is the next piece to write. */

static int sl_iid_dummy[4];
void *SL_IID_ENGINE = &sl_iid_dummy[0];
void *SL_IID_BUFFERQUEUE = &sl_iid_dummy[1];
void *SL_IID_PLAY = &sl_iid_dummy[2];

int slCreateEngine(void **engine, unsigned n, const void *opts, unsigned ni, const void *ids, const void *req) {
  debugPrintf("[sl] slCreateEngine called - OpenSL ES not implemented, returning FEATURE_UNSUPPORTED\n");
  if (engine) *engine = NULL;
  return 12; /* SL_RESULT_FEATURE_UNSUPPORTED */
}

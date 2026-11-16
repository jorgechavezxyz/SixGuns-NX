/* OBB directory cache: makes ZipFileSystem::LoadFileSystem fast on the Switch's SD card.
 *
 * At start-up the engine indexes each OBB (main 757 MB, patch 17 MB) by walking its zip *local*
 * headers from the front: read a 30-byte header and the name, fseek over the file's data, repeat
 * (pig::stream::ZipFileSystem::LoadFileSystem). Every fseek makes newlib refill its FILE buffer
 * with a fresh small read, i.e. one SD round trip per archived file: ~6 s of a hardware start-up
 * (profile 2026-11-13: 51% of the start-up samples in fsdev_read <- __srefill_r <- _fseeko_r <-
 * LoadFileSystem).
 *
 * Read-only fopen()s of *.obb now return a FILE backed by this module (fopencookie). Reads come
 * from the port's kept-open handle of the archive at absolute offsets. On the first run every
 * range read through such a FILE is recorded and written to cache/<obb name>.idx when the FILE is
 * closed; later runs load that file (checked against the archive's size and its first and last
 * 4 KB) and answer reads entirely inside recorded ranges from memory, i.e. the whole directory walk
 * costs one file read. Anything not covered is read from the archive as before, so a stale or
 * partial index can only cost speed. Marker `no_obb_index` keeps plain fopen; deleting
 * cache/*.idx rebuilds the index. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include "config.h"
#include "util.h"

typedef struct { u64 off; u32 len; u32 data; } region_t; /* data = offset into blob */

typedef struct {
  char path[256], idx_path[300];
  FsFile f;
  int have_file;
  s64 size;
  s64 pos;
  /* loaded index */
  region_t *reg; size_t nreg; uint8_t *blob; size_t blob_len; size_t cursor;
  int loaded;
  /* recording (first run) */
  region_t *rec; size_t nrec, caprec; uint8_t *rblob; size_t rblob_len, rblob_cap;
  int recording;
  unsigned long hits, misses;
  u8 fingerprint[8192];
} obbf_t;

static const char MAGIC[8] = "SGOBIDX1";
static obbf_t *live[4];
static Mutex live_mtx;

static int fp_read(obbf_t *o) {
  u64 got = 0;
  if (o->size < 8192) return 0;
  if (R_FAILED(fsFileRead(&o->f, 0, o->fingerprint, 4096, FsReadOption_None, &got)) || got != 4096) return 0;
  if (R_FAILED(fsFileRead(&o->f, o->size - 4096, o->fingerprint + 4096, 4096, FsReadOption_None, &got)) || got != 4096) return 0;
  return 1;
}

static void load_index(obbf_t *o) {
  FILE *f = fopen(o->idx_path, "rb");
  if (!f) return;
  char magic[8]; s64 size; u8 fp[8192]; u64 n, blen;
  int ok = fread(magic, 8, 1, f) == 1 && !memcmp(magic, MAGIC, 8) && fread(&size, 8, 1, f) == 1 && size == o->size &&
           fread(fp, sizeof(fp), 1, f) == 1 && !memcmp(fp, o->fingerprint, sizeof(fp)) && fread(&n, 8, 1, f) == 1 &&
           fread(&blen, 8, 1, f) == 1 && n < (1u << 20) && blen < (64u << 20);
  if (ok) {
    o->reg = malloc(n * sizeof(region_t));
    o->blob = malloc(blen ? blen : 1);
    ok = o->reg && o->blob && fread(o->reg, sizeof(region_t), n, f) == n && fread(o->blob, 1, blen, f) == blen;
    if (ok) { o->nreg = n; o->blob_len = blen; o->loaded = 1; }
    else { free(o->reg); free(o->blob); o->reg = NULL; o->blob = NULL; }
  }
  fclose(f);
  debugPrintf("[obb_index] %s: %s\n", strrchr(o->path, '/') + 1,
              ok ? "directory index loaded" : "index out of date, rebuilding");
}

/* Region containing [off, off+len) -> pointer, else NULL. Reads come in ascending order, so a
 * cursor makes the common case O(1); otherwise binary search. */
static const uint8_t *lookup(obbf_t *o, u64 off, size_t len) {
  if (!o->loaded || !o->nreg) return NULL;
  size_t i = o->cursor;
  if (i >= o->nreg || o->reg[i].off > off) {
    size_t lo = 0, hi = o->nreg;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (o->reg[m].off <= off) lo = m + 1; else hi = m; }
    if (!lo) return NULL;
    i = lo - 1;
  } else {
    while (i + 1 < o->nreg && o->reg[i + 1].off <= off) i++;
  }
  o->cursor = i;
  region_t *r = &o->reg[i];
  if (off < r->off || off + len > r->off + r->len) return NULL;
  return o->blob + r->data + (off - r->off);
}

/* Appends under live_mtx: obb_index_frame (main loop) saves the index under the same lock while a
 * loading thread may still be reading, and an append can realloc rec/rblob in the middle of the save. */
static void record_locked(obbf_t *o, u64 off, const void *buf, size_t len);
static void record(obbf_t *o, u64 off, const void *buf, size_t len) {
  if (!o->recording || !len) return;
  mutexLock(&live_mtx);
  record_locked(o, off, buf, len);
  mutexUnlock(&live_mtx);
}
static void record_locked(obbf_t *o, u64 off, const void *buf, size_t len) {
  if (!o->recording || !len) return;
  /* merge with the previous region when contiguous */
  if (o->nrec && o->rec[o->nrec - 1].off + o->rec[o->nrec - 1].len == off &&
      o->rec[o->nrec - 1].data + o->rec[o->nrec - 1].len == o->rblob_len) {
    o->rec[o->nrec - 1].len += len;
  } else {
    if (o->nrec == o->caprec) {
      size_t c = o->caprec ? o->caprec * 2 : 1024;
      region_t *nr = realloc(o->rec, c * sizeof(region_t));
      if (!nr) { o->recording = 0; return; }
      o->rec = nr; o->caprec = c;
    }
    o->rec[o->nrec++] = (region_t){ off, (u32)len, (u32)o->rblob_len };
  }
  if (o->rblob_len + len > o->rblob_cap) {
    size_t c = o->rblob_cap ? o->rblob_cap * 2 : (1 << 20);
    while (c < o->rblob_len + len) c *= 2;
    if (c > (64u << 20)) { o->recording = 0; return; }
    uint8_t *nb = realloc(o->rblob, c);
    if (!nb) { o->recording = 0; return; }
    o->rblob = nb; o->rblob_cap = c;
  }
  memcpy(o->rblob + o->rblob_len, buf, len);
  o->rblob_len += len;
}

static ssize_t c_read(void *c, char *buf, size_t n) {
  obbf_t *o = c;
  if (o->pos >= o->size) return 0;
  if ((s64)n > o->size - o->pos) n = (size_t)(o->size - o->pos);
  const uint8_t *hit = lookup(o, (u64)o->pos, n);
  if (hit) { memcpy(buf, hit, n); o->pos += n; o->hits++; return (ssize_t)n; }
  u64 got = 0;
  if (R_FAILED(fsFileRead(&o->f, o->pos, buf, n, FsReadOption_None, &got))) { errno = EIO; return -1; }
  record(o, (u64)o->pos, buf, (size_t)got);
  o->pos += (s64)got;
  o->misses++;
  return (ssize_t)got;
}

static int c_seek(void *c, off_t *off, int whence) {
  obbf_t *o = c;
  s64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? o->pos : o->size;
  s64 np = base + *off;
  if (np < 0) { errno = EINVAL; return -1; }
  o->pos = np;
  *off = np;
  return 0;
}

static void save_index(obbf_t *o) {
  if (!o->recording || o->nrec < 64) return;
  char tmp[320];
  snprintf(tmp, sizeof(tmp), "%s.tmp", o->idx_path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return;
  u64 n = o->nrec, blen = o->rblob_len;
  int ok = fwrite(MAGIC, 8, 1, f) == 1 && fwrite(&o->size, 8, 1, f) == 1 &&
           fwrite(o->fingerprint, sizeof(o->fingerprint), 1, f) == 1 && fwrite(&n, 8, 1, f) == 1 &&
           fwrite(&blen, 8, 1, f) == 1 && fwrite(o->rec, sizeof(region_t), n, f) == n &&
           fwrite(o->rblob, 1, blen, f) == blen;
  ok &= fclose(f) == 0;
  if (ok) { remove(o->idx_path); ok = rename(tmp, o->idx_path) == 0; }
  else remove(tmp);
  debugPrintf("[obb_index] %s: %s (%zu ranges, %zu KB)\n", strrchr(o->path, '/') + 1,
              ok ? "directory index saved" : "could not save index", o->nrec, o->rblob_len >> 10);
}

static int c_close(void *c) {
  obbf_t *o = c;
  mutexLock(&live_mtx);
  for (int i = 0; i < 4; i++) if (live[i] == o) live[i] = NULL;
  mutexUnlock(&live_mtx);
  debugPrintf("[obb_index] %s closed: %lu reads from the index, %lu from the archive\n", strrchr(o->path, '/') + 1,
              o->hits, o->misses);
  mutexLock(&live_mtx); save_index(o); mutexUnlock(&live_mtx);
  /* An index that no longer covers the walk (e.g. made with another buffer size) is dropped so
   * the next start rebuilds it. */
  if (o->loaded && o->misses > 16 && o->misses * 8 > o->hits) {
    remove(o->idx_path);
    debugPrintf("[obb_index] index did not match the reads, removed (rebuilt next start)\n");
  }
  if (o->have_file) fsFileClose(&o->f);
  free(o->reg); free(o->blob); free(o->rec); free(o->rblob);
  free(o);
  return 0;
}

static int enabled = -1;

/* Called every few seconds from the main loop: saves (once) the index of an archive whose
 * directory walk has finished (recording, enough ranges, no read for ~2 s). */
void obb_index_frame(void) {
  static u64 last_reads[4], quiet[4];
  mutexLock(&live_mtx);
  for (int i = 0; i < 4; i++) {
    obbf_t *o = live[i];
    if (!o || !o->recording) continue;
    u64 reads = o->misses;
    if (reads != last_reads[i]) { last_reads[i] = reads; quiet[i] = 0; continue; }
    if (++quiet[i] >= 2 && o->nrec >= 64) { save_index(o); o->recording = 0; }
  }
  mutexUnlock(&live_mtx);
}

FILE *obb_index_fopen(const char *path, const char *mode) {
  if (enabled < 0) {
    FILE *m = fopen(PORT_ROOT "/no_obb_index", "r");
    enabled = m == NULL;
    if (m) fclose(m);
  }
  size_t n = strlen(path);
  if (!enabled || n < 5 || strcmp(path + n - 4, ".obb") || strncmp(path, "sdmc:/", 6) || n >= 256 ||
      mode[0] != 'r' || strchr(mode, '+'))
    return NULL;
  obbf_t *o = calloc(1, sizeof(*o));
  if (!o) return NULL;
  snprintf(o->path, sizeof(o->path), "%s", path);
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (!fs || R_FAILED(fsFsOpenFile(fs, path + 5, FsOpenMode_Read, &o->f))) { free(o); return NULL; }
  o->have_file = 1;
  if (R_FAILED(fsFileGetSize(&o->f, &o->size)) || !fp_read(o)) { fsFileClose(&o->f); free(o); return NULL; }
  mkdir(CACHE_ROOT, 0777);
  snprintf(o->idx_path, sizeof(o->idx_path), "%s/%s.idx", CACHE_ROOT, strrchr(path, '/') + 1);
  load_index(o);
  o->recording = !o->loaded;
  if (o->recording) {
    mutexLock(&live_mtx);
    for (int i = 0; i < 4; i++) if (!live[i]) { live[i] = o; break; }
    mutexUnlock(&live_mtx);
  }
  cookie_io_functions_t io = { .read = c_read, .seek = c_seek, .close = c_close };
  FILE *f = fopencookie(o, mode, io);
  if (!f) { c_close(o); return NULL; }
  /* A zip local header plus its name is ~100 bytes; newlib's default 1 KB refill per fseek
   * made the index 8 MB. 256 bytes keeps most headers in one read and the index ~2 MB. */
  setvbuf(f, NULL, _IOFBF, 256);
  return f;
}

/* Memory-backed files for the game's saves and settings.
 *
 * The engine opens the same file several times at once (a read handle stays
 * open while it reopens the file to write). Android allows that; the Switch's
 * file system refuses the second open, so on hardware every save write failed
 * (seen 2026-11-10: save_a, save_td, save_settings never updated).
 *
 * Here each fopen gets its own in-memory copy instead: the file is read whole
 * at open and closed right away, and a stream that was written is put back on
 * disk when it is closed, or by memfile_commit_all (every couple of seconds for streams idle that
 * long, at once on focus loss/exit). fflush alone does not commit (the cookie has no flush hook).
 * The disk file is therefore never held open, so opens can't conflict.
 * Writes go to "<name>.tmp" first and are then renamed over the original, so
 * a power cut mid-save leaves either the old or the new file (and an open
 * falls back to the .tmp if the original is missing). */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include "config.h"
#include "util.h"

#define MAX_FILE (16u << 20) /* larger files use plain stdio */

typedef struct memfile {
  Mutex mtx;
  char path[512];
  char *data;
  size_t len, cap, pos;
  int readable, writable, append;
  int dirty;
  u64 dirty_since;
  FILE *fp; /* the stdio stream wrapping this file (memfile_is_stream) */
  struct memfile *prev,*next;
} memfile;

static Mutex list_mtx;
static memfile *open_files;
static unsigned open_count,open_peak;
static void unlink_open(memfile *m) {
  if(m->prev) m->prev->next=m->next;else open_files=m->next;
  if(m->next) m->next->prev=m->prev;
  open_count--;
}

static int grow(memfile *m, size_t need) {
  if (need > MAX_FILE + 1) { errno = EFBIG; return -1; }
  if (need <= m->cap) return 0;
  size_t cap = m->cap ? m->cap : 4096;
  while (cap < need) cap = cap > (MAX_FILE+1)/2 ? MAX_FILE+1:cap*2;
  char *d = realloc(m->data, cap);
  if (!d) return -1;
  m->data = d;
  m->cap = cap;
  return 0;
}

/* ---- background writer: SD writes never happen on the game's threads ---- */
typedef struct job { char path[512]; char *data; size_t len; u64 tick; int urgent; struct job *next; } job;
static Mutex q_mtx;
/* Readers must not observe the short original -> backup -> replacement rename gap. */
static Mutex disk_mtx;
static CondVar q_cv;
static job *q_head, *q_inflight, *q_failed; /* q_failed: could not be persisted; still served to readers */
static int q_urgent; /* exit/crash: write without settling */
static Thread w_thread;
static int w_started;
static unsigned write_errors;
static int writes_disabled;
/* Used only by the emulator-only self-test below. Never enabled by a gameplay path. */
static int test_snapshot_oom, test_pending_oom, test_install_fail;
static int test_pause_install,test_in_rename_gap;

static int write_disk_locked(const job *j) {
  char tmp[520], bak[520];
  snprintf(tmp, sizeof(tmp), "%s.tmp", j->path);
  snprintf(bak, sizeof(bak), "%s.bak", j->path);
  FILE *f = fopen(tmp, "wb");
  if (!f) { debugPrintf("[memfile] cannot write %s (errno %d)\n", tmp, errno); return -1; }
  size_t w = j->len ? fwrite(j->data, 1, j->len, f) : 0;
  int bad = (w != j->len) | (fclose(f) != 0);
  if (bad) { debugPrintf("[memfile] short write to %s\n", tmp); remove(tmp); errno=EIO; return -1; }
  /* Horizon cannot rename over an existing file. Preserve it until installation succeeds. */
  struct stat previous;
  if (stat(j->path,&previous)) {
    if (errno!=ENOENT) { remove(tmp); return -1; } /* unknown state: keep .bak untouched */
    /* A previous interrupted install may have left only .bak. Restore it before replacing. */
    if(rename(bak,j->path) && errno!=ENOENT) return -1;
  }
  if (remove(bak) && errno!=ENOENT) return -1;
  int had_old=rename(j->path,bak)==0;
  if (!had_old && errno!=ENOENT) return -1;
  if(test_pause_install) {__atomic_store_n(&test_in_rename_gap,1,__ATOMIC_RELEASE);svcSleepThread(5000000);}
  int installed;
  if (test_install_fail) { test_install_fail=0; errno=EIO; installed=-1; }
  else installed=rename(tmp,j->path);
  if (installed) {
    int err=errno;
    if (had_old && rename(bak,j->path)) debugPrintf("[save] original retained in %s after install failure\n",bak);
    debugPrintf("[memfile] rename %s failed (errno %d); previous save preserved\n",tmp,err);
    errno=err; return -1;
  }
  /* The previous version stays as .bak (one generation): a crash or power loss after an
   * unexpected empty/partial install still leaves the last good save on the card. */
  debugPrintf("[save] wrote %s (%zu bytes)\n", strrchr(j->path, '/') + 1, j->len);
  return 0;
}
static int write_disk(const job *j) {
  mutexLock(&disk_mtx);int r=write_disk_locked(j);mutexUnlock(&disk_mtx);return r;
}

enum { SETTLE_MS = 200, EMPTY_SETTLE_MS = 2000 };
/* Game progress saves (save/save_..., also in subfolders): never legitimately empty. Telemetry queues such as
 * save/gv3/Resume.bin are emptied every session and must not wait or fall back to .bak. */
static int progress_save(const char *path) {
  if (strncmp(path, PORT_ROOT "/save/", strlen(PORT_ROOT) + 6)) return 0;
  const char *b = strrchr(path, '/');
  return b && !strncmp(b + 1, "save_", 5);
}
void memfile_disable_writes(const char *what);
static void writer_main(void *arg) {
  (void)arg;
  for (;;) {
    mutexLock(&q_mtx);
    while (!q_head) condvarWait(&q_cv, &q_mtx);
    /* Let each file settle from its own last update: the engine writes a save twice in a
     * row (empty, then full), and a newer write of the same file replaces the queued one.
     * An empty snapshot waits longer, so it is not installed over a good save just before
     * the full write arrives. Exit/crash drains (q_urgent) skip the wait. */
    job *j = q_head;
    u64 age_ms = armTicksToNs(armGetSystemTick() - j->tick) / 1000000;
    u64 need = __atomic_load_n(&q_urgent, __ATOMIC_ACQUIRE) || j->urgent ? 0
             : j->len || !progress_save(j->path) ? SETTLE_MS : EMPTY_SETTLE_MS;
    if (age_ms < need) {
      mutexUnlock(&q_mtx);
      u64 wait = need - age_ms;
      svcSleepThread((wait < 50 ? wait : 50) * 1000000ULL);
      continue;
    }
    q_head = j->next;
    q_inflight = j;
    mutexUnlock(&q_mtx);
    int result=write_disk(j);
    for(int retry=0;result && retry<2;retry++) { svcSleepThread(100000000); result=write_disk(j); }
    mutexLock(&q_mtx);
    q_inflight = NULL;
    if (result) {
      /* Keep serving the unsaved data to readers (an older disk copy must not come back),
       * stop further writes and end the session, as for a failed save open. */
      write_errors++;
      j->next = q_failed;
      q_failed = j;
      /* Install nothing queued behind it either: a mix of old and new files (save_common old,
       * save_a new) is worse than the consistent old set. They stay readable from q_failed. */
      while (q_head) { job *n = q_head; q_head = n->next; n->next = q_failed; q_failed = n; }
      condvarWakeAll(&q_cv);
      mutexUnlock(&q_mtx);
      extern volatile int port_quit_requested;
      memfile_disable_writes("save could not be written after 3 attempts; stopping to protect progress");
      debugPrintf("[save] ERROR: could not persist %s\n", j->path);
      port_quit_requested = 1;
      continue;
    }
    condvarWakeAll(&q_cv);
    mutexUnlock(&q_mtx);
    free(j->data);
    free(j);
  }
}

/* A synchronous (fallback) write failed: same policy as the writer thread's failure. */
static void inline_failed(const char *path) {
  extern volatile int port_quit_requested;
  memfile_disable_writes("save could not be written; stopping to protect progress");
  debugPrintf("[save] ERROR: could not persist %s\n", path);
  port_quit_requested = 1;
}
/* Queue a snapshot of the file; a queued write of the same file is replaced
 * (only the newest content matters). Returns -1 on failure, including inline writes. */
static int enqueue(const char *path, const char *data, size_t len) {
  if(__atomic_load_n(&writes_disabled,__ATOMIC_ACQUIRE)) {errno=EIO;return -1;}
  job *j = test_snapshot_oom ? NULL:calloc(1, sizeof(*j));
  char *copy = test_snapshot_oom ? NULL:malloc(len ? len : 1);
  test_snapshot_oom=0;
  if (!j || !copy) {
    free(j); free(copy);
    /* No allocation is needed for an emergency synchronous write. Serialize it after older jobs. */
    job inline_job={0}; snprintf(inline_job.path,sizeof(inline_job.path),"%s",path);
    inline_job.data=(char *)data; inline_job.len=len;
    mutexLock(&q_mtx);
    while(q_head || q_inflight) condvarWait(&q_cv,&q_mtx);
    if(__atomic_load_n(&writes_disabled,__ATOMIC_ACQUIRE)) {mutexUnlock(&q_mtx);errno=EIO;return -1;}
    debugPrintf("[save] snapshot allocation failed; writing synchronously\n");
    int r=write_disk(&inline_job);
    mutexUnlock(&q_mtx);
    if(r) inline_failed(path);
    return r;
  }
  snprintf(j->path, sizeof(j->path), "%s", path);
  memcpy(copy, data, len);
  j->data = copy;
  j->len = len;
  j->tick = armGetSystemTick();
  mutexLock(&q_mtx);
  if(__atomic_load_n(&writes_disabled,__ATOMIC_ACQUIRE)) {
    mutexUnlock(&q_mtx);free(j->data);free(j);errno=EIO;return -1;
  }
  if (!w_started) {
    Result rc=threadCreate(&w_thread,writer_main,NULL,NULL,0x8000,0x2c,2);
    if (R_SUCCEEDED(rc)) { rc=threadStart(&w_thread); if(R_FAILED(rc)) threadClose(&w_thread); }
    w_started=R_SUCCEEDED(rc);
  }
  if (!w_started) { int r=write_disk(j); mutexUnlock(&q_mtx); free(j->data); free(j); if(r) inline_failed(path); return r; }
  job **pp = &q_head;
  for (; *pp; pp = &(*pp)->next)
    if (!strcmp((*pp)->path, path)) { free((*pp)->data); (*pp)->data = j->data; (*pp)->len = len; (*pp)->tick = j->tick; free(j); j = NULL; break; }
  if (j) *pp = j;
  condvarWakeAll(&q_cv);
  mutexUnlock(&q_mtx);
  return 0;
}

/* The newest content of a file that is queued or being written, if any. */
static int pending_copy(const char *path, char **data, size_t *len) {
  int found = 0;
  mutexLock(&q_mtx);
  const job *hit = NULL;
  for (job *j = q_head; j; j = j->next) if (!strcmp(j->path, path)) hit = j;
  if (!hit && q_inflight && !strcmp(q_inflight->path, path)) hit = q_inflight;
  for (job *j = q_failed; !hit && j; j = j->next) if (!strcmp(j->path, path)) hit = j;
  if (hit) {
    *data = test_pending_oom ? NULL:malloc(hit->len + 1);
    test_pending_oom=0;
    if (*data) { memcpy(*data, hit->data, hit->len); *len = hit->len; found = 1; }
    else { errno=ENOMEM; found=-1; }
  }
  mutexUnlock(&q_mtx);
  return found;
}

/* Is a write of this file still queued? (stat/access check before trusting
 * "file not found".) */
int memfile_pending(const char *path) {
  int found=0;
  mutexLock(&q_mtx);
  for(job *j=q_head;j;j=j->next) if(!strcmp(j->path,path)) {found=1;break;}
  if(!found && q_inflight && !strcmp(q_inflight->path,path)) found=1;
  for(job *j=q_failed;!found && j;j=j->next) if(!strcmp(j->path,path)) found=1;
  mutexUnlock(&q_mtx); return found;
}
unsigned memfile_error_count(void) { mutexLock(&q_mtx); unsigned n=write_errors; mutexUnlock(&q_mtx); return n; }
void memfile_note_failure(const char *what) {
  mutexLock(&q_mtx);write_errors++;mutexUnlock(&q_mtx);
  debugPrintf("[save] ERROR: %s; previous disk contents retained\n",what);
}
void memfile_disable_writes(const char *what) {
  __atomic_store_n(&writes_disabled,1,__ATOMIC_RELEASE);memfile_note_failure(what);
}
void memfile_stats(void) {
  mutexLock(&list_mtx);size_t bytes=0;
  for(memfile *m=open_files;m;m=m->next) {mutexLock(&m->mtx);bytes+=m->cap;mutexUnlock(&m->mtx);}
  debugPrintf("[save] memory streams: %u open, peak %u, %zu KB buffers\n",open_count,open_peak,bytes/1024);
  mutexUnlock(&list_mtx);
}
int memfile_snapshot(const char *path,const char *data,size_t len) {
  if(strlen(path)>=sizeof(((job *)0)->path) || len>MAX_FILE || (!data && len)) {
    memfile_note_failure("invalid persistence snapshot");errno=EINVAL;return -1;
  }
  return enqueue(path,data ? data:"",len);
}

/* stat/access/remove/rename of a file with a queued write: install that file now, plus the jobs
 * queued before it (order kept), without forcing the rest of the queue past its settle time. */
void memfile_sync_path(const char *path) {
  if (!w_started) return;
  mutexLock(&q_mtx);
  for (;;) {
    job *hit = NULL;
    for (job *j = q_head; j; j = j->next) if (!strcmp(j->path, path)) { hit = j; break; }
    if (hit) for (job *j = q_head; j; j = j->next) { j->urgent = 1; if (j == hit) break; }
    if (!hit && !(q_inflight && !strcmp(q_inflight->path, path))) break;
    condvarWakeAll(&q_cv);
    condvarWait(&q_cv, &q_mtx);
  }
  mutexUnlock(&q_mtx);
}

/* Wait until every queued write is on disk (exit paths). */
void memfile_drain(void) {
  if (!w_started) return;
  __atomic_store_n(&q_urgent, 1, __ATOMIC_RELEASE); /* exit path: no settling */
  mutexLock(&q_mtx);
  condvarWakeAll(&q_cv);
  while (q_head || q_inflight) condvarWait(&q_cv, &q_mtx);
  __atomic_store_n(&q_urgent, 0, __ATOMIC_RELEASE); /* also used on focus loss: settle again afterwards */
  mutexUnlock(&q_mtx);
}

/* Crash handler: let the writer finish what is queued, without taking any lock (the
 * crashed thread may hold one), for at most max_ms. Not when the writer itself crashed. */
void memfile_emergency_drain(unsigned max_ms) {
  if (!w_started || threadGetCurHandle() == w_thread.handle) return;
  __atomic_store_n(&q_urgent, 1, __ATOMIC_RELEASE);
  for (unsigned t = 0; t < max_ms; t += 10) {
    if (!__atomic_load_n(&q_head, __ATOMIC_ACQUIRE) && !__atomic_load_n(&q_inflight, __ATOMIC_ACQUIRE)) return;
    svcSleepThread(10000000);
  }
}

/* Hand the buffer to the writer. Caller holds m->mtx. */
static int commit(memfile *m) {
  if (!m->dirty) return 0;
  if (enqueue(m->path, m->data ? m->data : "", m->len)) return -1;
  m->dirty = 0;
  return 0;
}

static ssize_t mf_read(void *c, char *buf, size_t n) {
  memfile *m = c;
  mutexLock(&m->mtx);
  size_t left = m->pos < m->len ? m->len - m->pos : 0;
  if (n > left) n = left;
  if(n) memcpy(buf, m->data + m->pos, n);
  m->pos += n;
  mutexUnlock(&m->mtx);
  return n;
}

static ssize_t mf_write(void *c, const char *buf, size_t n) {
  if(__atomic_load_n(&writes_disabled,__ATOMIC_ACQUIRE)) {errno=EIO;return -1;}
  memfile *m = c;
  mutexLock(&m->mtx);
  if (m->append) m->pos = m->len;
  if(!n) {mutexUnlock(&m->mtx);return 0;}
  if(m->pos>MAX_FILE || n>MAX_FILE-m->pos) {mutexUnlock(&m->mtx);errno=EFBIG;return -1;}
  if (grow(m, m->pos + n) < 0) { mutexUnlock(&m->mtx); errno = ENOMEM; return -1; }
  if (m->pos > m->len) memset(m->data + m->len, 0, m->pos - m->len);
  memcpy(m->data + m->pos, buf, n);
  m->pos += n;
  if (m->pos > m->len) m->len = m->pos;
  m->dirty_since = armGetSystemTick(); /* settle from the last write, not the first */
  m->dirty = 1;
  mutexUnlock(&m->mtx);
  return n;
}

static int mf_seek(void *c, off_t *off, int whence) {
  memfile *m = c;
  mutexLock(&m->mtx);
  if(whence!=SEEK_SET && whence!=SEEK_CUR && whence!=SEEK_END) {mutexUnlock(&m->mtx);errno=EINVAL;return -1;}
  long long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long long)m->pos : (long long)m->len;
  if(*off < -base || *off > (off_t)MAX_FILE-base) {mutexUnlock(&m->mtx);errno=EINVAL;return -1;}
  long long np = base + *off;
  m->pos = np;
  *off = np;
  mutexUnlock(&m->mtx);
  return 0;
}

static int mf_close(void *c) {
  memfile *m = c;
  mutexLock(&list_mtx);
  unlink_open(m);
  mutexUnlock(&list_mtx);
  mutexLock(&m->mtx);
  int r = commit(m);
  mutexUnlock(&m->mtx);
  free(m->data);
  free(m);
  return r;
}

static cookie_io_functions_t mf_io = { .read = mf_read, .write = mf_write, .seek = mf_seek, .close = mf_close };

/* Which files get this treatment: the game's writable data under the port
 * folder (saves, settings, caches), never the OBBs, APK or assets. */
int memfile_wanted(const char *path) {
  if (strncmp(path, PORT_ROOT "/", strlen(PORT_ROOT) + 1) != 0) return 0;
  const char *rel = path + strlen(PORT_ROOT) + 1;
  return !strncmp(rel, "save/", 5) || !strncmp(rel, "internal/", 9) || !strncmp(rel, "sdcard/", 7) ||
         !strncmp(rel, "cache/", 6);
}

static int load_disk(memfile *m, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return errno==ENOENT ? -1:-2;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0 || (size_t)sz > MAX_FILE) { fclose(f); errno = EFBIG; return -2; }
  if (grow(m, sz + 1) < 0) { fclose(f); errno = ENOMEM; return -2; }
  size_t got = sz ? fread(m->data, 1, sz, f) : 0;
  fclose(f);
  if (got != (size_t)sz) { errno = EIO; return -2; }
  m->len = sz;
  return 0;
}
/* A save file is never legitimately empty; if one is, read the previous version (.bak). */
static int load_disk_or_bak(memfile *m, const char *path) {
  int r = load_disk(m, path);
  if (!(r == 0 && m->len == 0)) return r; /* only an empty file: a missing one may be a deliberate delete */
  if (!progress_save(path)) return r;
  char bak[520];
  snprintf(bak, sizeof(bak), "%s.bak", path);
  memfile tmp = {0};
  if (load_disk(&tmp, bak) == 0 && tmp.len > 0) {
    debugPrintf("[save] %s is empty; using the previous version (%s, %zu bytes)\n", strrchr(path, '/') + 1,
                strrchr(bak, '/') + 1, tmp.len);
    free(m->data);
    m->data = tmp.data; m->cap = tmp.cap; m->len = tmp.len;
    return 0;
  }
  free(tmp.data);
  return r;
}
static int load(memfile *m, const char *path) {
  char *pd; size_t pl;
  int pending=pending_copy(path,&pd,&pl);
  if(pending<0) return -2; /* never substitute stale data after snapshot allocation failure */
  if(pending) {free(m->data);m->data=pd;m->cap=pl+1;m->len=pl;return 0;}
  mutexLock(&disk_mtx);int r=load_disk_or_bak(m,path);mutexUnlock(&disk_mtx);return r;
}

/* Returns a FILE, or NULL with errno set; *fallback = 1 means "use plain
 * fopen" (file too large). */
FILE *memfile_open(const char *path, const char *mode, int *fallback) {
  *fallback = 0;
  if(strlen(path)>=sizeof(((memfile *)0)->path)) {errno=ENAMETOOLONG;return NULL;}
  memfile *m = calloc(1, sizeof(*m));
  if (!m) { errno = ENOMEM; return NULL; }
  mutexInit(&m->mtx);
  snprintf(m->path, sizeof(m->path), "%s", path);
  int plus = strchr(mode, '+') != NULL;
  switch (mode[0]) {
    case 'r': m->readable = 1; m->writable = plus; break;
    case 'w': m->writable = 1; m->readable = plus; m->dirty = 1; m->dirty_since = armGetSystemTick(); break; /* truncate/create even if nothing is written */
    case 'a': m->writable = 1; m->readable = plus; m->append = 1; break;
    default: free(m); errno = EINVAL; return NULL;
  }
  if (mode[0] != 'w') {
    int r = load(m, path);
    if (r == -1) {
      char tmp[520];
      snprintf(tmp,sizeof(tmp),"%s.bak",path);
      r=load(m,tmp); /* interrupted replacement: prefer the preserved complete old save */
      if(!r) {debugPrintf("[save] recovered %s from its .bak\n",strrchr(path,'/')+1);enqueue(path,m->data?m->data:"",m->len);}
      if(r==-1) {
      snprintf(tmp, sizeof(tmp), "%s.tmp", path);
      r = load(m, tmp); /* an interrupted save left only the temp file */
      if (r == 0) { debugPrintf("[save] recovered %s from its .tmp\n", strrchr(path, '/') + 1); enqueue(path, m->data ? m->data : "", m->len); }
      }
    }
    if (r == -2) { free(m->data); free(m); *fallback = errno == EFBIG; return NULL; }
    if (r == -1) {
      if (mode[0] == 'r') { free(m); errno = ENOENT; return NULL; }
      m->dirty = 1; /* 'a' creates the file */
      m->dirty_since = armGetSystemTick();
    }
  }
  if (m->append) m->pos = m->len;

  mutexLock(&list_mtx);
  m->next=open_files;if(open_files) open_files->prev=m;open_files=m;
  open_count++;if(open_count>open_peak) open_peak=open_count;
  mutexUnlock(&list_mtx);

  const char *cm = m->readable && m->writable ? "r+" : m->writable ? "w" : "r";
  FILE *f = fopencookie(m, cm, mf_io);
  if (!f) {
    mutexLock(&list_mtx);unlink_open(m);mutexUnlock(&list_mtx);
    free(m->data); free(m);
    return NULL;
  }
  /* Unbuffered: every write reaches the memory copy at once, so a periodic
   * commit never puts a half-buffered save on disk. */
  setvbuf(f, NULL, _IONBF, 0);
  m->fp = f;
  return f;
}

/* Is this stdio stream one of ours? (b_setvbuf must not re-enable buffering on it.) */
int memfile_is_stream(FILE *f) {
  int found = 0;
  mutexLock(&list_mtx);
  for (memfile *m = open_files; m && !found; m = m->next) found = m->fp == f;
  mutexUnlock(&list_mtx);
  return found;
}

/* Put dirty files on disk. `min_age_ms` > 0 only commits files dirty for at
 * least that long (so a save in progress isn't written half-done). Note: data
 * still in the FILE's own buffer isn't seen; fflush/fclose push it here. */
void memfile_commit_all(unsigned min_age_ms) {
  u64 now = armGetSystemTick();
  mutexLock(&list_mtx);
  for (memfile *m=open_files;m;m=m->next) {
    mutexLock(&m->mtx);
    if (m->dirty && armTicksToNs(now - m->dirty_since) >= (u64)min_age_ms * 1000000ULL) commit(m);
    mutexUnlock(&m->mtx);
  }
  mutexUnlock(&list_mtx);
}

/* Emulator-only fault injection, called before the game starts. All files are under cache/. */
static int test_check(int condition,const char *name) {
  debugPrintf("[memtest] %s %s\n",condition ? "PASS":"FAIL",name);
  return !condition;
}
static int test_contents(const char *path,const char *expected) {
  char b[64]={0}; FILE *f=fopen(path,"rb"); if(!f) return 0;
  size_t n=fread(b,1,sizeof(b)-1,f); int ok=!ferror(f) && n==strlen(expected) && !memcmp(b,expected,n);
  fclose(f); return ok;
}
typedef struct {job j;int done,result;} race_test;
static void test_replace_thread(void *arg) {
  race_test *r=arg;r->result=write_disk(&r->j);__atomic_store_n(&r->done,1,__ATOMIC_RELEASE);
}
int memfile_selftest(void) {
  int failures=0,fallback=0;
  const char *path=PORT_ROOT "/cache/memfile-selftest.dat";
  char tmp[520],bak[520]; snprintf(tmp,sizeof(tmp),"%s.tmp",path);snprintf(bak,sizeof(bak),"%s.bak",path);
  remove(path);remove(tmp);remove(bak);
  FILE *f=memfile_open(path,"w+",&fallback);
  failures+=test_check(f!=NULL && !fallback,"memory stream creation");
  if(!f) return failures;
  char b[16]={0};
  int io=fwrite("abcdef",1,6,f)==6 && !fseek(f,2,SEEK_SET) && fwrite("XY",1,2,f)==2 &&
         !fseek(f,0,SEEK_SET) && fread(b,1,6,f)==6 && !memcmp(b,"abXYef",6);
  failures+=test_check(io,"read/write/seek coherence");
  failures+=test_check(!fclose(f),"enqueue on close");
  memfile_drain(); failures+=test_check(test_contents(path,"abXYef"),"background persistence");
  FILE *many[96]={0};int all_memory=1;
  for(int i=0;i<96;i++) {int plain=0;many[i]=memfile_open(path,"r",&plain);if(!many[i] || plain) all_memory=0;}
  for(int i=0;i<96;i++) if(many[i]) fclose(many[i]);
  failures+=test_check(all_memory && !open_count,"more than 64 simultaneous streams stay memory backed");
  f=memfile_open(path,"a+",&fallback);
  int append=f && !fseek(f,0,SEEK_SET) && fwrite("!",1,1,f)==1;
  if(f) fclose(f);
  memfile_drain();
  failures+=test_check(append && test_contents(path,"abXYef!"),"append ignores seek for writes");
  memfile m={0}; mutexInit(&m.mtx);
  off_t off=-1; failures+=test_check(mf_seek(&m,&off,SEEK_SET)==-1,"negative seek rejected");
  off=0; failures+=test_check(mf_seek(&m,&off,99)==-1,"invalid seek origin rejected");
  off=(off_t)MAX_FILE+1; failures+=test_check(mf_seek(&m,&off,SEEK_SET)==-1,"oversized seek rejected");
  m.pos=MAX_FILE; failures+=test_check(mf_write(&m,"x",1)==-1 && errno==EFBIG,"write extent overflow rejected");
  failures+=test_check(mf_read(&m,b,0)==0 && mf_write(&m,"",0)==0,"zero length I/O without allocation");
  test_snapshot_oom=1;
  failures+=test_check(!enqueue(path,"oom",3) && test_contents(path,"oom"),"snapshot OOM synchronous fallback");
  race_test race={0};snprintf(race.j.path,sizeof(race.j.path),"%s",path);race.j.data="race";race.j.len=4;
  Thread replacer;test_pause_install=1;test_in_rename_gap=0;
  Result rc=threadCreate(&replacer,test_replace_thread,&race,NULL,0x8000,0x2c,2);
  int created=R_SUCCEEDED(rc);if(created) rc=threadStart(&replacer);
  int coherent=0;
  if(R_SUCCEEDED(rc)) {
    while(!__atomic_load_n(&test_in_rename_gap,__ATOMIC_ACQUIRE) && !__atomic_load_n(&race.done,__ATOMIC_ACQUIRE)) svcSleepThread(50000);
    memfile reader={0};int r=load(&reader,path);
    coherent=!r && reader.len==4 && !memcmp(reader.data,"race",4);free(reader.data);
    threadWaitForExit(&replacer);coherent &= !race.result;
  }
  if(created) threadClose(&replacer);
  test_pause_install=0;
  failures+=test_check(coherent,"reader cannot observe replacement rename gap");
  enqueue(path,"oom",3);memfile_drain();
  job j={0};snprintf(j.path,sizeof(j.path),"%s",path);j.data="replacement";j.len=11;
  test_install_fail=1;
  failures+=test_check(write_disk(&j)==-1 && test_contents(path,"oom"),"failed install preserves original save");
  remove(tmp);
  failures+=test_check(!rename(path,bak),"simulate interrupted replacement");
  test_install_fail=1;
  failures+=test_check(write_disk(&j)==-1 && test_contents(path,"oom"),"failed install also preserves interrupted backup");
  remove(tmp); rename(path,bak);
  f=memfile_open(path,"rb",&fallback); memset(b,0,sizeof(b));
  int recovered=f && fread(b,1,3,f)==3 && !memcmp(b,"oom",3);
  if(f) fclose(f);
  memfile_drain();
  failures+=test_check(recovered && test_contents(path,"oom"),"interrupted replacement recovers backup");
  enqueue(path,"newest",6);
  failures+=test_check(memfile_pending(path),"pending lookup requires no snapshot allocation");
  test_pending_oom=1;
  f=memfile_open(path,"rb",&fallback);
  failures+=test_check(!f && errno==ENOMEM && !fallback,"pending OOM never returns stale disk data");
  if(f) fclose(f);
  memfile_drain();
  failures+=test_check(test_contents(path,"newest"),"newest queued snapshot wins");
  remove(path);remove(tmp);remove(bak);
  debugPrintf("[memtest] RESULT %s (%d failures)\n",failures ? "FAIL":"PASS",failures);
  return failures;
}

/* Emulator stress fixture: keep many readers alive across real engine saves and region changes.
 * This exercises the former 64-stream fallback without editing any save data. */
void memfile_stress_fixture(void) {
  FILE *marker=fopen(PORT_ROOT "/stress_save_streams","r"),*em=marker ? fopen(PORT_ROOT "/emulator_mode","r"):NULL;
  if(marker) fclose(marker);
  if(!em) return;
  fclose(em);static FILE *readers[96];int n=0;
  for(;n<96;n++) {int fallback=0;readers[n]=memfile_open(SAVE_ROOT "/save_common","r",&fallback);
    if(!readers[n] || fallback) break;}
  debugPrintf("[memtest] %s stress fixture holds %d memory-backed save readers\n",n==96 ? "PASS":"FAIL",n);
}

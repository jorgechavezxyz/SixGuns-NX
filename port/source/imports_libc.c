/* Bionic (Android libc) -> newlib/libnx translation layer.
 * Functions whose ABI matches are mapped directly in imports.c; everything
 * here exists because a struct layout or constant differs between the two. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <switch.h>
#include "config.h"
#include "imports_libc.h"
#include "util.h"

#define PATH(p) char _pb[1024]; const char *rp = remap_path((p), _pb, sizeof(_pb))
/* Saves are written in the background (memfile.c); anything that looks at a
 * file on disk by name first waits for a queued write of that file. */
extern int memfile_pending(const char *path);
extern void memfile_sync_path(const char *path);
extern int memfile_wanted(const char *path);
extern int memfile_is_stream(FILE *f);
static void disk_sync(const char *path) { if (memfile_pending(path)) memfile_sync_path(path); }
/* A save the engine deletes or renames away must not come back from its .bak/.tmp (memfile_open
 * recovers a missing file from them). */
static void drop_save_shadows(const char *path) {
  if (!memfile_wanted(path)) return;
  char b[1040];
  snprintf(b, sizeof(b), "%s.bak", path); remove(b);
  snprintf(b, sizeof(b), "%s.tmp", path); remove(b);
}

/* ---------------------------------------------------------------- stdio */

/* bionic exports stdin/stdout/stderr as __sF[3], each a 152-byte FILE. */
uint8_t fake_sF[3][152];

static FILE *tf(void *f) {
  if (!f) return NULL;
  if (f == fake_sF[0]) return stdin;
  if (f == fake_sF[1]) return stdout;
  if (f == fake_sF[2]) return stderr;
  return f;
}

int b_vfprintf(void *f, const char *fmt, va_list va) {
  FILE *ff = tf(f);
  if (ff == stdout || ff == stderr) {
    char buf[2048];
    int n = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("%s", buf);
    return n;
  }
  return vfprintf(ff, fmt, va);
}
int b_fprintf(void *f, const char *fmt, ...) {
  va_list va; va_start(va, fmt);
  int r = b_vfprintf(f, fmt, va);
  va_end(va); return r;
}
int b_fputs(const char *s, void *f) { return f ? fputs(s, tf(f)) : EOF; }
int b_fputc(int c, void *f) { return f ? fputc(c, tf(f)) : EOF; }
size_t b_fwrite(const void *p, size_t s, size_t n, void *f) { return f ? fwrite(p, s, n, tf(f)) : 0; }

/* The engine doesn't always check fopen's result: pig::stream::FileStream::Open
 * passes it straight to CryptTea::readHeader, which fseeks it. A NULL FILE
 * crashed newlib on hardware (2026-11-10, save/save_a); these behave like a
 * closed stream instead (error result, EBADF). */
#define NULLFILE(ret) do { if (!f) { errno = EBADF; return ret; } } while (0)
int b_fclose(void *f);
int b_feof(void *f) { NULLFILE(1); return feof(tf(f)); }
int b_ferror(void *f) { NULLFILE(1); return ferror(tf(f)); }
void b_clearerr(void *f) { if (f) clearerr(tf(f)); }
char *b_fgets(char *s, int n, void *f) { NULLFILE(NULL); return fgets(s, n, tf(f)); }
size_t b_fread(void *p, size_t s, size_t n, void *f) { NULLFILE(0); return fread(p, s, n, tf(f)); }
int b_fseek(void *f, long off, int wh) { NULLFILE(-1); return fseek(tf(f), off, wh); }
int b_fseeko(void *f, off_t off, int wh) { NULLFILE(-1); return fseeko(tf(f), off, wh); }
long b_ftell(void *f) { NULLFILE(-1); return ftell(tf(f)); }
off_t b_ftello(void *f) { NULLFILE(-1); return ftello(tf(f)); }
int b_getc(void *f) { NULLFILE(EOF); return getc(tf(f)); }
int b_ungetc(int c, void *f) { NULLFILE(EOF); return ungetc(c, tf(f)); }
void b_rewind(void *f) { if (f) rewind(tf(f)); }
int b_fscanf(void *f, const char *fmt, ...) {
  NULLFILE(EOF);
  va_list va; va_start(va, fmt);
  int r = vfscanf(tf(f), fmt, va);
  va_end(va); return r;
}

int b_fclose(void *f) { NULLFILE(EOF); return fclose(tf(f)); }
int b_fflush(void *f) { return fflush(f ? tf(f) : NULL); }
/* Save streams stay unbuffered so a commit while they are open never misses a buffered tail. */
int b_setvbuf(void *f, char *b, int m, size_t s) { return memfile_is_stream(tf(f)) ? 0 : setvbuf(tf(f), b, m, s); }
int b_fileno(void *f) { return fileno(tf(f)); }
int b_printf(const char *fmt, ...) {
  char buf[2048]; va_list va; va_start(va, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, va);
  va_end(va); debugPrintf("%s", buf); return n;
}
int b_puts(const char *s) { debugPrintf("%s\n", s); return 1; }
int b_putchar(int c) { debugPrintf("%c", c); return c; }

/* /dev/urandom and /dev/random don't exist on Horizon: serve them from the
 * system CSPRNG (randomGet). fopen gets a cookie stream; open() gets a real
 * fd on a placeholder file whose reads are intercepted in b_read. */
static int is_random_dev(const char *p) {
  return p && (strcmp(p, "/dev/urandom") == 0 || strcmp(p, "/dev/random") == 0);
}

static ssize_t random_cookie_read(void *c, char *buf, size_t n) { randomGet(buf, n); return n; }
static int random_cookie_close(void *c) { return 0; }

static int random_fds[16];
static int n_random_fds;
static Mutex rnd_mtx;

static int is_random_fd(int fd) {
  for (int i = 0; i < n_random_fds; i++) if (random_fds[i] == fd) return 1;
  return 0;
}

/* Path behind each fd opened through b_open, so mmap can read through its
 * own handle instead of moving the engine's shared file position. */
#define MAX_FD_PATHS 256
static char *fd_paths[MAX_FD_PATHS];
static Mutex fd_paths_mtx;

static void fd_path_set(int fd, const char *path) {
  if (fd < 0 || fd >= MAX_FD_PATHS) return;
  mutexLock(&fd_paths_mtx);
  free(fd_paths[fd]);
  fd_paths[fd] = path ? strdup(path) : NULL;
  mutexUnlock(&fd_paths_mtx);
}

static int fd_path_get(int fd, char *out, size_t n) {
  int ok = 0;
  if (fd < 0 || fd >= MAX_FD_PATHS) return 0;
  mutexLock(&fd_paths_mtx);
  if (fd_paths[fd]) { snprintf(out, n, "%s", fd_paths[fd]); ok = 1; }
  mutexUnlock(&fd_paths_mtx);
  return ok;
}

/* I/O statistics (cumulative), logged with [perf] and on hitches. */
static _Atomic unsigned long st_read_n, st_read_b, st_read_ns, st_mmap_n, st_mmap_b, st_mmap_ns, st_fopen_n, st_fopen_ns;
static unsigned long lg[8];
void io_stats_log(const char *what) {
  unsigned long v[8] = { st_read_n, st_read_b, st_read_ns, st_mmap_n, st_mmap_b, st_mmap_ns, st_fopen_n, st_fopen_ns };
  debugPrintf("[perf] %s io: read %lu calls %lu KB %.1f ms | mmap %lu calls %lu KB %.1f ms | fopen %lu %.1f ms\n", what,
              v[0] - lg[0], (v[1] - lg[1]) >> 10, (v[2] - lg[2]) / 1e6, v[3] - lg[3], (v[4] - lg[4]) >> 10,
              (v[5] - lg[5]) / 1e6, v[6] - lg[6], (v[7] - lg[7]) / 1e6);
}
/* Per-frame: snapshot at frame start; on a slow frame, log what I/O it did. */
static unsigned long fr[8], fr_gl[5];
extern unsigned long gl_tex_n, gl_tex_kb, gl_tex_ns, gl_link_n, gl_link_ns;
void io_frame_begin(void) {
  unsigned long v[8] = { st_read_n, st_read_b, st_read_ns, st_mmap_n, st_mmap_b, st_mmap_ns, st_fopen_n, st_fopen_ns };
  memcpy(fr, v, sizeof(v));
  fr_gl[0] = gl_tex_n; fr_gl[1] = gl_tex_kb; fr_gl[2] = gl_tex_ns; fr_gl[3] = gl_link_n; fr_gl[4] = gl_link_ns;
}
void io_frame_hitch(unsigned long frame, double ms) {
  unsigned long v[8] = { st_read_n, st_read_b, st_read_ns, st_mmap_n, st_mmap_b, st_mmap_ns, st_fopen_n, st_fopen_ns };
  debugPrintf("[hitch] frame %lu took %.0f ms; io: mmap %lu/%lu KB/%.1f ms, fopen %lu/%.1f ms; textures %lu/%lu KB/%.1f ms; "
              "shaders linked %lu/%.1f ms\n",
              frame, ms, v[3] - fr[3], (v[4] - fr[4]) >> 10, (v[5] - fr[5]) / 1e6, v[6] - fr[6], (v[7] - fr[7]) / 1e6,
              gl_tex_n - fr_gl[0], gl_tex_kb - fr_gl[1], (gl_tex_ns - fr_gl[2]) / 1e6, gl_link_n - fr_gl[3],
              (gl_link_ns - fr_gl[4]) / 1e6);
}

/* Mark the start of an interval (each log reports since the last mark). */
void io_stats_mark(void) {
  unsigned long v[8] = { st_read_n, st_read_b, st_read_ns, st_mmap_n, st_mmap_b, st_mmap_ns, st_fopen_n, st_fopen_ns };
  memcpy(lg, v, sizeof(v));
}

ssize_t b_read(int fd, void *buf, size_t n) {
  if (is_random_fd(fd)) { randomGet(buf, n); return n; }
  u64 t0 = armGetSystemTick();
  ssize_t r = read(fd, buf, n);
  st_read_n++; if (r > 0) st_read_b += r;
  st_read_ns += armTicksToNs(armGetSystemTick() - t0);
  return r;
}

/* Read-only archive fd pool. The engine opens the OBB again for every stream it creates from it
 * (MMapStream2::SystemMOpen: open + fstat + mmap + close per texture/model), and on the Switch's
 * SD card each open/close/fstat is a file-system IPC round trip (fstat also converts timestamps
 * over IPC). Profile 2026-11-13: open() was ~27% of the shop's tab-rebuild stalls, and loading
 * screens are full of it. Read-only opens of .obb/.apk/.zip (archives that never change while the
 * game runs) now reuse a parked descriptor of the same file (position reset to 0), close() parks it
 * again, and fstat() answers from a per-file copy. A pooled descriptor has one owner at a time, so
 * the engine sees exactly what fresh opens gave it. Marker `no_fd_pool` turns it off. */
enum { FD_POOL = 32 };
static struct { char path[256]; int fd, busy, have_stat; struct stat st; } fdpool[FD_POOL];
static Mutex fdpool_mtx;
static int fdpool_on = -1;
static unsigned long fdpool_reused;

static int fdpool_enabled(void) {
  if (fdpool_on < 0) {
    FILE *f = fopen(PORT_ROOT "/no_fd_pool", "r");
    fdpool_on = f == NULL;
    if (f) fclose(f);
  }
  return fdpool_on;
}
static int fdpool_wanted(const char *p, int flags) {
  if ((flags & 3) != 0 || (flags & (0100 | 01000 | 02000))) return 0; /* write, O_CREAT|O_TRUNC|O_APPEND (bionic values) */
  size_t n = strlen(p);
  if (n >= sizeof(fdpool[0].path) || n < 4) return 0;
  const char *e = p + n - 4;
  return !strcmp(e, ".obb") || !strcmp(e, ".apk") || !strcmp(e, ".zip");
}
/* An idle pooled fd for path (marked busy), or -1. */
static int fdpool_take(const char *path) {
  int fd = -1;
  mutexLock(&fdpool_mtx);
  for (int i = 0; i < FD_POOL; i++)
    if (fdpool[i].fd > 0 && !fdpool[i].busy && !strcmp(fdpool[i].path, path)) {
      fdpool[i].busy = 1; fd = fdpool[i].fd; fdpool_reused++;
      break;
    }
  mutexUnlock(&fdpool_mtx);
  if (fd >= 0) lseek(fd, 0, SEEK_SET);
  return fd;
}
/* Register a fresh fd as pooled (busy); ignored when the pool is full. */
static void fdpool_add(int fd, const char *path) {
  mutexLock(&fdpool_mtx);
  for (int i = 0; i < FD_POOL; i++)
    if (fdpool[i].fd <= 0) {
      snprintf(fdpool[i].path, sizeof(fdpool[i].path), "%s", path);
      fdpool[i].fd = fd; fdpool[i].busy = 1; fdpool[i].have_stat = 0;
      break;
    }
  mutexUnlock(&fdpool_mtx);
}
/* 1 if fd is pooled: it is parked instead of closed. */
static int fdpool_release(int fd) {
  int r = 0;
  mutexLock(&fdpool_mtx);
  for (int i = 0; i < FD_POOL; i++)
    if (fdpool[i].fd == fd && fdpool[i].busy) { fdpool[i].busy = 0; r = 1; break; }
  mutexUnlock(&fdpool_mtx);
  return r;
}
/* fstat from the per-file copy; 0 if fd isn't pooled. */
static int fdpool_fstat(int fd, struct stat *out) {
  int idx = -1;
  mutexLock(&fdpool_mtx);
  for (int i = 0; i < FD_POOL; i++)
    if (fdpool[i].fd == fd && fdpool[i].busy) { idx = i; break; }
  if (idx >= 0 && fdpool[idx].have_stat) { *out = fdpool[idx].st; mutexUnlock(&fdpool_mtx); return 1; }
  mutexUnlock(&fdpool_mtx);
  if (idx < 0) return 0;
  struct stat s;
  if (fstat(fd, &s) != 0) return 0;
  mutexLock(&fdpool_mtx);
  /* the same file's copy for every pooled fd of that path */
  for (int i = 0; i < FD_POOL; i++)
    if (fdpool[i].fd > 0 && !strcmp(fdpool[i].path, fdpool[idx].path)) { fdpool[i].st = s; fdpool[i].have_stat = 1; }
  mutexUnlock(&fdpool_mtx);
  *out = s;
  return 1;
}
void fdpool_stats(void) {
  if (fdpool_reused) debugPrintf("[perf] archive opens served from the fd pool: %lu\n", fdpool_reused);
}

int b_close(int fd) {
  if (fdpool_release(fd)) return 0;
  extern void net_forget(int fd);
  net_forget(fd);
  fd_path_set(fd, NULL);
  mutexLock(&rnd_mtx);
  for (int i = 0; i < n_random_fds; i++)
    if (random_fds[i] == fd) { random_fds[i] = random_fds[--n_random_fds]; break; }
  mutexUnlock(&rnd_mtx);
  return close(fd);
}

FILE *b_fopen(const char *p, const char *mode) {
  if (is_random_dev(p)) {
    cookie_io_functions_t io = { .read = random_cookie_read, .close = random_cookie_close };
    return fopencookie(NULL, "r", io);
  }
  PATH(p);
  extern int memfile_wanted(const char *path);
  extern FILE *memfile_open(const char *path, const char *mode, int *fallback);
  FILE *f = NULL;
  int fallback = 1;
  u64 t0 = armGetSystemTick();
  {
    extern FILE *obb_index_fopen(const char *path, const char *mode);
    FILE *of = obb_index_fopen(rp, mode); /* read-only *.obb: indexed reads (obb_index.c) */
    if (of) {
      st_fopen_n++; st_fopen_ns += armTicksToNs(armGetSystemTick() - t0);
      debugPrintf("[io] fopen(%s) ok (indexed)\n", rp);
      return of;
    }
  }
  if (memfile_wanted(rp)) f = memfile_open(rp, mode, &fallback);
  if (!f && fallback) f = fopen(rp, mode);
  st_fopen_n++; st_fopen_ns += armTicksToNs(armGetSystemTick() - t0);
  if (!f) {
    int error=errno;
    debugPrintf("[io] fopen(%s, %s) failed (errno %d, fallback %d)\n",rp,mode,error,fallback);
    if(!strncmp(rp,SAVE_ROOT "/",strlen(SAVE_ROOT)+1) && error!=ENOENT) {
      extern void memfile_disable_writes(const char *);
      extern void memfile_stats(void);memfile_stats();
      extern volatile int port_quit_requested;
      memfile_disable_writes("save file could not be opened; stopping to prevent a fresh-state overwrite");
      port_quit_requested=1;
    }
    errno=error;
  }
  else if (strstr(rp, ".obb")) debugPrintf("[io] fopen(%s) ok\n", rp);
  return f;
}
FILE *b_freopen(const char *p, const char *mode, void *f) { PATH(p); return freopen(rp, mode, tf(f)); }

/* ---------------------------------------------------------------- files */

/* bionic aarch64 O_* values (asm-generic) */
#define B_O_CREAT     0100
#define B_O_EXCL      0200
#define B_O_TRUNC     01000
#define B_O_APPEND    02000
#define B_O_NONBLOCK  04000
#define B_O_DIRECTORY 040000

static int conv_oflags(int f) {
  int r = f & 3; /* O_RDONLY/O_WRONLY/O_RDWR are identical */
  if (f & B_O_CREAT) r |= O_CREAT;
  if (f & B_O_EXCL) r |= O_EXCL;
  if (f & B_O_TRUNC) r |= O_TRUNC;
  if (f & B_O_APPEND) r |= O_APPEND;
  if (f & B_O_NONBLOCK) r |= O_NONBLOCK;
  return r;
}

int b_open(const char *p, int flags, ...) {
  if (is_random_dev(p)) {
    int fd = open(PORT_ROOT "/random.placeholder", O_RDWR | O_CREAT, 0666);
    if (fd >= 0) {
      mutexLock(&rnd_mtx);
      if (n_random_fds < 16) random_fds[n_random_fds++] = fd;
      mutexUnlock(&rnd_mtx);
    }
    return fd;
  }
  PATH(p);
  int mode = 0;
  if (flags & B_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  int pooled = fdpool_enabled() && fdpool_wanted(rp, flags);
  if (pooled) {
    int pfd = fdpool_take(rp);
    if (pfd >= 0) return pfd; /* fd_path is still set from its first open */
  }
  disk_sync(rp);
  int fd = open(rp, conv_oflags(flags), mode);
  if (fd >= 0) fd_path_set(fd, rp);
  if (fd >= 0 && pooled) fdpool_add(fd, rp);
  if (fd < 0) debugPrintf("[io] open(%s, 0%o) failed\n", rp, flags);
  else if (strstr(rp, ".apk")) debugPrintf("[io] open(%s) ok (apk)\n", rp);
  else if (strstr(rp, ".obb")) {
    static int logged;
    if (logged++ < 2) debugPrintf("[io] open(%s) ok -> fd %d (further OBB opens not logged)\n", rp, fd);
  }
  return fd;
}

typedef struct {
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, __pad1;
  int64_t st_size;
  int32_t st_blksize, __pad2;
  int64_t st_blocks;
  struct { int64_t s, ns; } st_atim, st_mtim, st_ctim;
  uint32_t __unused4, __unused5;
} bionic_stat;

static void conv_stat(const struct stat *s, bionic_stat *b) {
  memset(b, 0, sizeof(*b));
  b->st_dev = s->st_dev;
  b->st_ino = s->st_ino;
  b->st_mode = s->st_mode; /* S_IFDIR/S_IFREG and permission bits use the same values */
  b->st_nlink = s->st_nlink;
  b->st_size = s->st_size;
  b->st_blksize = 4096;
  b->st_blocks = (s->st_size + 511) / 512;
  b->st_atim.s = s->st_atime;
  b->st_mtim.s = s->st_mtime;
  b->st_ctim.s = s->st_ctime;
}

int b_stat(const char *p, void *out) {
  PATH(p); struct stat s;
  disk_sync(rp);
  int r = stat(rp, &s);
  if (r == 0) conv_stat(&s, out);
  return r;
}
int b_fstat(int fd, void *out) {
  struct stat s;
  if (fdpool_fstat(fd, &s)) { conv_stat(&s, out); return 0; }
  int r = fstat(fd, &s);
  if (r == 0) conv_stat(&s, out);
  return r;
}
int b_statfs(const char *p, void *out) {
  /* f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_fsid, f_namelen, f_frsize ... */
  uint64_t *b = out;
  memset(out, 0, 120);
  b[1] = 4096; b[2] = b[3] = b[4] = (8ULL << 30) / 4096; b[5] = b[6] = 1 << 20; b[8] = 255; b[9] = 4096;
  return 0;
}

int b_access(const char *p, int m) { PATH(p); struct stat s; disk_sync(rp); return stat(rp, &s); }
int b_mkdir(const char *p, int m) { PATH(p); return mkdir(rp, 0777); }
int b_rmdir(const char *p) { PATH(p); return rmdir(rp); }
int b_unlink(const char *p) { PATH(p); disk_sync(rp); int r = unlink(rp); drop_save_shadows(rp); return r; }
int b_remove(const char *p) { PATH(p); disk_sync(rp); int r = remove(rp); drop_save_shadows(rp); return r; }
int b_chdir(const char *p) { PATH(p); return chdir(rp); }
int b_rename(const char *a, const char *b) {
  char b1[1024], b2[1024];
  const char *ra = remap_path(a, b1, sizeof(b1)), *rb = remap_path(b, b2, sizeof(b2));
  disk_sync(ra); disk_sync(rb);
  int r = rename(ra, rb);
  if (!r) drop_save_shadows(ra);
  return r;
}

typedef struct {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
} bionic_dirent;

typedef struct { DIR *d; bionic_dirent ent; } b_DIR;

void *b_opendir(const char *p) {
  PATH(p);
  DIR *d = opendir(rp);
  if (!d) return NULL;
  b_DIR *bd = calloc(1, sizeof(*bd));
  bd->d = d;
  return bd;
}
void *b_readdir(void *dir) {
  b_DIR *bd = dir;
  struct dirent *e = readdir(bd->d);
  if (!e) return NULL;
  memset(&bd->ent, 0, sizeof(bd->ent));
  strncpy(bd->ent.d_name, e->d_name, 255);
  bd->ent.d_reclen = sizeof(bd->ent);
  bd->ent.d_type = e->d_type == DT_DIR ? 4 : 8; /* DT_DIR / DT_REG */
  bd->ent.d_ino = 1;
  return &bd->ent;
}
int b_closedir(void *dir) {
  b_DIR *bd = dir;
  int r = closedir(bd->d);
  free(bd);
  return r;
}

/* ---------------------------------------------------------------- mmap */

#define B_MAP_ANONYMOUS 0x20

/* ---------------------------------------------------------------- mmap
 * The engine maps every asset out of the OBB archives, on the render thread:
 * ~750 maps for the main menu, ~5000 (670 MB) for a level, and it maps the
 * same file region again and again (627 MB of those 670 MB were repeats of a
 * region it had already mapped and released). Three things make this cheap:
 *  - each OBB stays open as one FsFile, read at an offset (fsFileRead is
 *    positional and thread-safe), instead of reopening the 700 MB archive per
 *    call; opening files is slow on the Switch's SD card;
 *  - no zero-fill of the whole buffer before reading;
 *  - a 320 MB LRU cache keyed by (archive, offset, length) keeps an untouched
 *    copy of each region read; the next map of that region is a memcpy from
 *    it instead of an SD read. Copies, not the engine's own buffers: the
 *    first version recycled released buffers and the engine evidently writes
 *    into some "read-only" mappings (animations glitched on hardware).
 * This was the stutter when menus, the shop or cutscene objects loaded. */
#define B_PROT_WRITE 0x2
#define MAP_BUCKETS 4096
#define CACHE_BUDGET (320ULL << 20)

typedef struct mapping {
  void *addr;
  size_t len;
  long off;
  int file;     /* 1-based index into obbh, 0 = not cacheable */
  struct mapping *next;
} mapping;
static mapping *live[MAP_BUCKETS];

typedef struct centry {
  void *buf;
  size_t len;
  long off;
  int file;
  struct centry *hnext, *prev, *lnext; /* hash chain; LRU list (head = newest) */
} centry;
static centry *chash[MAP_BUCKETS], *lru_head, *lru_tail;
static size_t cache_bytes;
static unsigned long cache_hits, cache_hit_bytes;
static Mutex map_mtx;

static unsigned addr_bucket(void *p) { return (unsigned)(((uintptr_t)p >> 12) * 2654435761u) & (MAP_BUCKETS - 1); }
static unsigned key_bucket(int file, long off, size_t len) {
  return (unsigned)(((u64)off * 0x9E3779B97F4A7C15ULL) ^ (len * 2654435761u) ^ (unsigned)file) & (MAP_BUCKETS - 1);
}

static void lru_unlink(centry *e) {
  if (e->prev) e->prev->lnext = e->lnext; else lru_head = e->lnext;
  if (e->lnext) e->lnext->prev = e->prev; else lru_tail = e->prev;
  e->prev = e->lnext = NULL;
}
static void hash_unlink(centry *e) {
  centry **pp = &chash[key_bucket(e->file, e->off, e->len)];
  while (*pp && *pp != e) pp = &(*pp)->hnext;
  if (*pp) *pp = e->hnext;
}
/* Caller holds map_mtx. Copies a cached region into dst; 1 on hit. */
static int cache_copy(int file, long off, size_t len, void *dst) {
  for (centry *e = chash[key_bucket(file, off, len)]; e; e = e->hnext) {
    if (e->file == file && e->off == off && e->len == len) {
      memcpy(dst, e->buf, len);
      if (lru_head != e) { /* move to front */
        lru_unlink(e);
        e->lnext = lru_head; if (lru_head) lru_head->prev = e; lru_head = e; if (!lru_tail) lru_tail = e;
      }
      cache_hits++; cache_hit_bytes += len;
      return 1;
    }
  }
  return 0;
}
static int cache_has(int file, long off, size_t len) {
  for (centry *e = chash[key_bucket(file, off, len)]; e; e = e->hnext)
    if (e->file == file && e->off == off && e->len == len) return 1;
  return 0;
}
/* Caller holds map_mtx. Takes ownership of buf. */
static void cache_put(void *buf, int file, long off, size_t len) {
  if (len > CACHE_BUDGET / 4 || cache_has(file, off, len)) { free(buf); return; }
  centry *e = malloc(sizeof(*e));
  if (!e) { free(buf); return; }
  *e = (centry){ .buf = buf, .len = len, .off = off, .file = file };
  unsigned b = key_bucket(file, off, len);
  e->hnext = chash[b]; chash[b] = e;
  e->lnext = lru_head; if (lru_head) lru_head->prev = e; lru_head = e; if (!lru_tail) lru_tail = e;
  cache_bytes += len;
  while (cache_bytes > CACHE_BUDGET && lru_tail) {
    centry *old = lru_tail;
    lru_unlink(old);
    hash_unlink(old);
    cache_bytes -= old->len;
    free(old->buf);
    free(old);
  }
}

void mmap_cache_stats(void) {
  mutexLock(&map_mtx);
  debugPrintf("[perf] asset cache: %lu hits (%lu MB not re-read), %zu MB held\n", cache_hits, cache_hit_bytes >> 20,
              cache_bytes >> 20);
  mutexUnlock(&map_mtx);
}

static Mutex obbh_mtx;
static struct { char path[256]; FsFile f; int open; } obbh[8];

/* 1-based handle index for an OBB/APK path, opening it on first use; 0 if not possible. */
static int mmap_handle(const char *path) {
  if (strncmp(path, "sdmc:/", 6) != 0 || strlen(path) >= sizeof(obbh[0].path)) return 0;
  int r = 0;
  mutexLock(&obbh_mtx);
  for (int i = 0; i < 8; i++)
    if (obbh[i].open && !strcmp(obbh[i].path, path)) { r = i + 1; break; }
  if (!r) {
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    for (int i = 0; fs && i < 8; i++) {
      if (obbh[i].open) continue;
      char p[FS_MAX_PATH];
      snprintf(p, sizeof(p), "%s", path + 5); /* drop "sdmc:" */
      if (R_SUCCEEDED(fsFsOpenFile(fs, p, FsOpenMode_Read, &obbh[i].f))) {
        snprintf(obbh[i].path, sizeof(obbh[i].path), "%s", path);
        obbh[i].open = 1;
        r = i + 1;
        debugPrintf("[mmap] keeping %s open for mapped reads\n", strrchr(path, '/') + 1);
      }
      break;
    }
  }
  mutexUnlock(&obbh_mtx);
  return r;
}

/* Allocation for a mapping; if memory runs short, drop the cache and retry. */
static void *map_alloc(size_t len) {
  void *p = memalign(0x1000, len);
  if (p) return p;
  mutexLock(&map_mtx);
  size_t had = cache_bytes;
  while (lru_tail) {
    centry *old = lru_tail;
    lru_unlink(old); hash_unlink(old);
    cache_bytes -= old->len;
    free(old->buf); free(old);
  }
  mutexUnlock(&map_mtx);
  debugPrintf("[mmap] out of memory for %zu KB; dropped %zu MB of asset cache\n", len >> 10, had >> 20);
  return memalign(0x1000, len);
}

void *b_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
  u64 t0 = armGetSystemTick();
  st_mmap_n++; st_mmap_b += len;
  void *p = NULL;
  int file = 0;
  if (!(flags & B_MAP_ANONYMOUS) && fd >= 0) {
    char path[512];
    int have_path = fd_path_get(fd, path, sizeof(path));
    int h = have_path ? mmap_handle(path) : 0;
    p = map_alloc(len);
    if (!p) return (void *)-1;
    int hit = 0;
#ifndef NO_ASSET_CACHE
    if (h) {
      mutexLock(&map_mtx);
      hit = cache_copy(h, off, len, p);
      mutexUnlock(&map_mtx);
    }
#endif
    if (!hit) {
      u64 got = 0;
      if (h && R_SUCCEEDED(fsFileRead(&obbh[h - 1].f, off, p, len, FsReadOption_None, &got))) {
        if (got < len) memset((char *)p + got, 0, len - got);
#ifndef NO_ASSET_CACHE
        /* Keep an untouched copy for the next map of this region. */
        if (len <= CACHE_BUDGET / 4) {
          void *copy = malloc(len);
          if (copy) {
            memcpy(copy, p, len);
            mutexLock(&map_mtx);
            cache_put(copy, h, off, len);
            mutexUnlock(&map_mtx);
          }
        }
#endif
      } else if (have_path) {
        /* Private handle: seeking the engine's fd would race with other
         * threads reading the same archive. */
        FILE *f = fopen(path, "rb");
        size_t n = 0;
        if (f) { fseek(f, off, SEEK_SET); n = fread(p, 1, len, f); fclose(f); }
        if (n < len) memset((char *)p + n, 0, len - n);
        file = 0;
      } else {
        static Mutex fallback_mtx;
        memset(p, 0, len);
        mutexLock(&fallback_mtx);
        long cur = lseek(fd, 0, SEEK_CUR);
        lseek(fd, off, SEEK_SET);
        read(fd, p, len);
        lseek(fd, cur, SEEK_SET);
        mutexUnlock(&fallback_mtx);
        debugPrintf("[mmap] fd %d has no known path, used shared position\n", fd);
        file = 0;
      }
    }
  } else {
    p = map_alloc(len);
    if (!p) return (void *)-1;
    memset(p, 0, len);
  }
#ifndef STOCK_INPUT
  if (!(flags & B_MAP_ANONYMOUS) && fd >= 0) {
    extern void text_patch_mapping(const char *path, long off, char *data, size_t len);
    char tp[512];
    if (fd_path_get(fd, tp, sizeof(tp))) text_patch_mapping(tp, off, p, len);
  }
#endif
  st_mmap_ns += armTicksToNs(armGetSystemTick() - t0);
  mapping *m = malloc(sizeof(*m));
  *m = (mapping){ .addr = p, .len = len, .off = off, .file = file };
  unsigned b = addr_bucket(p);
  mutexLock(&map_mtx);
  m->next = live[b]; live[b] = m;
  mutexUnlock(&map_mtx);
  return p;
}

int b_munmap(void *addr, size_t len) {
  mutexLock(&map_mtx);
  for (mapping **pp = &live[addr_bucket(addr)]; *pp; pp = &(*pp)->next) {
    if ((*pp)->addr == addr) {
      mapping *m = *pp; *pp = m->next;
      free(m->addr); /* the cache holds its own copies (see b_mmap) */
      free(m);
      break;
    }
  }
  mutexUnlock(&map_mtx);
  return 0;
}

/* ---------------------------------------------------------------- bionic TLS
 * Bionic keeps per-thread slots at TPIDR_EL0; the engine reads the stack
 * protector canary from slot 5 (TPIDR_EL0 + 0x28) in ~320 functions.
 * Horizon saves TPIDR_EL0 per thread and libnx doesn't use it, so each
 * thread that runs engine code gets its own fake slot block. */

#define BIONIC_TLS_SLOTS 16
#define BIONIC_STACK_GUARD 0x5e1f6a17c0de2b9dULL

void bionic_tls_init_thread(void) {
  uint64_t *tls = calloc(BIONIC_TLS_SLOTS, sizeof(uint64_t));
  tls[0] = (uint64_t)(uintptr_t)tls; /* TLS_SLOT_SELF */
  tls[5] = BIONIC_STACK_GUARD;       /* TLS_SLOT_STACK_GUARD */
  __asm__ volatile("msr tpidr_el0, %0" ::"r"(tls));
}

typedef struct { void *(*fn)(void *); void *arg; } thread_start;

/* Spread the engine's threads over the cores the main thread doesn't use.
 * libnx creates every thread on the process's default core (0), so the
 * renderer shared one core with ~20 engine threads (asset loading, audio
 * mixing, ...) while cores 1 and 2 sat idle. Each new thread moves itself to
 * core 1 or 2 in turn (allowed to run on either). */
int worker_core_pick(void) {
  static int cores[3], ncores = -1;
  static _Atomic unsigned next;
  if (ncores < 0) {
    u64 mask = 0;
    int n = 0;
    if (R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
      for (int c = 1; c <= 2; c++) if (mask & (1ULL << c)) cores[n++] = c;
    ncores = n;
  }
  if (!ncores) return -1;
  return cores[atomic_fetch_add(&next, 1) % ncores];
}
static void move_to_worker_core(void) {
  int core = worker_core_pick();
  if (core < 0) return;
  u64 mask = 0;
  svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, mask & 6);
  static int logged;
  if (R_FAILED(rc) && logged++ < 3) debugPrintf("[thread] couldn't move to core %d: 0x%x\n", core, rc);
}

static void *thread_trampoline(void *p) {
  thread_start s = *(thread_start *)p;
  free(p);
  move_to_worker_core();
  bionic_tls_init_thread();
  return s.fn(s.arg);
}

/* ---------------------------------------------------------------- pthread */

/* bionic's pthread_mutex_t / pthread_cond_t are arrays of int32 (only 4-byte
 * aligned), larger than newlib's, with different static initializers. Each
 * bionic object stores just a 32-bit word: 0/0x4000/0x8000 are bionic's static
 * initializer values; SLOT_FLAG|index points into a table of real objects,
 * created lazily. 32-bit atomics keep exclusive accesses naturally aligned
 * (64-bit ones on these objects fault on hardware). */

#define B_MUTEX_RECURSIVE_INIT 0x4000
#define SLOT_FLAG 0x80000000u
#define MAX_SLOTS 65536

static void *slots[MAX_SLOTS];
static uint32_t slot_free_list[MAX_SLOTS];
static int n_slot_free;
static uint32_t next_slot = 1;
static Mutex slot_mtx;

static uint32_t slot_alloc(void *p) {
  mutexLock(&slot_mtx);
  uint32_t idx = n_slot_free ? slot_free_list[--n_slot_free] : next_slot++;
  if (idx >= MAX_SLOTS) { mutexUnlock(&slot_mtx); fatal_error("out of pthread object slots"); }
  slots[idx] = p;
  mutexUnlock(&slot_mtx);
  return idx;
}

static void slot_release(uint32_t idx) {
  mutexLock(&slot_mtx);
  slots[idx] = NULL;
  slot_free_list[n_slot_free++] = idx;
  mutexUnlock(&slot_mtx);
}

static pthread_mutex_t *mtx_get(void *m) {
  _Atomic uint32_t *w = m;
  uint32_t v = atomic_load(w);
  if (v & SLOT_FLAG) return slots[v & ~SLOT_FLAG];

  pthread_mutex_t *nm = malloc(sizeof(*nm));
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  if (v == B_MUTEX_RECURSIVE_INIT || v == 1) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(nm, &a);
  pthread_mutexattr_destroy(&a);

  uint32_t idx = slot_alloc(nm);
  if (atomic_compare_exchange_strong(w, &v, SLOT_FLAG | idx)) return nm;
  /* another thread initialized it first */
  slot_release(idx);
  pthread_mutex_destroy(nm); free(nm);
  return slots[atomic_load(w) & ~SLOT_FLAG];
}

int b_pthread_mutexattr_init(long *a) { *a = 0; return 0; }
int b_pthread_mutexattr_destroy(long *a) { return 0; }
int b_pthread_mutexattr_settype(long *a, int t) { *a = t; return 0; }

int b_pthread_mutex_init(void *m, long *attr) {
  /* 1 = "make it recursive" marker for mtx_get */
  *(uint32_t *)m = (attr && *attr == 1) ? 1 : 0;
  mtx_get(m);
  return 0;
}
int b_pthread_mutex_destroy(void *m) {
  uint32_t v = *(uint32_t *)m;
  if (v & SLOT_FLAG) {
    pthread_mutex_t *p = slots[v & ~SLOT_FLAG];
    pthread_mutex_destroy(p); free(p);
    slot_release(v & ~SLOT_FLAG);
  }
  *(uint32_t *)m = 0;
  return 0;
}
int b_pthread_mutex_lock(void *m) { return pthread_mutex_lock(mtx_get(m)); }
int b_pthread_mutex_trylock(void *m) { return pthread_mutex_trylock(mtx_get(m)) == 0 ? 0 : 16 /* EBUSY */; }
int b_pthread_mutex_unlock(void *m) { return pthread_mutex_unlock(mtx_get(m)); }

static pthread_cond_t *cond_get(void *c) {
  _Atomic uint32_t *w = c;
  uint32_t v = atomic_load(w);
  if (v & SLOT_FLAG) return slots[v & ~SLOT_FLAG];
  pthread_cond_t *nc = malloc(sizeof(*nc));
  pthread_cond_init(nc, NULL);
  uint32_t idx = slot_alloc(nc);
  if (atomic_compare_exchange_strong(w, &v, SLOT_FLAG | idx)) return nc;
  slot_release(idx);
  pthread_cond_destroy(nc); free(nc);
  return slots[atomic_load(w) & ~SLOT_FLAG];
}
int b_pthread_cond_init(void *c, void *a) { *(uint32_t *)c = 0; cond_get(c); return 0; }
int b_pthread_cond_destroy(void *c) {
  uint32_t v = *(uint32_t *)c;
  if (v & SLOT_FLAG) {
    pthread_cond_t *p = slots[v & ~SLOT_FLAG];
    pthread_cond_destroy(p); free(p);
    slot_release(v & ~SLOT_FLAG);
  }
  *(uint32_t *)c = 0;
  return 0;
}
int b_pthread_cond_wait(void *c, void *m) { return pthread_cond_wait(cond_get(c), mtx_get(m)); }
int b_pthread_cond_timedwait(void *c, void *m, const struct timespec *t) {
  int r = pthread_cond_timedwait(cond_get(c), mtx_get(m), t);
  return r == ETIMEDOUT ? 110 /* bionic ETIMEDOUT */ : r;
}
int b_pthread_cond_signal(void *c) { return pthread_cond_signal(cond_get(c)); }
int b_pthread_cond_broadcast(void *c) { return pthread_cond_broadcast(cond_get(c)); }

typedef struct { int detached; size_t stack; } b_attr;
int b_pthread_attr_init(b_attr *a) { a->detached = 0; a->stack = 0; return 0; }
int b_pthread_attr_destroy(b_attr *a) { return 0; }
int b_pthread_attr_setdetachstate(b_attr *a, int d) { a->detached = d; return 0; }
int b_pthread_attr_setstacksize(b_attr *a, size_t s) { a->stack = s; return 0; }

extern const char *thread_entry_name(void *fn);

int b_pthread_create(pthread_t *t, b_attr *ba, void *(*fn)(void *), void *arg) {
  {
    static int nthreads;
    debugPrintf("[thread] #%d start %s\n", ++nthreads, thread_entry_name((void *)fn));
  }
  pthread_attr_t a;
  pthread_attr_init(&a);
  size_t stack = (ba && ba->stack) ? ba->stack : 0;
  if (stack < 1024 * 1024) stack = 1024 * 1024; /* Android's default is ~1 MB */
  pthread_attr_setstacksize(&a, stack);
  if (ba && ba->detached) pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  thread_start *s = malloc(sizeof(*s));
  s->fn = fn;
  s->arg = arg;
  int r = pthread_create(t, &a, thread_trampoline, s);
  if (r) free(s);
  pthread_attr_destroy(&a);
  if (r) debugPrintf("[pthread] create failed: %d\n", r);
  return r;
}

int b_pthread_once(_Atomic int *once, void (*fn)(void)) {
  int expected = 0;
  if (atomic_compare_exchange_strong(once, &expected, 1)) {
    fn();
    atomic_store(once, 2);
  } else {
    while (atomic_load(once) != 2) svcSleepThread(100000);
  }
  return 0;
}

int b_pthread_key_create(int *key, void (*dtor)(void *)) {
  pthread_key_t k;
  int r = pthread_key_create(&k, dtor);
  *key = (int)k;
  return r;
}
int b_pthread_key_delete(int key) { return pthread_key_delete((pthread_key_t)key); }
void *b_pthread_getspecific(int key) { return pthread_getspecific((pthread_key_t)key); }
int b_pthread_setspecific(int key, const void *v) { return pthread_setspecific((pthread_key_t)key, v); }

int b_pthread_getschedparam(pthread_t t, int *pol, int *param) { *pol = 0; *param = 0; return 0; }
int b_pthread_setschedparam(pthread_t t, int pol, const int *param) { return 0; }
int b_pthread_kill(pthread_t t, int sig) { return 0; }
int b_sched_get_priority_max(int p) { return 0; }
int b_sched_get_priority_min(int p) { return 0; }

/* ---------------------------------------------------------------- time */

typedef struct {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
  long tm_gmtoff;
  const char *tm_zone;
} bionic_tm;

static __thread bionic_tm tm_buf;
static void *to_btm(struct tm *t) {
  if (!t) return NULL;
  memcpy(&tm_buf, t, 9 * sizeof(int));
  tm_buf.tm_gmtoff = 0;
  tm_buf.tm_zone = "UTC";
  return &tm_buf;
}
void *b_localtime(const time_t *t) { return to_btm(localtime(t)); }
void *b_gmtime(const time_t *t) { return to_btm(gmtime(t)); }
void *b_localtime_r(const time_t *t, void *out) {
  struct tm tmp; localtime_r(t, &tmp);
  memset(out, 0, sizeof(bionic_tm)); memcpy(out, &tmp, 9 * sizeof(int));
  ((bionic_tm *)out)->tm_zone = "UTC";
  return out;
}
void *b_gmtime_r(const time_t *t, void *out) {
  struct tm tmp; gmtime_r(t, &tmp);
  memset(out, 0, sizeof(bionic_tm)); memcpy(out, &tmp, 9 * sizeof(int));
  ((bionic_tm *)out)->tm_zone = "UTC";
  return out;
}

int b_clock_gettime(int id, struct timespec *ts) {
  /* bionic: 0 = REALTIME, everything else treated as MONOTONIC */
  return clock_gettime(id == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, ts);
}

long b_sysconf(int name) {
  switch (name) {
    case 0x06: return 100;                       /* _SC_CLK_TCK */
    case 0x27: case 0x28: return 4096;           /* _SC_PAGESIZE */
    case 0x60: case 0x61: return 3;              /* _SC_NPROCESSORS_CONF/ONLN (core 3 is the OS's) */
    case 0x62: return (3ULL << 30) / 4096;       /* _SC_PHYS_PAGES */
  }
  debugPrintf("[libc] sysconf(0x%x) unknown\n", name);
  return -1;
}

/* ---------------------------------------------------------------- misc */

int *b_errno(void) { return &errno; }
size_t b_ctype_get_mb_cur_max(void) { return 1; }

void b_abort(void) { fatal_error("game called abort()"); }
void b_exit(int c) { debugPrintf("[libc] game called exit(%d)\n", c); log_close(); exit(c); }
void b_stack_chk_fail(void) { fatal_error("stack smashing detected"); }
void b_android_set_abort_message(const char *m) { debugPrintf("[abort] %s\n", m); }
int b_cxa_atexit(void (*f)(void *), void *a, void *d) { return 0; }
void b_cxa_finalize(void *d) {}

int b_isinf(double x) { return __builtin_isinf(x); }
int b_isnan(double x) { return __builtin_isnan(x); }
int b_isinff(float x) { return __builtin_isinf(x); }
int b_isnanf(float x) { return __builtin_isnan(x); }
int b_signbit(double x) { return __builtin_signbit(x); }
int b_signbitf(float x) { return __builtin_signbit(x); }

/* locale: the game only ever uses the C locale */
void *b_newlocale(int m, const char *l, void *base) { return (void *)1; }
void *b_uselocale(void *l) { return (void *)1; }
void b_freelocale(void *l) {}
char *b_setlocale(int c, const char *l) { return "C"; }

#define L1(n, f) int b_##n##_l(int c, void *l) { return f(c); }
L1(isdigit, isdigit) L1(islower, islower) L1(isupper, isupper) L1(isxdigit, isxdigit)
L1(tolower, tolower) L1(toupper, toupper)
#define W1(n, f) int b_##n##_l(wint_t c, void *l) { return f(c); }
W1(iswalpha, iswalpha) W1(iswblank, iswblank) W1(iswcntrl, iswcntrl) W1(iswdigit, iswdigit)
W1(iswlower, iswlower) W1(iswprint, iswprint) W1(iswpunct, iswpunct) W1(iswspace, iswspace)
W1(iswupper, iswupper) W1(iswxdigit, iswxdigit)
wint_t b_towlower_l(wint_t c, void *l) { return towlower(c); }
wint_t b_towupper_l(wint_t c, void *l) { return towupper(c); }
int b_strcoll_l(const char *a, const char *b, void *l) { return strcoll(a, b); }
size_t b_strxfrm_l(char *d, const char *s, size_t n, void *l) { return strxfrm(d, s, n); }
int b_wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { return wcscoll(a, b); }
size_t b_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { return wcsxfrm(d, s, n); }
size_t b_strftime_l(char *s, size_t n, const char *f, const void *tm, void *l) { return strftime(s, n, f, tm); }
long long b_strtoll_l(const char *s, char **e, int b, void *l) { return strtoll(s, e, b); }
unsigned long long b_strtoull_l(const char *s, char **e, int b, void *l) { return strtoull(s, e, b); }
long double b_strtold_l(const char *s, char **e, void *l) { return strtold(s, e); }

/* processes, users, terminals: not available, report failure */
int b_ret0(void) { return 0; }
int b_retm1(void) { errno = ENOSYS; return -1; }
void *b_retnull(void) { return NULL; }
int b_getpid(void) { return 1000; }
int b_gethostname(char *n, size_t l) { strncpy(n, "switch", l); return 0; }
char *b_getenv(const char *n) { return NULL; }

/* toupper/tolower as bionic implements them (ASCII only, no locale). newlib's go through
 * __locale_ctype_ptr on every call; the engine calls toupper per character in its
 * case-insensitive string hash (pig::core::StringDB::CStrHashFunction), which showed in every
 * hardware profile (2% of the shop). Same results for every input as on Android. */
int b_toupper_ascii(int c) { return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c; }
int b_tolower_ascii(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }

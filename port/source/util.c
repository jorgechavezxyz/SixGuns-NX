#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>
#include "config.h"
#include "util.h"

static FILE *logf_;
static Mutex log_mtx;

/* Log lines collect in memory and a background thread writes them out every
 * half second. Writing + flushing each line on the calling thread stalled the
 * game on the Switch's SD card (stutters on hardware only). Crashes and
 * fatal errors flush synchronously. */
#define LOGBUF (1024 * 1024)
static char logbuf[LOGBUF];
static size_t loglen;
static Thread log_thread;
static volatile int log_thread_run;
/* Woken early when the buffer is half full, so a burst of lines (profiler
 * reports, hitch dumps) never makes the caller write to the SD card itself:
 * the hardware profile of 2026-11-12 had 8 s of main-thread fflush there. */
static UEvent log_wake;
static unsigned long log_dropped;

static void log_write_locked(void) {
  if (logf_ && loglen) { fwrite(logbuf, 1, loglen, logf_); fflush(logf_); }
  loglen = 0;
}

static void log_thread_main(void *arg) {
  (void)arg;
  while (log_thread_run) {
    waitSingle(waiterForUEvent(&log_wake), 500000000ULL);
    static char out[LOGBUF];
    size_t n;
    mutexLock(&log_mtx);
    n = loglen;
    memcpy(out, logbuf, n);
    loglen = 0;
    unsigned long dropped = log_dropped;
    log_dropped = 0;
    mutexUnlock(&log_mtx);
    if (n && logf_) { fwrite(out, 1, n, logf_); fflush(logf_); }
    if (dropped && logf_) { fprintf(logf_, "[log] %lu lines dropped (writer behind)\n", dropped); fflush(logf_); }
  }
}

void log_init(void) {
  mutexInit(&log_mtx);
  ueventCreate(&log_wake, true);
  /* Keep the previous run's log: a launch that fails often only makes sense
   * next to the one before it. */
  remove(PORT_ROOT "/log.prev.txt");
  rename(LOG_PATH, PORT_ROOT "/log.prev.txt");
  logf_ = fopen(LOG_PATH, "w");
  /* Priority 0x3b, the lowest a homebrew thread may have: the 0x3f used before
   * is refused on hardware (Ryubing accepts it), so the Switch never had this
   * writer and every log line was written + flushed by the game's own thread
   * (hardware profile 2026-11-13: ~10% of a 15 s level load). */
  log_thread_run = 1;
  Result rc = threadCreate(&log_thread, log_thread_main, NULL, NULL, 0x4000, 0x3b, 2);
  if (R_FAILED(rc)) rc = threadCreate(&log_thread, log_thread_main, NULL, NULL, 0x4000, 0x2c, -2);
  if (R_SUCCEEDED(rc)) {rc = threadStart(&log_thread);if(R_FAILED(rc)) threadClose(&log_thread);}
  if (R_FAILED(rc)) log_thread_run = 0;
  debugPrintf("[log] writer thread %s (rc 0x%x)\n", log_thread_run ? "running" : "NOT running, writing on the caller", rc);
}

/* Synchronous flush; usable from the crash handler (doesn't wait forever on
 * a lock a crashed thread may hold). */
void log_flush(void) {
  int locked = 0;
  for (int i = 0; i < 100 && !(locked = mutexTryLock(&log_mtx)); i++) svcSleepThread(1000000ULL);
  log_write_locked();
  /* Commit to the card, not only to newlib: an emulator keeps unflushed file
   * writes in a host buffer, and a power-off loses them on hardware. */
  if (logf_) fsync(fileno(logf_));
  if (locked) mutexUnlock(&log_mtx);
}

void log_close(void) {
  if (log_thread_run) {
    log_thread_run = 0;
    ueventSignal(&log_wake);
    threadWaitForExit(&log_thread);
    threadClose(&log_thread);
  }
  log_flush();
  /* engine threads keep running (teardown is skipped): close under the log lock */
  mutexLock(&log_mtx);
  if (logf_) fclose(logf_);
  logf_ = NULL;
  mutexUnlock(&log_mtx);
}

void debugPrintf(const char *fmt, ...) {
  char line[2048];
  int n = snprintf(line, sizeof(line), "%7.3f ", (double)armTicksToNs(armGetSystemTick()) / 1e9);
  va_list va;
  va_start(va, fmt);
  int m = vsnprintf(line + n, sizeof(line) - n, fmt, va);
  va_end(va);
  size_t len = n + (m < 0 ? 0 : (size_t)m < sizeof(line) - n ? (size_t)m : sizeof(line) - n - 1);
  mutexLock(&log_mtx);
  if (!log_thread_run) log_write_locked();
  if (loglen + len <= LOGBUF) { memcpy(logbuf + loglen, line, len); loglen += len; }
  else log_dropped++; /* the writer is behind by a full megabyte: drop rather than stall the game */
  if (!log_thread_run) log_write_locked();
  else if (loglen > LOGBUF / 2) ueventSignal(&log_wake);
  mutexUnlock(&log_mtx);
  fwrite(line + n, 1, len - n, stdout); /* visible over nxlink stdio when launched with nxlink -s */
}

void (*fatal_cleanup)(void);

void fatal_error(const char *fmt, ...) {
  char buf[1024];
  va_list va;
  va_start(va, fmt);
  vsnprintf(buf, sizeof(buf), fmt, va);
  va_end(va);
  debugPrintf("FATAL: %s\n", buf);
  { extern void memfile_commit_all(unsigned), memfile_drain(void); memfile_commit_all(2000); memfile_drain(); } /* never a stream mid-write (another thread may be saving) */
  log_close();

  /* Release the screen if EGL owns it, then draw a text console ourselves.
   * (The system error applet crashed when launched from here.) */
  if (fatal_cleanup) fatal_cleanup();
  fatal_cleanup = NULL;

  consoleInit(NULL);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  printf("\n  Six Guns loader error\n  =====================\n\n  %s\n\n", buf);
  printf("  Details: sdmc:/switch/sixguns/log.txt\n\n  Press + to exit.\n");
  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
  }
  consoleExit(NULL);
  /* End the whole process (back to the HOME menu) rather than returning to
   * hbloader: the engine's threads, code mapping and GPU memory would stay
   * behind in the reused process and break the next launch. */
  svcExitProcess();
}

static const struct { const char *from, *to; } remaps[] = {
  { "/data/data/" PACKAGE_NAME "/", PORT_ROOT "/internal/" },
  { "/data/user/0/" PACKAGE_NAME "/", PORT_ROOT "/internal/" },
  { "/storage/emulated/0/", PORT_ROOT "/sdcard/" },
  { "/mnt/sdcard/", PORT_ROOT "/sdcard/" },
  { "/sdcard/", PORT_ROOT "/sdcard/" },
};

const char *remap_path(const char *path, char *buf, size_t bufsize) {
  if (!path) return path;
  /* The engine reads its own .so from the app's native library dir. */
  const char *base = strrchr(path, '/');
  if (base && strcmp(base, "/libAndroid.so") == 0) return SO_PATH;
  for (size_t i = 0; i < sizeof(remaps) / sizeof(*remaps); i++) {
    size_t n = strlen(remaps[i].from);
    if (strncmp(path, remaps[i].from, n) == 0) {
      snprintf(buf, bufsize, "%s%s", remaps[i].to, path + n);
      return buf;
    }
  }
  return path;
}

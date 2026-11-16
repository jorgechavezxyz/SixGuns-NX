/* Sampling profiler for the main (game/render) thread. Marker: `profile`.
 *
 * A high-priority thread wakes every ~2 ms, pauses the main thread
 * (svcSetThreadActivity), reads its registers (svcGetThreadContext3), walks its
 * frame-pointer chain while it is still paused, resumes it, and appends the sample
 * {pc, lr, return addresses} to a ring buffer. Nothing else happens on the sampler
 * side: names are resolved later, on the main thread, from a sorted copy of the
 * engine's symbol table (binary search), after the ring has been swapped out under
 * a short lock, so a report never blocks the sampler or holds a lock while it works.
 *
 * Safety of the stack walk: frames are read only inside the main thread's stack
 * mapping (bounds from svcQueryMemory, measured once), with an overflow-free bound
 * check, strictly increasing frame addresses and 8-byte alignment; a return address
 * is kept only if it lies in code (the engine image or the port's own text).
 *
 * Output (lines start with [prof]):
 *   - "section <screen>": for every screen the game shows (profiler_section, called
 *     by social.c when the active menu changes) and every 1800 frames: top functions
 *     by sample share, then the top call chains (functions, innermost first).
 *   - "hitch <ms> ms in <screen>": for a frame whose game work (between
 *     profiler_frame_begin/end, i.e. the measured t0..t1) took over 100 ms: the
 *     functions and top 3 chains of exactly those samples.
 * Engine frames print as names; port/newlib/libnx/Mesa frames as `loader 0x...`
 * (resolve with tools/scripts/prof_symbolize.py and tools/builds/sixguns-<id>.elf).
 * A frame marked `~` comes from the link register: the caller of a leaf function
 * (it is used only when the sampled pc is not in that same function). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "config.h"
#include "so_util.h"
#include "util.h"

enum { RET = 7 };            /* return addresses kept from the frame-pointer chain */
enum { RING = 16384 };       /* samples per report window (~33 s at 2 ms) */
enum { FRAME_MAX = 1024 };   /* samples kept for one frame */
enum { DEPTH = 1 + 1 + RET }; /* folded chain: leaf, optional lr caller, returns */

typedef struct { u64 pc, lr, ret[RET]; } sample_t;

static sample_t *ring[2];
static u32 ring_n[2], ring_drop[2];
static int ring_cur;
static sample_t *frame_buf, *frame_snap;
static u32 frame_n, snap_n, frame_seen, snap_seen;
static int frame_on;

static Thread thr;
static Handle main_thread;
static volatile int running;
static Mutex lock;
static so_module *mod;
static u64 stack_lo, stack_hi, code_lo, code_hi;
static char section[64] = "startup";

/* ---------------------------------------------------------------- engine symbols */

typedef struct { u64 addr, size; u32 name; } sym_t;
static sym_t *syms;
static size_t nsyms;

static int sym_cmp(const void *a, const void *b) {
  u64 x = ((const sym_t *)a)->addr, y = ((const sym_t *)b)->addr;
  return x < y ? -1 : x > y;
}

static void build_symbols(void) {
  size_t n = 0;
  for (size_t i = 0; i < mod->num_dynsym; i++)
    if (mod->dynsym[i].st_shndx && mod->dynsym[i].st_value && (mod->dynsym[i].st_info & 0xf) == 2 /* STT_FUNC */) n++;
  syms = malloc(n * sizeof(*syms));
  if (!syms) return;
  for (size_t i = 0; i < mod->num_dynsym; i++) {
    const Elf64_Sym *s = &mod->dynsym[i];
    if (!s->st_shndx || !s->st_value || (s->st_info & 0xf) != 2 /* STT_FUNC */) continue;
    syms[nsyms].addr = mod->base + s->st_value;
    syms[nsyms].size = s->st_size;
    syms[nsyms].name = s->st_name;
    nsyms++;
  }
  qsort(syms, nsyms, sizeof(*syms), sym_cmp);
}

static int in_engine(u64 a) { return a >= mod->base && a < mod->base + mod->image_size; }

/* Start address of the engine function containing a (0 if none), and its name. */
static u64 sym_at(u64 a, const char **name) {
  if (!syms || !in_engine(a)) return 0;
  size_t lo = 0, hi = nsyms;
  while (lo < hi) { size_t mid = (lo + hi) / 2; if (syms[mid].addr <= a) lo = mid + 1; else hi = mid; }
  if (!lo) return 0;
  /* outside the symbol's extent: unexported engine code, not part of that function */
  if (syms[lo - 1].size && a >= syms[lo - 1].addr + syms[lo - 1].size) return 0;
  if (name) *name = mod->dynstr + syms[lo - 1].name;
  return syms[lo - 1].addr;
}

/* Aggregation key of an address: the engine function start, else the address
 * itself (port code; 64-byte block for a sampled pc, exact for return addresses). */
static u64 fold(u64 a, int leaf) {
  u64 f = sym_at(a, NULL);
  if (f) return f;
  return leaf ? (a & ~0x3fULL) : a;
}

/* ---------------------------------------------------------------- sampling */

static int is_code(u64 a) {
  if (a & 3) return 0;
  if (in_engine(a)) return 1;
  return a >= code_lo && a < code_hi;
}

static void walk(const ThreadContext *ctx, sample_t *s) {
  memset(s, 0, sizeof(*s));
  s->pc = ctx->pc.x;
  if (is_code(ctx->lr)) s->lr = ctx->lr;
  if (!stack_lo) { /* the main thread's stack mapping, measured once */
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, ctx->sp)) || !(mi.perm & Perm_R) || mi.size < 16) return;
    stack_lo = mi.addr;
    stack_hi = mi.addr + mi.size;
  }
  u64 fp = ctx->fp, prev = ctx->sp;
  int n = 0;
  /* overflow-free bounds: fp in [stack_lo, stack_hi - 16], aligned, above sp/previous frame */
  while (n < RET && fp >= prev && fp >= stack_lo && fp < stack_hi && stack_hi - fp >= 16 && !(fp & 7)) {
    const u64 *f = (const u64 *)fp;
    u64 next = f[0], ret = f[1];
    if (!is_code(ret)) break;
    s->ret[n++] = ret;
    if (next <= fp || next >= stack_hi) break;
    prev = fp;
    fp = next;
  }
}

static void sample_loop(void *arg) {
  (void)arg;
  while (running) {
    svcSleepThread(2000000); /* 2 ms */
    if (R_FAILED(svcSetThreadActivity(main_thread, ThreadActivity_Paused))) continue;
    ThreadContext ctx;
    sample_t s;
    Result rc = svcGetThreadContext3(&ctx, main_thread);
    if (R_SUCCEEDED(rc)) walk(&ctx, &s); /* read the stack while the thread is still paused */
    svcSetThreadActivity(main_thread, ThreadActivity_Runnable);
    if (R_FAILED(rc)) continue;
    mutexLock(&lock); /* taken only after the main thread runs again */
    int c = ring_cur;
    if (ring_n[c] < RING) ring[c][ring_n[c]++] = s;
    else ring_drop[c]++;
    if (frame_on) { if (frame_n < FRAME_MAX) frame_buf[frame_n++] = s; frame_seen++; }
    mutexUnlock(&lock);
  }
}

static double hitch_ms = 100.0;

void profiler_init(so_module *engine) {
  FILE *f = fopen(PORT_ROOT "/profile", "r");
  if (!f) return;
  /* Optional content: the hitch threshold in ms (default 100), e.g. "30" to break down
   * shorter stalls such as shop tab rebuilds in the emulator. */
  { int t = 0; if (fscanf(f, "%d", &t) == 1 && t >= 20 && t <= 1000) hitch_ms = t; }
  fclose(f);
  mod = engine;
  ring[0] = malloc(RING * sizeof(sample_t));
  ring[1] = malloc(RING * sizeof(sample_t));
  frame_buf = malloc(FRAME_MAX * sizeof(sample_t));
  frame_snap = malloc(FRAME_MAX * sizeof(sample_t));
  if (!ring[0] || !ring[1] || !frame_buf || !frame_snap) {
    debugPrintf("[prof] not enough memory for the profiler\n");
    return;
  }
  build_symbols();
  main_thread = threadGetCurHandle();
  mutexInit(&lock);
  { /* the port's own code (loader, newlib, libnx, Mesa): one executable mapping */
    MemoryInfo mi; u32 pi;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)&profiler_init)) && (mi.perm & Perm_X)) {
      code_lo = mi.addr;
      code_hi = mi.addr + mi.size;
    }
  }
  running = 1;
  /* priority 0x20 is above the game threads (0x2c); core 2 keeps it off the main core */
  if (R_FAILED(threadCreate(&thr, sample_loop, NULL, NULL, 0x4000, 0x20, 2)) || R_FAILED(threadStart(&thr))) {
    running = 0;
    debugPrintf("[prof] could not start the sampler thread\n");
    return;
  }
  /* For loader/library pages: elf address = page - (this address - its elf address). */
  debugPrintf("[prof] sampling the main thread every 2 ms with call chains (marker `profile`); "
              "profiler_init is at %p, port code 0x%lx-0x%lx, %lu engine symbols\n",
              (void *)&profiler_init, (unsigned long)code_lo, (unsigned long)code_hi, (unsigned long)nsyms);
}

/* ---------------------------------------------------------------- analysis (main thread, no lock) */

/* The folded chain of a sample: leaf function, the link-register caller when the
 * pc is in a leaf (lr outside the pc's own function), then the return addresses. */
static int fold_chain(const sample_t *s, u64 *out, u8 *is_lr) {
  int n = 0;
  u64 leaf = fold(s->pc, 1);
  out[n] = leaf; is_lr[n++] = 0;
  if (s->lr) {
    u64 lf = fold(s->lr, 0);
    /* same function as pc (stale lr from an earlier call) -> skip; for port code
     * (no symbols at run time) "same function" = within 1 KB */
    int same = sym_at(s->pc, NULL) ? (lf == leaf) : (s->lr > s->pc ? s->lr - s->pc : s->pc - s->lr) < 0x400;
    if (!same && (!s->ret[0] || fold(s->ret[0], 0) != lf)) { out[n] = lf; is_lr[n++] = 1; }
  }
  for (int i = 0; i < RET && s->ret[i] && n < DEPTH; i++) {
    u64 r = fold(s->ret[i], 0);
    if (r != out[n - 1]) { out[n] = r; is_lr[n++] = 0; }
  }
  return n;
}

static u64 chain_hash(const u64 *c, int n) {
  u64 h = 1469598103934665603ULL;
  for (int i = 0; i < n; i++) h = (h ^ (c[i] >> 2)) * 1099511628211ULL;
  return h ^ (u64)n;
}

static void frame_name(u64 key, int lr, char *out, size_t sz) {
  const char *nm = NULL;
  if (in_engine(key) && sym_at(key, &nm) == key && nm)
    snprintf(out, sz, "%s%.80s", lr ? "~" : "", nm);
  else if (in_engine(key))
    snprintf(out, sz, "%sengine+0x%lx", lr ? "~" : "", (unsigned long)(key - mod->base));
  else
    snprintf(out, sz, "%sloader 0x%lx", lr ? "~" : "", (unsigned long)key);
}

typedef struct { u64 key; u32 n; } fcount_t;
typedef struct { u64 c[DEPTH]; u8 lr[DEPTH]; u8 len; u32 n; } ccount_t;

/* Counts samples by function and by folded chain, prints the top entries. */
static void analyse(const sample_t *s, u32 n, int top_f, int top_c, const char *head) {
  enum { FB = 1 << 14, CB = 1 << 13 };
  static fcount_t *ft;
  static ccount_t *ct;
  if (!ft) ft = malloc(FB * sizeof(*ft));
  if (!ct) ct = malloc(CB * sizeof(*ct));
  if (!ft || !ct || !n) return;
  memset(ft, 0, FB * sizeof(*ft));
  memset(ct, 0, CB * sizeof(*ct));
  u32 fdrop = 0, cdrop = 0;
  for (u32 i = 0; i < n; i++) {
    u64 c[DEPTH]; u8 lr[DEPTH];
    int len = fold_chain(&s[i], c, lr);
    u32 h = (u32)((c[0] >> 2) * 0x9E3779B97F4A7C15ULL >> 40) & (FB - 1);
    for (int p = 0;; p++, h = (h + 1) & (FB - 1)) {
      if (p == FB) { fdrop++; break; }
      if (ft[h].n && ft[h].key == c[0]) { ft[h].n++; break; }
      if (!ft[h].n) { ft[h].key = c[0]; ft[h].n = 1; break; }
    }
    u64 hh = chain_hash(c, len);
    u32 k = (u32)(hh >> 20) & (CB - 1);
    for (int p = 0;; p++, k = (k + 1) & (CB - 1)) {
      if (p == CB) { cdrop++; break; }
      if (ct[k].n && ct[k].len == len && !memcmp(ct[k].c, c, len * sizeof(u64))) { ct[k].n++; break; }
      if (!ct[k].n) { memcpy(ct[k].c, c, len * sizeof(u64)); memcpy(ct[k].lr, lr, len); ct[k].len = len; ct[k].n = 1; break; }
    }
  }
  debugPrintf("%s\n", head);
  for (int t = 0; t < top_f; t++) {
    int best = -1;
    for (int j = 0; j < FB; j++) if (ft[j].n && (best < 0 || ft[j].n > ft[best].n)) best = j;
    if (best < 0) break;
    char nm[100];
    frame_name(ft[best].key, 0, nm, sizeof(nm));
    if (in_engine(ft[best].key))
      debugPrintf("[prof] %5.1f%% %6u  %s (engine +0x%lx)\n", 100.0 * ft[best].n / n, ft[best].n, nm,
                  (unsigned long)(ft[best].key - mod->base));
    else
      debugPrintf("[prof] %5.1f%% %6u  %s\n", 100.0 * ft[best].n / n, ft[best].n, nm);
    ft[best].n = 0;
  }
  /* code without run-time names (port, libs, unexported engine code) is counted in
   * 64-byte blocks; print every remaining block with >= 0.1% so prof_symbolize.py can
   * merge them into whole functions */
  for (int j = 0; j < FB; j++)
    if (ft[j].n && ft[j].n * 1000u >= n && !sym_at(ft[j].key, NULL))
      debugPrintf("[prof]   block %5.1f%% %6u  %s 0x%lx\n", 100.0 * ft[j].n / n, ft[j].n,
                  in_engine(ft[j].key) ? "engine" : "loader", (unsigned long)ft[j].key);
  for (int t = 0; t < top_c; t++) {
    int best = -1;
    for (int j = 0; j < CB; j++) if (ct[j].n && (best < 0 || ct[j].n > ct[best].n)) best = j;
    if (best < 0) break;
    char line[1000];
    int len = snprintf(line, sizeof(line), "[prof]   chain %5.1f%% %u:", 100.0 * ct[best].n / n, ct[best].n);
    for (int i = 0; i < ct[best].len && len < (int)sizeof(line) - 110; i++) {
      char nm[100];
      frame_name(ct[best].c[i], ct[best].lr[i], nm, sizeof(nm));
      len += snprintf(line + len, sizeof(line) - len, "%s%s", i ? " <- " : " ", nm);
    }
    debugPrintf("%s\n", line);
    ct[best].n = 0;
  }
  if (fdrop || cdrop) debugPrintf("[prof]   (tables full: %u/%u samples not counted)\n", fdrop, cdrop);
}

/* Swaps the ring under a short lock and analyses the samples taken since the last
 * report. min_samples: below this the window is dropped without printing (so a
 * screen shown briefly is not charged to the next one). */
static void flush(const char *what, u32 min_samples) {
  if (!running) return;
  mutexLock(&lock);
  int c = ring_cur;
  ring_cur ^= 1;
  ring_n[ring_cur] = 0;
  ring_drop[ring_cur] = 0;
  u32 n = ring_n[c], drop = ring_drop[c];
  mutexUnlock(&lock);
  if (n < min_samples) return;
  char head[200];
  snprintf(head, sizeof(head), "[prof] %s: %u samples (%.1f s%s), top functions on the main thread:", what, n,
           (n + drop) * 0.002, drop ? ", window overflowed" : "");
  analyse(ring[c], n, 40, 12, head);
}

void profiler_report(void) {
  char what[96];
  snprintf(what, sizeof(what), "section %s (periodic)", section);
  flush(what, 1);
}

/* The active screen changed (social.c): report what ran on the previous screen,
 * then start a new window labelled with the new one. */
void profiler_section(const char *name) {
  if (!running) return;
  char what[96];
  snprintf(what, sizeof(what), "section %s", section);
  flush(what, 100);
  snprintf(section, sizeof(section), "%s", (name && *name) ? name : "gameplay");
}

/* Frame window = the game's own work for one frame (main.c: begin at t0, end at t1). */
void profiler_frame_begin(void) {
  if (!running) return;
  mutexLock(&lock);
  frame_n = 0;
  frame_seen = 0;
  frame_on = 1;
  mutexUnlock(&lock);
}

void profiler_frame_end(void) {
  if (!running) return;
  mutexLock(&lock);
  frame_on = 0;
  snap_n = frame_n;
  snap_seen = frame_seen;
  memcpy(frame_snap, frame_buf, snap_n * sizeof(sample_t));
  mutexUnlock(&lock);
}

/* Called once per frame with that frame's game time (t1 - t0). Over 100 ms: print
 * where those samples were. Rate-limited to 200 reports. */
void profiler_frame(double ms) {
  if (!running) return;
  static int reports;
  if (ms <= hitch_ms || !snap_n || reports >= 200) return;
  reports++;
  char head[160];
  if (snap_seen > snap_n)
    snprintf(head, sizeof(head), "[prof] hitch %.0f ms in %s, first %u of %u samples:", ms, section, snap_n, snap_seen);
  else
    snprintf(head, sizeof(head), "[prof] hitch %.0f ms in %s, %u samples:", ms, section, snap_n);
  analyse(frame_snap, snap_n, 6, 3, head);
  snap_n = 0;
}

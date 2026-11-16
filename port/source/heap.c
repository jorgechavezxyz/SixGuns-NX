/* Heap setup (replaces libnx's weak __libnx_initheap, runs before main).
 *
 * Under title override, hbloader keeps one game process alive across
 * homebrew launches and hands each NRO the same heap. If an earlier launch
 * ended without tearing everything down (a crash, or returning to hbloader
 * with the engine still mapped as code), parts of that heap are still
 * borrowed: code-mapped pages read as perm 0 and fault on first touch, and
 * old GPU buffers stay device-mapped. Seen on hardware 2026-11-10: the first
 * big malloc landed in a 27 MB hole left by the previous run's engine image.
 *
 * So, instead of trusting the whole range, use the largest run of plain
 * read/write heap pages (no attributes) inside it. heap_report() logs what
 * was skipped once logging is up. */
#include <switch.h>
#include "util.h"

extern char *fake_heap_start, *fake_heap_end;
extern size_t __nx_heap_size;

static u64 given_addr, given_size, skipped;

void __libnx_initheap(void) {
  void *addr = NULL;
  size_t size = 0;
  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    u64 avail = 0, used = 0;
    svcGetInfo(&avail, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (avail > used + 0x200000) size = (avail - used - 0x200000) & ~0x1FFFFFULL;
    if (size == 0) size = 0x2000000 * 16;
    if (R_FAILED(svcSetHeapSize(&addr, size))) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }
  given_addr = (u64)addr;
  given_size = size;

  /* Largest clean run inside [addr, addr + size). */
  u64 best = (u64)addr, best_len = 0, end = (u64)addr + size, a = (u64)addr;
  u64 run = 0, run_len = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 rs = mi.addr < a ? a : mi.addr;
    u64 re = mi.addr + mi.size > end ? end : mi.addr + mi.size;
    int clean = mi.type == MemType_Heap && mi.perm == Perm_Rw && mi.attr == 0;
    if (clean) {
      if (!run_len) run = rs;
      run_len = re - run;
      if (run_len > best_len) { best = run; best_len = run_len; }
    } else {
      run_len = 0;
    }
    if (re <= a) break;
    a = re;
  }
  if (best_len == 0) { best = (u64)addr; best_len = size; } /* query failed: old behaviour */
  skipped = size - best_len;
  fake_heap_start = (char *)best;
  fake_heap_end = (char *)(best + best_len);
}

void heap_report(void) {
  if (skipped)
    debugPrintf("[heap] %llu MB of the %llu MB heap at %p was still in use by an earlier launch; using %p..%p\n",
                (unsigned long long)(skipped >> 20), (unsigned long long)(given_size >> 20), (void *)given_addr,
                fake_heap_start, fake_heap_end);
}

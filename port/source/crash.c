/* Crash reporter: libnx calls __libnx_exception_handler when a thread faults.
 * We log the registers and name the engine function at PC/LR, then exit. */
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>
#include "util.h"

u32 __nx_exception_ignoredebug = 1;   /* handle crashes even with a debugger attached */
static u8 exception_stack[0x10000] __attribute__((aligned(16)));
void *__nx_exception_stack = exception_stack;
u64 __nx_exception_stack_size = sizeof(exception_stack);

extern const char *thread_entry_name(void *addr);  /* "symbol+off" in the engine */

static const char *describe(u64 type) {
  switch (type) {
    case ThreadExceptionDesc_InstructionAbort: return "instruction abort";
    case ThreadExceptionDesc_MisalignedPC: return "misaligned PC";
    case ThreadExceptionDesc_MisalignedSP: return "misaligned SP";
    case ThreadExceptionDesc_SError: return "SError";
    case ThreadExceptionDesc_BadSVC: return "bad SVC";
    case ThreadExceptionDesc_Trap: return "trap";
    case ThreadExceptionDesc_Other: return "data abort / other";
    default: return "unknown";
  }
}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  debugPrintf("\n=== CRASH: %s (desc 0x%x) ===\n", describe(ctx->error_desc), ctx->error_desc);
  debugPrintf("PC  %016llx  %s\n", (unsigned long long)ctx->pc.x, thread_entry_name((void *)ctx->pc.x));
  debugPrintf("LR  %016llx  %s\n", (unsigned long long)ctx->lr.x, thread_entry_name((void *)ctx->lr.x));
  debugPrintf("SP  %016llx  FAR %016llx  ESR %08x\n", (unsigned long long)ctx->sp.x,
              (unsigned long long)ctx->far.x, ctx->esr);
  /* x0..x28 (cpu_gprs has 29 entries; fp/lr/sp are printed separately) */
  for (int i = 0; i < 29; i += 4) {
    char line[160];
    int n = snprintf(line, sizeof(line), "x%-2d", i);
    for (int k = i; k < i + 4 && k < 29; k++)
      n += snprintf(line + n, sizeof(line) - n, " %016llx", (unsigned long long)ctx->cpu_gprs[k].x);
    debugPrintf("%s\n", line);
  }
  /* Walk the frame-pointer chain a few levels for a rough backtrace, reading only inside
   * the crashed thread's readable stack mapping (a bad fp must not fault in here). */
  u64 fp = ctx->fp.x, lo = 0, hi = 0;
  {
    MemoryInfo mi; u32 pi;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx->sp.x)) && (mi.perm & Perm_R)) { lo = mi.addr; hi = mi.addr + mi.size; }
  }
  for (int i = 0; i < 12 && fp >= lo && fp < hi && hi - fp >= 16 && !(fp & 7); i++) {
    u64 ret = ((u64 *)fp)[1], next = ((u64 *)fp)[0];
    debugPrintf("  #%d %016llx %s\n", i, (unsigned long long)ret, thread_entry_name((void *)ret));
    if (next <= fp) break;
    fp = next;
  }
  /* Give queued saves up to 3 s to reach the card before the process ends. */
  { extern void memfile_emergency_drain(unsigned); memfile_emergency_drain(3000); }
  log_flush(); /* not log_close: the crashed thread may be the log thread */
  /* Kill the process outright: exit() from a crashed engine hangs on its
   * threads, and returning to hbloader leaves the heap half-mapped. */
  svcExitProcess();
}

/* Ryujinx's crash report reads a thread-name pointer at TLS offset 0x198 (the
 * Nintendo SDK layout). In homebrew that offset falls inside libnx's path
 * buffer, so the report itself faults and recurses until Ryujinx's own stack
 * overflows, hiding the real crash. An initialized TLS array of valid name
 * pointers placed first in .tdata covers that offset. Harmless on hardware. */
__attribute__((used)) __thread const char *ryujinx_thread_name_slots[80] = { [0 ... 79] = "sixguns" };

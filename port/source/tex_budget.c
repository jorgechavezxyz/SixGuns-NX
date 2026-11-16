/* Texture-streaming budget (walking stutter).
 *
 * pig::video::TextureLoader::UpdateStreaming (called every frame from
 * Game::FrameRender) first finalizes every texture whose background load has
 * finished: for each queued job it asks WorkerThread::GetFinishedJob(job) and
 * keeps going while that says yes, creating the GL texture each time (Mesa
 * allocates GPU memory over IPC, the port decodes ETC1). When several loads
 * finished together -- walking into a new area -- they were all finalized in
 * the same frame: the 120-250 ms hitches in the hardware profile
 * (svcSendSyncRequest, etc1_rows, memcpy).
 *
 * GetFinishedJob (only caller: UpdateStreaming) is replaced by this version:
 * same lookup/erase in the worker's finished list (vector at +0x18/+0x20), but
 * once ~4 ms have passed since the first finalized job of this frame it
 * answers "not finished yet", so the loop stops and the rest are finalized on
 * the next frames. At least one job per frame always goes through; loading
 * screens are not limited. Marker `no_tex_budget` restores the original. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include "config.h"
#include "util.h"

extern unsigned long port_frame_no;
static unsigned long budget_frame = ~0UL;
static u64 budget_t0;
static unsigned deferred_total, done_total;

int tex_get_finished_job(uint8_t *worker, void *job) {
  void **b = *(void ***)(worker + 0x18), **e = *(void ***)(worker + 0x20);
  void **p = b;
  while (p < e && *p != job) p++;
  if (p == e) return 0;
  extern const char *current_menu_class;
  int loading = strstr(current_menu_class, "LoadingScreen") != NULL;
  u64 now = armGetSystemTick();
  if (budget_frame != port_frame_no) {
    budget_frame = port_frame_no;
    budget_t0 = now;
  } else if (!loading && armTicksToNs(now - budget_t0) > 4000000ULL) {
    deferred_total++;
    return 0; /* finished, but finalize it next frame */
  }
  memmove(p, p + 1, (size_t)(e - p - 1) * sizeof(void *));
  *(void ***)(worker + 0x20) = e - 1;
  done_total++;
  return 1;
}

void tex_budget_stats(void) {
  if (deferred_total)
    debugPrintf("[perf] texture finalize: %u done, %u deferred to a later frame (4 ms budget)\n", done_total,
                deferred_total);
}

int tex_budget_enabled(void) {
  FILE *f = fopen(PORT_ROOT "/no_tex_budget", "r");
  if (f) { fclose(f); return 0; }
  return 1;
}

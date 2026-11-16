/* Turns off analytics inside the engine. The port runs offline, so nothing
 * could be sent anyway, but these systems still queue events on the SD card
 * (save/gv3/*.bin keeps growing) and spin worker threads retrying uploads.
 *
 * Each entry is an exported engine function whose return value no caller
 * uses; so_stub_function turns it into an immediate return. */
#include "so_util.h"
#include "util.h"

static const char *const stubs[] = {
  /* glotv3: Gameloft's analytics. Every event passes through AddEventInternal;
   * QueueForWriting is what persists the queue to save/gv3/. */
  "_ZN6glotv319TrackingManagerImpl16AddEventInternalENSt6__ndk110shared_ptrINS_5EventEEEb",
  "_ZN6glotv325SingletonMutexedProcessor15QueueForWritingENSt6__ndk110shared_ptrINS_5EventEEEbbb",
};

void telemetry_disable(so_module *mod) {
  int n = 0;
  for (size_t i = 0; i < sizeof(stubs) / sizeof(*stubs); i++)
    if (so_stub_function(mod, stubs[i]) == 0) n++;
  debugPrintf("[telemetry] stubbed %d of %d analytics functions\n", n, (int)(sizeof(stubs) / sizeof(*stubs)));
}

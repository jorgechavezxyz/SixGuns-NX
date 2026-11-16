/* In-place replacement of a few game strings. The game loads its text from the
 * OBB's `texts` file into the heap; once loaded, the exact original sentence is
 * found and overwritten with new wording of equal or shorter length (so no
 * pointers or allocations change). Only the English text is replaced. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>
#include "util.h"

extern char *fake_heap_start;

static const struct { const char *from, *to; } replacements[] = {
  /* tutorial step 0 */
  { "Drag the V-Pad to walk and run around.\nDrag it now!",
    "Use the joystick to control movement.\nTry it now!" },
  /* tutorial step 4 and the aim step */
  { "Touch or hold the Fire icon to attack.", "Press or hold ZR to attack." },
  { "Touch or hold the Aim icon to zoom in and strafe.", "Hold ZL to aim, zoom in and strafe." },
  /* tutorial step 3 (interact) and other touch-worded hints (2026-11-11). A is
   * the interact button (JoystickHW A -> PlayerCtrl::_A_ButtonAction); the game
   * itself has a controller wording "...you can press A to get on a horse." */
  { "The hand icon means you can interact with a person or object.",
    "Press A at the hand icon to interact with a person or object." },
  { "Touch the hand icon to talk with Jonas the Gambler.", "Press A to talk with Jonas the Gambler." },
  { "Tap the hand icon to pick flowers.", "Press A to pick flowers." },
  /* tutorial step 18 */
  { "A horseshoe icon indicates that you can get on a horse.", "A horseshoe icon means press A to mount a horse." },
  /* Help menu pages (Menu_Help shows STR_HELPSCREEN_001..053, the touch-worded set; the game's
   * own gamepad set 064..070 says R1/L1 and is never shown). Reworded for the Switch controls
   * (README "Controls"); 2026-11-12. */
  { "#F10Drag the V-Pad to walk and run around.\nDrag the screen to move the camera.\nPinch the screen to zoom in or out\nDouble tap the V-Pad to spin the camera 180 degrees.\nYou can also tap the Roll icon to evade enemy attacks.\n",
    "#F10Use the left stick to walk and run around.\nUse the right stick to move the camera.\nPress R3 to turn around 180 degrees.\nPress B to roll and evade enemy attacks.\n" },
  { "#F10Tap or hold the Attack icon to shoot.\nYou can change the equipped weapon in the Shop menu or by dragging the weapon icon on the screen.\nTap the weapon icon to reload.\n And remember: When near an enemy, you can tap the Kick icon to knock him down.\n",
    "#F10Press or hold ZR to shoot.\nYou can change the equipped weapon in the Shop menu, or press X for the next weapon.\nPress Y to reload.\nAnd remember: When near an enemy, you can press A to knock him down.\n" },
  { "#F10Touch or hold the Aim icon to zoom in and strafe. Weapon precision increases when aiming.\nYou can also aim while riding a horse or in cover.\n",
    "#F10Hold ZL to zoom in and strafe. Weapon precision increases when aiming.\nYou can also aim while riding a horse or in cover.\n" },
  { "#F10Tap the Cover icon to take cover and avoid incoming damage from your enemies.\nYou can move while in cover, as well as shoot and aim.\n",
    "#F10When the wall icon shows, press L3 (left stick) to take cover from enemies.\nYou can move while in cover, as well as shoot and aim.\n" },
  { "Some obstacles in the world are climbable: Place yourself next to one of them and move the V-Pad against it to climb it!\n",
    "Some obstacles in the world are climbable: Stand next to one of them and push the left stick toward it to climb it!\n" },
  { "#F10Tap the Horse icon to call your horse.\nDrag the screen and the V-Pad to control the direction and speed.\nYou can also tap the Spur icon to boost the speed until the horse runs out of stamina.\n The stamina bar auto-refills quickly when not on boost.\n",
    "#F10Press Left on the D-pad to call your horse (again to dismount).\nSteer with the left stick, look around with the right stick.\nPress A to spur the horse until it runs out of stamina.\n The stamina bar auto-refills quickly when not on boost.\n" },
  { "#F10Visit the Shop whenever you can to get more powerful weapons, faster horses and clothes that give you more protection.\nTap on the green Price icon to buy the chosen item.\nAnd remember: you can upgrade your weapons by tapping the Upgrade icon in the Shop, then double tapping the selected upgrade.\n",
    "#F10Visit the Shop whenever you can to get more powerful weapons, faster horses and clothes that give you more protection.\nSelect the green Price icon to buy the chosen item.\nAnd remember: you can upgrade your weapons with the Upgrade icon in the Shop, then press A twice on the selected upgrade.\n" },
  { "#F10Completing quests is the fastest way to obtain experience and gold coins.\nTo take a quest, tap on the mini-map to access the world map, and tap any quest icon.\nTo start the quest, travel to the quest location or tap the Fast Travel icon.\nSome missions change the time to day or night when started!\n",
    "#F10Completing quests is the fastest way to obtain experience and gold coins.\nTo take a quest, press - to open the world map, and select any quest icon.\nTo start the quest, travel to the quest location or select the Fast Travel icon.\nSome missions change the time to day or night when started!\n" },
  { "#F10Some quests ask you to gather plants and animals. To do so, get next to an animal corpse or a plant while in an active collection quest and tap the Interact icon.\nRoam the world to find new quests and secret places!\n",
    "#F10Some quests ask you to gather plants and animals. To do so, get next to an animal corpse or a plant while in an active collection quest and press A.\nRoam the world to find new quests and secret places!\n" },
  { "#F10Tap on the mini-map to open the full map. The map provides the locations of quests, roads and other important info. \n",
    "#F10Press - to open the full map. It shows the locations of quests, roads and other important info.\n" },
};

#define N_REPL (sizeof(replacements) / sizeof(*replacements))
_Static_assert(N_REPL <= 32, "replacement sets are tracked in 32-bit masks");
static int done[N_REPL];

/* Replaces in one readable stretch of heap; returns the number replaced. */
static int patch_range(char *start, char *end, unsigned todo) {
  int total = 0;
  for (size_t i = 0; i < N_REPL; i++) {
    if (!(todo & (1u << i))) continue;
    size_t flen = strlen(replacements[i].from) + 1; /* include NUL: exact string only */
    size_t tlen = strlen(replacements[i].to) + 1;
    if (tlen > flen) { debugPrintf("[text] replacement %zu is longer than the original, skipped\n", i); continue; }
    char *p = start;
    while (p < end && (p = memmem(p, end - p, replacements[i].from, flen))) {
      memcpy(p, replacements[i].to, tlen);
      p += flen;
      done[i]++;
      total++;
    }
  }
  return total;
}

int text_patch_mapped; /* strings replaced while the game read its texts (see b_mmap) */

/* Where the `texts` file sits in each OBB (stored, uncompressed). The engine
 * maps it out of the archive; b_mmap calls text_patch_mapping for every map so
 * the strings are replaced as they are read, once, instead of by scanning the
 * whole heap (that scan took ~2 s on hardware and ran at every tutorial step,
 * which was the freeze when a hint message appeared). */
static const struct { const char *obb; long off; long len; } texts_in_obb[] = {
  { "/main.29431.", 0x22002d90, 8606473 },
  { "/patch.29523.", 0x7e9230, 8614246 },
};

void text_patch_mapping(const char *path, long off, char *data, size_t len) {
  for (size_t k = 0; k < sizeof(texts_in_obb) / sizeof(*texts_in_obb); k++) {
    long a = texts_in_obb[k].off, b = a + texts_in_obb[k].len;
    if (off >= b || off + (long)len <= a || !strstr(path, texts_in_obb[k].obb)) continue;
    long s = off > a ? off : a, e = off + (long)len < b ? off + (long)len : b;
    int n = patch_range(data + (s - off), data + (e - off), (1u << N_REPL) - 1);
    text_patch_mapped += n;
    if (!n && len < (1u << 20)) continue; /* small re-reads of a piece: nothing to say */
    debugPrintf("[text] texts mapped from %s (+0x%lx, %zu KB): replaced %d string(s)\n", texts_in_obb[k].obb + 1, off,
                len >> 10, n);
  }
}

/* Fallback when the texts never came through b_mmap. Scans the used part of the heap; returns the number of strings replaced.
 * Only plain read/write heap is touched: on hardware the engine image's
 * source pages are handed to the code mapping and fault if read (seen on a
 * Switch 2026-11-10; the emulator runs the engine from the heap instead). */
int text_patch_apply(void) {
  if (text_patch_mapped) return 0;
  char *start = fake_heap_start, *end = sbrk(0);
  if (!start || !end || end <= start) return 0;
  unsigned todo = 0; /* strings not replaced yet; all their copies are found in this pass */
  for (size_t i = 0; i < N_REPL; i++) if (!done[i]) todo |= 1u << i;
  if (!todo) return 0;
  int total = 0;
  u64 a = (u64)start;
  while (a < (u64)end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 re = mi.addr + mi.size;
    if (re > (u64)end) re = (u64)end;
    if (re <= a) break;
    if (mi.type == MemType_Heap && mi.perm == Perm_Rw && mi.attr == 0)
      total += patch_range((char *)a, (char *)re, todo);
    a = re;
  }
  if (total) debugPrintf("[text] replaced %d string(s) in %zu MB of heap\n", total, (size_t)(end - start) >> 20);
  return total;
}

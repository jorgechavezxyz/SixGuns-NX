/* Per-frame input snapshot, with recording and replay (debug aid):
 *
 *   PORT_ROOT/record_input  (marker file) -> writes PORT_ROOT/input.rec
 *   PORT_ROOT/replay_input  (marker file) -> plays PORT_ROOT/input.rec, then live input
 *
 * input.rec is text. Input lines are written whenever the input changes:
 *   <frame> <held buttons hex> <lx> <ly> <rx> <ry> <touch count> [<x> <y> <id>]...
 * Sync lines mark game events (menu changes, tutorial steps):
 *   S <event>
 * Input frames are relative to the most recent sync event. Loading times
 * differ between runs, so on replay each sync line waits for the same event
 * before continuing, which keeps a recorded session aligned with the game.
 * Files without sync lines are plain absolute-frame recordings.
 * Screenshot lines (replay only): P <frame> <name> saves screenshots/<name>.tga at that frame,
 * input unchanged (needs the screenshots folder). */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "config.h"
#include "input.h"
#include "util.h"

#define REC_PATH PORT_ROOT "/input.rec"

static FILE *rec, *play;
static int auto_capture;
static InputFrame cur, prev, next;
static int have_next;             /* `next` holds an input line */
static char wait_event[128];      /* replay: waiting for this sync event */
static unsigned long base;        /* frame of the last sync event (rec + replay) */
static unsigned long now_frame;

/* Events seen during replay that the file hasn't asked for yet (an event can
 * fire before the replay reaches its sync line). */
static struct { char name[128]; unsigned long frame; } seen[32];
static int n_seen;

static InputFrame last_line;     /* the last input line read (P lines repeat it) */
static char next_shot[64];        /* name for the screenshot when `next` is applied */
static char shot_name[64];

static int read_next(void) {
  char line[512];
  next_shot[0] = 0;
  while (play && fgets(line, sizeof(line), play)) {
    if (line[0] == 'S' && line[1] == ' ') {
      line[strcspn(line, "\r\n")] = 0;
      snprintf(wait_event, sizeof(wait_event), "%s", line + 2);
      /* Already happened? Then rebase on it right away. */
      for (int i = 0; i < n_seen; i++) {
        if (strcmp(seen[i].name, wait_event) == 0) {
          /* Time the next segment from now: the replay got here later than
           * the event, and rebasing on the past would fire it all at once. */
          base = now_frame;
          wait_event[0] = 0;
          n_seen = 0;
          debugPrintf("[input] replay sync '%s' (already happened)\n", seen[i].name);
          break;
        }
      }
      if (wait_event[0]) return 0; /* hold until input_sync() delivers it */
      continue;
    }
    if (line[0] == 'P' && line[1] == ' ') {
      unsigned long pf;
      char nm[64];
      if (sscanf(line + 2, "%lu %63s", &pf, nm) == 2) {
        next = last_line;
        next.frame = pf;
        snprintf(next_shot, sizeof(next_shot), "%s", nm);
        return 1;
      }
      continue;
    }
    InputFrame f = {0};
    unsigned long fr;
    unsigned long long held;
    int lx, ly, rx, ry, n, off = 0, used;
    if (sscanf(line, "%lu %llx %d %d %d %d %d%n", &fr, &held, &lx, &ly, &rx, &ry, &n, &off) < 7) continue;
    f.frame = fr; f.held = held;
    f.ls.x = lx; f.ls.y = ly; f.rs.x = rx; f.rs.y = ry;
    for (int i = 0; i < n && i < INPUT_MAX_TOUCH; i++) {
      if (sscanf(line + off, "%d %d %d%n", &f.t[i].x, &f.t[i].y, &f.t[i].id, &used) < 3) break;
      off += used;
      f.ntouch++;
    }
    next = f;
    last_line = f;
    return 1;
  }
  if (play && !wait_event[0]) { fclose(play); play = NULL; debugPrintf("[input] replay finished, live input from here\n"); }
  return 0;
}

void input_init(void) {
  struct stat st;
  auto_capture=stat(PORT_ROOT "/auto_screenshots",&st)==0;
  if (stat(PORT_ROOT "/replay_input", &st) == 0) {
    play = fopen(REC_PATH, "r");
    debugPrintf("[input] replaying %s: %s\n", REC_PATH, play ? "ok" : "missing");
    have_next = read_next();
  } else if (stat(PORT_ROOT "/record_input", &st) == 0) {
    rec = fopen(REC_PATH, "w");
    debugPrintf("[input] recording to %s: %s\n", REC_PATH, rec ? "ok" : "failed");
  }
}
int input_debug_capture_active(void) {return rec || play || auto_capture;}

/* Called by the port when a game event happens (menu change, tutorial step). */
void input_sync(const char *event) {
  if (rec) {
    fprintf(rec, "S %s\n", event);
    fflush(rec);
    base = now_frame;
  }
  if (play) {
    if (wait_event[0] && strcmp(event, wait_event) == 0) {
      debugPrintf("[input] replay sync '%s'\n", event);
      wait_event[0] = 0;
      n_seen = 0;
      base = now_frame;
      have_next = read_next();
    } else if (n_seen < 32) {
      snprintf(seen[n_seen].name, sizeof(seen[n_seen].name), "%s", event);
      seen[n_seen++].frame = now_frame;
    }
  }
}

static int same(const InputFrame *a, const InputFrame *b) {
  if (a->held != b->held || a->ntouch != b->ntouch) return 0;
  if (memcmp(&a->ls, &b->ls, sizeof(a->ls)) || memcmp(&a->rs, &b->rs, sizeof(a->rs))) return 0;
  return memcmp(a->t, b->t, sizeof(a->t[0]) * a->ntouch) == 0;
}

void input_poll(PadState *pad, unsigned long frame, InputFrame *out) {
  now_frame = frame;
  prev = cur;
  if (play) {
    /* Hold the last recorded state until the next recorded change; while
     * waiting for a sync event nothing advances. */
    while (!wait_event[0] && have_next && base + next.frame <= frame) {
      cur = next;
      if (next_shot[0]) {
        extern const char *debug_shot_request;
        snprintf(shot_name, sizeof(shot_name), "%s", next_shot);
        debug_shot_request = shot_name;
      }
      have_next = read_next();
    }
    padUpdate(pad); /* keep the pad serviced even while replaying */
  } else {
    padUpdate(pad);
    memset(&cur, 0, sizeof(cur));
    cur.held = padGetButtons(pad);
    cur.ls = padGetStickPos(pad, 0);
    cur.rs = padGetStickPos(pad, 1);
    HidTouchScreenState ts = {0};
    if (hidGetTouchScreenStates(&ts, 1)) {
      for (int i = 0; i < ts.count && i < INPUT_MAX_TOUCH; i++) {
        cur.t[i].x = ts.touches[i].x;
        cur.t[i].y = ts.touches[i].y;
        cur.t[i].id = ts.touches[i].finger_id & 15;
        cur.ntouch++;
      }
    }
  }
  cur.frame = frame;
  cur.down = cur.held & ~prev.held;
  cur.up = prev.held & ~cur.held;
  u32 fingers = 0, old_fingers = 0;
  for (int i = 0; i < cur.ntouch; i++)
    if (cur.t[i].id >= 0 && cur.t[i].id < 16) fingers |= 1u << cur.t[i].id;
  for (int i = 0; i < prev.ntouch; i++)
    if (prev.t[i].id >= 0 && prev.t[i].id < 16) old_fingers |= 1u << prev.t[i].id;
  cur.touch_started = fingers & ~old_fingers;

  if (rec && !same(&cur, &prev)) {
    fprintf(rec, "%lu %llx %d %d %d %d %d", frame - base, (unsigned long long)cur.held, cur.ls.x, cur.ls.y,
            cur.rs.x, cur.rs.y, cur.ntouch);
    for (int i = 0; i < cur.ntouch; i++) fprintf(rec, " %d %d %d", cur.t[i].x, cur.t[i].y, cur.t[i].id);
    fputc('\n', rec);
    fflush(rec); /* a crash must not lose the last inputs; lines are only written on change */
  }
  *out = cur;
}

void input_close(void) {
  if (rec) { fclose(rec); rec = NULL; }
}

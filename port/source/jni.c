/* A fake Java VM. The engine talks to "Java" for paths, preferences, device
 * info and audio (android.media.AudioTrack). Every call it makes is logged
 * once with its name and signature, which is the main tool for figuring out
 * what to implement next. */
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <SDL2/SDL.h>
#include <switch.h>
#include "config.h"
#include "jni.h"
#include "util.h"


/* Per-install device id (GetSerial / GetHDIDFV / GetAndroidID). All installs reported the same fixed
 * "4e58000000000001", and the multiplayer sign-in id is built from it, so two Switches (or emulators) on
 * one network were the same player. Generated once (random, 16 hex digits, "4e58" prefix kept) and kept
 * in switch/sixguns/device_id.txt. Saves are not tied to it (CryptTea uses a fixed key). */
const char *port_device_id(void) {
  static char id[17];
  if (id[0]) return id;
  FILE *f = fopen(PORT_ROOT "/device_id.txt", "r");
  int keep_file = 0; /* a read error other than "missing" must not replace the stored id */
  if (f) {
    if (fscanf(f, "%16s", id) != 1 || strlen(id) != 16) id[0] = 0;
    fclose(f);
  } else if (errno != ENOENT) keep_file = 1;
  if (!id[0]) {
    u64 r = 0;
    randomGet(&r, sizeof(r));
    /* the emulator's random source starts the same in every instance (two emulators got one id) */
    r ^= armGetSystemTick() * 0x9E3779B97F4A7C15ull ^ (u64)time(NULL) * 0xBF58476D1CE4E5B9ull;
    snprintf(id, sizeof(id), "4e58%012llx", (unsigned long long)(r & 0xffffffffffffull));
    if (!keep_file) { /* tmp + rename via the save writer: a power cut never leaves it empty */
      extern int memfile_snapshot(const char *path, const char *data, size_t len);
      char line[24]; int n = snprintf(line, sizeof(line), "%s\n", id);
      memfile_snapshot(PORT_ROOT "/device_id.txt", line, (size_t)n);
    }
    debugPrintf("[jni] new device id %s%s\n", id, keep_file ? " (for this session; device_id.txt unreadable)" : "");
  }
  return id;
}

static void jni_unimplemented(int idx) {
  static uint8_t seen[240];
  if (!seen[idx]) { seen[idx] = 1; debugPrintf("[jni] unimplemented JNIEnv function #%d\n", idx); }
}
#include "jni_stubs.inc"

/* ---------------------------------------------------------------- objects */

enum { K_CLASS = 1, K_STRING, K_ARRAY, K_OBJECT };

typedef struct jobj {
  int kind;
  int refs;
  char *cls;          /* class name (K_CLASS: the name itself) */
  char *str;          /* K_STRING: UTF-8 */
  char elem;          /* K_ARRAY: JNI type char of elements ('B','I','L',...) */
  int len;            /* K_ARRAY: element count */
  void *data;         /* K_ARRAY: elements; K_OBJECT: per-class state */
} jobj;

typedef struct {
  char *cls, *name, *sig;
  int is_static;
  int logged;
} jmeth; /* used for both methods and fields */

typedef union { uint8_t z; int8_t b; uint16_t c; int16_t s; int32_t i; int64_t j; float f; double d; void *l; } jvalue;

static Mutex obj_mtx;

static jobj *new_obj(int kind, const char *cls) {
  jobj *o = calloc(1, sizeof(*o));
  o->kind = kind;
  o->refs = 1;
  o->cls = strdup(cls ? cls : "java/lang/Object");
  return o;
}

static jobj *new_string(const char *s) {
  jobj *o = new_obj(K_STRING, "java/lang/String");
  o->str = strdup(s ? s : "");
  return o;
}

static int elem_size(char t) {
  switch (t) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    default: return 8; /* J, D, L / [ */
  }
}

static jobj *new_array(char elem, int len) {
  char cls[4] = { '[', elem, 0 };
  jobj *o = new_obj(K_ARRAY, cls);
  o->elem = elem;
  o->len = len;
  o->data = calloc(len ? len : 1, elem_size(elem));
  return o;
}

static void unref(jobj *o) {
  if (!o || o->kind == K_CLASS) return; /* classes live forever */
  mutexLock(&obj_mtx);
  int dead = --o->refs <= 0;
  mutexUnlock(&obj_mtx);
  if (!dead) return;
  free(o->cls); free(o->str);
  if (o->kind == K_ARRAY) free(o->data);
  free(o);
}

static jobj *find_class(const char *name) {
  static jobj *classes[512];
  static int n;
  mutexLock(&obj_mtx);
  for (int i = 0; i < n; i++)
    if (strcmp(classes[i]->cls, name) == 0) { mutexUnlock(&obj_mtx); return classes[i]; }
  jobj *c = new_obj(K_CLASS, name);
  if (n < 512) classes[n++] = c;
  mutexUnlock(&obj_mtx);
  return c;
}

static jmeth *new_meth(jobj *cls, const char *name, const char *sig, int is_static) {
  jmeth *m = calloc(1, sizeof(*m));
  m->cls = strdup(cls ? cls->cls : "?");
  m->name = strdup(name);
  m->sig = strdup(sig);
  m->is_static = is_static;
  return m;
}

/* ---------------------------------------------------------------- preferences
 * SharedPreferences replacement: "file/key=value" lines in prefs.txt. */

#define MAX_PREFS 512
static char *pref_k[MAX_PREFS], *pref_v[MAX_PREFS];
static int n_prefs;
static int prefs_read_failed;
static const char *prefs_path=PREFS_PATH;
static int test_pref_oom,test_pref_serial_oom;
static char *prefs_dup(const char *s) {
  if(test_pref_oom) {test_pref_oom=0;errno=ENOMEM;return NULL;}
  return strdup(s);
}
FILE *memfile_open(const char *,const char *,int *);
int memfile_snapshot(const char *,const char *,size_t);
void memfile_note_failure(const char *);
void memfile_disable_writes(const char *);

static void prefs_load(void) {
  int fallback=0;
  FILE *f = memfile_open(prefs_path,"r",&fallback);
  if (!f) {
    if(errno==ENOENT) return;
    fatal_error("Cannot read preferences safely. Existing files were preserved; check the SD card and backups.");
  }
  char line[2048];
  while (n_prefs < MAX_PREFS && fgets(line, sizeof(line), f)) {
    if(!strchr(line,'\n') && strlen(line)==sizeof(line)-1)
      fatal_error("Preferences contain an oversized entry. Existing files were preserved.");
    line[strcspn(line, "\r\n")] = 0;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *k=prefs_dup(line),*v=prefs_dup(eq+1);
    if(!k || !v) {free(k);free(v);fatal_error("Not enough memory to load preferences safely.");}
    pref_k[n_prefs]=k;pref_v[n_prefs++]=v;
  }
  int extra=n_prefs==MAX_PREFS ? fgetc(f):EOF;
  if(ferror(f) || extra!=EOF)
    fatal_error("Preferences could not be loaded completely. Existing files were preserved.");
  fclose(f);
}

static void prefs_save(void) {
  if(prefs_read_failed) return; /* a missing value due to OOM must never regenerate/overwrite identity */
  size_t bytes=0;
  for(int i=0;i<n_prefs;i++) bytes+=strlen(pref_k[i])+strlen(pref_v[i])+2;
  if(bytes>(16u<<20)) {memfile_note_failure("preferences exceed snapshot limit");return;}
  char *out=test_pref_serial_oom ? NULL:malloc(bytes ? bytes:1);test_pref_serial_oom=0;
  if(!out) {memfile_note_failure("preferences serialization allocation failed");return;}
  char *p=out;
  for(int i=0;i<n_prefs;i++) {
    size_t k=strlen(pref_k[i]),v=strlen(pref_v[i]);memcpy(p,pref_k[i],k);p+=k;*p++='=';
    memcpy(p,pref_v[i],v);p+=v;*p++='\n';
  }
  memfile_snapshot(prefs_path,out,bytes);free(out);
}

static RMutex prefs_mtx;

/* Copies the value into a fresh string (caller frees) or returns NULL. Values
 * are replaced by other threads, so never hand out pointers into the table. */
static char *pref_get_copy(const char *file, const char *key);

static const char *pref_get(const char *file, const char *key) {
  char k[512];
  snprintf(k, sizeof(k), "%s/%s", file ? file : "", key ? key : "");
  for (int i = 0; i < n_prefs; i++) if (strcmp(pref_k[i], k) == 0) return pref_v[i];
  return NULL;
}

static char *pref_get_copy(const char *file, const char *key) {
  rmutexLock(&prefs_mtx);
  const char *v = pref_get(file, key);
  char *c = v ? prefs_dup(v) : NULL;
  if(v && !c) {prefs_read_failed=1;memfile_disable_writes("preferences read allocation failed; persistence disabled to protect identity");}
  rmutexUnlock(&prefs_mtx);
  return c;
}

static void pref_set(const char *file, const char *key, const char *val) {
  rmutexLock(&prefs_mtx);
  {
    static Thread *first;
    static int logged;
    if (!first) first = threadGetSelf();
    if (threadGetSelf() != first && logged++ < 5) debugPrintf("[prefs] set %s/%s from another thread %p\n", file, key, (void *)threadGetSelf());
  }
  char k[512];
  if(snprintf(k,sizeof(k),"%s/%s",file ? file:"",key ? key:"")>=sizeof(k)) {
    memfile_note_failure("preferences key exceeds limit");rmutexUnlock(&prefs_mtx);return;
  }
  if(prefs_read_failed) {rmutexUnlock(&prefs_mtx);return;}
  if(!val) val="";
  /* Gaia's identity values (_GAIA_ANON_GLUID, _GAIA_ENC_KEY_GLUID) end in a newline (Base64 line end).
   * Older builds wrote them as is and read them back without it (the line end), so the stored form, the
   * one ValidateSave has always compared, has no trailing CR/LF. Refusing them left a new install's
   * prefs.txt without its identity. Trailing line ends are dropped; one inside a value is still refused. */
  char trimmed[2048]; size_t vl=strlen(val);
  if(vl && (val[vl-1]=='\n' || val[vl-1]=='\r') && vl<sizeof(trimmed)) {
    while(vl && (val[vl-1]=='\n' || val[vl-1]=='\r')) vl--;
    memcpy(trimmed,val,vl); trimmed[vl]=0; val=trimmed;
  }
  if(strlen(k)+strlen(val)+2>=2048 || strpbrk(k,"\r\n=") || strpbrk(val,"\r\n")) {
    { const char *bad=strpbrk(val,"\r\n"); debugPrintf("[prefs] refused %s: key %zu + value %zu bytes, key bad %d, value CR/LF at %ld of %zu (0x%02x)\n",k,strlen(k),strlen(val),strpbrk(k,"\r\n=")!=NULL,bad?(long)(bad-val):-1L,strlen(val),bad?(unsigned char)*bad:0); }
    memfile_note_failure("preferences entry cannot be serialized safely");rmutexUnlock(&prefs_mtx);return;
  }
  for (int i = 0; i < n_prefs; i++)
    if (strcmp(pref_k[i], k) == 0) {
      if(strcmp(pref_v[i],val)) {char *v=prefs_dup(val);if(v) {free(pref_v[i]);pref_v[i]=v;prefs_save();}
        else memfile_note_failure("preferences value allocation failed");}
      rmutexUnlock(&prefs_mtx);return;
    }
  if(n_prefs<MAX_PREFS) {
    char *new_key=prefs_dup(k),*new_value=prefs_dup(val);
    if(new_key && new_value) {pref_k[n_prefs]=new_key;pref_v[n_prefs++]=new_value;prefs_save();}
    else {free(new_key);free(new_value);memfile_note_failure("preferences entry allocation failed");}
  } else memfile_note_failure("preferences table is full");
  rmutexUnlock(&prefs_mtx);
}

/* Run LAST in the emulator persistence suite: the final test disables writes until process exit.
 * No real preferences/identity paths are accessed; the table is empty before jni_init. */
int prefs_selftest(void) {
  if(n_prefs) return 1;
  const char *path=CACHE_ROOT "/prefs-selftest.txt";
  prefs_path=path;remove(path);
  extern void memfile_drain(void);extern int memfile_pending(const char *);
  pref_set("test","identity","original");memfile_drain();
  int failed=n_prefs!=1 || !pref_get("test","identity") || strcmp(pref_get("test","identity"),"original");
  pref_set("test","identity","original");failed |= memfile_pending(path);
  test_pref_oom=1;pref_set("test","identity","changed");
  failed |= strcmp(pref_get("test","identity"),"original")!=0;
  test_pref_oom=1;pref_set("test","new","value");failed |= n_prefs!=1;
  pref_set("test","identity","updated");
  int fallback=0;FILE *f=memfile_open(path,"r",&fallback);char b[128]={0};
  if(f) {fread(b,1,sizeof(b)-1,f);fclose(f);}
  failed |= strcmp(b,"test/identity=updated\n")!=0;
  memfile_drain();test_pref_serial_oom=1;prefs_save();
  test_pref_oom=1;char *v=pref_get_copy("test","identity");failed |= v!=NULL;free(v);
  failed |= memfile_snapshot(path,"corrupt",7)!=-1;
  memset(b,0,sizeof(b));f=fopen(path,"r");if(f) {fread(b,1,sizeof(b)-1,f);fclose(f);}
  failed |= strcmp(b,"test/identity=updated\n")!=0;
  debugPrintf("[prefstest] %s unchanged values, replacement/new-entry OOM, pending reads, serialization OOM, identity protection\n",failed ? "FAIL":"PASS");
  for(int i=0;i<n_prefs;i++) {free(pref_k[i]);free(pref_v[i]);pref_k[i]=pref_v[i]=NULL;}
  n_prefs=0;prefs_path=PREFS_PATH;prefs_read_failed=0;remove(path);return failed;
}

/* ---------------------------------------------------------------- AudioTrack -> SDL */

typedef struct {
  SDL_AudioDeviceID dev;
  int rate, channels, bytes_per_sample;
  int min_queue;
  int audout; /* output through audout (default) instead of SDL */
} audiotrack;

/* ---- audout output ----
 * SDL's Switch audio backend could stop for good in Ryubing (about every other
 * start): its play loop queues a wave buffer and waits frame after frame for it
 * to finish; when the voice had stopped during a startup hiccup, nothing
 * restarted it, the loop never ended, and closing the device hung too. The
 * game's sound now goes straight to audout from the game's own mixing thread
 * (vox DriverAndroid::UpdateThreadedAT, which calls AudioTrack.write): 16-bit
 * input at any rate, mono or stereo, linearly resampled to audout's 48 kHz
 * stereo, 4 buffers of 1024 frames (~85 ms). The write blocks until a buffer
 * is free, like AudioTrack.write; a wait never exceeds 100 ms (data dropped
 * instead), so the game can't hang on audio. Marker `sdl_audio`: old path. */
enum { AO_BUFS = 4, AO_FRAMES = 1024 };
static AudioOutBuffer ao_buf[AO_BUFS];
static int16_t *ao_data[AO_BUFS];
static int ao_free[AO_BUFS], ao_cur = -1, ao_fill, ao_ready = -1;
static int16_t ao_prev[2];
static double ao_phase;
static unsigned ao_dropped;

static int audout_start(void) {
  if (ao_ready >= 0) return ao_ready;
  ao_ready = 0;
  Result rc = audoutInitialize();
  if (R_FAILED(rc)) { debugPrintf("[audio] audoutInitialize failed 0x%x\n", rc); return 0; }
  rc = audoutStartAudioOut();
  if (R_FAILED(rc)) { debugPrintf("[audio] audoutStartAudioOut failed 0x%x\n", rc); audoutExit(); return 0; }
  for (int i = 0; i < AO_BUFS; i++) {
    ao_data[i] = memalign(0x1000, AO_FRAMES * 4);
    if (!ao_data[i]) return 0;
    memset(ao_data[i], 0, AO_FRAMES * 4);
    ao_buf[i] = (AudioOutBuffer){ .buffer = ao_data[i], .buffer_size = AO_FRAMES * 4, .data_size = AO_FRAMES * 4 };
    ao_free[i] = 1;
  }
  debugPrintf("[audio] audout: %u Hz, %u ch\n", audoutGetSampleRate(), audoutGetChannelCount());
  ao_ready = 1;
  return 1;
}

/* Gets a free buffer, waiting (<=100 ms) for audout to release one. */
static int audout_take(void) {
  for (int tries = 0; tries < 2; tries++) {
    for (int i = 0; i < AO_BUFS; i++) if (ao_free[i]) { ao_free[i] = 0; return i; }
    AudioOutBuffer *rel = NULL;
    u32 n = 0;
    Result rc = audoutWaitPlayFinish(&rel, &n, 100000000ull);
    if (R_SUCCEEDED(rc) && rel && n)
      for (int i = 0; i < AO_BUFS; i++) if (rel == &ao_buf[i]) ao_free[i] = 1;
  }
  return -1;
}

/* Set when no output buffer came back within audout_take's wait: the rest of the current
 * write is dropped instead of waiting again for every sample (a stalled voice would
 * otherwise block a 1024-frame write for minutes). Reset per write. */
static int ao_give_up;
static void audout_emit(int16_t l, int16_t r) {
  if (ao_cur < 0) {
    if (ao_give_up) { ao_dropped++; return; }
    ao_cur = audout_take(); ao_fill = 0;
    if (ao_cur < 0) { ao_give_up = 1; ao_dropped++; return; }
  }
  ao_data[ao_cur][ao_fill * 2] = l;
  ao_data[ao_cur][ao_fill * 2 + 1] = r;
  if (++ao_fill == AO_FRAMES) {
    audoutAppendAudioOutBuffer(&ao_buf[ao_cur]);
    ao_cur = -1;
  }
}

static void audout_write(audiotrack *t, const uint8_t *data, int bytes) {
  ao_give_up = 0;
  int ch = t->channels, frames = bytes / (ch * t->bytes_per_sample);
  double step = (double)t->rate / 48000.0;
  for (int f = 0; f < frames; f++) {
    int16_t cur[2];
    if (t->bytes_per_sample == 2) {
      const int16_t *p = (const int16_t *)data + f * ch;
      cur[0] = p[0]; cur[1] = ch > 1 ? p[1] : p[0];
    } else {
      const uint8_t *p = data + f * ch;
      cur[0] = (int16_t)((p[0] - 128) << 8); cur[1] = ch > 1 ? (int16_t)((p[1] - 128) << 8) : cur[0];
    }
    while (ao_phase < 1.0) {
      audout_emit((int16_t)(ao_prev[0] + (cur[0] - ao_prev[0]) * ao_phase),
                  (int16_t)(ao_prev[1] + (cur[1] - ao_prev[1]) * ao_phase));
      ao_phase += step;
    }
    ao_phase -= 1.0;
    ao_prev[0] = cur[0];
    ao_prev[1] = cur[1];
  }
  { /* every 10 s: frames handed to audout (48000 per second when healthy) */
    static unsigned long frames_out, last_out;
    static u64 last;
    frames_out += frames;
    u64 now = armGetSystemTick();
    if (!last) last = now;
    if (armTicksToNs(now - last) >= 10000000000ull) {
      static unsigned reports;
      if (reports++ < 2 || reports % 30 == 0) debugPrintf("[audio] audout: %lu input frames in 10 s\n", frames_out - last_out);
      last_out = frames_out;
      last = now;
    }
  }
  static unsigned logged_drop;
  if (ao_dropped > logged_drop + 4800) { logged_drop = ao_dropped; debugPrintf("[audio] audout busy: %u frames dropped so far\n", ao_dropped); }
}

static jobj *audiotrack_new(jvalue *a) {
  /* AudioTrack(streamType, sampleRate, channelConfig, audioFormat, bufferSize, mode) */
  jobj *o = new_obj(K_OBJECT, "android/media/AudioTrack");
  audiotrack *t = calloc(1, sizeof(*t));
  t->rate = a[1].i;
  t->channels = (a[2].i == 4 || a[2].i == 2) ? 1 : 2;   /* CHANNEL_OUT_MONO / CHANNEL_CONFIGURATION_MONO */
  t->bytes_per_sample = (a[3].i == 3) ? 1 : 2;          /* ENCODING_PCM_8BIT : 16BIT */
  t->min_queue = a[4].i > 0 ? a[4].i : 8192;

  {
    FILE *m = fopen(PORT_ROOT "/sdl_audio", "r");
    if (m) fclose(m);
    t->audout = !m && audout_start();
  }
  if (t->audout) {
    debugPrintf("[audio] AudioTrack %d Hz, %d ch, %d-bit, buf %d -> audout\n", t->rate, t->channels,
                t->bytes_per_sample * 8, a[4].i);
    o->data = t;
    return o;
  }
  SDL_AudioSpec want = {0}, have;
  want.freq = t->rate;
  want.format = t->bytes_per_sample == 1 ? AUDIO_U8 : AUDIO_S16LSB;
  want.channels = t->channels;
  want.samples = 1024;
  t->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  debugPrintf("[audio] AudioTrack %d Hz, %d ch, %d-bit, buf %d -> SDL dev %u %s\n", t->rate, t->channels,
              t->bytes_per_sample * 8, a[4].i, t->dev, t->dev ? "" : SDL_GetError());
  o->data = t;
  return o;
}

static int audiotrack_write(jobj *o, jobj *arr, int off, int size_units) {
  audiotrack *t = o ? o->data : NULL;
  if (t && t->audout && arr) {
    int es = elem_size(arr->elem);
    audout_write(t, (const uint8_t *)arr->data + off * es, size_units * es);
    return size_units;
  }
  if (!t || !t->dev || !arr) return size_units;
  int es = elem_size(arr->elem);
  /* AudioTrack.write blocks until the data fits in its buffer; emulate that. */
  while (SDL_GetQueuedAudioSize(t->dev) > (Uint32)t->min_queue * 2) SDL_Delay(1);
  SDL_QueueAudio(t->dev, (uint8_t *)arr->data + off * es, size_units * es);
  return size_units;
}

/* ---------------------------------------------------------------- text input
 * GameRenderer.setKeyboard asks Java to open the Android IME. We record the
 * request; main.c shows the Switch keyboard applet between frames and hands
 * the result back via jni_keyboard_finish (-> GameRenderer.onKeyboardFinish). */

static struct {
  int pending;
  int args[6];
  char text[512];
  int returned;
} kb;

int jni_keyboard_take_request(char *initial, size_t n, int args[6]) {
  if (!kb.pending) return 0;
  kb.pending = 0;
  snprintf(initial, n, "%s", kb.text);
  memcpy(args, kb.args, sizeof(kb.args));
  return 1;
}

void jni_keyboard_set_result(const char *text) {
  snprintf(kb.text, sizeof(kb.text), "%s", text ? text : "");
  kb.returned = 1;
}

/* ---------------------------------------------------------------- method dispatch */

typedef struct {
  jmeth *m;
  jobj *self;
  jvalue *a;   /* parsed arguments */
  int n;
} call_ctx;

static int sig_args(const char *sig, char *types, int max);

/* Argument i as a C string, or NULL if it isn't a String (checked against the
 * method signature, since ints and objects share the jvalue slots). */
static const char *arg_str(call_ctx *c, int i) {
  char t[16];
  int n = sig_args(c->m->sig, t, 16);
  if (i >= n || i >= c->n || t[i] != 'L') return NULL;
  jobj *o = c->a[i].l;
  return (o && o->kind == K_STRING) ? o->str : NULL;
}

static char ret_type(const char *sig) { const char *p = strchr(sig, ')'); return p ? p[1] : 'V'; }

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static jobj *read_file_bytes(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
  if (n < 0 || n > (64L << 20) || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; } /* unknown size: no array */
  jobj *arr = new_array('B', n);
  if (!arr || !arr->data) { fclose(f); return NULL; }
  size_t got = n ? fread(arr->data, 1, n, f) : 0;
  if (got < (size_t)n) memset((char *)arr->data + got, 0, n - got);
  fclose(f);
  return arr;
}

/* Returns 1 if handled. Add handlers here as the log reveals what the game needs. */
static int dispatch(call_ctx *c, jvalue *r) {
  const char *n = c->m->name;
  /* Hot path: the engine asks for the time ~800 times per frame. */
  if (n[0] == 'G' && strcmp(n, "GetCurrentTime") == 0) { r->j = now_ms(); return 1; }
  const char *s1 = arg_str(c, 0), *s2 = arg_str(c, 1);
  char buf[64];

#define IS(x) (strcmp(n, x) == 0)
  /* acp_utils (PackageUtils/AndroidUtils) folder paths, read once by JNIBridge.NativeInit */
  if (IS("RetrieveSDCardPath")) { r->l = new_string(PORT_ROOT "/sdcard"); return 1; }
  if (IS("RetrieveObbPath")) { r->l = new_string(OBB_ROOT); return 1; }
  if (IS("RetrieveDataPath")) { r->l = new_string(PORT_ROOT "/internal"); return 1; }
  if (IS("RetrieveSavePath")) { r->l = new_string(SAVE_ROOT); return 1; }
  if (IS("RetrieveTempPath")) { r->l = new_string(CACHE_ROOT); return 1; }
  if (IS("RetrieveNativeLibraryPath")) { r->l = new_string(PORT_ROOT); return 1; }
  /* hardware identifiers: harmless fixed values */
  if (IS("GetSerial") || IS("GetCPUSerial") || IS("GetHDIDFV")) { r->l = new_string(port_device_id()); return 1; }
  if (IS("GetDeviceManufacturer")) { r->l = new_string("Nintendo"); return 1; }
  if (IS("GetCPUAbi")) { r->l = new_string("arm64-v8a"); return 1; }
  if (IS("GetPhoneProduct") || IS("GetPhoneDevice")) { r->l = new_string("Switch"); return 1; }
  if (IS("GetFirmware")) { r->l = new_string("9"); return 1; }
  if (IS("GetMacAddress")) { r->l = new_string("02:00:00:00:00:00"); return 1; }
  if (IS("GetDeviceIMEI")) { r->l = new_string(""); return 1; }
  if (IS("GetOperatorMCC") || IS("GetOperatorMNC")) { r->l = new_string(""); return 1; }

  /* Keyboard (GameRenderer.*) */
  if (IS("setKeyboard")) {
    /* setKeyboard(int maxLength, String text, int inputType, int x, int y, int w, int h) */
    if (!c->a[1].l) {
      /* null text: VirtualKeyboard::HideVirtualKeyboard calls setKeyboard(0, NULL, 255, 0, h/2, w, h/2);
       * a show (acp_utils ShowKeyboard) passes a real string. (e.g. leaving the multiplayer sign-in page
       * with B): a hide, not a request. Opening the Switch keyboard for it raised an empty keyboard
       * ~2 s after leaving multiplayer (owner, hardware 2026-11-14). */
      if (kb.pending) debugPrintf("[kbd] keyboard request withdrawn (hide)\n");
      kb.pending = 0;
      return 1;
    }
    kb.args[0] = c->a[0].i;
    for (int i = 0; i < 5; i++) kb.args[1 + i] = c->a[2 + i].i;
    snprintf(kb.text, sizeof(kb.text), "%s", s2 ? s2 : "");
    kb.returned = 0;
    kb.pending = 1;
    debugPrintf("[kbd] setKeyboard(%d, \"%s\", %d, %d, %d, %d, %d)\n", kb.args[0], kb.text,
                kb.args[1], kb.args[2], kb.args[3], kb.args[4], kb.args[5]);
    return 1;
  }
  if (IS("setKeyboardText")) { snprintf(kb.text, sizeof(kb.text), "%s", s1 ? s1 : ""); return 1; }
  if (IS("getKeyboardText")) {
    size_t len = strlen(kb.text) + 1; /* engine strcpy()s it, so include the NUL */
    jobj *arr = new_array('B', len);
    memcpy(arr->data, kb.text, len);
    r->l = arr;
    return 1;
  }
  if (IS("isReturnkeyboard")) { r->i = kb.returned; return 1; }
  if (IS("isKeyboardRequired")) { r->i = 0; return 1; }

  /* Values acp_utils refreshes on every resume */
  if (IS("GetCountry")) { r->l = new_string("US"); return 1; }
  if (IS("GetDeviceLanguage")) { r->l = new_string("en"); return 1; }
  if (IS("GetUserAgent")) { r->l = new_string("Mozilla/5.0 (Linux; Android 9; Nintendo Switch)"); return 1; }
  if (IS("GetCarrierAgent")) { r->l = new_string(""); return 1; }
  if (IS("initCheckConnectionType")) { r->i = -1; return 1; }   /* no connection */

  /* acp_utils PackageUtils paths */
  if (IS("GetApkPath") || IS("getApkPath") || IS("getPackageCodePath") || IS("getPackageResourcePath")) {
    r->l = new_string(APK_PATH); return 1;
  }
  if (IS("GetNativeLibraryPath") || IS("getNativeLibraryPath")) { r->l = new_string(PORT_ROOT); return 1; }

  /* Small key/value stores, all backed by prefs.txt */
  if (IS("getSharedValue")) { char *v = pref_get_copy("shared", s1); r->l = new_string(v ? v : ""); free(v); return 1; }
  if (IS("isSharedValue")) { char *v = pref_get_copy("shared", s1); r->z = v != NULL; free(v); return 1; }
  if (IS("setSharedValue")) { pref_set("shared", s1, s2 ? s2 : ""); return 1; }
  if (IS("getuservalue")) { char *v = pref_get_copy("user", s1); r->l = new_string(v ? v : (s2 ? s2 : "")); free(v); return 1; }
  if (IS("setuservalue")) { pref_set("user", s1, s2 ? s2 : ""); return 1; }

  if (IS("GetAndroidVersion")) { r->i = 28; return 1; }
  if (IS("AreNotificationsEnabled")) { r->z = 0; return 1; }

  /* Save games and caches */
  if (IS("getSaveFolder") || IS("RetrieveSavePath") || IS("GetSaveFolderPath") || IS("getSaveFolderPath")) {
    r->l = new_string(SAVE_ROOT "/"); return 1;
  }
  if (IS("GetCachePath") || IS("getCacheDir") || IS("GetCacheFolder")) { r->l = new_string(CACHE_ROOT "/"); return 1; }

  /* Device info (Game.* and glads AndroidDevice.*) */
  if (IS("GetPhoneManufacturer") || IS("GetManufacturer")) { r->l = new_string("Nintendo"); return 1; }
  if (IS("GetPhoneModel") || IS("GetModel") || IS("GetDeviceModel")) { r->l = new_string("Switch"); return 1; }
  if (IS("GetPhoneCPUName")) { r->l = new_string("NVIDIA Tegra X1"); return 1; }
  if (IS("GetPhoneCPUFreq")) { r->f = 1020.0f; return 1; }
  if (IS("GetPhoneGPUName")) { r->l = new_string("NVIDIA Tegra X1 (Maxwell)"); return 1; }
  if (IS("GetSystemVersion")) { r->l = new_string("9"); return 1; }
  if (IS("GetUniqueDeviceID") || IS("GetAndroidID")) { r->l = new_string(port_device_id()); return 1; }
  if (IS("GetGoogleAdID")) { r->l = new_string(""); return 1; }
  if (IS("GetGoogleOptOut")) { r->l = new_string("1"); return 1; }  /* opted out of ad tracking */
  if (IS("GetGoogleAdIdStatus")) { r->i = 0; return 1; }
  if (IS("GetLogicalDensity")) { r->f = 2.0f; return 1; }
  if (IS("GetOrientation")) { r->i = 2; return 1; }          /* ORIENTATION_LANDSCAPE */
  if (IS("GetNetworkCountryIso")) { r->l = new_string("us"); return 1; }

  /* The engine reads its data straight out of the OBB zip archives. */
  if (IS("GetMainObbName") || IS("getMainObbName")) { r->l = new_string(OBB_ROOT "/" MAIN_OBB); return 1; }
  if (IS("GetPatchObbName") || IS("getPatchObbName")) { r->l = new_string(OBB_ROOT "/" PATCH_OBB); return 1; }
  if (IS("GetExtraObbName") || IS("getExtraObbName")) { r->l = new_string(""); return 1; }
  if (strstr(n, "ObbPath") || strstr(n, "ObbFolder") || strstr(n, "ObbAbsolutePath")) { r->l = new_string(OBB_ROOT "/"); return 1; }
  if (strstr(n, "ExternalFolder") || IS("getSDFolder") || IS("GetSDFolder")) {
    r->l = new_string(DATA_ROOT "/"); return 1;
  }
  if (IS("GetPreferenceString")) { char *v = pref_get_copy(s1, s2); r->l = new_string(v ? v : arg_str(c, 2)); free(v); return 1; }
  if (IS("GetPreferenceInt"))    { char *v = pref_get_copy(s1, s2); r->i = v ? atoi(v) : c->a[2].i; free(v); return 1; }
  if (IS("GetPreferenceLong"))   { char *v = pref_get_copy(s1, s2); r->j = v ? atoll(v) : c->a[2].j; free(v); return 1; }
  if (IS("GetPreferenceBool"))   { char *v = pref_get_copy(s1, s2); r->z = v ? atoi(v) : c->a[2].z; free(v); return 1; }
  if (IS("SavePreferenceString")) { pref_set(s1, s2, arg_str(c, 2) ? arg_str(c, 2) : ""); return 1; }
  if (IS("SavePreferenceInt"))   { snprintf(buf, sizeof(buf), "%d", c->a[2].i); pref_set(s1, s2, buf); return 1; }
  if (IS("SavePreferenceLong"))  { snprintf(buf, sizeof(buf), "%lld", (long long)c->a[2].j); pref_set(s1, s2, buf); return 1; }
  if (IS("SavePreferenceBool"))  { snprintf(buf, sizeof(buf), "%d", c->a[2].z); pref_set(s1, s2, buf); return 1; }
  if (IS("GetXDpi") || IS("GetYDpi")) { r->f = 237.0f; return 1; }
  if (IS("GetCurrentTime")) { r->j = now_ms(); return 1; }
  if (IS("GetDiskFreeSpace")) { r->j = 8LL << 30; return 1; }
  /* Game::_SaveLevel skips writing progress (and shows a "progress was not
   * saved" toast) when this says no; unanswered it returned false, so no
   * story progress was ever saved before 2026-11-10. */
  if (IS("isFreeSpaceAvailable")) { r->z = 1; return 1; }
  /* The game quitting by itself (Game.Exit from appDestroy after WantToQuit, AndroidUtils.ExitApplication):
   * without this the engine stops updating and the port kept showing a frozen frame (docs/re/subsystems/platform_jni.md). */
  if (IS("Exit") || IS("ExitApplication")) {
    extern volatile int port_quit_requested;
    port_quit_requested = 1;
    debugPrintf("[app] the game asked to quit (%s)\n", c->m->name);
    return 1;
  }
  if (IS("showToast")) { if (s1) debugPrintf("[toast] %s\n", s1); return 1; }
  if (IS("isAppInstalled")) { r->z = 0; return 1; }
  if (IS("IsVideoCompleted")) { r->i = 1; return 1; }
  if (IS("requestStoragePermission")) { r->i = 1; return 1; }
  if (IS("GetAssetAsString") && s1) {
    char path[1024];
    snprintf(path, sizeof(path), ASSETS_ROOT "/%s", s1);
    r->l = read_file_bytes(path);
    if (!r->l) debugPrintf("[jni] GetAssetAsString: missing %s\n", s1);
    return 1;
  }
  if (IS("GetChildDirectories") && s1) {
    char pb[1024];
    const char *p = remap_path(s1, pb, sizeof(pb));
    jobj *names[256]; int k = 0;
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && k < 256 && (e = readdir(d)))
      if (e->d_type == DT_DIR && e->d_name[0] != '.') names[k++] = new_string(e->d_name);
    if (d) closedir(d);
    jobj *arr = new_array('L', k);
    memcpy(arr->data, names, k * sizeof(void *));
    r->l = arr;
    return 1;
  }

  /* android.media.AudioTrack */
  if (strcmp(c->m->cls, "android/media/AudioTrack") == 0) {
    if (IS("getMinBufferSize")) { r->i = 4096 * 2; return 1; }
    if (IS("write") && c->n >= 3) { r->i = audiotrack_write(c->self, c->a[0].l, c->a[1].i, c->a[2].i); return 1; }
    if (IS("play") || IS("pause") || IS("stop") || IS("flush")) {
      audiotrack *t = c->self ? c->self->data : NULL;
      if (t && t->dev) {
        if (IS("play")) SDL_PauseAudioDevice(t->dev, 0);
        else if (IS("flush") || IS("stop")) SDL_ClearQueuedAudio(t->dev);
        if (IS("pause") || IS("stop")) SDL_PauseAudioDevice(t->dev, 1);
      }
      return 1;
    }
    if (IS("release")) {
      audiotrack *t = c->self ? c->self->data : NULL;
      if (t && t->dev) { SDL_CloseAudioDevice(t->dev); t->dev = 0; }
      return 1;
    }
    if (IS("getState")) { r->i = 1; return 1; }       /* STATE_INITIALIZED */
    if (IS("getPlayState")) { r->i = 3; return 1; }   /* PLAYSTATE_PLAYING */
    return 1; /* setStereoVolume etc: ignore */
  }
#undef IS
  return 0;
}

unsigned long jni_call_count;

/* Per-method call counts, for finding hot or chatty Java calls. */
#define MAX_COUNTED 256
static struct { jmeth *m; unsigned long n; } counted[MAX_COUNTED];
static int n_counted;

static Mutex count_mtx;
static void count_call(jmeth *m) {
  mutexLock(&count_mtx);
  for (int i = 0; i < n_counted; i++)
    if (counted[i].m == m) { counted[i].n++; mutexUnlock(&count_mtx); return; }
  if (n_counted < MAX_COUNTED) { counted[n_counted].m = m; counted[n_counted++].n = 1; }
  mutexUnlock(&count_mtx);
}

void jni_dump_call_counts(void) {
  for (int k = 0; k < 12; k++) {
    int best = -1;
    for (int i = 0; i < n_counted; i++)
      if (counted[i].n && (best < 0 || counted[i].n > counted[best].n)) best = i;
    if (best < 0) break;
    debugPrintf("[perf]   %8lu x %s.%s%s\n", counted[best].n, counted[best].m->cls, counted[best].m->name, counted[best].m->sig);
    counted[best].n = 0;
  }
}

static Thread *jni_main_thread;
static jvalue invoke(jmeth *m, jobj *self, jvalue *args, int nargs) {
  /* Hot path, skipping the bookkeeping below: Game.GetCurrentTime is called
   * ~800 times per frame. */
  if (m->name[0] == 'G' && strcmp(m->name, "GetCurrentTime") == 0) {
    jvalue t;
    t.j = now_ms();
    return t;
  }
  if (!jni_main_thread) jni_main_thread = threadGetSelf();
  if (threadGetSelf() != jni_main_thread) {
    static char *seen_bg[64]; static int nbg;
    int known = 0;
    for (int i = 0; i < nbg; i++) if (seen_bg[i] == m->name) known = 1;
    if (!known && nbg < 64) { seen_bg[nbg++] = m->name; debugPrintf("[jni] background thread %p calls %s.%s\n", (void *)threadGetSelf(), m->cls, m->name); }
  }
  jvalue r = {0};
  jni_call_count++;
  count_call(m);
  call_ctx c = { m, self, args, nargs };

  /* Log each distinct method the first time it is called. */
  static char *seen[1024];
  static int nseen;
  int first = 1;
  mutexLock(&obj_mtx);
  for (int i = 0; i < nseen; i++) if (seen[i] == m->name) { first = 0; break; }
  if (first && nseen < 1024) seen[nseen++] = m->name;
  mutexUnlock(&obj_mtx);

  int handled = dispatch(&c, &r);
  if (!handled) {
    /* Defaults: "" for strings, empty arrays for arrays, 0/NULL otherwise. */
    const char *rt = strchr(m->sig, ')') + 1;
    if (strcmp(rt, "Ljava/lang/String;") == 0) r.l = new_string("");
    else if (rt[0] == '[') r.l = new_array(rt[1], 0);
  }
  if (first) {
    char line[1024];
    int n = snprintf(line, sizeof(line), "[jni] %s %s.%s%s", handled ? "call" : "UNHANDLED", m->cls, m->name, m->sig);
    for (int i = 0; i < nargs && i < 6 && n < (int)sizeof(line) - 64; i++) {
      const char *str = arg_str(&c, i);
      if (str) n += snprintf(line + n, sizeof(line) - n, " \"%.48s\"", str);
    }
    debugPrintf("%s\n", line);
  }
  return r;
}

/* Parses argument types from a JNI signature. */
static int sig_args(const char *sig, char *types, int max) {
  int n = 0;
  const char *p = sig + 1;
  while (*p && *p != ')' && n < max) {
    char t = *p;
    if (t == '[') { while (*p == '[') p++; if (*p == 'L') p = strchr(p, ';'); t = 'L'; }
    else if (t == 'L') { p = strchr(p, ';'); }
    types[n++] = t;
    p++;
  }
  return n;
}

#define MAX_ARGS 16

static int args_from_va(jmeth *m, va_list va, jvalue *out) {
  char t[MAX_ARGS];
  int n = sig_args(m->sig, t, MAX_ARGS);
  for (int i = 0; i < n; i++) {
    switch (t[i]) {
      case 'Z': case 'B': case 'C': case 'S': case 'I': out[i].i = va_arg(va, int); break;
      case 'J': out[i].j = va_arg(va, int64_t); break;
      case 'F': out[i].f = (float)va_arg(va, double); break;
      case 'D': out[i].d = va_arg(va, double); break;
      default: out[i].l = va_arg(va, void *); break;
    }
  }
  return n;
}

static int args_from_a(jmeth *m, const jvalue *in, jvalue *out) {
  char t[MAX_ARGS];
  int n = sig_args(m->sig, t, MAX_ARGS);
  for (int i = 0; i < n; i++) out[i] = in[i];
  return n;
}

/* ---------------------------------------------------------------- JNIEnv functions */

typedef void *JNIEnvP;

static int GetVersion(JNIEnvP e) { return 0x00010006; }
static void *FindClass(JNIEnvP e, const char *name) { return find_class(name); }
static void *GetSuperclass(JNIEnvP e, void *c) { return find_class("java/lang/Object"); }
static int IsAssignableFrom(JNIEnvP e, void *a, void *b) { return 1; }
static int Throw(JNIEnvP e, void *t) { return 0; }
static int ThrowNew(JNIEnvP e, void *c, const char *msg) { debugPrintf("[jni] ThrowNew: %s\n", msg); return 0; }
static void *ExceptionOccurred(JNIEnvP e) { return NULL; }
static void ExceptionDescribe(JNIEnvP e) {}
static void ExceptionClear(JNIEnvP e) {}
static void FatalError(JNIEnvP e, const char *msg) { fatal_error("JNI FatalError: %s", msg); }
static int PushLocalFrame(JNIEnvP e, int c) { return 0; }
static void *PopLocalFrame(JNIEnvP e, void *r) { return r; }
static void *NewRef(JNIEnvP e, jobj *o) {
  if (o) { mutexLock(&obj_mtx); o->refs++; mutexUnlock(&obj_mtx); }
  return o;
}
static void DeleteRef(JNIEnvP e, jobj *o) { unref(o); }
static int IsSameObject(JNIEnvP e, void *a, void *b) { return a == b; }
static int EnsureLocalCapacity(JNIEnvP e, int c) { return 0; }
static void *AllocObject(JNIEnvP e, jobj *c) { return new_obj(K_OBJECT, c ? c->cls : NULL); }

static void *NewObjectCommon(jobj *cls, jmeth *m, jvalue *a, int n) {
  if (cls && strcmp(cls->cls, "android/media/AudioTrack") == 0 && n >= 5) return audiotrack_new(a);
  if (cls && strcmp(cls->cls, "java/lang/String") == 0) return new_string("");
  debugPrintf("[jni] NewObject %s%s\n", cls ? cls->cls : "?", m ? m->sig : "");
  return new_obj(K_OBJECT, cls ? cls->cls : NULL);
}
static void *NewObject(JNIEnvP e, jobj *c, jmeth *m, ...) {
  jvalue a[MAX_ARGS]; va_list va; va_start(va, m);
  int n = args_from_va(m, va, a); va_end(va);
  return NewObjectCommon(c, m, a, n);
}
static void *NewObjectV(JNIEnvP e, jobj *c, jmeth *m, va_list va) {
  jvalue a[MAX_ARGS]; va_list cp; va_copy(cp, va);
  int n = args_from_va(m, cp, a); va_end(cp);
  return NewObjectCommon(c, m, a, n);
}
static void *NewObjectA(JNIEnvP e, jobj *c, jmeth *m, const jvalue *in) {
  jvalue a[MAX_ARGS]; int n = args_from_a(m, in, a);
  return NewObjectCommon(c, m, a, n);
}

static void *GetObjectClass(JNIEnvP e, jobj *o) { return find_class(o ? o->cls : "java/lang/Object"); }
static int IsInstanceOf(JNIEnvP e, jobj *o, jobj *c) { return o && c && strcmp(o->cls, c->cls) == 0; }
static void *GetMethodID(JNIEnvP e, jobj *c, const char *n, const char *s) { return new_meth(c, n, s, 0); }
static void *GetStaticMethodID(JNIEnvP e, jobj *c, const char *n, const char *s) { return new_meth(c, n, s, 1); }
static void *GetFieldID(JNIEnvP e, jobj *c, const char *n, const char *s) { return new_meth(c, n, s, 0); }
static void *GetStaticFieldID(JNIEnvP e, jobj *c, const char *n, const char *s) { return new_meth(c, n, s, 1); }

/* Call<Type>Method{,V,A}, CallStatic<Type>Method{,V,A}, CallNonvirtual<Type>Method{,V,A}.
 * All go through invoke(); only the return slot differs. */
#define CALLS(Name, T, field)                                                                       \
  static T Call##Name##Method(JNIEnvP e, jobj *o, jmeth *m, ...) {                                  \
    jvalue a[MAX_ARGS]; va_list va; va_start(va, m); int n = args_from_va(m, va, a); va_end(va);    \
    return (T)invoke(m, o, a, n).field; }                                                           \
  static T Call##Name##MethodV(JNIEnvP e, jobj *o, jmeth *m, va_list va) {                          \
    jvalue a[MAX_ARGS]; va_list cp; va_copy(cp, va); int n = args_from_va(m, cp, a); va_end(cp);    \
    return (T)invoke(m, o, a, n).field; }                                                           \
  static T Call##Name##MethodA(JNIEnvP e, jobj *o, jmeth *m, const jvalue *in) {                    \
    jvalue a[MAX_ARGS]; int n = args_from_a(m, in, a); return (T)invoke(m, o, a, n).field; }        \
  static T CallNV##Name##Method(JNIEnvP e, jobj *o, jobj *c, jmeth *m, ...) {                       \
    jvalue a[MAX_ARGS]; va_list va; va_start(va, m); int n = args_from_va(m, va, a); va_end(va);    \
    return (T)invoke(m, o, a, n).field; }                                                           \
  static T CallNV##Name##MethodV(JNIEnvP e, jobj *o, jobj *c, jmeth *m, va_list va) {               \
    jvalue a[MAX_ARGS]; va_list cp; va_copy(cp, va); int n = args_from_va(m, cp, a); va_end(cp);    \
    return (T)invoke(m, o, a, n).field; }                                                           \
  static T CallNV##Name##MethodA(JNIEnvP e, jobj *o, jobj *c, jmeth *m, const jvalue *in) {         \
    jvalue a[MAX_ARGS]; int n = args_from_a(m, in, a); return (T)invoke(m, o, a, n).field; }

CALLS(Object, void *, l) CALLS(Boolean, uint8_t, z) CALLS(Byte, int8_t, b) CALLS(Char, uint16_t, c)
CALLS(Short, int16_t, s) CALLS(Int, int32_t, i) CALLS(Long, int64_t, j) CALLS(Float, float, f)
CALLS(Double, double, d)

static void CallVoidMethod(JNIEnvP e, jobj *o, jmeth *m, ...) {
  jvalue a[MAX_ARGS]; va_list va; va_start(va, m); int n = args_from_va(m, va, a); va_end(va);
  invoke(m, o, a, n);
}
static void CallVoidMethodV(JNIEnvP e, jobj *o, jmeth *m, va_list va) {
  jvalue a[MAX_ARGS]; va_list cp; va_copy(cp, va); int n = args_from_va(m, cp, a); va_end(cp);
  invoke(m, o, a, n);
}
static void CallVoidMethodA(JNIEnvP e, jobj *o, jmeth *m, const jvalue *in) {
  jvalue a[MAX_ARGS]; int n = args_from_a(m, in, a); invoke(m, o, a, n);
}
static void CallNVVoidMethod(JNIEnvP e, jobj *o, jobj *c, jmeth *m, ...) {
  jvalue a[MAX_ARGS]; va_list va; va_start(va, m); int n = args_from_va(m, va, a); va_end(va);
  invoke(m, o, a, n);
}
static void CallNVVoidMethodV(JNIEnvP e, jobj *o, jobj *c, jmeth *m, va_list va) {
  jvalue a[MAX_ARGS]; va_list cp; va_copy(cp, va); int n = args_from_va(m, cp, a); va_end(cp);
  invoke(m, o, a, n);
}
static void CallNVVoidMethodA(JNIEnvP e, jobj *o, jobj *c, jmeth *m, const jvalue *in) {
  jvalue a[MAX_ARGS]; int n = args_from_a(m, in, a); invoke(m, o, a, n);
}

/* Fields: Build.* strings and SDK_INT are the only ones that matter. */
static const char *build_field(const char *n) {
  if (!strcmp(n, "MANUFACTURER") || !strcmp(n, "BRAND")) return "Nintendo";
  if (!strcmp(n, "MODEL") || !strcmp(n, "DEVICE") || !strcmp(n, "PRODUCT") || !strcmp(n, "HARDWARE")) return "Switch";
  if (!strcmp(n, "RELEASE")) return "9";
  if (!strcmp(n, "SERIAL") || !strcmp(n, "ID")) return "NX0000000000";
  return NULL;
}
static void *GetStaticObjectField(JNIEnvP e, jobj *c, jmeth *f) {
  const char *v = build_field(f->name);
  if (!v && !f->logged++) debugPrintf("[jni] static field %s.%s %s -> \"\"\n", f->cls, f->name, f->sig);
  return strcmp(f->sig, "Ljava/lang/String;") == 0 ? new_string(v ? v : "") : NULL;
}
static int32_t GetStaticIntField(JNIEnvP e, jobj *c, jmeth *f) {
  if (!strcmp(f->name, "SDK_INT")) return 28;
  if (!f->logged++) debugPrintf("[jni] static int field %s.%s -> 0\n", f->cls, f->name);
  return 0;
}
static int64_t GetFieldZero(JNIEnvP e, jobj *o, jmeth *f) {
  if (!f->logged++) debugPrintf("[jni] field %s.%s %s -> 0\n", f->cls, f->name, f->sig);
  return 0;
}
static float GetFloatFieldZero(JNIEnvP e, jobj *o, jmeth *f) { return 0; }
static double GetDoubleFieldZero(JNIEnvP e, jobj *o, jmeth *f) { return 0; }
static void SetFieldIgnore(JNIEnvP e, jobj *o, jmeth *f, ...) {}

/* Strings */
static void *NewStringUTF(JNIEnvP e, const char *s) { return new_string(s); }
static void *NewString(JNIEnvP e, const uint16_t *u, int len) {
  char *s = malloc(len * 3 + 1); int k = 0;
  for (int i = 0; i < len; i++) {
    uint16_t c = u[i];
    if (c < 0x80) s[k++] = c;
    else if (c < 0x800) { s[k++] = 0xC0 | (c >> 6); s[k++] = 0x80 | (c & 0x3F); }
    else { s[k++] = 0xE0 | (c >> 12); s[k++] = 0x80 | ((c >> 6) & 0x3F); s[k++] = 0x80 | (c & 0x3F); }
  }
  s[k] = 0;
  jobj *o = new_string(s); free(s); return o;
}
static int utf16_len(const char *s) {
  int n = 0;
  for (const uint8_t *p = (const uint8_t *)s; *p; p++) if ((*p & 0xC0) != 0x80) n++;
  return n;
}
static int GetStringLength(JNIEnvP e, jobj *s) { return s && s->str ? utf16_len(s->str) : 0; }
static int GetStringUTFLength(JNIEnvP e, jobj *s) { return s && s->str ? strlen(s->str) : 0; }
static const char *GetStringUTFChars(JNIEnvP e, jobj *s, uint8_t *copy) {
  if (copy) *copy = 0;
  return s && s->str ? s->str : "";
}
static void ReleaseStringUTFChars(JNIEnvP e, jobj *s, const char *c) {}
static const uint16_t *GetStringChars(JNIEnvP e, jobj *s, uint8_t *copy) {
  if (copy) *copy = 1;
  const uint8_t *p = (const uint8_t *)(s && s->str ? s->str : "");
  uint16_t *out = malloc((strlen((const char *)p) + 1) * 2), *o = out;
  while (*p) {
    if (*p < 0x80) *o++ = *p++;
    else if ((*p & 0xE0) == 0xC0) { *o++ = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
    else { *o++ = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
  }
  *o = 0;
  return out;
}
static void ReleaseStringChars(JNIEnvP e, jobj *s, const uint16_t *c) { free((void *)c); }
static void GetStringUTFRegion(JNIEnvP e, jobj *s, int start, int len, char *buf) {
  /* ASCII approximation: start/len are UTF-16 units */
  const char *src = s && s->str ? s->str : "";
  int sl = strlen(src);
  if (start > sl) start = sl;
  if (start + len > sl) len = sl - start;
  memcpy(buf, src + start, len); buf[len] = 0;
}
static void GetStringRegion(JNIEnvP e, jobj *s, int start, int len, uint16_t *buf) {
  const uint16_t *u = GetStringChars(e, s, NULL);
  memcpy(buf, u + start, len * 2);
  free((void *)u);
}

/* Arrays */
static int GetArrayLength(JNIEnvP e, jobj *a) { return a ? a->len : 0; }
static void *NewObjectArray(JNIEnvP e, int len, jobj *cls, jobj *init) {
  jobj *a = new_array('L', len);
  for (int i = 0; i < len; i++) ((jobj **)a->data)[i] = init;
  return a;
}
static void *GetObjectArrayElement(JNIEnvP e, jobj *a, int i) {
  return (a && i >= 0 && i < a->len) ? ((jobj **)a->data)[i] : NULL;
}
static void SetObjectArrayElement(JNIEnvP e, jobj *a, int i, jobj *v) {
  if (a && i >= 0 && i < a->len) ((jobj **)a->data)[i] = v;
}
#define NEWARR(Name, t) static void *New##Name##Array(JNIEnvP e, int len) { return new_array(t, len); }
NEWARR(Boolean, 'Z') NEWARR(Byte, 'B') NEWARR(Char, 'C') NEWARR(Short, 'S') NEWARR(Int, 'I')
NEWARR(Long, 'J') NEWARR(Float, 'F') NEWARR(Double, 'D')
static void *GetArrayElements(JNIEnvP e, jobj *a, uint8_t *copy) { if (copy) *copy = 0; return a ? a->data : NULL; }
static void ReleaseArrayElements(JNIEnvP e, jobj *a, void *p, int mode) {}
static void GetArrayRegion(JNIEnvP e, jobj *a, int start, int len, void *buf) {
  if (a) memcpy(buf, (uint8_t *)a->data + start * elem_size(a->elem), len * elem_size(a->elem));
}
static void SetArrayRegion(JNIEnvP e, jobj *a, int start, int len, const void *buf) {
  if (a) memcpy((uint8_t *)a->data + start * elem_size(a->elem), buf, len * elem_size(a->elem));
}

typedef struct { const char *name, *sig; void *fn; } JNINativeMethod;
static int RegisterNatives(JNIEnvP e, jobj *c, const JNINativeMethod *m, int n) {
  for (int i = 0; i < n; i++) debugPrintf("[jni] RegisterNatives %s.%s%s\n", c ? c->cls : "?", m[i].name, m[i].sig);
  return 0;
}
static int UnregisterNatives(JNIEnvP e, jobj *c) { return 0; }
static int MonitorOp(JNIEnvP e, jobj *o) { return 0; }
static int GetJavaVM(JNIEnvP e, void **vm);
static int ExceptionCheck(JNIEnvP e) { return 0; }
static void *NewDirectByteBuffer(JNIEnvP e, void *addr, int64_t cap) {
  jobj *o = new_obj(K_OBJECT, "java/nio/ByteBuffer"); o->data = addr; o->len = cap; return o;
}
static void *GetDirectBufferAddress(JNIEnvP e, jobj *o) { return o ? o->data : NULL; }
static int64_t GetDirectBufferCapacity(JNIEnvP e, jobj *o) { return o ? o->len : -1; }
static int GetObjectRefType(JNIEnvP e, jobj *o) { return 1; }

/* ---------------------------------------------------------------- tables */

static void *env_table[233];
static void *env_ptr = env_table;
void *jni_env = &env_ptr;

static int vm_DestroyJavaVM(void *vm) { return 0; }
static int vm_AttachCurrentThread(void *vm, void **env, void *args) { *env = jni_env; return 0; }
static int vm_DetachCurrentThread(void *vm) { return 0; }
static int vm_GetEnv(void *vm, void **env, int ver) { *env = jni_env; return 0; }
static void *vm_table[8] = { NULL, NULL, NULL, (void *)vm_DestroyJavaVM, (void *)vm_AttachCurrentThread,
                             (void *)vm_DetachCurrentThread, (void *)vm_GetEnv, (void *)vm_AttachCurrentThread };
static void *vm_ptr = vm_table;
void *jni_vm = &vm_ptr;

static int GetJavaVM(JNIEnvP e, void **vm) { *vm = jni_vm; return 0; }

#define SET(i, f) env_table[i] = (void *)(f)

void jni_init(void) {
  mutexInit(&obj_mtx);
  prefs_load();
  memcpy(env_table, jni_stubs, sizeof(env_table));

  SET(4, GetVersion); SET(6, FindClass); SET(10, GetSuperclass); SET(11, IsAssignableFrom);
  SET(13, Throw); SET(14, ThrowNew); SET(15, ExceptionOccurred); SET(16, ExceptionDescribe);
  SET(17, ExceptionClear); SET(18, FatalError); SET(19, PushLocalFrame); SET(20, PopLocalFrame);
  SET(21, NewRef); SET(22, DeleteRef); SET(23, DeleteRef); SET(24, IsSameObject); SET(25, NewRef);
  SET(26, EnsureLocalCapacity); SET(27, AllocObject); SET(28, NewObject); SET(29, NewObjectV);
  SET(30, NewObjectA); SET(31, GetObjectClass); SET(32, IsInstanceOf); SET(33, GetMethodID);

  /* 34..63: Call<T>Method / V / A, in order Object Boolean Byte Char Short Int Long Float Double Void */
  void *calls[] = {
    CallObjectMethod, CallObjectMethodV, CallObjectMethodA, CallBooleanMethod, CallBooleanMethodV,
    CallBooleanMethodA, CallByteMethod, CallByteMethodV, CallByteMethodA, CallCharMethod, CallCharMethodV,
    CallCharMethodA, CallShortMethod, CallShortMethodV, CallShortMethodA, CallIntMethod, CallIntMethodV,
    CallIntMethodA, CallLongMethod, CallLongMethodV, CallLongMethodA, CallFloatMethod, CallFloatMethodV,
    CallFloatMethodA, CallDoubleMethod, CallDoubleMethodV, CallDoubleMethodA, CallVoidMethod,
    CallVoidMethodV, CallVoidMethodA };
  void *nvcalls[] = {
    CallNVObjectMethod, CallNVObjectMethodV, CallNVObjectMethodA, CallNVBooleanMethod, CallNVBooleanMethodV,
    CallNVBooleanMethodA, CallNVByteMethod, CallNVByteMethodV, CallNVByteMethodA, CallNVCharMethod,
    CallNVCharMethodV, CallNVCharMethodA, CallNVShortMethod, CallNVShortMethodV, CallNVShortMethodA,
    CallNVIntMethod, CallNVIntMethodV, CallNVIntMethodA, CallNVLongMethod, CallNVLongMethodV,
    CallNVLongMethodA, CallNVFloatMethod, CallNVFloatMethodV, CallNVFloatMethodA, CallNVDoubleMethod,
    CallNVDoubleMethodV, CallNVDoubleMethodA, CallNVVoidMethod, CallNVVoidMethodV, CallNVVoidMethodA };
  for (int i = 0; i < 30; i++) {
    env_table[34 + i] = calls[i];    /* instance */
    env_table[64 + i] = nvcalls[i];  /* nonvirtual */
    env_table[114 + i] = calls[i];   /* static: (env, jclass, methodID, ...) has the same shape */
  }

  SET(94, GetFieldID);
  for (int i = 95; i <= 103; i++) SET(i, GetFieldZero);
  SET(102, GetFloatFieldZero); SET(103, GetDoubleFieldZero);
  for (int i = 104; i <= 112; i++) SET(i, SetFieldIgnore);
  SET(113, GetStaticMethodID);
  SET(144, GetStaticFieldID);
  for (int i = 145; i <= 153; i++) SET(i, GetFieldZero);
  SET(145, GetStaticObjectField); SET(150, GetStaticIntField); SET(152, GetFloatFieldZero); SET(153, GetDoubleFieldZero);
  for (int i = 154; i <= 162; i++) SET(i, SetFieldIgnore);

  SET(163, NewString); SET(164, GetStringLength); SET(165, GetStringChars); SET(166, ReleaseStringChars);
  SET(167, NewStringUTF); SET(168, GetStringUTFLength); SET(169, GetStringUTFChars);
  SET(170, ReleaseStringUTFChars); SET(171, GetArrayLength); SET(172, NewObjectArray);
  SET(173, GetObjectArrayElement); SET(174, SetObjectArrayElement);
  SET(175, NewBooleanArray); SET(176, NewByteArray); SET(177, NewCharArray); SET(178, NewShortArray);
  SET(179, NewIntArray); SET(180, NewLongArray); SET(181, NewFloatArray); SET(182, NewDoubleArray);
  for (int i = 183; i <= 190; i++) SET(i, GetArrayElements);
  for (int i = 191; i <= 198; i++) SET(i, ReleaseArrayElements);
  for (int i = 199; i <= 206; i++) SET(i, GetArrayRegion);
  for (int i = 207; i <= 214; i++) SET(i, SetArrayRegion);
  SET(215, RegisterNatives); SET(216, UnregisterNatives); SET(217, MonitorOp); SET(218, MonitorOp);
  SET(219, GetJavaVM); SET(220, GetStringRegion); SET(221, GetStringUTFRegion);
  SET(222, GetArrayElements); SET(223, ReleaseArrayElements);
  SET(224, GetStringChars); SET(225, ReleaseStringChars);
  SET(226, NewRef); SET(227, DeleteRef); SET(228, ExceptionCheck);
  SET(229, NewDirectByteBuffer); SET(230, GetDirectBufferAddress); SET(231, GetDirectBufferCapacity);
  SET(232, GetObjectRefType);
}

void *jni_new_string(const char *s) { return new_string(s); }
void *jni_find_class(const char *name) { return find_class(name); }

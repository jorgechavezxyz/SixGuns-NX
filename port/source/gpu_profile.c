/* Optional, asynchronous GPU timing. Marker gpu_profile enables it. Never waits for a query
 * result or forces glFinish; screenshots/readbacks and presentation are outside the interval.
 * EXT_disjoint_timer_query invalidates results after a GPU clock/disjoint event. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "config.h"
#include "util.h"
#define ELAPSED 0x88bf
#define AVAILABLE 0x8867
#define RESULT 0x8866
#define DISJOINT 0x8fbb
static void (*gen)(GLsizei,GLuint *),(*begin)(GLenum,GLuint),(*end)(GLenum);
static void (*get_u)(GLuint,GLenum,GLuint *),(*get_64)(GLuint,GLenum,uint64_t *);
static void (*get_i)(GLenum,GLint *);
static struct {GLuint id;int pending,invalid,kind;} slots[8];
static struct {uint64_t total,max;unsigned n,over,hist[512];} groups[3];
static int enabled,active=-1,next;
static unsigned disjoint_events,skipped;
static const char *names[]={"gameplay","menus","loading"};
void gpu_profile_init(void) {
  FILE *f=fopen(PORT_ROOT "/gpu_profile","r");if(!f) return;fclose(f);
  const GLubyte *(*string)(GLenum)=(void *)eglGetProcAddress("glGetString");
  const char *ext=string ? (const char *)string(GL_EXTENSIONS):NULL;
  const char *name="GL_EXT_disjoint_timer_query",*hit=ext ? strstr(ext,name):NULL;
  if(!hit || (hit!=ext && hit[-1]!=' ') || (hit[strlen(name)] && hit[strlen(name)]!=' ')) {
    debugPrintf("[gpu] timer extension unavailable; profiling disabled\n");return;
  }
#define F(v,s) if(!(*(void **)&v=(void *)eglGetProcAddress(s))) {debugPrintf("[gpu] %s unavailable; profiling disabled\n",s);return;}
  F(gen,"glGenQueriesEXT");F(begin,"glBeginQueryEXT");F(end,"glEndQueryEXT");
  F(get_u,"glGetQueryObjectuivEXT");F(get_64,"glGetQueryObjectui64vEXT");F(get_i,"glGetIntegerv");
#undef F
  GLuint ids[8];gen(8,ids);for(int i=0;i<8;i++) slots[i].id=ids[i];
  enabled=1;debugPrintf("[gpu] asynchronous timing enabled; no query waits; 16.67 ms budget\n");
  f=fopen(PORT_ROOT "/emulator_mode","r");
  if(f) {fclose(f);debugPrintf("[gpu] emulator timing is diagnostic only, not Switch performance evidence\n");}
}
void gpu_profile_begin(const char *menu) {
  if(!enabled) return;
  GLint disjoint=0;get_i(DISJOINT,&disjoint);
  if(disjoint) {disjoint_events++;for(int i=0;i<8;i++) if(slots[i].pending) slots[i].invalid=1;}
  for(int i=0;i<8;i++) if(slots[i].pending) {
    GLuint ready=0;get_u(slots[i].id,AVAILABLE,&ready);if(!ready) continue;
    uint64_t ns=0;get_64(slots[i].id,RESULT,&ns);slots[i].pending=0;
    if(slots[i].invalid) {slots[i].invalid=0;continue;}
    int k=slots[i].kind;groups[k].total+=ns;groups[k].n++;
    if(ns>groups[k].max) groups[k].max=ns;
    if(ns>16666667) groups[k].over++;
    unsigned bucket=ns/250000;groups[k].hist[bucket<512 ? bucket:511]++;
  }
  active=-1;
  for(int j=0;j<8;j++) {int i=(next+j)%8;if(!slots[i].pending) {
    active=i;next=(i+1)%8;slots[i].kind=menu && strstr(menu,"LoadingScreen") ? 2:menu && *menu ? 1:0;
    begin(ELAPSED,slots[i].id);break;
  }}
  if(active<0) skipped++;
}
void gpu_profile_end(void) {
  if(enabled && active>=0) {end(ELAPSED);slots[active].pending=1;active=-1;}
}
void gpu_profile_stats(void) {
  if(!enabled) return;
  for(int k=0;k<3;k++) if(groups[k].n) {
    unsigned sum=0,p95=0,threshold=groups[k].n-groups[k].n/20;
    for(;p95<511;p95++) {sum+=groups[k].hist[p95];if(sum>=threshold) break;}
    debugPrintf("[gpu] %s: %u samples, avg %.2f ms, p95 <= %.2f ms%s, max %.2f ms; %.1f%% over budget\n",
      names[k],groups[k].n,groups[k].total/(groups[k].n*1e6),(p95+1)*.25,p95==511 ? " (overflow)":"",
      groups[k].max/1e6,groups[k].over*100.0/groups[k].n);
  }
  debugPrintf("[gpu] disjoint events %u, ring-full frames skipped %u\n",disjoint_events,skipped);
}

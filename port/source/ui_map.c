/* Port-owned map controls/chrome. The engine still owns geography, routes, icon availability,
 * quest activation and travel rules. Its old menu page is neither rendered nor touchable.
 * Tutorial maps keep the original presentation; marker old_map restores the original entirely. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "config.h"
#include "input.h"
#include "so_util.h"
#include "util.h"

so_module *port_engine_module(void);
int sui_ready(void);
void sui_begin(void), sui_end(void);
void sui_color(float, float, float, float), sui_rect(float, float, float, float);
void sui_frame(float, float, float, float, float);
float sui_text(float, float, const char *, int, float), sui_text_w(const char *, int);
float sui_wrap(float,float,float,const char *,int,float,float,float);

static int enabled, active, sel, direction;
static unsigned long frame_no, repeat_at, draw_frame;
static void *selected_obj, *seen_menu, *draw_menu;
static int selected_type;
static int zoom_direction;
static unsigned long zoom_repeat;
static void *(*mi)(int), *(*current)(void *), *(*obj)(void *, int), *(*gi)(int);
static int (*tutorial_done)(void *), (*quest_name)(void *);
static void *(*si)(int);
static const char *(*string)(void *, int);
static void (*render_layer)(void *, int, int), (*focus_player)(void *), (*zoom)(void *, float), (*reset_touches)(void *);
static void (*orig_render)(void *, int, int);
static int (*orig_update)(void *, int);
static int dialog_action;
static int (*button_text)(void *);
static void (*add_released)(void *), (*orig_root_update)(void *,int,int,int);
typedef struct { float x,y; int type, extra; void *object; } point;
static point points[256];
static int count;
static float fit_zoom(void *l) {
  float z=*(float *)((char *)l+0x54)+*(float *)((char *)l+0x58);
  float w=*(float *)((char *)l+0x74),h=*(float *)((char *)l+0x78);
  if(!isfinite(z) || z<=0 || w<=0 || h<=0) return .61f;
  float fit=fminf(*(float *)((char *)l+0x4c)*z/w,*(float *)((char *)l+0x50)*z/h);
  return fminf(.61f,fmaxf(.1f,fit));
}
static void settle_gesture(void *l) {
  reset_touches(l);
  float *p=(float *)((char *)l+0x54);
  p[0]+=p[1];p[1]=0;
  p[2]+=p[4];p[3]+=p[5];p[4]=p[5]=0;
}
static void clamp_pan(void *l) {
  float *p=(float *)((char *)l+0x3c);
  float w=*(float *)((char *)l+0x74),h=*(float *)((char *)l+0x78);
  float *x=(float *)((char *)l+0x5c),*y=(float *)((char *)l+0x60);
  *x=w<=p[4] ? p[0]+(p[4]-w)*.5f:fminf(p[0],fmaxf(p[0]+p[4]-w,*x));
  *y=h<=p[5] ? p[1]+(p[5]-h)*.5f:fminf(p[1],fmaxf(p[1]+p[5]-h,*y));
  *(float *)((char *)l+0x64)=*(float *)((char *)l+0x68)=0;
}
static void set_zoom(void *l,float z) {
  settle_gesture(l);
  float oldw=*(float *)((char *)l+0x74),oldh=*(float *)((char *)l+0x78);
  float x=*(float *)((char *)l+0x5c),y=*(float *)((char *)l+0x60);
  zoom(l,z);
  /* Native SetZoomValue writes the vertical fit correction into the horizontal delta when
   * the map fits vertically. Compute each axis independently and preserve the viewport center. */
  *(float *)((char *)l+0x5c)=x+(oldw-*(float *)((char *)l+0x74))*.5f;
  *(float *)((char *)l+0x60)=y+(oldh-*(float *)((char *)l+0x78))*.5f;
  clamp_pan(l);
}
static void reveal_selection(void *l) {
  if (!count) return;
  float x=points[sel].x,y=points[sel].y,dx=0,dy=0;
  if (x<24) dx=24-x; else if(x>544) dx=544-x;
  if (y<56) dy=56-y; else if(y>258) dy=258-y;
  if (dx || dy) {settle_gesture(l);*(float *)((char *)l+0x5c)+=dx;*(float *)((char *)l+0x60)+=dy;clamp_pan(l);}
}

static const char *class_name(void *p) {
  void *t = p ? (*(void ***)p)[-1] : NULL;
  return t ? *(const char **)((char *)t + 8) : "";
}
static void *page(void *m) { return m ? *(void **)((char *)m + 8) : NULL; }
static void *layer(void *m) { void *p = page(m); return p ? obj(p, 1) : NULL; }
static int can_own(void *m) {
  if(!enabled || !m || strcmp(class_name(m),"12Menu_IGM_Map")) return 0;
  void *g = gi ? gi(0) : NULL;
  return g && tutorial_done && (tutorial_done(g) & 1) && layer(m) && sui_ready();
}
static int popup(void) {
  extern int port_map_popup_up(void);
  return port_map_popup_up();
}
static void read_points(void *l) {
  count = 0;
  uint8_t *b = *(uint8_t **)((char *)l + 0xe0), *e = *(uint8_t **)((char *)l + 0xe8);
  if (!b || !e || e < b || (size_t)(e-b) > sizeof(points) || (e-b) % sizeof(point)) return;
  for (; b < e; b += sizeof(point)) {
    point p; memcpy(&p, b, sizeof(p));
    if (!isfinite(p.x) || !isfinite(p.y) || !p.object || p.type < 0 || p.type > 3) continue;
    points[count++] = p;
  }
  int found = -1;
  for (int i=0;i<count;i++) if (points[i].object == selected_obj && points[i].type == selected_type) {
    /* One quest can have several map locations. Preserve the chosen occurrence. */
    if (found<0 || abs(i-sel)<abs(found-sel)) found=i;
  }
  if (found >= 0) sel = found;
  else if(selected_obj) sel=0; /* selected quest was removed or the region changed */
  if (sel >= count) sel = count-1;
  if (sel < 0) sel = 0;
  if (count) { selected_obj=points[sel].object; selected_type=points[sel].type; }
  else selected_obj=NULL;
}
static void hide_old_controls(void *m) {
  uint8_t *p = page(m);
  void **b = p ? *(void ***)(p+0x68) : NULL, **e = p ? *(void ***)(p+0x70) : NULL;
  void *canvas = layer(m);
  if (!b || !e || e < b || e-b > 1000) return;
  for (void **it=b; it<e; it++) if (*it) {
    void (*visible)(void *, int) = (void *)((*(uintptr_t **)*it)[0xa8/8]);
    visible(*it, *it == canvas);
  }
}
static int update_hook(void *m, int dt) {
  if (can_own(m)) hide_old_controls(m);
  return orig_update(m, dt);
}
static void root_update_hook(void *root,int dt,int x,int y) {
  orig_root_update(root,dt,x,y);
  if (!dialog_action) return;
  void *mgr=mi ? mi(0):NULL,*m=mgr && current ? current(mgr):NULL;
  if (!m || strcmp(class_name(m),"12Menu_IGM_Map") || !((uint8_t *)m)[0x40]) {dialog_action=0;return;}
  uint8_t *p=page(m); void **b=p ? *(void ***)(p+0x68):NULL,**e=p ? *(void ***)(p+0x70):NULL;
  if(!b || !e || e<b || e-b>1000) return;
  for(void **it=b;it<e;it++) if(*it && strstr(class_name(*it),"LayerButton") && button_text(*it)==dialog_action) {
    /* Map's UpdateUI resets ButtonActionMgr immediately before updating the dialog roots.
     * Deliver here, after that reset, instead of tapping hidden/relocated page coordinates. */
    add_released(*it); debugPrintf("[mapui] dialog action %d delivered\n",dialog_action);dialog_action=0;break;
  }
}
static void render_hook(void *m, int a, int b) {
  if (!can_own(m)) { orig_render(m,a,b); return; }
  /* Rendering just LayerMap avoids the old money bar, promotion buttons and duplicated page. */
  void *l=layer(m);
  *(uint32_t *)((char *)l+0xc8)=0xffffffff;
  render_layer(l,0,0);
  draw_menu=m; draw_frame=frame_no;
}
static int fresh(const InputFrame *in, int i) {
  int id=in->t[i].id; return id>=0 && id<16 && (in->touch_started & (1u<<id));
}
static void choose(int i) {
  if (i<0 || i>=count) return;
  if(sel!=i) debugPrintf("[mapui] selection %d -> %d\n",sel,i);
  sel=i; selected_obj=points[i].object; selected_type=points[i].type;
}
void map_ui_input(InputFrame *in) {
  frame_no++;
  extern void *menus_ui_live_menu(void);
  void *m=menus_ui_live_menu();
  int was_active=active;
  active=can_own(m) && !popup();
  if (!active) {dialog_action=0;zoom_direction=0;return;}
  void *l=layer(m);
  if (!was_active) { reset_touches(l); *(void **)((char *)l+0xb8)=NULL; }
  if (m!=seen_menu || *(float *)((char *)l+0x4c)!=1280/2.25f ||
      *(float *)((char *)l+0x50)!=536/2.25f) { seen_menu=m; sel=0; selected_obj=NULL; direction=0;
    /* Keep geography inside the new chrome; the engine uses these bounds for pan/zoom too. */
    *(float *)((char *)l+0x3c)=0; *(float *)((char *)l+0x40)=88/2.25f;
    *(float *)((char *)l+0x4c)=1280/2.25f; *(float *)((char *)l+0x50)=536/2.25f;
    float *rect=(float *)((char *)l+0x18);
    rect[0]=0; rect[1]=88/2.25f; rect[2]=1280/2.25f; rect[3]=624/2.25f;
    focus_player(l);
    debugPrintf("[mapui] map on\n"); }
  read_points(l);
  static int logged_count=-1;
  if (count!=logged_count) { logged_count=count;
    for(int i=0;i<count;i++) debugPrintf("[mapui] point %d type %d at %.1f,%.1f\n",i,points[i].type,points[i].x,points[i].y); }
  int confirm=(in->down & HidNpadButton_A)!=0, center=(in->down & HidNpadButton_Y)!=0;
  int zin=(in->down & HidNpadButton_ZR)!=0, zout=(in->down & HidNpadButton_ZL)!=0;
  int question=((uint8_t *)m)[0x40]!=0;
  int zd=!question ? ((in->held & HidNpadButton_ZL) ? -1:(in->held & HidNpadButton_ZR) ? 1:0):0;
  if(zd && zd==zoom_direction && frame_no>=zoom_repeat) {
    if(zd<0) zout=1;else zin=1;zoom_repeat=frame_no+6;
  } else if(zd!=zoom_direction) zoom_repeat=frame_no+18;
  zoom_direction=zd;
  for (int i=0;i<in->ntouch;i++) {
    int x=in->t[i].x,y=in->t[i].y;
    /* Canvas gestures stay with LayerMap. Chrome touches never reach the hidden page. */
    if (!question && y>=88 && y<624) continue;
    if (fresh(in,i)) {
      if (question) {
        if (y>=410 && y<482) { if (x>=380 && x<620) confirm=1; else if (x>=660 && x<900) in->down|=HidNpadButton_B; }
      } else if (y>=624) {
        if (x>=1100) in->down|=HidNpadButton_B;
        else if (x>=880) center=1;
        else if (x>=700) zin=1;
        else if (x>=520) zout=1;
        else if (x<300) confirm=1;
      }
    }
    in->t[i]=in->t[--in->ntouch]; i--;
  }
  if (question) {
    if (in->down & (HidNpadButton_B|HidNpadButton_Minus|HidNpadButton_Plus)) {
      /* Use the dialog's own NO action, without closing the whole map as well. */
      dialog_action=0x1a; in->down &= ~(HidNpadButton_B|HidNpadButton_Minus|HidNpadButton_Plus);
    } else if (confirm) dialog_action=0x19;
  } else {
    int old_sel=sel;
    int dir=0;
    if (in->held & HidNpadButton_Up) dir=1;
    else if (in->held & HidNpadButton_Down) dir=2;
    else if (in->held & HidNpadButton_Left) dir=3;
    else if (in->held & HidNpadButton_Right) dir=4;
    else if (abs(in->ls.x)>16000 || abs(in->ls.y)>16000) {
      if (abs(in->ls.x)>abs(in->ls.y)) dir=in->ls.x<0 ? 3:4;
      else dir=in->ls.y>0 ? 1:2;
    }
    if (count && dir && (dir!=direction || frame_no>=repeat_at)) {
      float best=1e30f; int pick=-1;
      for (int i=0;i<count;i++) if (i!=sel) {
        float dx=points[i].x-points[sel].x,dy=points[i].y-points[sel].y;
        float forward=dir==1 ? -dy : dir==2 ? dy : dir==3 ? -dx : dx;
        float side=dir<=2 ? fabsf(dx) : fabsf(dy);
        if (forward>2 && forward+side*2<best) { best=forward+side*2; pick=i; }
      }
      choose(pick); repeat_at=frame_no+(dir!=direction ? 22:8);
    }
    direction=dir;
    if (count && in->down & HidNpadButton_L) choose((sel+count-1)%count);
    if (count && in->down & HidNpadButton_R) choose((sel+1)%count);
    if (old_sel!=sel) reveal_selection(l);
    if (center) {settle_gesture(l);focus_player(l);clamp_pan(l);}
    if (zin || zout) { float z=*(float *)((char *)l+0x54)+*(float *)((char *)l+0x58);
      float min=fit_zoom(l);
      set_zoom(l,fminf(2.5f,fmaxf(min,z*(zin ? 1.15f:1/1.15f))));
      debugPrintf("[mapui] zoom %.3f (full map %.3f)\n",*(float *)((char *)l+0x54),min); }
    float dx=in->rs.x/32767.0f,dy=in->rs.y/32767.0f;
    if(fabsf(dx)<=.2f) dx=0;
    if(fabsf(dy)<=.2f) dy=0;
    if (dx || dy) {
      settle_gesture(l);
      *(float *)((char *)l+0x5c)-=dx*3;
      *(float *)((char *)l+0x60)+=dy*3;
      clamp_pan(l);
    }
    if (confirm && count && !(in->down & (HidNpadButton_B|HidNpadButton_Minus|HidNpadButton_Plus))) {
      reset_touches(l); *(void **)((char *)l+0xb8)=points[sel].object;
      debugPrintf("[mapui] activate point %d, type %d\n",sel,points[sel].type);
    }
  }
  const u64 keep=HidNpadButton_B|HidNpadButton_Plus|HidNpadButton_Minus;
  in->down &= keep; in->held &= keep;
  in->ls.x=in->ls.y=in->rs.x=in->rs.y=0;
}
void map_ui_overlay(void) {
  void *m=draw_menu; draw_menu=NULL;
  void *mgr=mi ? mi(0) : NULL;
  if (!m || draw_frame!=frame_no || !mgr || current(mgr)!=m || !can_own(m) || popup()) return;
  read_points(layer(m));
  {extern const char *debug_shot_request;if(debug_shot_request) {
    char *l=layer(m);debugPrintf("[mapui] capture %s: zoom %.4f pan %.2f,%.2f size %.2f,%.2f\n",debug_shot_request,
      *(float *)(l+0x54),*(float *)(l+0x5c),*(float *)(l+0x60),*(float *)(l+0x74),*(float *)(l+0x78));}}
  sui_begin();
  sui_color(.12f,.06f,.02f,1); sui_rect(0,0,1280,88); sui_rect(0,624,1280,720);
  sui_color(.78f,.58f,.26f,1); sui_rect(0,85,1280,88); sui_rect(0,624,1280,627);
  sui_color(1,.86f,.45f,1); sui_text(30,56,"Map",40,180);
  char label[256]="No available locations";
  const char *types[]={"Quest","Objective","Campsite","Travel"};
  if (count) {
    const char *name=NULL;
    if (points[sel].type==0 && quest_name && string && si) { void *s=si(0); int id=quest_name(points[sel].object); if (s && id>0) name=string(s,id); }
    snprintf(label,sizeof(label),"%d / %d   %s",sel+1,count,name && *name ? name:types[points[sel].type]);
    float x=points[sel].x*2.25f,y=points[sel].y*2.25f;
    /* Match the engine's GetRectForObject: +/-20 logical units, scaled to 720p. */
    if(y+45>88 && y-45<624 && x+45>0 && x-45<1280)
      sui_frame(fmaxf(0,x-45),fmaxf(88,y-45),fminf(1280,x+45),fminf(624,y+45),3);
  }
  sui_text(230,52,label,26,1000);
  sui_color(.95f,.88f,.70f,1);
  sui_text(30,662,"\xee\x83\xa0 Select",24,250);
  sui_text(520,662,"ZL Zoom out",22,165); sui_text(700,662,"ZR Zoom in",22,165);
  sui_text(880,662,"\xee\x83\xa3 Center",24,190); sui_text(1100,662,"\xee\x83\xa1 Back",24,160);
  sui_text(30,702,"D-pad Choose location    L/R Cycle locations    Right stick Pan    Touch Drag / pinch",22,1200);
  if (((uint8_t *)m)[0x40]) {
    sui_color(0,0,0,.65f); sui_rect(0,0,1280,720);
    sui_color(.12f,.06f,.02f,1); sui_rect(300,240,980,510);
    sui_color(1,.86f,.45f,1); sui_text(380,295,"Confirm",36,500);
    const char *message=*(const char **)((char *)m+0x50);
    sui_color(.95f,.88f,.70f,1); sui_wrap(380,340,540,message ? message:"Travel to this location?",24,28,320,398);
    sui_color(1,.84f,.4f,1); sui_rect(380,410,620,482);
    sui_color(.95f,.87f,.69f,1); sui_rect(660,410,900,482);
    sui_color(.22f,.12f,.04f,1); sui_text(410,456,"\xee\x83\xa0 Yes",28,190); sui_text(690,456,"\xee\x83\xa1 No",28,190);
  }
  sui_end();
}
int map_ui_active(void) { return active; }
static int patch(so_module *mod,const char *symbol,const char *method,void *fn,void **original) {
  uintptr_t vt=so_symbol(mod,symbol),old=so_symbol(mod,method);
  if (!vt || !old) return 0;
  uintptr_t *v=(uintptr_t *)(mod->image+vt-mod->base);
  for(int i=2;i<40;i++) if(v[i]==old) { *original=(void *)old; v[i]=(uintptr_t)fn; return 1; }
  return 0;
}
int map_ui_install(so_module *mod) {
  FILE *f=fopen(PORT_ROOT "/old_map","r"); if(f) {fclose(f); return 0;}
#define S(v,n) if(!(*(void **)&v=(void *)so_symbol(mod,n))) { debugPrintf("[mapui] missing %s\n",n); return 0; }
  S(mi,"_ZN9SingletonI9MGR_MenusE11GetInstanceEb"); S(current,"_ZNK9MGR_Menus21GetCurrentPlayingMenuEv");
  S(obj,"_ZN9BasicPage15GetInterfaceObjEi"); S(gi,"_ZN9SingletonI4GameE11GetInstanceEb");
  S(tutorial_done,"_ZNK4Game19IsTutorialCompletedEv"); S(render_layer,"_ZN8LayerMap6RenderEii");
  S(focus_player,"_ZN8LayerMap11FocusPlayerEv"); S(zoom,"_ZN8LayerMap12SetZoomValueEf");
  S(reset_touches,"_ZN8LayerMap12ResetTouchesEv"); S(quest_name,"_ZN5Quest13GetNameTextIDEv");
  S(si,"_ZN9SingletonI9StringMgrE11GetInstanceEb"); S(string,"_ZN9StringMgr9GetStringEi");
  S(button_text,"_ZN11LayerButton9GetTextIDEv"); S(add_released,"_ZN15ButtonActionMgr17AddReleasedButtonEP11LayerButton");
#undef S
  if (!patch(mod,"_ZTV12Menu_IGM_Map","_ZN12Menu_IGM_Map8RenderUIEii",render_hook,(void **)&orig_render)) return 0;
  if (!patch(mod,"_ZTV12Menu_IGM_Map","_ZN12Menu_IGM_Map8UpdateUIEi",update_hook,(void **)&orig_update)) return 0;
  if (!patch(mod,"_ZTV9RootLayer","_ZN9RootLayer6UpdateEiii",root_update_hook,(void **)&orig_root_update)) return 0;
  enabled=1; debugPrintf("[mapui] map chrome and controls replaced\n"); return 1;
}

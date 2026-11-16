/* Low-cost image-quality controls. Only mipmapped 2D textures are eligible; UI atlases and
 * framebuffer attachments retain their existing filtering. No resolution or clock changes.
 * The level comes from the graphics menu (gfx_settings.c, graphics.txt: 1 = off .. 16, default 2), capped
 * by the GPU's limit; graphics_quality_set changes it for every mipmapped texture already loaded. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "config.h"
#include "util.h"

#define TEX_ANISO 0x84fe
#define MAX_ANISO 0x84ff
static GLfloat desired=1, gpu_max=1;
static int initialized, unit;
static GLuint bound[32], applied[4096], mipped[4096]; /* mipped: every mipmapped texture seen */
static unsigned eligible, changed;
static void (*parameter)(GLenum,GLenum,GLfloat);

void graphics_quality_init(void) {
  const GLubyte *(*get_string)(GLenum)=(void *)eglGetProcAddress("glGetString");
  void (*get_float)(GLenum,GLfloat *)=(void *)eglGetProcAddress("glGetFloatv");
  const char *extensions=get_string ? (const char *)get_string(GL_EXTENSIONS):NULL;
  const char *name="GL_EXT_texture_filter_anisotropic";
  const char *hit=extensions ? strstr(extensions,name):NULL;
  int supported=hit && (hit==extensions || hit[-1]==' ') && (!hit[strlen(name)] || hit[strlen(name)]==' ');
  GLfloat max=1;
  parameter=(void *)eglGetProcAddress("glTexParameterf");
  if(supported && parameter && get_float) {
    get_float(MAX_ANISO,&max);
    extern float gfx_anisotropy(void);
    gpu_max=max;
    desired=fminf(max,fminf(16,fmaxf(1,gfx_anisotropy())));
  }
  initialized=1;
  debugPrintf("[quality] 720p; anisotropy %.1fx (extension %s, limit %.1fx); mipmapped textures only\n",
              desired,supported ? "available":"unavailable",max);
}
void graphics_quality_active(GLenum texture) { unit=texture>=GL_TEXTURE0 && texture<GL_TEXTURE0+32 ? texture-GL_TEXTURE0:-1; }
void graphics_quality_bind(GLenum target,GLuint texture) { if(target==GL_TEXTURE_2D && unit>=0) bound[unit]=texture; }
void graphics_quality_delete(GLsizei n,const GLuint *textures) {
  for(int i=0;i<n;i++) {
    GLuint id=textures[i]; unsigned slot=(id*2654435761u)&4095;
    if(applied[slot]==id) applied[slot]=0;
    if(mipped[slot]==id) mipped[slot]=0;
    for(int j=0;j<32;j++) if(bound[j]==id) bound[j]=0;
  }
}
void graphics_quality_filter(GLenum target,GLenum pname,GLfloat value) {
  if(!initialized || target!=GL_TEXTURE_2D || pname!=GL_TEXTURE_MIN_FILTER || unit<0) return;
  GLuint id=bound[unit]; if(!id) return;
  unsigned slot=(id*2654435761u)&4095;
  int mip=value==GL_NEAREST_MIPMAP_NEAREST || value==GL_LINEAR_MIPMAP_NEAREST ||
          value==GL_NEAREST_MIPMAP_LINEAR || value==GL_LINEAR_MIPMAP_LINEAR;
  if(mip) mipped[slot]=id; else if(mipped[slot]==id) mipped[slot]=0;
  if(desired<=1 && applied[slot]!=id) return;
  if(value!=GL_NEAREST_MIPMAP_NEAREST && value!=GL_LINEAR_MIPMAP_NEAREST &&
     value!=GL_NEAREST_MIPMAP_LINEAR && value!=GL_LINEAR_MIPMAP_LINEAR) {
    /* Textures can be repurposed after creation. Restore the default for UI. */
    parameter(target,TEX_ANISO,1); if(applied[slot]==id) applied[slot]=0; return;
  }
  eligible++;
  if(applied[slot]==id) return;
  parameter(target,TEX_ANISO,desired); applied[slot]=id; changed++;
}
void graphics_quality_stats(void) { debugPrintf("[quality] %u textures upgraded, %u eligible filter changes\n",changed,eligible); }

/* graphics menu: a new level for every mipmapped texture loaded so far (GL thread, between frames) */
void graphics_quality_set(float level) {
  if(!initialized || !parameter || gpu_max<=1) return;
  GLfloat want=fminf(gpu_max,fminf(16,fmaxf(1,level)));
  if(want==desired) return;
  desired=want;
  void (*bind)(GLenum,GLuint)=(void *)eglGetProcAddress("glBindTexture");
  if(!bind || unit<0) return;
  unsigned n=0;
  for(int i=0;i<4096;i++) {
    GLuint id=mipped[i]; if(!id) continue;
    bind(GL_TEXTURE_2D,id);
    parameter(GL_TEXTURE_2D,TEX_ANISO,desired);
    applied[i]=desired>1 ? id:0;
    n++;
  }
  bind(GL_TEXTURE_2D,bound[unit]); /* the engine's binding on the active unit */
  debugPrintf("[quality] anisotropy %.0fx on %u loaded textures\n",desired,n);
}

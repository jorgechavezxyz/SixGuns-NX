/* Resolves libAndroid.so's imports by name.
 * Order of lookup: this table -> GL via eglGetProcAddress -> unresolved stub. */
#define _GNU_SOURCE
#include <ctype.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <zlib.h>
#include <EGL/egl.h>
#include "imports.h"
#include "gl_debug.h"
#include "imports_libc.h"
#include "so_util.h"
#include "util.h"

/* Functions defined in imports_libc.c / android.c. Declared loosely: we only
 * take their addresses. */
#define X(n) extern void n(void);
X(b_vfprintf) X(b_fprintf) X(b_fputs) X(b_fputc) X(b_fwrite) X(b_fflush) X(b_setvbuf) X(b_fileno)
X(b_printf) X(b_puts) X(b_putchar) X(b_fopen) X(b_freopen) X(b_fclose) X(b_feof) X(b_ferror) X(b_clearerr) X(b_fgets) X(b_fread) X(b_fseek) X(b_fseeko) X(b_ftell) X(b_ftello) X(b_getc) X(b_ungetc) X(b_rewind) X(b_fscanf) X(b_open) X(b_read) X(b_close) X(b_stat) X(b_fstat) X(b_statfs)
X(b_access) X(b_mkdir) X(b_rmdir) X(b_unlink) X(b_remove) X(b_chdir) X(b_rename)
X(b_opendir) X(b_readdir) X(b_closedir) X(b_mmap) X(b_munmap)
X(b_pthread_mutexattr_init) X(b_pthread_mutexattr_destroy) X(b_pthread_mutexattr_settype)
X(b_pthread_mutex_init) X(b_pthread_mutex_destroy) X(b_pthread_mutex_lock) X(b_pthread_mutex_trylock)
X(b_pthread_mutex_unlock) X(b_pthread_cond_init) X(b_pthread_cond_destroy) X(b_pthread_cond_wait)
X(b_pthread_cond_timedwait) X(b_pthread_cond_signal) X(b_pthread_cond_broadcast)
X(b_pthread_attr_init) X(b_pthread_attr_destroy) X(b_pthread_attr_setdetachstate)
X(b_pthread_attr_setstacksize) X(b_pthread_create) X(b_pthread_once) X(b_pthread_key_create)
X(b_pthread_key_delete) X(b_pthread_getspecific) X(b_pthread_setspecific) X(b_pthread_getschedparam)
X(b_pthread_setschedparam) X(b_pthread_kill) X(b_sched_get_priority_max) X(b_sched_get_priority_min)
X(b_localtime) X(b_gmtime) X(b_localtime_r) X(b_gmtime_r) X(b_clock_gettime) X(b_sysconf)
X(b_errno) X(b_ctype_get_mb_cur_max) X(b_abort) X(b_exit) X(b_stack_chk_fail)
X(b_android_set_abort_message) X(b_cxa_atexit) X(b_cxa_finalize)
X(b_isinf) X(b_isnan) X(b_isinff) X(b_isnanf) X(b_signbit) X(b_signbitf)
X(b_newlocale) X(b_uselocale) X(b_freelocale) X(b_setlocale)
X(b_tolower_ascii) X(b_toupper_ascii) X(b_isdigit_l) X(b_islower_l) X(b_isupper_l) X(b_isxdigit_l) X(b_tolower_l) X(b_toupper_l)
X(b_iswalpha_l) X(b_iswblank_l) X(b_iswcntrl_l) X(b_iswdigit_l) X(b_iswlower_l) X(b_iswprint_l)
X(b_iswpunct_l) X(b_iswspace_l) X(b_iswupper_l) X(b_iswxdigit_l) X(b_towlower_l) X(b_towupper_l)
X(b_strcoll_l) X(b_strxfrm_l) X(b_wcscoll_l) X(b_wcsxfrm_l) X(b_strftime_l) X(b_strtoll_l)
X(b_strtoull_l) X(b_strtold_l) X(b_ret0) X(b_retm1) X(b_retnull) X(b_getpid) X(b_gethostname) X(b_getenv)
X(AAssetManager_fromJava) X(AAssetManager_open) X(AAsset_read) X(AAsset_seek) X(AAsset_getLength)
X(AAsset_getBuffer) X(AAsset_close) X(AAssetManager_openDir) X(AAssetDir_getNextFileName)
X(AAssetDir_rewind) X(AAssetDir_close) X(ANativeWindow_fromSurface) X(ANativeWindow_release)
X(ALooper_forThread) X(ALooper_prepare) X(ASensorManager_getInstance) X(ASensorManager_getDefaultSensor)
X(ASensorManager_createEventQueue) X(ASensorEventQueue_enableSensor) X(ASensorEventQueue_disableSensor)
X(ASensorEventQueue_setEventRate) X(ASensorEventQueue_getEvents)
X(__android_log_write) X(__android_log_vprint) X(__android_log_print) X(slCreateEngine)
#undef X
extern void *SL_IID_ENGINE, *SL_IID_BUFFERQUEUE, *SL_IID_PLAY;

/* ---------------------------------------------------------------- dl* */

static so_module *game_mod;
static int fake_dl_handle;

static void *my_dlopen(const char *name, int flags) {
  debugPrintf("[dl] dlopen(%s)\n", name ? name : "NULL");
  return &fake_dl_handle;
}
static void *my_dlsym(void *h, const char *name) {
  void *p = resolve_import(name);
  if (!p && game_mod) p = (void *)so_symbol(game_mod, name);
  if (!p) debugPrintf("[dl] dlsym(%s) not found\n", name);
  return p;
}
static int my_dlclose(void *h) { return 0; }
static char *my_dlerror(void) { return NULL; }

typedef struct {
  uint64_t addr;
  const char *name;
  const void *phdr;
  uint16_t phnum;
  unsigned long long adds, subs;
  size_t tls_modid;
  void *tls_data;
} bionic_dl_phdr_info;

/* Used by the engine's built-in unwinder to find .eh_frame for C++ exceptions. */
static int my_dl_iterate_phdr(int (*cb)(bionic_dl_phdr_info *, size_t, void *), void *data) {
  if (!game_mod) return 0;
  int n;
  bionic_dl_phdr_info info = {0};
  info.addr = game_mod->base;
  info.name = "libAndroid.so";
  info.phdr = so_phdrs(game_mod, &n);
  info.phnum = n;
  info.adds = 1;
  return cb(&info, sizeof(info), data);
}

/* ---------------------------------------------------------------- misc wrappers */

static int my_strerror_r(int e, char *buf, size_t n) {
  strncpy(buf, strerror(e), n);
  if (n) buf[n - 1] = 0;
  return 0;
}
static long my_times(void *buf) { memset(buf, 0, 32); return clock(); }
static int my_getaddrinfo(const char *n, const char *s, const void *h, void **res) {
  debugPrintf("[net] getaddrinfo(%s) -> offline\n", n ? n : "");
  *res = NULL;
  return 4; /* EAI_FAIL */
}
static void my_noop(void) {}

static gzFile my_gzopen(const char *p, const char *mode) {
  char buf[1024];
  return gzopen(remap_path(p, buf, sizeof(buf)), mode);
}

/* net.c */
int n_socket(), n_accept(), n_bind(), n_connect(), n_listen(), n_shutdown(), n_setsockopt(), n_getsockopt(),
    n_getpeername(), n_getsockname(), n_poll(), n_select(), n_fcntl();
long n_recv(), n_recvfrom(), n_send(), n_sendto();
void *n_gethostbyname();
unsigned n_inet_addr();
char *n_inet_ntoa();
const char *n_inet_ntop();

/* ---------------------------------------------------------------- table */

#define F(n) { #n, (void *)&n }
#define W(n, f) { #n, (void *)&f }

static const so_import table[] = {
  /* data symbols */
  W(__sF, fake_sF),
  { "SL_IID_ENGINE", &SL_IID_ENGINE }, { "SL_IID_BUFFERQUEUE", &SL_IID_BUFFERQUEUE },
  { "SL_IID_PLAY", &SL_IID_PLAY },

  /* Android NDK */
  F(AAssetDir_close), F(AAssetDir_getNextFileName), F(AAssetDir_rewind), F(AAssetManager_fromJava),
  F(AAssetManager_open), F(AAssetManager_openDir), F(AAsset_close), F(AAsset_getBuffer),
  F(AAsset_getLength), F(AAsset_read), F(AAsset_seek), F(ALooper_forThread), F(ALooper_prepare),
  F(ANativeWindow_fromSurface), F(ANativeWindow_release), F(ASensorEventQueue_disableSensor),
  F(ASensorEventQueue_enableSensor), F(ASensorEventQueue_getEvents), F(ASensorEventQueue_setEventRate),
  F(ASensorManager_createEventQueue), F(ASensorManager_getDefaultSensor), F(ASensorManager_getInstance),
  F(__android_log_print), F(__android_log_vprint), F(__android_log_write), F(slCreateEngine),
  W(android_set_abort_message, b_android_set_abort_message),

  /* runtime */
  W(__errno, b_errno), W(__ctype_get_mb_cur_max, b_ctype_get_mb_cur_max), W(__cxa_atexit, b_cxa_atexit),
  W(__cxa_finalize, b_cxa_finalize), W(__stack_chk_fail, b_stack_chk_fail), W(abort, b_abort),
  W(exit, b_exit), W(__isinff, b_isinff), W(__isnanf, b_isnanf), W(__signbit, b_signbit),
  W(__signbitf, b_signbitf), W(isinf, b_isinf), W(isnan, b_isnan),
  W(_setjmp, setjmp), W(_longjmp, longjmp), F(setjmp), F(longjmp),
  W(dlopen, my_dlopen), W(dlsym, my_dlsym), W(dlclose, my_dlclose), W(dlerror, my_dlerror),
  W(dl_iterate_phdr, my_dl_iterate_phdr),

  /* memory & strings */
  F(malloc), F(calloc), F(realloc), F(free), F(memchr), F(memcmp), F(memcpy), F(memmove), F(memset),
  F(strcasecmp), F(strcat), F(strchr), F(strcmp), F(strcpy), F(strcspn), F(strdup), F(strerror),
  W(strerror_r, my_strerror_r), F(strlcpy), F(strlen), F(strncasecmp), F(strncat), F(strncmp),
  F(strncpy), F(strnlen), F(strpbrk), F(strrchr), F(strstr), F(strtok), F(strtok_r),
  F(strtod), F(strtof), F(strtol), F(strtold), F(strtoll), F(strtoul), F(strtoull), F(strftime),
  F(strptime), F(atof), F(atoi), F(atol), F(qsort), F(rand), F(srand), F(div),
  F(snprintf), F(sprintf), F(sscanf), F(vasprintf), F(vsnprintf), F(vsprintf), F(vsscanf),
  W(mmap, b_mmap), W(munmap, b_munmap), W(msync, b_ret0),

  /* ctype / wide chars / locale */
  F(isalnum), F(isalpha), F(iscntrl), F(islower), F(ispunct), F(isspace), F(isupper), F(isxdigit),
  W(tolower, b_tolower_ascii), W(toupper, b_toupper_ascii), /* bionic semantics, no locale lookup */
  W(isdigit_l, b_isdigit_l), W(islower_l, b_islower_l), W(isupper_l, b_isupper_l),
  W(isxdigit_l, b_isxdigit_l), W(tolower_l, b_tolower_l), W(toupper_l, b_toupper_l),
  W(iswalpha_l, b_iswalpha_l), W(iswblank_l, b_iswblank_l), W(iswcntrl_l, b_iswcntrl_l),
  W(iswdigit_l, b_iswdigit_l), W(iswlower_l, b_iswlower_l), W(iswprint_l, b_iswprint_l),
  W(iswpunct_l, b_iswpunct_l), W(iswspace_l, b_iswspace_l), W(iswupper_l, b_iswupper_l),
  W(iswxdigit_l, b_iswxdigit_l), W(towlower_l, b_towlower_l), W(towupper_l, b_towupper_l),
  W(strcoll_l, b_strcoll_l), W(strxfrm_l, b_strxfrm_l), W(wcscoll_l, b_wcscoll_l),
  W(wcsxfrm_l, b_wcsxfrm_l), W(strftime_l, b_strftime_l), W(strtoll_l, b_strtoll_l),
  W(strtoull_l, b_strtoull_l), W(strtold_l, b_strtold_l),
  W(newlocale, b_newlocale), W(uselocale, b_uselocale), W(freelocale, b_freelocale),
  W(setlocale, b_setlocale), F(localeconv),
  F(btowc), F(wctob), F(mbrlen), F(mbrtowc), F(mbsnrtowcs), F(mbsrtowcs), F(mbstowcs), F(mbtowc),
  F(wcrtomb), F(wcsnrtombs), F(wcslen), F(swprintf), F(wcstod), F(wcstof), F(wcstol), F(wcstold),
  F(wcstoll), F(wcstoul), F(wcstoull), F(wmemchr), F(wmemcmp), F(wmemcpy), F(wmemmove), F(wmemset),

  /* math */
  F(acos), F(acosf), F(asin), F(asinf), F(atan), F(atan2), F(atan2f), F(atanf), F(cos), F(cosf),
  F(cosh), F(exp), F(expf), F(fmod), F(frexp), F(ldexp), F(ldexpf), F(log), F(log10), F(log10f),
  F(logf), F(modf), F(modff), F(pow), F(powf), F(sin), F(sinf), F(sinh), F(sinhf), F(tan), F(tanf),
  F(tanh),

  /* stdio */
  W(fprintf, b_fprintf), W(vfprintf, b_vfprintf), W(fputs, b_fputs), W(fputc, b_fputc),
  W(putc, b_fputc), W(fwrite, b_fwrite), W(fflush, b_fflush), W(setvbuf, b_setvbuf),
  W(fileno, b_fileno), W(printf, b_printf), W(puts, b_puts), W(putchar, b_putchar),
  W(fopen, b_fopen), W(freopen, b_freopen), W(fclose, b_fclose), F(fdopen), W(feof, b_feof), W(ferror, b_ferror), W(clearerr, b_clearerr),
  W(fgets, b_fgets), W(fread, b_fread), W(fscanf, b_fscanf), W(fseek, b_fseek), W(fseeko, b_fseeko),
  W(ftell, b_ftell), W(ftello, b_ftello), W(getc, b_getc), W(ungetc, b_ungetc), W(rewind, b_rewind), F(perror), F(tmpfile), F(mkstemp),

  /* files */
  W(open, b_open), W(close, b_close), W(read, b_read), F(write), F(lseek), W(stat, b_stat), W(fstat, b_fstat),
  W(statfs, b_statfs), W(access, b_access), W(mkdir, b_mkdir), W(rmdir, b_rmdir),
  W(unlink, b_unlink), W(remove, b_remove), W(rename, b_rename), W(chdir, b_chdir),
  W(opendir, b_opendir), W(readdir, b_readdir), W(closedir, b_closedir),
  W(chmod, b_ret0), W(umask, b_ret0), W(utime, b_ret0), W(fcntl, n_fcntl), W(ioctl, b_retm1),
  W(tcgetattr, b_retm1), W(tcsetattr, b_retm1),

  /* threads */
  W(pthread_attr_destroy, b_pthread_attr_destroy), W(pthread_attr_init, b_pthread_attr_init),
  W(pthread_attr_setdetachstate, b_pthread_attr_setdetachstate),
  W(pthread_attr_setstacksize, b_pthread_attr_setstacksize),
  W(pthread_cond_broadcast, b_pthread_cond_broadcast), W(pthread_cond_destroy, b_pthread_cond_destroy),
  W(pthread_cond_init, b_pthread_cond_init), W(pthread_cond_signal, b_pthread_cond_signal),
  W(pthread_cond_timedwait, b_pthread_cond_timedwait), W(pthread_cond_wait, b_pthread_cond_wait),
  W(pthread_create, b_pthread_create), F(pthread_detach), F(pthread_equal), F(pthread_exit),
  W(pthread_getschedparam, b_pthread_getschedparam), W(pthread_getspecific, b_pthread_getspecific),
  F(pthread_join), W(pthread_key_create, b_pthread_key_create), W(pthread_key_delete, b_pthread_key_delete),
  W(pthread_kill, b_pthread_kill), W(pthread_mutex_destroy, b_pthread_mutex_destroy),
  W(pthread_mutex_init, b_pthread_mutex_init), W(pthread_mutex_lock, b_pthread_mutex_lock),
  W(pthread_mutex_trylock, b_pthread_mutex_trylock), W(pthread_mutex_unlock, b_pthread_mutex_unlock),
  W(pthread_mutexattr_destroy, b_pthread_mutexattr_destroy),
  W(pthread_mutexattr_init, b_pthread_mutexattr_init),
  W(pthread_mutexattr_settype, b_pthread_mutexattr_settype), W(pthread_once, b_pthread_once),
  F(pthread_self), W(pthread_setschedparam, b_pthread_setschedparam),
  W(pthread_setspecific, b_pthread_setspecific),
  W(sched_get_priority_max, b_sched_get_priority_max), W(sched_get_priority_min, b_sched_get_priority_min),
  F(sched_yield),

  /* time */
  F(time), F(clock), W(clock_gettime, b_clock_gettime), F(gettimeofday), F(nanosleep), F(usleep),
  F(difftime), F(mktime), F(asctime), W(localtime, b_localtime), W(localtime_r, b_localtime_r),
  W(gmtime, b_gmtime), W(gmtime_r, b_gmtime_r), W(times, my_times), W(sysconf, b_sysconf),

  /* process / user: nothing to report */
  W(getpid, b_getpid), W(getuid, b_ret0), W(geteuid, b_ret0), W(getgid, b_ret0), W(getegid, b_ret0),
  W(getpwuid, b_retnull), W(getenv, b_getenv), W(gethostname, b_gethostname),
  W(system, b_retm1), W(popen, b_retnull), W(pclose, b_retm1), W(pipe, b_retm1),
  W(signal, b_retnull), W(sigaction, b_ret0), W(openlog, my_noop), W(syslog, my_noop),
  W(closelog, my_noop), W(eventfd, b_retm1), W(epoll_create, b_retm1), W(epoll_create1, b_retm1),
  W(epoll_ctl, b_retm1), W(epoll_wait, b_retm1),

  /* networking: net.c (real sockets with the `lan` marker, offline without) */
  W(socket, n_socket), W(accept, n_accept), W(bind, n_bind), W(connect, n_connect), W(listen, n_listen),
  W(recv, n_recv), W(recvfrom, n_recvfrom), W(recvmsg, b_retm1), W(send, n_send),
  W(sendmsg, b_retm1), W(sendto, n_sendto), W(shutdown, n_shutdown), W(setsockopt, n_setsockopt),
  W(getsockopt, n_getsockopt), W(getpeername, n_getpeername), W(getsockname, n_getsockname), W(poll, n_poll),
  W(select, n_select), W(getaddrinfo, my_getaddrinfo), W(freeaddrinfo, my_noop),
  W(gethostbyname, n_gethostbyname), W(gethostbyaddr, b_retnull), W(getprotobyname, b_retnull),
  W(inet_addr, n_inet_addr), W(inet_ntoa, n_inet_ntoa), W(inet_ntop, n_inet_ntop),

  /* zlib */
  W(gzopen, my_gzopen), F(gzclose), F(gzeof), F(gzerror), F(gzgets), F(gzputs), F(uncompress),
};

void imports_set_module(so_module *mod) { game_mod = mod; }

void *resolve_import(const char *name) {
  for (size_t i = 0; i < sizeof(table) / sizeof(*table); i++)
    if (strcmp(table[i].name, name) == 0) return table[i].addr;
  if (name[0] == 'g' && name[1] == 'l') {
    void *p = (void *)eglGetProcAddress(name);
    if (p) return gl_debug_wrap(name, p);
  }
  return NULL;
}

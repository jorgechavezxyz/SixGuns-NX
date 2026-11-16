/* Rotating save backups, made at every launch before the engine can touch the
 * save folder:  save_backups/1 = previous launch, 2, 3 = older (3 is dropped).
 * A crash in the middle of a save can then always be rolled back by copying a
 * backup over save/. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "config.h"
#include "util.h"

#define BACKUP_ROOT PORT_ROOT "/save_backups"
#define KEEP 3
static int test_copy_fail;

static void remove_tree(const char *path) {
  DIR *d = opendir(path);
  if (!d) { unlink(path); return; }
  struct dirent *e;
  char child[512];
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= sizeof(child)) continue;
    struct stat st;
    if (!lstat(child,&st) && S_ISDIR(st.st_mode)) remove_tree(child); else unlink(child);
  }
  closedir(d);
  rmdir(path);
}

static int copy_file(const char *from, const char *to) {
  if(test_copy_fail && !--test_copy_fail) {errno=EIO;return -1;}
  FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
  if (!in || !out) { if (in) fclose(in); return -1; }
  char buf[16384];
  size_t n;
  int bad=0;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    if (fwrite(buf, 1, n, out)!=n) {bad=1;break;}
  bad |= ferror(in)!=0;
  fclose(in);
  bad |= fclose(out)!=0;
  if(bad) unlink(to);
  return bad ? -1:0;
}

static int copy_tree(const char *from, const char *to) {
  DIR *d = opendir(from);
  if (!d) return -1;
  if (mkdir(to, 0777) && errno!=EEXIST) {closedir(d);return -1;}
  struct dirent *e;
  char a[512], b[512];
  int files = 0;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if (snprintf(a, sizeof(a), "%s/%s", from, e->d_name)>=sizeof(a) ||
        snprintf(b, sizeof(b), "%s/%s", to, e->d_name)>=sizeof(b)) {files=-1;break;}
    struct stat st;
    if(lstat(a,&st)) {files=-1;break;}
    int n=S_ISDIR(st.st_mode) ? copy_tree(a,b):S_ISREG(st.st_mode) ? (copy_file(a,b) ? -1:1):-1;
    if(n<0) {files=-1;break;}
    files+=n;
  }
  closedir(d);
  return files;
}

/* Byte-identical trees (same names, types and contents)? */
static int same_file(const char *a, const char *b) {
  FILE *x = fopen(a, "rb"), *y = x ? fopen(b, "rb") : NULL;
  if (!x || !y) { if (x) fclose(x); return 0; }
  char p[4096], q[4096]; int same = 1; size_t n, m;
  do { n = fread(p, 1, sizeof(p), x); m = fread(q, 1, sizeof(q), y); if (n != m || memcmp(p, q, n)) { same = 0; break; } } while (n);
  same &= !ferror(x) && !ferror(y);
  fclose(x); fclose(y); return same;
}
static int count_entries(const char *path) {
  DIR *d = opendir(path); if (!d) return -1;
  int n = 0; struct dirent *e;
  while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
  closedir(d); return n;
}
static int same_tree(const char *a, const char *b) {
  int na = count_entries(a);
  if (na < 0 || na != count_entries(b)) return 0;
  DIR *d = opendir(a); if (!d) return 0;
  struct dirent *e; char x[512], y[512]; int same = 1;
  while (same && (e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if (snprintf(x, sizeof(x), "%s/%s", a, e->d_name) >= (int)sizeof(x) || snprintf(y, sizeof(y), "%s/%s", b, e->d_name) >= (int)sizeof(y)) { same = 0; break; }
    struct stat sx, sy;
    if (lstat(x, &sx) || lstat(y, &sy) || S_ISDIR(sx.st_mode) != S_ISDIR(sy.st_mode)) { same = 0; break; }
    same = S_ISDIR(sx.st_mode) ? same_tree(x, y) : sx.st_size == sy.st_size && same_file(x, y);
  }
  closedir(d); return same;
}

/* A power cut between "save -> save.bak" and "save.tmp -> save" leaves no main file. Put the .bak back
 * (or a lone .tmp) before anything looks at the folder, so stat/access/fopen and the backup all agree. */
static void recover_orphans(const char *dir) {
  DIR *d = opendir(dir); if (!d) return;
  char names[128][128]; int n = 0; struct dirent *e;
  while ((e = readdir(d)) && n < 128) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) >= 128) continue;
    strcpy(names[n++], e->d_name);
  }
  closedir(d);
  for (int i = 0; i < n; i++) {
    char p[512], main_[512], bak[520]; struct stat st;
    if (snprintf(p, sizeof(p), "%s/%s", dir, names[i]) >= (int)sizeof(p)) continue;
    if (!lstat(p, &st) && S_ISDIR(st.st_mode)) { recover_orphans(p); continue; }
    size_t l = strlen(p);
    int is_bak = l > 4 && !strcmp(p + l - 4, ".bak"), is_tmp = l > 4 && !strcmp(p + l - 4, ".tmp");
    if (!is_bak && !is_tmp) continue;
    memcpy(main_, p, l - 4); main_[l - 4] = 0;
    if (!lstat(main_, &st) || errno != ENOENT) continue;
    snprintf(bak, sizeof(bak), "%s.bak", main_);
    if (is_tmp && !lstat(bak, &st)) continue; /* prefer the complete old file; the .bak pass restores it */
    if (!rename(p, main_)) debugPrintf("[backup] restored %s from its %s (interrupted save)\n", strrchr(main_, '/') + 1, is_bak ? ".bak" : ".tmp");
  }
}

static int snapshot(const char *source,const char *root,const char *prefs,const char *identity) {
  char stage[512],a[512],b[512];
  if(snprintf(stage,sizeof(stage),"%s/.pending",root)>=sizeof(stage)) return -1;
  if(mkdir(root,0777) && errno!=EEXIST) return -1;
  remove_tree(stage);
  int n=copy_tree(source,stage);
  const char *metadata[]={prefs,identity};
  const char *names[]={".port-prefs.txt",".port-device_id.txt"};
  for(int i=0;n>=0 && i<2;i++) if(metadata[i]) {
    struct stat st;
    if(!stat(metadata[i],&st)) {
      if(snprintf(b,sizeof(b),"%s/%s",stage,names[i])>=sizeof(b) || copy_file(metadata[i],b)) n=-1;
    } else if(errno!=ENOENT) n=-1;
  }
  /* Never rotate out a usable generation until every source byte was copied. */
  if(n<0) {remove_tree(stage);return -1;}
  /* Unchanged since the last launch: rotating would only push older, different generations out (a crash
   * loop on a bad save replaced every good backup within three launches). */
  snprintf(a,sizeof(a),"%s/1",root);
  if(same_tree(stage,a)) {remove_tree(stage);return -2;}
  snprintf(a,sizeof(a),"%s/%d",root,KEEP);remove_tree(a);
  for(int i=KEEP-1;i>=1;i--) {
    snprintf(a,sizeof(a),"%s/%d",root,i);snprintf(b,sizeof(b),"%s/%d",root,i+1);
    if(rename(a,b) && errno!=ENOENT) return -1;
  }
  snprintf(a,sizeof(a),"%s/1",root);
  return rename(stage,a) ? -1:n;
}

void backup_saves(void) {
  struct stat st;
  if (stat(SAVE_ROOT, &st) != 0) return; /* first launch: nothing to back up */
  recover_orphans(SAVE_ROOT);
  { char pb[300]; struct stat ps; snprintf(pb,sizeof(pb),"%s.bak",PREFS_PATH);
    if(stat(PREFS_PATH,&ps) && errno==ENOENT && !rename(pb,PREFS_PATH)) debugPrintf("[backup] restored prefs.txt from its .bak\n"); }
  int n=snapshot(SAVE_ROOT,BACKUP_ROOT,PREFS_PATH,PORT_ROOT "/device_id.txt");
  if(n==-2) debugPrintf("[backup] saves unchanged since save_backups/1; generations kept\n");
  else if(n<0) debugPrintf("[backup] ERROR: snapshot incomplete; retained previous generations\n");
  else debugPrintf("[backup] saved %d files and identity metadata to save_backups/1 (keeping %d launches)\n",n,KEEP);
}

/* After a clean quit (every save written): save_backups/clean, replaced only by the next clean quit, so a
 * session that later breaks the saves can always go back to the last state that was quit normally. */
void backup_clean_exit(void) {
  char stage[300], dst[300];
  snprintf(stage, sizeof(stage), "%s/.clean-pending", BACKUP_ROOT);
  snprintf(dst, sizeof(dst), "%s/clean", BACKUP_ROOT);
  if (mkdir(BACKUP_ROOT, 0777) && errno != EEXIST) return;
  remove_tree(stage);
  int n = copy_tree(SAVE_ROOT, stage);
  char b[320];
  const char *meta[] = { PREFS_PATH, PORT_ROOT "/device_id.txt" }, *names[] = { ".port-prefs.txt", ".port-device_id.txt" };
  for (int i = 0; n >= 0 && i < 2; i++) {
    struct stat st;
    if (!stat(meta[i], &st)) { snprintf(b, sizeof(b), "%s/%s", stage, names[i]); if (copy_file(meta[i], b)) n = -1; }
  }
  if (n < 0) { remove_tree(stage); debugPrintf("[backup] ERROR: clean-exit copy incomplete; previous kept\n"); return; }
  if (same_tree(stage, dst)) { remove_tree(stage); return; }
  remove_tree(dst);
  if (rename(stage, dst)) debugPrintf("[backup] ERROR: clean-exit copy not installed\n");
  else debugPrintf("[backup] clean-exit copy: %d files in save_backups/clean\n", n);
}

/* Called only by the emulator-only persistence test; uses no live save/prefs paths. */
int backup_selftest(void) {
  const char *base=CACHE_ROOT "/backup-selftest";
  const char *src=CACHE_ROOT "/backup-selftest/source";
  const char *root=CACHE_ROOT "/backup-selftest/generations";
  const char *data=CACHE_ROOT "/backup-selftest/source/data";
  const char *meta=CACHE_ROOT "/backup-selftest/identity";
  remove_tree(base); mkdir(base,0777);mkdir(src,0777);
  FILE *f=fopen(data,"wb");if(!f) return 1;fputs("first",f);fclose(f);
  f=fopen(meta,"wb");if(!f) return 1;fputs("test-identity",f);fclose(f);
  int failed=snapshot(src,root,meta,NULL)!=1;
  test_copy_fail=2; /* save copies successfully, metadata fails */
  failed |= snapshot(src,root,meta,NULL)!=-1;
  struct stat st;
  failed |= stat(CACHE_ROOT "/backup-selftest/generations/1/data",&st)!=0;
  failed |= stat(CACHE_ROOT "/backup-selftest/generations/2",&st)==0;
  failed |= stat(CACHE_ROOT "/backup-selftest/generations/.pending",&st)==0;
  f=fopen(data,"wb");if(!f) return 1;fputs("second",f);fclose(f);
  failed |= snapshot(src,root,meta,NULL)!=1;
  failed |= stat(CACHE_ROOT "/backup-selftest/generations/2/.port-prefs.txt",&st)!=0;
  failed |= snapshot(src,root,meta,NULL)!=-2; /* unchanged: no rotation */
  failed |= stat(CACHE_ROOT "/backup-selftest/generations/3",&st)==0;
  { FILE *o=fopen(CACHE_ROOT "/backup-selftest/orphan.bak","wb"); if(o){fputs("x",o);fclose(o);} }
  recover_orphans(base);
  failed |= stat(CACHE_ROOT "/backup-selftest/orphan",&st)!=0;
  debugPrintf("[backuptest] %s staged copy, failed metadata copy preserves generation, rotation retains identity, unchanged skips rotation, orphan .bak restored\n",failed ? "FAIL":"PASS");
  remove_tree(base); return failed;
}

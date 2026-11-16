#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include "so_util.h"
#include "util.h"

#define PAGE 0x1000
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((uintptr_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uintptr_t)(a) - 1))

/* Called for any import we could not resolve. The load log lists their names. */
static long so_unresolved_stub(void) {
  debugPrintf("[so] an unresolved import was called (see 'unresolved' lines in log)\n");
  return 0;
}

int so_load(so_module *mod, const char *path) {
  memset(mod, 0, sizeof(*mod));

  FILE *f = fopen(path, "rb");
  if (!f) { debugPrintf("[so] cannot open %s\n", path); return -1; }
  fseek(f, 0, SEEK_END);
  mod->file_size = ftell(f);
  fseek(f, 0, SEEK_SET);
  mod->file = malloc(mod->file_size);
  if (!mod->file || fread(mod->file, 1, mod->file_size, f) != mod->file_size) {
    fclose(f);
    debugPrintf("[so] read failed\n");
    return -1;
  }
  fclose(f);

  Elf64_Ehdr *eh = mod->ehdr = (Elf64_Ehdr *)mod->file;
  if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 || eh->e_ident[4] != 2 || eh->e_machine != 183) {
    debugPrintf("[so] not an aarch64 ELF64\n");
    return -1;
  }
  mod->phdr = (Elf64_Phdr *)(mod->file + eh->e_phoff);

  /* Size of the whole image = highest end of any PT_LOAD. */
  uintptr_t end = 0;
  for (int i = 0; i < eh->e_phnum; i++) {
    Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type == PT_LOAD && p->p_vaddr + p->p_memsz > end)
      end = p->p_vaddr + p->p_memsz;
  }
  mod->image_size = ALIGN_UP(end, PAGE);
  mod->image = memalign(PAGE, mod->image_size);
  if (!mod->image) { debugPrintf("[so] out of memory for image (%zu)\n", mod->image_size); return -1; }
  memset(mod->image, 0, mod->image_size); /* also zero-fills .bss */

  for (int i = 0; i < eh->e_phnum; i++) {
    Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type == PT_LOAD)
      memcpy(mod->image + p->p_vaddr, mod->file + p->p_offset, p->p_filesz);
  }

  /* Dynamic symbol table from section headers (still present in stripped NDK libs). */
  Elf64_Shdr *sh = (Elf64_Shdr *)(mod->file + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type == SHT_DYNSYM) {
      mod->dynsym = (Elf64_Sym *)(mod->file + sh[i].sh_offset);
      mod->num_dynsym = sh[i].sh_size / sizeof(Elf64_Sym);
      mod->dynstr = (const char *)(mod->file + sh[sh[i].sh_link].sh_offset);
    }
  }
  if (!mod->dynsym) { debugPrintf("[so] no .dynsym\n"); return -1; }

  /* Dynamic section: relocations and initializers. Pointers are vaddrs, read from the image. */
  for (int i = 0; i < eh->e_phnum; i++) {
    if (mod->phdr[i].p_type != PT_DYNAMIC) continue;
    Elf64_Dyn *d = (Elf64_Dyn *)(mod->image + mod->phdr[i].p_vaddr);
    for (; d->d_tag != DT_NULL; d++) {
      switch (d->d_tag) {
        case DT_RELA:         mod->rela = (Elf64_Rela *)(mod->image + d->d_val); break;
        case DT_RELASZ:       mod->num_rela = d->d_val / sizeof(Elf64_Rela); break;
        case DT_JMPREL:       mod->plt_rela = (Elf64_Rela *)(mod->image + d->d_val); break;
        case DT_PLTRELSZ:     mod->num_plt_rela = d->d_val / sizeof(Elf64_Rela); break;
        case DT_INIT:         mod->init_off = d->d_val; break;
        case DT_INIT_ARRAY:   mod->init_array_off = d->d_val; break;
        case DT_INIT_ARRAYSZ: mod->num_init_array = d->d_val / sizeof(void *); break;
      }
    }
  }

  /* Pick the final address now so relocations can target it. In emulator
   * mode the image runs where it was built (see so_emulator_mode). */
  if (so_emulator_mode) {
    mod->base = (uintptr_t)mod->image;
  } else {
    virtmemLock();
    mod->base = (uintptr_t)virtmemFindCodeMemory(mod->image_size, PAGE);
    virtmemUnlock();
  }
  if (!mod->base) { debugPrintf("[so] no free code memory region\n"); return -1; }

  debugPrintf("[so] loaded %s: %zu bytes, image 0x%zx, base %p, %zu rela, %zu plt, %zu syms\n",
              path, mod->file_size, mod->image_size, (void *)mod->base,
              mod->num_rela, mod->num_plt_rela, mod->num_dynsym);
  return 0;
}

static int apply_relocs(so_module *mod, Elf64_Rela *rels, size_t count, so_resolver resolve) {
  for (size_t i = 0; i < count; i++) {
    Elf64_Rela *r = &rels[i];
    uint64_t *where = (uint64_t *)(mod->image + r->r_offset);
    uint32_t type = ELF64_R_TYPE(r->r_info);
    Elf64_Sym *sym = &mod->dynsym[ELF64_R_SYM(r->r_info)];

    if (type == R_AARCH64_RELATIVE) {
      *where = mod->base + r->r_addend;
      continue;
    }
    if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT) {
      debugPrintf("[so] unsupported relocation type %u at 0x%llx\n", type, (unsigned long long)r->r_offset);
      return -1;
    }

    uintptr_t value;
    if (sym->st_shndx != SHN_UNDEF) {
      value = mod->base + sym->st_value; /* symbol defined in the library itself */
    } else {
      const char *name = mod->dynstr + sym->st_name;
      void *addr = resolve(name);
      if (!addr) {
        static const char *last; /* the same name shows up in several relocations */
        if (last != name) debugPrintf("[so] unresolved: %s\n", name);
        last = name;
        mod->unresolved++;
        addr = (void *)so_unresolved_stub;
      }
      value = (uintptr_t)addr;
    }
    *where = value + r->r_addend;
  }
  return 0;
}

int so_relocate(so_module *mod, so_resolver resolve) {
  if (apply_relocs(mod, mod->rela, mod->num_rela, resolve) < 0) return -1;
  if (apply_relocs(mod, mod->plt_rela, mod->num_plt_rela, resolve) < 0) return -1;
  debugPrintf("[so] relocated, %d unresolved import relocations\n", mod->unresolved);
  return 0;
}

/* Same trick hbloader uses: send the current-process pseudo-handle to
 * ourselves over IPC; the kernel turns it into a real handle in transit. */
static Handle received_proc_handle;

static void proc_handle_thread(void *arg) {
  Handle session = (Handle)(uintptr_t)arg;
  s32 idx = 0;
  void *base = armGetTls();
  hipcMakeRequestInline(base);
  Result rc = svcReplyAndReceive(&idx, &session, 1, INVALID_HANDLE, UINT64_MAX);
  if (R_SUCCEEDED(rc)) {
    HipcParsedRequest r = hipcParseRequest(base);
    if (r.meta.num_copy_handles >= 1) received_proc_handle = r.data.copy_handles[0];
  } else {
    debugPrintf("[so] proc handle receive failed: 0x%x\n", rc);
  }
  svcCloseHandle(session);
}

Handle get_own_process_handle(void) {
  Handle h = envGetOwnProcessHandle();
  if (h != INVALID_HANDLE) return h;

  debugPrintf("[so] launcher gave no process handle, requesting one via IPC\n");
  Handle server, client;
  Result rc = svcCreateSession(&server, &client, 0, 0);
  if (R_FAILED(rc)) { debugPrintf("[so] svcCreateSession failed: 0x%x\n", rc); return CUR_PROCESS_HANDLE; }

  Thread t;
  rc = threadCreate(&t, proc_handle_thread, (void *)(uintptr_t)server, NULL, 0x4000, 0x20, -2);
  if (R_FAILED(rc)) { debugPrintf("[so] threadCreate failed: 0x%x\n", rc); return CUR_PROCESS_HANDLE; }
  threadStart(&t);

  HipcRequest req = hipcMakeRequestInline(armGetTls(), .num_copy_handles = 1);
  req.copy_handles[0] = CUR_PROCESS_HANDLE;
  svcSendSyncRequest(client); /* returns an error since nobody replies; the handle is already delivered */
  svcCloseHandle(client);
  threadWaitForExit(&t);
  threadClose(&t);

  debugPrintf("[so] got process handle 0x%x\n", received_proc_handle);
  return received_proc_handle ? received_proc_handle : CUR_PROCESS_HANDLE;
}

int so_emulator_mode;

int so_finalize(so_module *mod) {
  if (so_emulator_mode) {
    /* Ryujinx's JIT executes from plain heap memory, and its code-memory
     * remapping deadlocks with GPU/audio memory sharing. Hardware never
     * takes this path. */
    /* No cache maintenance: the JIT doesn't need it, and per-line `ic ivau`
     * over 27 MB made Ryujinx crawl. */
    mod->finalized = 1;
    debugPrintf("[so] EMULATOR MODE: running from heap at %p, no code mapping\n", (void *)mod->base);
    return 0;
  }
  Handle self = get_own_process_handle();
  Result rc;

  /* Flush the freshly written image before it becomes code. */
  debugPrintf("[so] flushing dcache\n");
  armDCacheFlush(mod->image, mod->image_size);

  debugPrintf("[so] svcMapProcessCodeMemory(handle 0x%x, %p <- %p, 0x%zx)\n", self, (void *)mod->base, mod->image, mod->image_size);
  virtmemLock();
  rc = svcMapProcessCodeMemory(self, mod->base, (u64)mod->image, mod->image_size);
  if (R_SUCCEEDED(rc)) virtmemAddReservation((void *)mod->base, mod->image_size);
  virtmemUnlock();
  if (R_FAILED(rc)) {
    debugPrintf("[so] svcMapProcessCodeMemory failed: 0x%x (launch via title override, not album)\n", rc);
    return -1;
  }

  debugPrintf("[so] mapped, setting permissions\n");
  {
    struct stat dbg;
    if (stat("sdmc:/switch/sixguns/debug_noperm", &dbg) == 0) {
      debugPrintf("[so] DEBUG: skipping permission changes\n");
      mod->image = NULL; mod->finalized = 1;
      return 0;
    }
  }
  /* Everything read-only first, then open up each segment. */
  rc = svcSetProcessMemoryPermission(self, mod->base, mod->image_size, Perm_R);
  if (R_FAILED(rc)) { debugPrintf("[so] set R failed: 0x%x\n", rc); return -1; }

  for (int i = 0; i < mod->ehdr->e_phnum; i++) {
    Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD) continue;
    uintptr_t start = ALIGN_DOWN(p->p_vaddr, PAGE);
    uintptr_t end = ALIGN_UP(p->p_vaddr + p->p_memsz, PAGE);
    u32 perm = (p->p_flags & PF_X) ? Perm_Rx : (p->p_flags & PF_W) ? Perm_Rw : Perm_R;
    debugPrintf("[so]  perm %u on +0x%lx..+0x%lx\n", perm, start, end);
    rc = svcSetProcessMemoryPermission(self, mod->base + start, end - start, perm);
    if (R_FAILED(rc)) {
      debugPrintf("[so] set perm %u on 0x%lx+0x%lx failed: 0x%x\n", perm, start, end - start, rc);
      return -1;
    }
  }

  /* The heap buffer is now owned by the mapping; never touch mod->image again. */
  mod->image = NULL;
  mod->rela = mod->plt_rela = NULL;
  mod->finalized = 1;
  debugPrintf("[so] mapped at %p\n", (void *)mod->base);
  return 0;
}

void so_run_init(so_module *mod) {
  if (mod->init_off) {
    debugPrintf("[so] DT_INIT\n");
    ((void (*)(void))(mod->base + mod->init_off))();
  }
  void (**arr)(void) = (void (**)(void))(mod->base + mod->init_array_off);
  for (size_t i = 0; mod->init_array_off && i < mod->num_init_array; i++) {
    if (arr[i] && arr[i] != (void (*)(void))-1) arr[i]();
  }
  debugPrintf("[so] ran %zu init_array entries\n", mod->num_init_array);
}

uintptr_t so_symbol(so_module *mod, const char *name) {
  for (size_t i = 0; i < mod->num_dynsym; i++) {
    Elf64_Sym *s = &mod->dynsym[i];
    if (s->st_shndx != SHN_UNDEF && strcmp(mod->dynstr + s->st_name, name) == 0)
      return mod->base + s->st_value;
  }
  return 0;
}

const Elf64_Phdr *so_phdrs(so_module *mod, int *count) {
  *count = mod->ehdr->e_phnum;
  /* The first PT_LOAD starts at file offset 0, so the phdrs are inside the mapped image. */
  return (const Elf64_Phdr *)(mod->base + mod->ehdr->e_phoff);
}

/* Name of the exported symbol containing addr (for logging), or NULL. */
const char *so_symbol_name(so_module *mod, uintptr_t addr, uintptr_t *offset) {
  const Elf64_Sym *best = NULL;
  for (size_t i = 0; i < mod->num_dynsym; i++) {
    const Elf64_Sym *s = &mod->dynsym[i];
    if (s->st_shndx == SHN_UNDEF || !s->st_value) continue;
    uintptr_t a = mod->base + s->st_value;
    if (a <= addr && (!best || a > mod->base + best->st_value)) best = s;
  }
  if (!best) return NULL;
  if (offset) *offset = addr - (mod->base + best->st_value);
  return mod->dynstr + best->st_name;
}

/* Replaces the start of an exported function with `mov w0, #0; ret`, so every
 * caller (through the PLT or direct) gets an immediate return. Only valid
 * between so_load and so_finalize, while the image is still writable. */
int so_stub_function(so_module *mod, const char *name) { return so_stub_function_value(mod, name, 0); }

/* Same as so_stub_function, but the function returns `value` (0..65535). */
int so_stub_function_value(so_module *mod, const char *name, uint16_t value) {
  if (mod->finalized || !mod->image) return -1;
  for (size_t i = 0; i < mod->num_dynsym; i++) {
    Elf64_Sym *s = &mod->dynsym[i];
    if (s->st_shndx == SHN_UNDEF || strcmp(mod->dynstr + s->st_name, name) != 0) continue;
    uint32_t *code = (uint32_t *)(mod->image + s->st_value);
    code[0] = 0x52800000 | ((uint32_t)value << 5); /* movz w0, #value */
    code[1] = 0xd65f03c0; /* ret */
    return 0;
  }
  debugPrintf("[so] stub target not found: %s\n", name);
  return -1;
}

/* Redirects an engine function to `target` (absolute branch: ldr x16, #8;
 * br x16; .quad target). Before finalize only, like the stubs. */
int so_hook_function(so_module *mod, const char *name, void *target) {
  if (mod->finalized || !mod->image) return -1;
  for (size_t i = 0; i < mod->num_dynsym; i++) {
    Elf64_Sym *s = &mod->dynsym[i];
    if (s->st_shndx == SHN_UNDEF || strcmp(mod->dynstr + s->st_name, name) != 0) continue;
    if (s->st_size && s->st_size < 16) { debugPrintf("[so] hook target too small: %s\n", name); return -1; }
    uint32_t *code = (uint32_t *)(mod->image + s->st_value);
    code[0] = 0x58000050; /* ldr x16, #8 */
    code[1] = 0xd61f0200; /* br x16 */
    memcpy(&code[2], &target, sizeof(target));
    return 0;
  }
  debugPrintf("[so] hook target not found: %s\n", name);
  return -1;
}

/* PLT bypass. The engine is a shared library built with -fPIC, so even calls
 * between its own functions go through PLT stubs (adrp x16; ldr x17,[x16,#o];
 * add x16,x16,#o; br x17), an extra load and indirect branch per call. The
 * shop's UI code makes huge numbers of tiny calls (vector accessors, sprite
 * getters), and those stubs showed up as ~6% of a shop frame. After
 * relocation the GOT holds the final addresses, so every `bl`/`b` that
 * targets a stub whose GOT slot (validated against the JUMP_SLOT
 * relocations) points back into the engine is rewritten to branch there
 * directly. Calls to imports (port/newlib functions, out of branch range)
 * keep their stubs. Before finalize only. Returns the number rewritten. */
static int plt_stub_got(so_module *mod, uint64_t off, uint64_t *got) {
  if (off + 16 > mod->image_size) return 0;
  const uint32_t *p = (const uint32_t *)(mod->image + off);
  if ((p[0] & 0x9f00001f) != 0x90000010) return 0;          /* adrp x16 */
  if ((p[1] & 0xffc003ff) != 0xf9400211) return 0;          /* ldr x17, [x16, #imm] */
  if ((p[2] & 0xffc003ff) != 0x91000210) return 0;          /* add x16, x16, #imm */
  if (p[3] != 0xd61f0220) return 0;                          /* br x17 */
  int64_t imm = (int64_t)(((p[0] >> 29) & 3) | (((p[0] >> 5) & 0x7ffff) << 2));
  if (imm & (1 << 20)) imm -= 1 << 21;
  uint64_t page = (off & ~0xfffULL) + (uint64_t)(imm << 12);
  *got = page + ((p[1] >> 10) & 0xfff) * 8;
  return *got + 8 <= mod->image_size;
}

int so_bypass_plt(so_module *mod) {
  if (mod->finalized || !mod->image) return -1;
  /* GOT slots of the PLT (JUMP_SLOT relocation offsets), for validation */
  uint64_t got_lo = ~0ULL, got_hi = 0;
  for (size_t i = 0; i < mod->num_plt_rela; i++) {
    uint64_t o = mod->plt_rela[i].r_offset;
    if (o < got_lo) got_lo = o;
    if (o > got_hi) got_hi = o;
  }
  if (!mod->num_plt_rela) return 0;
  int n = 0, seen = 0;
  for (int k = 0; k < mod->ehdr->e_phnum; k++) {
    Elf64_Phdr *ph = &mod->phdr[k];
    if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X)) continue;
    uint64_t lo = ph->p_vaddr, hi = ph->p_vaddr + ph->p_filesz;
    for (uint64_t pc = lo; pc + 4 <= hi; pc += 4) {
      uint32_t *ins = (uint32_t *)(mod->image + pc);
      uint32_t op = *ins & 0xfc000000;
      if (op != 0x94000000 && op != 0x14000000) continue;   /* bl / b */
      int64_t d = (int64_t)(*ins & 0x3ffffff);
      if (d & (1 << 25)) d -= 1 << 26;
      uint64_t tgt = pc + (uint64_t)(d * 4), got;
      if (tgt < lo || tgt >= hi || !plt_stub_got(mod, tgt, &got)) continue;
      if (got < got_lo || got > got_hi) continue;
      seen++;
      uint64_t v = *(uint64_t *)(mod->image + got);
      if (v < mod->base || v >= mod->base + mod->image_size) continue; /* import: keep the stub */
      uint64_t to = v - mod->base;
      if (to < lo || to >= hi) continue;
      int64_t nd = ((int64_t)to - (int64_t)pc) / 4;
      if (nd < -(1 << 25) || nd >= (1 << 25)) continue;
      *ins = op | (uint32_t)(nd & 0x3ffffff);
      n++;
    }
  }
  debugPrintf("[so] PLT bypass: %d of %d calls through PLT stubs now branch directly\n", n, seen);
  return n;
}

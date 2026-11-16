#pragma once
#include <stddef.h>
#include <stdint.h>

/* Minimal loader for an Android aarch64 shared object on Horizon.
 * The image is built in a heap buffer, relocated against its final address,
 * then mapped as process code memory with per-segment permissions. */

typedef struct {
  unsigned char e_ident[16];
  uint16_t e_type, e_machine;
  uint32_t e_version;
  uint64_t e_entry, e_phoff, e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  uint32_t p_type, p_flags;
  uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct {
  uint32_t sh_name, sh_type;
  uint64_t sh_flags, sh_addr, sh_offset, sh_size;
  uint32_t sh_link, sh_info;
  uint64_t sh_addralign, sh_entsize;
} Elf64_Shdr;

typedef struct {
  uint32_t st_name;
  unsigned char st_info, st_other;
  uint16_t st_shndx;
  uint64_t st_value, st_size;
} Elf64_Sym;

typedef struct {
  uint64_t r_offset, r_info;
  int64_t r_addend;
} Elf64_Rela;

typedef struct {
  int64_t d_tag;
  uint64_t d_val;
} Elf64_Dyn;

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define SHT_DYNSYM 11
#define DT_NULL 0
#define DT_PLTRELSZ 2
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_INIT 12
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define SHN_UNDEF 0
#define ELF64_R_SYM(i)  ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
#define R_AARCH64_ABS64     257
#define R_AARCH64_GLOB_DAT  1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE  1027

typedef struct {
  const char *name;
  void *addr;
} so_import;

/* Returns the address for an imported symbol, or NULL if unknown. */
typedef void *(*so_resolver)(const char *name);

typedef struct {
  uint8_t *file;         /* whole ELF file, kept for symbol/dynamic parsing */
  size_t file_size;
  uint8_t *image;        /* page-aligned heap buffer the segments are built in */
  uintptr_t base;        /* final address after mapping */
  size_t image_size;

  Elf64_Ehdr *ehdr;
  Elf64_Phdr *phdr;
  Elf64_Sym *dynsym;
  size_t num_dynsym;
  const char *dynstr;

  Elf64_Rela *rela, *plt_rela;
  size_t num_rela, num_plt_rela;

  uint64_t init_off, init_array_off;
  size_t num_init_array;

  int unresolved;
  int finalized;
} so_module;

/* Set before so_load. Emulator-only: execute the image from the heap instead
 * of mapping it as code memory (Ryujinx's remapping deadlocks). */
extern int so_emulator_mode;

int so_load(so_module *mod, const char *path);
int so_relocate(so_module *mod, so_resolver resolve);
int so_finalize(so_module *mod);
void so_run_init(so_module *mod);
uintptr_t so_symbol(so_module *mod, const char *name);
const Elf64_Phdr *so_phdrs(so_module *mod, int *count);

/* Real handle to our own process (via hbloader, or an IPC trick). */
#include <switch/types.h>
Handle get_own_process_handle(void);
const char *so_symbol_name(so_module *mod, uintptr_t addr, uintptr_t *offset);
int so_stub_function(so_module *mod, const char *name);
int so_stub_function_value(so_module *mod, const char *name, uint16_t value);
int so_hook_function(so_module *mod, const char *name, void *target);
int so_bypass_plt(so_module *mod);

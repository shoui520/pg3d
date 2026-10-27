#ifndef PG3D_SO_LOADER_H
#define PG3D_SO_LOADER_H

#include <stddef.h>
#include <stdint.h>
#include <psp2/types.h>

#include "loader/elf32.h"

typedef struct so_module {
  struct so_module *next;
  char name[32];
  SceUID block;
  uintptr_t base;        /* load bias: module vaddr 0 maps here */
  size_t span;           /* bytes mapped from base */
  uintptr_t text_start, text_end;
  uintptr_t data_start, data_end;
  uintptr_t arena_start, arena_cur, arena_end; /* RWX scratch for stubs and trampolines */
  Elf32_Dyn *dynamic;
  Elf32_Sym *dynsym;
  uint32_t nsyms;
  const char *dynstr;
  uint32_t *hash;
  Elf32_Rel *rel;
  uint32_t nrel;
  Elf32_Rel *jmprel;
  uint32_t njmprel;
  void (**init_array)(void);
  uint32_t ninit;
  void (**fini_array)(void);
  uint32_t nfini;
  const uint32_t *exidx;
  uint32_t exidx_count;  /* entries of 8 bytes */
} so_module;

/* Called for every undefined symbol. Returns 0 when not resolvable. */
typedef uintptr_t (*so_resolver)(so_module *mod, const char *name, int is_func);

int so_load(so_module *mod, const char *path, uintptr_t base, size_t arena_size);
int so_relocate(so_module *mod, so_resolver resolve);
void so_flush(so_module *mod);
void so_run_init(so_module *mod);

uintptr_t so_symbol(const so_module *mod, const char *name);
so_module *so_by_name(const char *name);
so_module *so_by_addr(uintptr_t addr);
int so_addr_info(uintptr_t addr, const char **modname, uintptr_t *modbase, const char **symname, uintptr_t *symaddr);
const void *so_find_exidx(uintptr_t pc, int *count);

void *so_arena_alloc(so_module *mod, size_t size);
/* Redirects an ARM-mode function to dst with LDR PC. Returns 0 on failure. */
int so_hook_arm(uintptr_t addr, const void *dst);
/* Writes words into module code (caller flushes). */
void so_write(uintptr_t addr, const void *data, size_t size);

#endif

/*
 * Minimal multi-module ELF loader for Android ARM shared objects.
 *
 * Order of operations per module: so_load (map + copy segments), so_relocate
 * (relocations and import resolution), caller-applied patches, so_flush, and
 * only then so_run_init.
 */
#include "loader/so_loader.h"

#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <kubridge.h>

#include "util/log.h"

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static so_module *modules;

static void unresolved_called(const char *name) {
  fatal("[so] unresolved import called: %s (caller lr=%p)", name, __builtin_return_address(0));
}

static int read_full(SceUID fd, void *dst, size_t size, uint32_t offset) {
  if (sceIoLseek(fd, offset, SCE_SEEK_SET) != (SceOff)offset)
    return -1;
  uint8_t *p = dst;
  while (size) {
    size_t chunk = size > 0x400000 ? 0x400000 : size;
    int r = sceIoRead(fd, p, chunk);
    if (r <= 0)
      return -1;
    p += r;
    size -= r;
  }
  return 0;
}

int so_load(so_module *mod, const char *path, uintptr_t base, size_t arena_size) {
  memset(mod, 0, sizeof(*mod));
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) {
    LOG("[so] cannot open %s: 0x%08X", path, fd);
    return fd;
  }

  Elf32_Ehdr eh;
  if (read_full(fd, &eh, sizeof(eh), 0) < 0 || memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0 ||
      eh.e_ident[4] != 1 || eh.e_machine != 40 || eh.e_phentsize != sizeof(Elf32_Phdr)) {
    LOG("[so] %s: not a 32-bit ARM ELF", path);
    sceIoClose(fd);
    return -1;
  }

  Elf32_Phdr *ph = malloc(eh.e_phnum * sizeof(Elf32_Phdr));
  if (!ph || read_full(fd, ph, eh.e_phnum * sizeof(Elf32_Phdr), eh.e_phoff) < 0) {
    free(ph);
    sceIoClose(fd);
    return -1;
  }

  uintptr_t max_end = 0;
  for (int i = 0; i < eh.e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > max_end)
      max_end = ph[i].p_vaddr + ph[i].p_memsz;

  size_t image = ALIGN_UP(max_end, 0x1000);
  arena_size = ALIGN_UP(arena_size, 0x1000);
  mod->span = image + arena_size;

  SceKernelAllocMemBlockKernelOpt opt;
  memset(&opt, 0, sizeof(opt));
  opt.size = sizeof(opt);
  opt.attr = 0x1; /* fixed virtual base */
  opt.field_C = base;
  mod->block = kuKernelAllocMemBlock("pg3d_so", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, mod->span, &opt);
  if (mod->block < 0) {
    LOG("[so] %s: kuKernelAllocMemBlock(0x%08X, 0x%X) failed: 0x%08X", path, base, mod->span, mod->block);
    free(ph);
    sceIoClose(fd);
    return mod->block;
  }
  void *blk;
  sceKernelGetMemBlockBase(mod->block, &blk);
  mod->base = (uintptr_t)blk;
  int r = kuKernelMemProtect(blk, mod->span, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_EXEC);
  if (r < 0)
    LOG("[so] %s: kuKernelMemProtect failed: 0x%08X", path, r);
  sceClibMemset(blk, 0, mod->span);

  for (int i = 0; i < eh.e_phnum; i++) {
    Elf32_Phdr *p = &ph[i];
    if (p->p_type == PT_LOAD) {
      if (p->p_filesz && read_full(fd, (void *)(mod->base + p->p_vaddr), p->p_filesz, p->p_offset) < 0) {
        LOG("[so] %s: segment %d read failed", path, i);
        sceKernelFreeMemBlock(mod->block);
        free(ph);
        sceIoClose(fd);
        return -1;
      }
      if (p->p_flags & PF_X) {
        mod->text_start = mod->base + p->p_vaddr;
        mod->text_end = mod->text_start + p->p_memsz;
      } else if (p->p_flags & PF_W) {
        mod->data_start = mod->base + p->p_vaddr;
        mod->data_end = mod->data_start + p->p_memsz;
      }
    } else if (p->p_type == PT_DYNAMIC) {
      mod->dynamic = (Elf32_Dyn *)(mod->base + p->p_vaddr);
    } else if (p->p_type == PT_ARM_EXIDX) {
      mod->exidx = (const uint32_t *)(mod->base + p->p_vaddr);
      mod->exidx_count = p->p_memsz / 8;
    }
  }
  free(ph);
  sceIoClose(fd);

  if (!mod->dynamic) {
    LOG("[so] %s: no PT_DYNAMIC", path);
    return -1;
  }

  uint32_t relsz = 0, pltrelsz = 0, initsz = 0, finisz = 0, soname = 0;
  for (Elf32_Dyn *d = mod->dynamic; d->d_tag != DT_NULL; d++) {
    switch (d->d_tag) {
    case DT_SYMTAB: mod->dynsym = (Elf32_Sym *)(mod->base + d->d_un.d_ptr); break;
    case DT_STRTAB: mod->dynstr = (const char *)(mod->base + d->d_un.d_ptr); break;
    case DT_HASH: mod->hash = (uint32_t *)(mod->base + d->d_un.d_ptr); break;
    case DT_REL: mod->rel = (Elf32_Rel *)(mod->base + d->d_un.d_ptr); break;
    case DT_RELSZ: relsz = d->d_un.d_val; break;
    case DT_JMPREL: mod->jmprel = (Elf32_Rel *)(mod->base + d->d_un.d_ptr); break;
    case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
    case DT_INIT_ARRAY: mod->init_array = (void (**)(void))(mod->base + d->d_un.d_ptr); break;
    case DT_INIT_ARRAYSZ: initsz = d->d_un.d_val; break;
    case DT_FINI_ARRAY: mod->fini_array = (void (**)(void))(mod->base + d->d_un.d_ptr); break;
    case DT_FINI_ARRAYSZ: finisz = d->d_un.d_val; break;
    case DT_SONAME: soname = d->d_un.d_val; break;
    case DT_INIT:
    case DT_FINI:
      LOG("[so] %s: DT_INIT/DT_FINI present but unsupported", path);
      break;
    default: break;
    }
  }
  mod->nrel = relsz / sizeof(Elf32_Rel);
  mod->njmprel = pltrelsz / sizeof(Elf32_Rel);
  mod->ninit = initsz / sizeof(void *);
  mod->nfini = finisz / sizeof(void *);
  mod->nsyms = mod->hash ? mod->hash[1] : 0;
  const char *n = soname ? mod->dynstr + soname : path;
  const char *slash = strrchr(n, '/');
  strncpy(mod->name, slash ? slash + 1 : n, sizeof(mod->name) - 1);

  mod->arena_start = mod->arena_cur = mod->base + image;
  mod->arena_end = mod->base + mod->span;

  mod->next = modules;
  modules = mod;
  LOG("[so] loaded %s at 0x%08X span 0x%X text 0x%08X-0x%08X data 0x%08X-0x%08X", mod->name, mod->base,
      mod->span, mod->text_start, mod->text_end, mod->data_start, mod->data_end);
  return 0;
}

void *so_arena_alloc(so_module *mod, size_t size) {
  size = ALIGN_UP(size, 8);
  if (mod->arena_cur + size > mod->arena_end)
    return NULL;
  void *p = (void *)mod->arena_cur;
  mod->arena_cur += size;
  return p;
}

static uintptr_t make_unresolved_stub(so_module *mod, const char *name) {
  uint32_t *s = so_arena_alloc(mod, 16);
  if (!s)
    return (uintptr_t)unresolved_called;
  s[0] = 0xe59f0000; /* ldr r0, [pc] -> s[2] */
  s[1] = 0xe59ff000; /* ldr pc, [pc] -> s[3] */
  s[2] = (uint32_t)name;
  s[3] = (uint32_t)unresolved_called;
  return (uintptr_t)s;
}

int so_relocate(so_module *mod, so_resolver resolve) {
  uintptr_t *cache = calloc(mod->nsyms ? mod->nsyms : 1, sizeof(uintptr_t));
  uint8_t *done = calloc(mod->nsyms ? mod->nsyms : 1, 1);
  int unresolved = 0;

  for (int pass = 0; pass < 2; pass++) {
    Elf32_Rel *rels = pass ? mod->jmprel : mod->rel;
    uint32_t count = pass ? mod->njmprel : mod->nrel;
    for (uint32_t i = 0; i < count; i++) {
      uint32_t type = ELF32_R_TYPE(rels[i].r_info);
      uint32_t si = ELF32_R_SYM(rels[i].r_info);
      uintptr_t *where = (uintptr_t *)(mod->base + rels[i].r_offset);

      if (type == R_ARM_RELATIVE) {
        *where += mod->base;
        continue;
      }
      if (type == R_ARM_NONE)
        continue;
      if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) {
        LOG("[so] %s: unsupported relocation type %u at 0x%X", mod->name, type, rels[i].r_offset);
        continue;
      }

      uintptr_t S = 0;
      if (si) {
        if (si < mod->nsyms && done[si]) {
          S = cache[si];
        } else {
          Elf32_Sym *sym = &mod->dynsym[si];
          const char *name = mod->dynstr + sym->st_name;
          if (sym->st_shndx != SHN_UNDEF) {
            S = mod->base + sym->st_value;
          } else {
            int is_func = type == R_ARM_JUMP_SLOT || ELF32_ST_TYPE(sym->st_info) == STT_FUNC;
            S = resolve(mod, name, is_func);
            if (!S && ELF32_ST_BIND(sym->st_info) != STB_WEAK) {
              unresolved++;
              LOG("[so] %s: unresolved %s %s", mod->name, is_func ? "function" : "object", name);
              if (is_func)
                S = make_unresolved_stub(mod, name);
            }
          }
          if (si < mod->nsyms) {
            cache[si] = S;
            done[si] = 1;
          }
        }
      }

      if (type == R_ARM_ABS32)
        *where += S;
      else
        *where = S;
    }
  }

  free(cache);
  free(done);
  LOG("[so] %s: relocated, %d unresolved imports", mod->name, unresolved);
  return unresolved;
}

void so_flush(so_module *mod) {
  kuKernelFlushCaches((void *)mod->base, mod->span);
}

void so_run_init(so_module *mod) {
  for (uint32_t i = 0; i < mod->ninit; i++) {
    void (*f)(void) = mod->init_array[i];
    if (f && f != (void (*)(void))-1)
      f();
  }
  LOG("[so] %s: ran %u initializers", mod->name, mod->ninit);
}

static uint32_t elf_hash(const char *name) {
  uint32_t h = 0, g;
  while (*name) {
    h = (h << 4) + (uint8_t)*name++;
    g = h & 0xf0000000;
    if (g)
      h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

uintptr_t so_symbol(const so_module *mod, const char *name) {
  if (!mod->hash)
    return 0;
  uint32_t nbucket = mod->hash[0];
  const uint32_t *bucket = &mod->hash[2];
  const uint32_t *chain = &bucket[nbucket];
  for (uint32_t i = bucket[elf_hash(name) % nbucket]; i; i = chain[i]) {
    const Elf32_Sym *s = &mod->dynsym[i];
    if (s->st_shndx == SHN_UNDEF || ELF32_ST_BIND(s->st_info) == STB_LOCAL)
      continue;
    if (strcmp(mod->dynstr + s->st_name, name) == 0)
      return mod->base + s->st_value;
  }
  return 0;
}

so_module *so_by_name(const char *name) {
  const char *slash = strrchr(name, '/');
  if (slash)
    name = slash + 1;
  for (so_module *m = modules; m; m = m->next)
    if (strcmp(m->name, name) == 0)
      return m;
  return NULL;
}

so_module *so_by_addr(uintptr_t addr) {
  for (so_module *m = modules; m; m = m->next)
    if (addr >= m->base && addr < m->base + m->span)
      return m;
  return NULL;
}

int so_addr_info(uintptr_t addr, const char **modname, uintptr_t *modbase, const char **symname, uintptr_t *symaddr) {
  so_module *m = so_by_addr(addr);
  if (!m)
    return 0;
  *modname = m->name;
  *modbase = m->base;
  *symname = NULL;
  *symaddr = 0;
  uintptr_t best = 0;
  for (uint32_t i = 1; i < m->nsyms; i++) {
    const Elf32_Sym *s = &m->dynsym[i];
    if (s->st_shndx == SHN_UNDEF)
      continue;
    uintptr_t a = m->base + (s->st_value & ~1u);
    if (a <= addr && a > best && (s->st_size == 0 || addr < a + s->st_size)) {
      best = a;
      *symname = m->dynstr + s->st_name;
      *symaddr = m->base + s->st_value;
    }
  }
  return 1;
}

const void *so_find_exidx(uintptr_t pc, int *count) {
  so_module *m = so_by_addr(pc);
  if (!m || !m->exidx) {
    *count = 0;
    return NULL;
  }
  *count = m->exidx_count;
  return m->exidx;
}

void so_write(uintptr_t addr, const void *data, size_t size) {
  sceClibMemcpy((void *)addr, data, size);
}

int so_hook_arm(uintptr_t addr, const void *dst) {
  if (!addr)
    return 0;
  if (addr & 1) {
    addr &= ~1u;
    uint16_t *p = (uint16_t *)addr;
    if (addr & 2)
      *p++ = 0xbf00; /* nop to align */
    p[0] = 0xf8df;   /* ldr.w pc, [pc, #0] */
    p[1] = 0xf000;
    *(uint32_t *)&p[2] = (uint32_t)dst;
    kuKernelFlushCaches((void *)addr, 12);
  } else {
    uint32_t *p = (uint32_t *)addr;
    p[0] = 0xe51ff004; /* ldr pc, [pc, #-4] */
    p[1] = (uint32_t)dst;
    kuKernelFlushCaches((void *)addr, 8);
  }
  return 1;
}

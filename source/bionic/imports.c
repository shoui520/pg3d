/*
 * Import table for the game libraries: Android symbol name -> our address.
 * Built without libc headers (targets are declared as opaque symbols) and
 * with -fno-builtin; see imports_table.h.
 */
#include <stdint.h>

#include "loader/so_loader.h"

typedef struct {
  const char *name;
  const void *addr;
} import_entry;

#define IMP(n, t) extern char t[];
#include "bionic/imports_table.h"
#undef IMP

static const import_entry table[] = {
#define IMP(n, t) { n, t },
#include "bionic/imports_table.h"
#undef IMP
};

void log_printf(const char *fmt, ...);

static int cmp(const char *a, const char *b) {
  while (*a && *a == *b)
    a++, b++;
  return (unsigned char)*a - (unsigned char)*b;
}

uintptr_t imports_lookup(const char *name) {
  int lo = 0, hi = sizeof(table) / sizeof(table[0]) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int c = cmp(name, table[mid].name);
    if (c == 0)
      return (uintptr_t)table[mid].addr;
    if (c < 0)
      hi = mid - 1;
    else
      lo = mid + 1;
  }
  return 0;
}

extern so_module *gamebin_il2cpp;

/* so_relocate resolver for libunity.so and libil2cpp.so. */
uintptr_t imports_resolve(so_module *mod, const char *name, int is_func) {
  uintptr_t a = imports_lookup(name);
  if (a)
    return a;
  /* bdwgc's DATASTART: the start of libil2cpp's own data (its _end is local) */
  if (!cmp(name, "data_start") && gamebin_il2cpp)
    return gamebin_il2cpp->data_start;
  return 0;
}

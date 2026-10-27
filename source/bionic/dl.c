/* dlopen/dlsym emulation over loaded modules, the import table, GL and OpenSL. */
#include <string.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <vitaGL.h>

#include "bionic/bionic.h"
#include "gfx/gl.h"
#include "loader/so_loader.h"
#include "util/log.h"

uintptr_t imports_lookup(const char *name);

enum { H_MOD = 0, H_GLOBAL, H_GL, H_SLES, H_LIBC };

typedef struct {
  int kind;
  so_module *mod;
} dl_handle;

static dl_handle h_global = { H_GLOBAL, NULL };
static dl_handle h_gl = { H_GL, NULL };
static dl_handle h_sles = { H_SLES, NULL };
static dl_handle h_libc = { H_LIBC, NULL };
static dl_handle h_mods[8];
static const char *last_error;

static int ends_with(const char *s, const char *suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && strcmp(s + a - b, suffix) == 0;
}

void *b_dlopen(const char *name, int flags) {
  DLOG("[dl] dlopen(%s)", name ? name : "(null)");
  if (!name)
    return &h_global;
  if (ends_with(name, "libGLESv2.so") || ends_with(name, "libGLESv3.so") || ends_with(name, "libGLESv1_CM.so") ||
      ends_with(name, "libEGL.so"))
    return &h_gl;
  if (ends_with(name, "libOpenSLES.so"))
    return &h_sles;
  so_module *m = so_by_name(name);
  if (m) {
    for (int i = 0; i < 8; i++) {
      if (h_mods[i].mod == m)
        return &h_mods[i];
      if (!h_mods[i].mod) {
        h_mods[i].kind = H_MOD;
        h_mods[i].mod = m;
        return &h_mods[i];
      }
    }
  }
  if (ends_with(name, "libc.so") || ends_with(name, "libm.so") || ends_with(name, "libdl.so") ||
      ends_with(name, "liblog.so") || ends_with(name, "libandroid.so") || ends_with(name, "libz.so"))
    return &h_libc;
  LOG("[dl] dlopen(%s) failed", name);
  last_error = "library not found";
  return NULL;
}

int b_dlclose(void *h) {
  return 0;
}

const char *b_dlerror(void) {
  const char *e = last_error;
  last_error = NULL;
  return e;
}

static void *sles_sym(const char *name) {
#define S(x) if (strcmp(name, #x) == 0) return (void *)&x
  S(SL_IID_ENGINE);
  S(SL_IID_PLAY);
  S(SL_IID_VOLUME);
  S(SL_IID_BUFFERQUEUE);
  S(SL_IID_ANDROIDSIMPLEBUFFERQUEUE);
  S(SL_IID_OUTPUTMIX);
  S(SL_IID_RECORD);
  S(SL_IID_EFFECTSEND);
  S(SL_IID_ENVIRONMENTALREVERB);
  S(SL_IID_PRESETREVERB);
  S(SL_IID_PLAYBACKRATE);
  S(SL_IID_SEEK);
  S(SL_IID_PREFETCHSTATUS);
  S(SL_IID_ANDROIDCONFIGURATION);
  S(SL_IID_NULL);
#undef S
  if (strcmp(name, "slCreateEngine") == 0)
    return (void *)slCreateEngine;
  return NULL;
}

/* AT_HWCAP: swp half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt */
#define AT_HWCAP 16
static unsigned long b_getauxval(unsigned long type) {
  return type == AT_HWCAP ? 0x1f0d7 : 0;
}

void *b_dlsym(void *handle, const char *name) {
  dl_handle *h = handle;
  void *r = NULL;
  if (!h || h == (void *)-1 || h == (void *)0xffffffff)
    h = &h_global;
  switch (h->kind) {
  case H_MOD:
    r = (void *)so_symbol(h->mod, name);
    break;
  case H_GL:
    r = gl_lookup(name);
    break;
  case H_SLES:
    r = sles_sym(name);
    break;
  case H_LIBC:
    r = strcmp(name, "getauxval") == 0 ? (void *)b_getauxval : (void *)imports_lookup(name);
    break;
  case H_GLOBAL:
    r = (void *)imports_lookup(name);
    for (so_module *m = so_by_name("libunity.so"); !r && m; m = NULL)
      r = (void *)so_symbol(m, name);
    if (!r) {
      so_module *m = so_by_name("libil2cpp.so");
      if (m)
        r = (void *)so_symbol(m, name);
    }
    break;
  }
  if (!r) {
    DLOG("[dl] dlsym(%d:%s, %s) not found", h->kind, h->mod ? h->mod->name : "-", name);
    last_error = "symbol not found";
  }
  return r;
}

/* bionic Dl_info */
typedef struct {
  const char *dli_fname;
  void *dli_fbase;
  const char *dli_sname;
  void *dli_saddr;
} b_dl_info;

int b_dladdr(const void *addr, b_dl_info *info) {
  const char *mod, *sym;
  uintptr_t base, saddr;
  if (!so_addr_info((uintptr_t)addr, &mod, &base, &sym, &saddr))
    return 0;
  info->dli_fname = mod;
  info->dli_fbase = (void *)base;
  info->dli_sname = sym;
  info->dli_saddr = (void *)saddr;
  return 1;
}

/* Unwinders (ours from libgcc, and the copies statically linked into the game
 * libraries) locate unwind tables through this hook. */
extern const uint32_t __exidx_start[] __attribute__((weak));
extern const uint32_t __exidx_end[] __attribute__((weak));

const void *__gnu_Unwind_Find_exidx(uintptr_t pc, int *count) {
  const void *r = so_find_exidx(pc, count);
  if (r)
    return r;
  *count = (__exidx_end - __exidx_start) / 2;
  return __exidx_start;
}

/*
 * Pixel Gun 3D 12.5.0 loader: maps the game's own libil2cpp.so and
 * libunity.so, links them against our Bionic/Android/JNI layer and runs the
 * Unity player lifecycle.
 *
 * User-supplied files (from the official APK/OBB) in ux0:data/pg3d/:
 *   libunity.so, libil2cpp.so, base.apk, obb/com.pixel.gun3d/main.1383.com.pixel.gun3d.obb
 */
#include <stdio.h>
#include <string.h>
#include <psp2/appmgr.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#include <kubridge.h>

#include "bionic/archive.h"
#include "bionic/bionic.h"
#include "bionic/bthread.h"
#include "bionic/net.h"
#include "game/gamebin.h"
#include "gfx/gl.h"
#include "input/input.h"
#include "jni/java.h"
#include "loader/so_loader.h"
#include "util/log.h"

#define DATA_ROOT "ux0:data/pg3d"
#define IL2CPP_BASE 0x9A000000
#define UNITY_BASE 0x98000000
#define GC_ARENA_SIZE (64 * 1024 * 1024) /* il2cpp's GC heap; more comes from malloc once full */

/* newlib heap: Unity's own allocations (malloc/memalign), except the big ones,
 * which get a heap of their own; past both they go to the overflow heap
 * (bionic/alloc.c) */
int _newlib_heap_size_user = 136 * 1024 * 1024;
#define BIG_HEAP_SIZE (64 * 1024 * 1024)

uintptr_t imports_resolve(so_module *mod, const char *name, int is_func);
int mman_init(size_t size);
void alloc_init_big(size_t size);
void alloc_map_big(void);
void bionic_ctype_init(void);
void unity_start(void);

static so_module mod_unity, mod_il2cpp;

static void load_module(so_module *m, const char *file, uintptr_t base) {
  char path[256];
  snprintf(path, sizeof(path), DATA_ROOT "/%s", file);
  if (so_load(m, path, base, 0x10000) < 0)
    fatal("cannot load %s", path);
}

static void check_files(void) {
  static const char *files[] = { "libunity.so", "libil2cpp.so", "base.apk",
                                 "obb/com.pixel.gun3d/main.1383.com.pixel.gun3d.obb" };
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    char path[256];
    snprintf(path, sizeof(path), DATA_ROOT "/%s", files[i]);
    FILE *f = fopen(path, "rb");
    if (!f)
      fatal("missing %s (copy it from the Pixel Gun 3D 12.5.0 APK/OBB)", path);
    fclose(f);
  }
}

static void log_memory(const char *when) {
  SceKernelFreeMemorySizeInfo info;
  info.size = sizeof(info);
  sceKernelGetFreeMemorySize(&info);
  LOG("[mem] %s: user %u KB, cdram %u KB, phycont %u KB", when, info.size_user / 1024, info.size_cdram / 1024,
      info.size_phycont / 1024);
}

int main(void) {
  sceIoMkdir(DATA_ROOT, 0777);
  log_init(DATA_ROOT "/log.txt");
  LOG("Pixel Gun 3D loader starting");

  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  log_memory("start");
  bthread_init();
  bionic_ctype_init();
  bionic_paths_init(DATA_ROOT);
  archive_init();
  bionic_sysfiles_init();
  check_files();
  if (gamebin_verify(DATA_ROOT "/libunity.so", DATA_ROOT "/libil2cpp.so", DATA_ROOT "/gamebin.ok") < 0)
    fatal("unsupported game binaries: Pixel Gun 3D 12.5.0 (armeabi-v7a) is required");

  /* the net module loads into user memory: before vitaGL takes what is left */
  net_init();

  /* the GC arena must be reserved before vitaGL takes the remaining RAM */
  if (mman_init(GC_ARENA_SIZE) < 0)
    fatal("cannot reserve the mmap arena");
  alloc_init_big(BIG_HEAP_SIZE);

  load_module(&mod_il2cpp, "libil2cpp.so", IL2CPP_BASE);
  gamebin_il2cpp = &mod_il2cpp;
  load_module(&mod_unity, "libunity.so", UNITY_BASE);
  gamebin_unity = &mod_unity;
  int unresolved = so_relocate(&mod_il2cpp, imports_resolve) + so_relocate(&mod_unity, imports_resolve);
  if (unresolved)
    LOG("%d unresolved imports (they abort when called)", unresolved);
  gamebin_patch();
  so_flush(&mod_il2cpp);
  so_flush(&mod_unity);
  log_memory("modules loaded");

  gfx_init();
  alloc_map_big();
  log_memory("vitaGL ready");

  so_run_init(&mod_il2cpp);
  so_run_init(&mod_unity);

  jvm_init();
  java_android_init();
  input_init();

  int (*JNI_OnLoad)(JavaVM *, void *) = (void *)so_symbol(&mod_unity, "JNI_OnLoad");
  int ver = JNI_OnLoad(jvm_vm, NULL);
  LOG("JNI_OnLoad -> 0x%X", ver);

  unity_start();
  return 0;
}

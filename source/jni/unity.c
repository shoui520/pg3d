/*
 * com.unity3d.player.UnityPlayer: the Java methods libunity calls back, and
 * the lifecycle the Java side drives, replayed in the order of UnityPlayer's
 * constructor and its UnityMain thread:
 *   initJni(context), nativeFile(apk), nativeFile(obb) [useObb],
 *   nativeInitWWW, nativeInitWebRequest, nativeRecreateGfxState(0, surface),
 *   nativeResume, nativeFocusChanged(true), then nativeRender() per frame.
 */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <vitaGL.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/bthread.h"
#include "input/input.h"
#include "jni/java.h"
#include "util/log.h"

#define UP "com/unity3d/player/UnityPlayer"
#define APK_PATH "/data/app/com.pixel.gun3d-1/base.apk"
#define OBB_PATH "/storage/emulated/0/Android/obb/com.pixel.gun3d/main.1383.com.pixel.gun3d.obb"

static jvalue ret_void(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_void();
}

static jvalue ret_false(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_bool(0);
}

static jvalue up_getSplashMode(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(2); /* settings.xml splash_mode */
}

static jvalue up_kill(jobj *thiz, const jvalue *args, jmeth *m) {
  LOG("[unity] UnityPlayer.kill()");
  java_prefs_flush();
  sceKernelExitProcess(0);
  return jv_void();
}

static jvalue up_showVideoPlayer(jobj *thiz, const jvalue *args, jmeth *m) {
  LOG("[unity] showVideoPlayer(%s) -> not supported", ARG_STR(0));
  return jv_bool(0);
}

static jvalue up_showSoftInput(jobj *thiz, const jvalue *args, jmeth *m) {
  input_show_keyboard(ARG_STR(0), args[1].i, args[2].z, args[3].z, args[4].z, args[5].z, ARG_STR(6));
  return jv_void();
}

static jvalue up_hideSoftInput(jobj *thiz, const jvalue *args, jmeth *m) {
  input_hide_keyboard();
  return jv_void();
}

static jvalue up_getView(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(java_unity_player));
}

static jobj *settings_bundle;

static jvalue up_getSettings(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(settings_bundle));
}

static jvalue bundle_getInt(jobj *thiz, const jvalue *args, jmeth *m) {
  return !strcmp(ARG_STR(0), "splash_mode") ? jv_int(2) : jv_int(0);
}

static jvalue bundle_getBoolean(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_bool(!strcmp(ARG_STR(0), "useObb"));
}

static jvalue bundle_getString(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(NULL);
}

static jvalue bundle_containsKey(jobj *thiz, const jvalue *args, jmeth *m) {
  const char *k = ARG_STR(0);
  return jv_bool(!strcmp(k, "useObb") || !strcmp(k, "splash_mode"));
}

void java_unity_init(void) {
  jvm_method(UP, "getSplashMode", "()I", up_getSplashMode);
  jvm_method(UP, "executeGLThreadJobs", "()V", ret_void);
  jvm_method(UP, "disableLogger", "()V", ret_void);
  jvm_method(UP, "kill", "()V", up_kill);
  jvm_method(UP, "startActivityIndicator", "()V", ret_void);
  jvm_method(UP, "stopActivityIndicator", "()V", ret_void);
  jvm_method(UP, "showVideoPlayer", "(Ljava/lang/String;IIIZII)Z", up_showVideoPlayer);
  jvm_method(UP, "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;)V", up_showSoftInput);
  jvm_method(UP, "hideSoftInput", "()V", up_hideSoftInput);
  jvm_method(UP, "setSoftInputStr", "(Ljava/lang/String;)V", ret_void);
  jvm_method(UP, "isFinishing", "()Z", ret_false);
  jvm_method(UP, "getView", "()Landroid/view/View;", up_getView);
  jvm_method(UP, "getSettings", "()Landroid/os/Bundle;", up_getSettings);
  jvm_method(UP, "loadLibrary", "(Ljava/lang/String;)Z", ret_false);
  settings_bundle = jvm_pin(jvm_new("android/os/Bundle"));
  jvm_method("android/os/Bundle", "getInt", "(Ljava/lang/String;)I", bundle_getInt);
  jvm_method("android/os/Bundle", "getBoolean", "(Ljava/lang/String;)Z", bundle_getBoolean);
  jvm_method("android/os/Bundle", "getString", "(Ljava/lang/String;)Ljava/lang/String;", bundle_getString);
  jvm_method("android/os/Bundle", "containsKey", "(Ljava/lang/String;)Z", bundle_containsKey);
}

/* ---------- lifecycle ---------- */

typedef void (*fn_obj)(JNIEnv *, jobject, jobject);
typedef void (*fn_int_obj)(JNIEnv *, jobject, jint, jobject);
typedef void (*fn_void)(JNIEnv *, jobject);
typedef jboolean (*fn_bool)(JNIEnv *, jobject);
typedef void (*fn_z)(JNIEnv *, jobject, jboolean);
typedef jboolean (*fn_event)(JNIEnv *, jobject, jobject);
typedef void (*fn_str)(JNIEnv *, jobject, jstring);

static fn_event native_inject;
static fn_str native_set_input;
static fn_void native_soft_input_closed;
static fn_z native_input_canceled;

/* Each call into the player's natives runs in its own JNI local frame, as
 * from Java: what Unity leaves behind is released when the call returns. */
#define NATIVE(call)    \
  ({                    \
    jvm_frame_push();   \
    __auto_type r_ = call; \
    jvm_frame_pop(NULL); \
    r_;                 \
  })
#define NATIVE_VOID(call) \
  do {                    \
    jvm_frame_push();     \
    call;                 \
    jvm_frame_pop(NULL);  \
  } while (0)

int unity_inject(void *event) {
  return NATIVE(native_inject(jvm_env, (jobject)java_unity_player, (jobject)event));
}

void unity_soft_input_done(const char *text, int canceled) {
  jobj *s = jvm_string(text ? text : "");
  NATIVE_VOID(native_set_input(jvm_env, (jobject)java_unity_player, (jstring)s));
  jvm_release(s);
  if (canceled)
    NATIVE_VOID(native_input_canceled(jvm_env, (jobject)java_unity_player, 1));
  NATIVE_VOID(native_soft_input_closed(jvm_env, (jobject)java_unity_player));
}

static void call_file(fn_str f, const char *path) {
  jobj *s = jvm_string(path);
  LOG("[unity] nativeFile(%s)", path);
  NATIVE_VOID(f(jvm_env, (jobject)java_unity_player, (jstring)s));
  jvm_release(s);
}

static volatile unsigned unity_frames;
static SceUID unity_tid;
void thread_sample(SceUID tid, const char *tag);
SceUID thread_by_name(const char *name);
void thread_scan_faults(void);
void mman_stats(size_t *used, size_t *peak, size_t *total);
void alloc_extend(void);

void alloc_dump_callers(void);
size_t alloc_extra_used(size_t *total);
size_t alloc_big_used(size_t *peak, size_t *total);

void unity_log_mem(void) {
  static unsigned high_water;
  struct mallinfo mi = mallinfo();
  size_t extra_total, extra_used = alloc_extra_used(&extra_total);
  size_t used, peak, total;
  mman_stats(&used, &peak, &total);
  SceKernelFreeMemorySizeInfo fm = { .size = sizeof(fm) };
  sceKernelGetFreeMemorySize(&fm);
  LOG("[mem] heap in use %u KB (arena %u KB, free %u KB) | gc mmap %u/%u KB peak %u | kernel free user %d KB",
      mi.uordblks / 1024, mi.arena / 1024, mi.fordblks / 1024, (unsigned)(used / 1024), (unsigned)(total / 1024),
      (unsigned)(peak / 1024), (int)fm.size_user / 1024); /* reads ~2.5 MB low: negative near the end */
  size_t big_peak, big_total, big_used = alloc_big_used(&big_peak, &big_total);
  LOG("[mem] big heap %u/%u KB peak %u | overflow heap %u/%u KB", (unsigned)(big_used / 1024),
      (unsigned)(big_total / 1024), (unsigned)(big_peak / 1024), (unsigned)(extra_used / 1024),
      (unsigned)(extra_total / 1024));
  LOG("[mem] vitaGL free: vram %u/%u KB, ram %u/%u KB, phycont %u/%u KB", vglMemFree(VGL_MEM_VRAM) / 1024,
      vglMemTotal(VGL_MEM_VRAM) / 1024, vglMemFree(VGL_MEM_RAM) / 1024, vglMemTotal(VGL_MEM_RAM) / 1024,
      vglMemFree(VGL_MEM_PHYCONT) / 1024, vglMemTotal(VGL_MEM_PHYCONT) / 1024);
  int textures, buffers, framebuffers;
  size_t tex[VGL_MEM_ALL], buf;
  vglGetObjectStats(&textures, tex, &buffers, &buf, &framebuffers);
  LOG("[mem] vitaGL objects: %d textures (vram %u KB, phycont %u KB, heap %u KB), %d buffers (%u KB), %d framebuffers",
      textures, (unsigned)(tex[VGL_MEM_VRAM] / 1024), (unsigned)(tex[VGL_MEM_PHYCONT] / 1024),
      (unsigned)((tex[VGL_MEM_RAM] + tex[VGL_MEM_EXTERNAL]) / 1024), buffers, (unsigned)(buf / 1024), framebuffers);
  uint32_t slice, pool = vglGetCircularPoolPeak(&slice);
  LOG("[mem] vitaGL circular pool: peak %u of %u KB per frame", pool / 1024, slice / 1024);
  /* who holds the heap, each time it grows by another 16 MB */
  unsigned in_use = (mi.uordblks + extra_used + big_used) >> 24;
  if (in_use > high_water) {
    high_water = in_use;
    alloc_dump_callers();
  }
}

/* Loads have held a frame for up to 20 s and still showed hundreds of frames
 * a minute; past these, the game is hung and a core dump (every thread's
 * state) is worth more than waiting */
#define HUNG_S 60       /* no frame at all */
#define HUNG_BOOT_S 180 /* the same before the first frame */
#define CRAWL_S 90      /* window for... */
#define CRAWL_FRAMES 10 /* ...fewer frames than this: out of memory, every
                           allocation failing slowly, showed 0 fps for minutes */

/* Its own thread, taking no lock but the log's (which times out), so whatever
 * the game is stuck on cannot stop it. Suspended apps run no threads, so time
 * in the background is not counted. */
static int hang_watch(SceSize args, void *argp) {
  unsigned seen[CRAWL_S / 2]; /* unity_frames, one per 2 s tick */
  unsigned last = ~0u, n = 0, first = 0; /* tick of the first frame */
  int stalled = 0; /* seconds without a finished frame */
  for (;;) {
    sceKernelDelayThread(2 * 1000 * 1000);
    if (!unity_tid)
      continue;
    unsigned f = unity_frames;
    unsigned before = seen[n % (CRAWL_S / 2)]; /* CRAWL_S ago, once n has wrapped */
    seen[n++ % (CRAWL_S / 2)] = f;
    if (f && !first)
      first = n;
    if (f != last) {
      last = f;
      stalled = 0;
    } else {
      stalled += 2;
    }
    if (stalled >= (f ? HUNG_S : HUNG_BOOT_S))
      fatal("[wd] UnityMain has not finished a frame in %d s (frame %u): crashing for a core dump", stalled, f);
    if (first && n - first > CRAWL_S / 2 && f - before < CRAWL_FRAMES)
      fatal("[wd] %u frames in %d s (frame %u): crashing for a core dump", f - before, CRAWL_S, f);
    if (stalled && stalled % 20 == 0) {
      thread_sample(unity_tid, "UnityMain stalled");
      SceUID gw = thread_by_name("UnityGfxDeviceWorker");
      if (gw >= 0)
        thread_sample(gw, "UnityGfxDeviceWorker");
    }
  }
  return 0;
}

/* Faults, stack use, the overflow heap and memory logs */
static int watchdog(SceSize args, void *argp) {
  for (int tick = 1;; tick++) {
    sceKernelDelayThread(2 * 1000 * 1000);
    thread_scan_faults();
    bthread_check_stacks();
    if (unity_frames >= 300)
      alloc_extend(); /* Unity's worker threads exist by now */
    if (tick % 5 == 0)
      unity_log_mem();
  }
  return 0;
}

static void *unity_main(void *arg) {
  JNIEnv *env = jvm_env;
  unity_tid = sceKernelGetThreadId();
  jobject up = (jobject)java_unity_player;

  fn_obj initJni = jvm_native(UP, "initJni", "(Landroid/content/Context;)V");
  fn_str nativeFile = jvm_native(UP, "nativeFile", "(Ljava/lang/String;)V");
  fn_obj initWWW = jvm_native(UP, "nativeInitWWW", "(Ljava/lang/Class;)V");
  fn_obj initWebRequest = jvm_native(UP, "nativeInitWebRequest", "(Ljava/lang/Class;)V");
  fn_int_obj recreateGfx = jvm_native(UP, "nativeRecreateGfxState", "(ILandroid/view/Surface;)V");
  fn_void resume = jvm_native(UP, "nativeResume", "()V");
  fn_z focus = jvm_native(UP, "nativeFocusChanged", "(Z)V");
  fn_bool render = jvm_native(UP, "nativeRender", "()Z");
  native_inject = jvm_native(UP, "nativeInjectEvent", "(Landroid/view/InputEvent;)Z");
  native_set_input = jvm_native(UP, "nativeSetInputString", "(Ljava/lang/String;)V");
  native_soft_input_closed = jvm_native(UP, "nativeSoftInputClosed", "()V");
  native_input_canceled = jvm_native(UP, "nativeSetInputCanceled", "(Z)V");

  LOG("[unity] initJni");
  NATIVE_VOID(initJni(env, up, (jobject)java_activity));
  call_file(nativeFile, APK_PATH);
  call_file(nativeFile, OBB_PATH);
  LOG("[unity] nativeInitWWW / nativeInitWebRequest");
  NATIVE_VOID(initWWW(env, up, (jobject)jvm_class("com/unity3d/player/WWW")->obj));
  NATIVE_VOID(initWebRequest(env, up, (jobject)jvm_class("com/unity3d/player/UnityWebRequest")->obj));

  jobj *surface = jvm_pin(jvm_new("android/view/Surface"));
  LOG("[unity] nativeRecreateGfxState");
  NATIVE_VOID(recreateGfx(env, up, 0, (jobject)surface));
  LOG("[unity] nativeResume");
  NATIVE_VOID(resume(env, up));
  NATIVE_VOID(focus(env, up, 1));

  LOG("[unity] entering render loop");
  for (unsigned frame = 0;; frame++) {
    java_ui_drain();
    input_poll();
    if (!NATIVE(render(env, up))) {
      LOG("[unity] nativeRender returned false at frame %u", frame);
      break;
    }
    unity_frames = frame + 1;
    java_prefs_tick();
    if (frame < 3 || frame % 600 == 0)
      LOG("[unity] frame %u", frame);
  }
  java_prefs_flush();
  sceKernelExitProcess(0);
  return NULL;
}

void unity_start(void) {
  bthread *t;
  SceUID wd = sceKernelCreateThread("watchdog", watchdog, 0x40, 0x4000, 0, 0, NULL);
  if (wd >= 0)
    sceKernelStartThread(wd, 0, NULL);
  SceUID hw = sceKernelCreateThread("hang_watch", hang_watch, 0x40, 0x4000, 0, 0, NULL);
  if (hw >= 0)
    sceKernelStartThread(hw, 0, NULL);
  /* its deepest in a long session was 68 KB */
  if (bthread_create(&t, "UnityMain", unity_main, NULL, 1024 * 1024, 0, 0x10000100) != 0)
    fatal("[unity] cannot create UnityMain");
  bthread_join(t, NULL);
}

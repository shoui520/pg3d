/*
 * The Android framework as seen by the game: one activity (the Context),
 * java.io.File, package info, Build/Environment/Locale constants, a few
 * system services, and Uri percent-encoding (PlayerPrefs keys use it).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bionic/bionic.h"
#include "bionic/net.h"
#include "jni/java.h"
#include "jni/jvm.h"
#include "util/log.h"

#define PKG "com.pixel.gun3d"
#define APK_PATH "/data/app/com.pixel.gun3d-1/base.apk"
#define DATA_DIR "/data/data/com.pixel.gun3d"
#define LIB_DIR "/data/app/com.pixel.gun3d-1/lib/arm"
#define EXT_DIR "/storage/emulated/0"
#define VERSION_NAME "12.5.0"
#define VERSION_CODE 1383

#define ACTIVITY_CLASS "com/prime31/UnityPlayerNativeActivity"

jobj *java_activity;
jobj *java_unity_player;

/* ---------- java.io.File ---------- */

jobj *java_file(const char *path) {
  jobj *f = jvm_new("java/io/File");
  jvm_set_field(f, "path", "Ljava/lang/String;", jv_obj(jvm_string(path)));
  jvm_release((jobj *)jvm_get_field(f, "path").l); /* field holds the only ref */
  return f;
}

static const char *file_path(jobj *f) {
  return jvm_str((jobj *)jvm_get_field(f, "path").l);
}

static jvalue file_init_s(jobj *thiz, const jvalue *args, jmeth *m) {
  jvm_set_field(thiz, "path", "Ljava/lang/String;", args[0]);
  return jv_void();
}

static jvalue file_init_fs(jobj *thiz, const jvalue *args, jmeth *m) {
  char p[512];
  snprintf(p, sizeof(p), "%s/%s", file_path(ARG_OBJ(0)), ARG_STR(1));
  jobj *s = jvm_string(p);
  jvm_set_field(thiz, "path", "Ljava/lang/String;", jv_obj(s));
  jvm_release(s);
  return jv_void();
}

static jvalue file_init_ss(jobj *thiz, const jvalue *args, jmeth *m) {
  char p[512];
  snprintf(p, sizeof(p), "%s/%s", ARG_STR(0), ARG_STR(1));
  jobj *s = jvm_string(p);
  jvm_set_field(thiz, "path", "Ljava/lang/String;", jv_obj(s));
  jvm_release(s);
  return jv_void();
}

static jvalue file_getPath(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_string(file_path(thiz)));
}

static jvalue file_getName(jobj *thiz, const jvalue *args, jmeth *m) {
  const char *p = file_path(thiz), *s = strrchr(p, '/');
  return jv_obj(jvm_string(s ? s + 1 : p));
}

static int file_stat(jobj *f, struct stat *st) {
  char vp[512];
  bionic_map_path(file_path(f), vp, sizeof(vp));
  return stat(vp, st);
}

static jvalue file_exists(jobj *thiz, const jvalue *args, jmeth *m) {
  struct stat st;
  return jv_bool(file_stat(thiz, &st) == 0);
}

static jvalue file_isDirectory(jobj *thiz, const jvalue *args, jmeth *m) {
  struct stat st;
  return jv_bool(file_stat(thiz, &st) == 0 && S_ISDIR(st.st_mode));
}

static jvalue file_isFile(jobj *thiz, const jvalue *args, jmeth *m) {
  struct stat st;
  return jv_bool(file_stat(thiz, &st) == 0 && S_ISREG(st.st_mode));
}

static jvalue file_length(jobj *thiz, const jvalue *args, jmeth *m) {
  struct stat st;
  return jv_long(file_stat(thiz, &st) == 0 ? st.st_size : 0);
}

static void mkdirs(const char *vita_path) {
  char p[512];
  snprintf(p, sizeof(p), "%s", vita_path);
  for (char *s = strchr(p, '/'); s; s = strchr(s + 1, '/')) {
    *s = 0;
    mkdir(p, 0777);
    *s = '/';
  }
  mkdir(p, 0777);
}

static jvalue file_mkdirs(jobj *thiz, const jvalue *args, jmeth *m) {
  char vp[512];
  bionic_map_path(file_path(thiz), vp, sizeof(vp));
  mkdirs(vp);
  return jv_bool(1);
}

static jvalue file_delete(jobj *thiz, const jvalue *args, jmeth *m) {
  char vp[512];
  bionic_map_path(file_path(thiz), vp, sizeof(vp));
  return jv_bool(remove(vp) == 0);
}

/* ---------- Context / Activity ---------- */

static jvalue ctx_getPackageName(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_string(PKG));
}

static jvalue ctx_getPackageCodePath(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_string(APK_PATH));
}

static jvalue ctx_getFilesDir(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(java_file(DATA_DIR "/files"));
}

static jvalue ctx_getCacheDir(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(java_file(DATA_DIR "/cache"));
}

static jvalue ctx_getExternalFilesDir(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(java_file(EXT_DIR "/Android/data/" PKG "/files"));
}

static jvalue ctx_getExternalCacheDir(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(java_file(EXT_DIR "/Android/data/" PKG "/cache"));
}

static jvalue ctx_getObbDir(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(java_file(EXT_DIR "/Android/obb/" PKG));
}

static jobj *app_info;

static jvalue ctx_getApplicationInfo(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(app_info));
}

static jvalue ctx_getApplicationContext(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(java_activity));
}

static jobj *package_manager, *content_resolver, *resources, *configuration, *intent, *window;

static jvalue ctx_getPackageManager(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(package_manager));
}

static jvalue ctx_getContentResolver(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(content_resolver));
}

static jvalue ctx_getResources(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(resources));
}

static jvalue ctx_getIntent(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(intent));
}

static jvalue act_getWindow(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(window));
}

static jvalue ctx_checkPermission(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(0); /* PERMISSION_GRANTED */
}

static jvalue act_getRequestedOrientation(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(6); /* SCREEN_ORIENTATION_SENSOR_LANDSCAPE */
}

static jvalue ret_void(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_void();
}

static jvalue ret_false(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_bool(0);
}

static jvalue ret_true(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_bool(1);
}

static jvalue ret_null(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(NULL);
}

static jvalue ret_zero(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(0);
}

static jvalue cm_getActiveNetworkInfo(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(net_enabled() ? jvm_new("android/net/NetworkInfo") : NULL);
}

static jvalue ni_getType(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(1); /* TYPE_WIFI */
}

/* ---------- system services ---------- */

static jobj *svc_window, *svc_audio, *svc_connectivity, *svc_vibrator, *svc_display, *svc_input, *display_obj;

static jvalue ctx_getSystemService(jobj *thiz, const jvalue *args, jmeth *m) {
  const char *n = ARG_STR(0);
  jobj *r = NULL;
  if (!strcmp(n, "window"))
    r = svc_window;
  else if (!strcmp(n, "audio"))
    r = svc_audio;
  else if (!strcmp(n, "connectivity"))
    r = svc_connectivity;
  else if (!strcmp(n, "vibrator"))
    r = svc_vibrator;
  else if (!strcmp(n, "display"))
    r = svc_display;
  else if (!strcmp(n, "input"))
    r = svc_input;
  else
    LOG("[java] getSystemService(%s) -> null", n);
  return jv_obj(jvm_retain(r));
}

static jvalue wm_getDefaultDisplay(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(display_obj));
}

static jvalue display_getMetrics(jobj *thiz, const jvalue *args, jmeth *m) {
  jobj *dm = ARG_OBJ(0);
  if (dm) {
    jvm_set_field(dm, "widthPixels", "I", jv_int(960));
    jvm_set_field(dm, "heightPixels", "I", jv_int(544));
    jvm_set_field(dm, "densityDpi", "I", jv_int(220));
    jvm_set_field(dm, "density", "F", jv_float(1.375f));
    jvm_set_field(dm, "scaledDensity", "F", jv_float(1.375f));
    jvm_set_field(dm, "xdpi", "F", jv_float(220.0f));
    jvm_set_field(dm, "ydpi", "F", jv_float(220.0f));
  }
  return jv_void();
}

static jvalue display_getRotation(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(0);
}

static jvalue display_getWidth(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(960);
}

static jvalue display_getHeight(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(544);
}

static jvalue display_getRefreshRate(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_float(60.0f);
}

static jvalue audio_getProperty(jobj *thiz, const jvalue *args, jmeth *m) {
  const char *k = ARG_STR(0);
  if (strstr(k, "FRAMES_PER_BUFFER"))
    return jv_obj(jvm_string("256"));
  if (strstr(k, "SAMPLE_RATE"))
    return jv_obj(jvm_string("48000"));
  return jv_obj(NULL);
}

static jvalue audio_requestFocus(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(1); /* AUDIOFOCUS_REQUEST_GRANTED */
}

static jvalue audio_getStreamVolume(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(15);
}

/* ---------- package manager ---------- */

static jobj *package_info;

static jvalue pm_getPackageInfo(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(package_info));
}

static uint8_t *apk_cert;
static size_t apk_cert_len;

static jvalue sig_toByteArray(jobj *thiz, const jvalue *args, jmeth *m) {
  jobj *a = jvm_array('B', apk_cert_len);
  memcpy(a->arr.data, apk_cert, apk_cert_len);
  return jv_obj(a);
}

static jvalue sig_toCharsString(jobj *thiz, const jvalue *args, jmeth *m) {
  char *hex = malloc(apk_cert_len * 2 + 1);
  for (size_t i = 0; i < apk_cert_len; i++)
    sprintf(hex + i * 2, "%02x", apk_cert[i]);
  hex[apk_cert_len * 2] = 0;
  jobj *s = jvm_string(hex);
  free(hex);
  return jv_obj(s);
}

/* PackageInfo.signatures: the game checks its hash and shuts itself down on a mismatch */
static void package_signatures(void) {
  char path[256];
  apk_cert = java_apk_certificate(bionic_map_path(APK_PATH, path, sizeof(path)), &apk_cert_len);
  if (!apk_cert)
    return;
  jvm_method("android/content/pm/Signature", "toByteArray", "()[B", sig_toByteArray);
  jvm_method("android/content/pm/Signature", "toCharsString", "()Ljava/lang/String;", sig_toCharsString);
  jobj *sigs = jvm_array('L', 1);
  ((jobj **)sigs->arr.data)[0] = jvm_new("android/content/pm/Signature");
  jvm_set_field(package_info, "signatures", "[Landroid/content/pm/Signature;", jv_obj(jvm_pin(sigs)));
}

static jvalue pm_getApplicationInfo(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(app_info));
}

static jvalue pm_getInstallerPackageName(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_string("com.android.vending"));
}

/* ---------- misc framework statics ---------- */

static jvalue env_getExternalStorageState(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_string("mounted"));
}

static jvalue env_getExternalStorageDirectory(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(java_file(EXT_DIR));
}

static jvalue settings_getString(jobj *thiz, const jvalue *args, jmeth *m) {
  const char *k = ARG_STR(1);
  if (!strcmp(k, "android_id"))
    return jv_obj(jvm_string(java_device_id()));
  LOG("[java] Settings.getString(%s) -> null", k);
  return jv_obj(NULL);
}

static jvalue settings_getInt(jobj *thiz, const jvalue *args, jmeth *m) {
  return args[2]; /* default */
}

static jobj *locale;

static jvalue locale_getDefault(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(locale));
}

static jvalue locale_getLanguage(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_string(java_language()));
}

static jvalue locale_getCountry(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_string("US"));
}

static jvalue locale_toString(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_stringf("%s_US", java_language()));
}

static jvalue system_currentTimeMillis(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_long(bionic_realtime_us() / 1000);
}

static jvalue system_nanoTime(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_long(bionic_monotonic_us() * 1000);
}

static jvalue system_getProperty(jobj *thiz, const jvalue *args, jmeth *m) {
  const char *k = ARG_STR(0);
  if (!strcmp(k, "http.agent"))
    return jv_obj(jvm_string("Dalvik/1.6.0 (Linux; U; Android 4.4.4; PS Vita)"));
  if (!strcmp(k, "line.separator"))
    return jv_obj(jvm_string("\n"));
  if (!strcmp(k, "file.separator"))
    return jv_obj(jvm_string("/"));
  LOG("[java] System.getProperty(%s) -> null", k);
  return jv_obj(NULL);
}

static jvalue thread_currentThread(jobj *thiz, const jvalue *args, jmeth *m) {
  static jobj *t;
  if (!t)
    t = jvm_pin(jvm_new("java/lang/Thread"));
  return jv_obj(t);
}

/* ---------- android.net.Uri percent coding ---------- */

static int uri_unreserved(unsigned char c, const char *allow) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
    return 1;
  if (strchr("_-!.~'()*", c) && c)
    return 1;
  return allow && c && strchr(allow, c);
}

char *java_uri_encode(const char *s, const char *allow) {
  size_t n = strlen(s);
  char *out = malloc(n * 3 + 1), *o = out;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (uri_unreserved(*p, allow)) {
      *o++ = *p;
    } else {
      sprintf(o, "%%%02X", *p);
      o += 3;
    }
  }
  *o = 0;
  return out;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

char *java_uri_decode(const char *s) {
  char *out = malloc(strlen(s) + 1), *o = out;
  for (const char *p = s; *p; p++) {
    int h, l;
    if (*p == '%' && (h = hexval(p[1])) >= 0 && (l = hexval(p[2])) >= 0) {
      *o++ = (char)(h * 16 + l);
      p += 2;
    } else {
      *o++ = *p;
    }
  }
  *o = 0;
  return out;
}

static jvalue uri_encode(jobj *thiz, const jvalue *args, jmeth *m) {
  if (!ARG_OBJ(0))
    return jv_obj(NULL);
  char *e = java_uri_encode(ARG_STR(0), m->nargs > 1 && ARG_OBJ(1) ? ARG_STR(1) : NULL);
  jobj *r = jvm_string(e);
  free(e);
  return jv_obj(r);
}

static jvalue uri_decode(jobj *thiz, const jvalue *args, jmeth *m) {
  if (!ARG_OBJ(0))
    return jv_obj(NULL);
  char *d = java_uri_decode(ARG_STR(0));
  jobj *r = jvm_string(d);
  free(d);
  return jv_obj(r);
}

/* ---------- registration ---------- */

static void S_str(const char *cls, const char *name, const char *v) {
  jvm_static(cls, name, "Ljava/lang/String;", jv_obj(jvm_string(v)));
}

static void S_int(const char *cls, const char *name, int v) {
  jvm_static(cls, name, "I", jv_int(v));
}

static jobj *service(const char *cls) {
  return jvm_pin(jvm_new(cls));
}

void java_android_init(void) {
  /* class hierarchy the game's casts and IsInstanceOf checks rely on */
  jvm_declare("android/content/Context", NULL);
  jvm_declare("android/content/ContextWrapper", "android/content/Context");
  jvm_declare("android/view/ContextThemeWrapper", "android/content/ContextWrapper");
  jvm_declare("android/app/Activity", "android/view/ContextThemeWrapper");
  jvm_declare("android/app/NativeActivity", "android/app/Activity");
  jvm_declare("com/unity3d/player/UnityPlayerActivity", "android/app/Activity");
  jvm_declare("com/unity3d/player/UnityPlayerNativeActivity", "com/unity3d/player/UnityPlayerActivity");
  jvm_declare(ACTIVITY_CLASS, "com/unity3d/player/UnityPlayerNativeActivity");
  jvm_declare("android/view/View", NULL);
  jvm_declare("android/view/ViewGroup", "android/view/View");
  jvm_declare("android/widget/FrameLayout", "android/view/ViewGroup");
  jvm_declare("com/unity3d/player/UnityPlayer", "android/widget/FrameLayout");
  jvm_declare("java/io/File", NULL);

  const char *C = "android/content/Context";
  jvm_method(C, "getPackageName", "()Ljava/lang/String;", ctx_getPackageName);
  jvm_method(C, "getPackageCodePath", "()Ljava/lang/String;", ctx_getPackageCodePath);
  jvm_method(C, "getPackageResourcePath", "()Ljava/lang/String;", ctx_getPackageCodePath);
  jvm_method(C, "getFilesDir", "()Ljava/io/File;", ctx_getFilesDir);
  jvm_method(C, "getCacheDir", "()Ljava/io/File;", ctx_getCacheDir);
  jvm_method(C, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", ctx_getExternalFilesDir);
  jvm_method(C, "getExternalCacheDir", "()Ljava/io/File;", ctx_getExternalCacheDir);
  jvm_method(C, "getObbDir", "()Ljava/io/File;", ctx_getObbDir);
  jvm_method(C, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;", ctx_getApplicationInfo);
  jvm_method(C, "getApplicationContext", "()Landroid/content/Context;", ctx_getApplicationContext);
  jvm_method(C, "getPackageManager", "()Landroid/content/pm/PackageManager;", ctx_getPackageManager);
  jvm_method(C, "getContentResolver", "()Landroid/content/ContentResolver;", ctx_getContentResolver);
  jvm_method(C, "getResources", "()Landroid/content/res/Resources;", ctx_getResources);
  jvm_method(C, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", ctx_getSystemService);
  jvm_method(C, "checkCallingOrSelfPermission", "(Ljava/lang/String;)I", ctx_checkPermission);
  jvm_method(C, "startActivity", "(Landroid/content/Intent;)V", ret_void);
  jvm_method(C, "bindService", "(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z", ret_false);
  jvm_method(C, "unbindService", "(Landroid/content/ServiceConnection;)V", ret_void);
  const char *A = "android/app/Activity";
  jvm_method(A, "getIntent", "()Landroid/content/Intent;", ctx_getIntent);
  jvm_method(A, "getWindow", "()Landroid/view/Window;", act_getWindow);
  jvm_method(A, "getRequestedOrientation", "()I", act_getRequestedOrientation);
  jvm_method(A, "setRequestedOrientation", "(I)V", ret_void);
  jvm_method(A, "isFinishing", "()Z", ret_false);
  jvm_method(A, "finish", "()V", ret_void);
  jvm_method("android/content/Intent", "getExtras", "()Landroid/os/Bundle;", ret_null);
  jvm_method("android/content/Intent", "getData", "()Landroid/net/Uri;", ret_null);
  jvm_method("android/content/Intent", "getAction", "()Ljava/lang/String;", ret_null);
  jvm_method("android/content/Intent", "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;", ret_null);
  jvm_method("android/view/Window", "setFlags", "(II)V", ret_void);
  jvm_method("android/view/Window", "addFlags", "(I)V", ret_void);
  jvm_method("android/view/Window", "clearFlags", "(I)V", ret_void);

  jvm_method("java/io/File", "<init>", "(Ljava/lang/String;)V", file_init_s);
  jvm_method("java/io/File", "<init>", "(Ljava/io/File;Ljava/lang/String;)V", file_init_fs);
  jvm_method("java/io/File", "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", file_init_ss);
  jvm_method("java/io/File", "getPath", "()Ljava/lang/String;", file_getPath);
  jvm_method("java/io/File", "getAbsolutePath", "()Ljava/lang/String;", file_getPath);
  jvm_method("java/io/File", "getCanonicalPath", "()Ljava/lang/String;", file_getPath);
  jvm_method("java/io/File", "toString", "()Ljava/lang/String;", file_getPath);
  jvm_method("java/io/File", "getName", "()Ljava/lang/String;", file_getName);
  jvm_method("java/io/File", "exists", "()Z", file_exists);
  jvm_method("java/io/File", "isDirectory", "()Z", file_isDirectory);
  jvm_method("java/io/File", "isFile", "()Z", file_isFile);
  jvm_method("java/io/File", "length", "()J", file_length);
  jvm_method("java/io/File", "mkdirs", "()Z", file_mkdirs);
  jvm_method("java/io/File", "mkdir", "()Z", file_mkdirs);
  jvm_method("java/io/File", "delete", "()Z", file_delete);
  jvm_method("java/io/File", "canWrite", "()Z", ret_true);

  /* services */
  svc_window = service("android/view/WindowManager");
  svc_audio = service("android/media/AudioManager");
  svc_connectivity = service("android/net/ConnectivityManager");
  svc_vibrator = service("android/os/Vibrator");
  svc_display = service("android/hardware/display/DisplayManager");
  svc_input = service("android/hardware/input/InputManager");
  display_obj = service("android/view/Display");
  jvm_method("android/view/WindowManager", "getDefaultDisplay", "()Landroid/view/Display;", wm_getDefaultDisplay);
  jvm_method("android/view/Display", "getMetrics", "(Landroid/util/DisplayMetrics;)V", display_getMetrics);
  jvm_method("android/view/Display", "getRealMetrics", "(Landroid/util/DisplayMetrics;)V", display_getMetrics);
  jvm_method("android/view/Display", "getRotation", "()I", display_getRotation);
  jvm_method("android/view/Display", "getWidth", "()I", display_getWidth);
  jvm_method("android/view/Display", "getHeight", "()I", display_getHeight);
  jvm_method("android/view/Display", "getRefreshRate", "()F", display_getRefreshRate);
  jvm_method("android/view/Display", "getDisplayId", "()I", ret_zero);
  jvm_method("android/media/AudioManager", "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", audio_getProperty);
  jvm_method("android/media/AudioManager", "requestAudioFocus",
             "(Landroid/media/AudioManager$OnAudioFocusChangeListener;II)I", audio_requestFocus);
  jvm_method("android/media/AudioManager", "abandonAudioFocus",
             "(Landroid/media/AudioManager$OnAudioFocusChangeListener;)I", audio_requestFocus);
  jvm_method("android/media/AudioManager", "getStreamVolume", "(I)I", audio_getStreamVolume);
  jvm_method("android/media/AudioManager", "getStreamMaxVolume", "(I)I", audio_getStreamVolume);
  jvm_method("android/media/AudioManager", "isMusicActive", "()Z", ret_false);
  jvm_method("android/media/AudioManager", "isWiredHeadsetOn", "()Z", ret_false);
  /* Wi-Fi when a server is set up, else no network */
  jvm_method("android/net/ConnectivityManager", "getActiveNetworkInfo", "()Landroid/net/NetworkInfo;",
             cm_getActiveNetworkInfo);
  jvm_method("android/net/NetworkInfo", "isConnected", "()Z", ret_true);
  jvm_method("android/net/NetworkInfo", "isConnectedOrConnecting", "()Z", ret_true);
  jvm_method("android/net/NetworkInfo", "isAvailable", "()Z", ret_true);
  jvm_method("android/net/NetworkInfo", "getType", "()I", ni_getType);
  jvm_method("android/net/NetworkInfo", "getSubtype", "()I", ret_zero);
  jvm_method("android/os/Vibrator", "vibrate", "(J)V", ret_void);
  jvm_method("android/os/Vibrator", "cancel", "()V", ret_void);
  jvm_method("android/os/Vibrator", "hasVibrator", "()Z", ret_false);
  jvm_method("android/hardware/display/DisplayManager", "getDisplay", "(I)Landroid/view/Display;", wm_getDefaultDisplay);
  jvm_method("android/hardware/display/DisplayManager", "registerDisplayListener",
             "(Landroid/hardware/display/DisplayManager$DisplayListener;Landroid/os/Handler;)V", ret_void);
  jvm_method("android/hardware/display/DisplayManager", "unregisterDisplayListener",
             "(Landroid/hardware/display/DisplayManager$DisplayListener;)V", ret_void);
  jvm_method("android/hardware/input/InputManager", "registerInputDeviceListener",
             "(Landroid/hardware/input/InputManager$InputDeviceListener;Landroid/os/Handler;)V", ret_void);
  jvm_method("android/hardware/input/InputManager", "unregisterInputDeviceListener",
             "(Landroid/hardware/input/InputManager$InputDeviceListener;)V", ret_void);

  /* packages */
  package_manager = service("android/content/pm/PackageManager");
  content_resolver = service("android/content/ContentResolver");
  resources = service("android/content/res/Resources");
  configuration = service("android/content/res/Configuration");
  intent = service("android/content/Intent");
  window = service("android/view/Window");
  jvm_set_field(resources, "configuration", "Landroid/content/res/Configuration;", jv_obj(configuration));
  app_info = jvm_pin(jvm_new("android/content/pm/ApplicationInfo"));
  jvm_set_field(app_info, "dataDir", "Ljava/lang/String;", jv_obj(jvm_pin(jvm_string(DATA_DIR))));
  jvm_set_field(app_info, "nativeLibraryDir", "Ljava/lang/String;", jv_obj(jvm_pin(jvm_string(LIB_DIR))));
  jvm_set_field(app_info, "sourceDir", "Ljava/lang/String;", jv_obj(jvm_pin(jvm_string(APK_PATH))));
  jvm_set_field(app_info, "publicSourceDir", "Ljava/lang/String;", jv_obj(jvm_pin(jvm_string(APK_PATH))));
  jvm_set_field(app_info, "packageName", "Ljava/lang/String;", jv_obj(jvm_pin(jvm_string(PKG))));
  jvm_set_field(app_info, "flags", "I", jv_int(0));
  jvm_set_field(app_info, "metaData", "Landroid/os/Bundle;", jv_obj(NULL));
  package_info = jvm_pin(jvm_new("android/content/pm/PackageInfo"));
  jvm_set_field(package_info, "versionName", "Ljava/lang/String;", jv_obj(jvm_pin(jvm_string(VERSION_NAME))));
  jvm_set_field(package_info, "versionCode", "I", jv_int(VERSION_CODE));
  jvm_set_field(package_info, "packageName", "Ljava/lang/String;", jv_obj(jvm_pin(jvm_string(PKG))));
  jvm_set_field(package_info, "applicationInfo", "Landroid/content/pm/ApplicationInfo;", jv_obj(app_info));
  package_signatures();
  const char *PM = "android/content/pm/PackageManager";
  jvm_method(PM, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", pm_getPackageInfo);
  jvm_method(PM, "getApplicationInfo", "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;", pm_getApplicationInfo);
  jvm_method(PM, "getInstallerPackageName", "(Ljava/lang/String;)Ljava/lang/String;", pm_getInstallerPackageName);
  jvm_method(PM, "hasSystemFeature", "(Ljava/lang/String;)Z", ret_false);
  S_int(PM, "GET_META_DATA", 128);
  S_int(PM, "GET_SIGNATURES", 64);
  S_int(PM, "PERMISSION_GRANTED", 0);
  S_str(PM, "FEATURE_AUDIO_LOW_LATENCY", "android.hardware.audio.low_latency");

  /* constants */
  S_str(C, "AUDIO_SERVICE", "audio");
  S_str(C, "CONNECTIVITY_SERVICE", "connectivity");
  S_str(C, "DISPLAY_SERVICE", "display");
  S_str(C, "INPUT_SERVICE", "input");
  S_str(C, "LOCATION_SERVICE", "location");
  S_str(C, "MEDIA_ROUTER_SERVICE", "media_router");
  S_str(C, "VIBRATOR_SERVICE", "vibrator");
  S_str(C, "WINDOW_SERVICE", "window");
  S_int(C, "MODE_PRIVATE", 0);
  S_int(C, "BIND_AUTO_CREATE", 1);
  S_str("android/Manifest$permission", "VIBRATE", "android.permission.VIBRATE");
  S_str("android/os/Build", "MODEL", "PS Vita");
  S_str("android/os/Build", "MANUFACTURER", "Sony");
  S_str("android/os/Build", "BRAND", "Sony");
  S_str("android/os/Build", "DEVICE", "vita");
  S_str("android/os/Build", "PRODUCT", "vita");
  S_str("android/os/Build", "HARDWARE", "vita");
  S_str("android/os/Build", "BOARD", "vita");
  S_str("android/os/Build", "ID", "KTU84P");
  S_str("android/os/Build", "TAGS", "release-keys");
  S_str("android/os/Build", "FINGERPRINT", "Sony/vita/vita:4.4.4/KTU84P/1:user/release-keys");
  S_str("android/os/Build", "CPU_ABI", "armeabi-v7a");
  S_str("android/os/Build", "SERIAL", "unknown");
  S_int("android/os/Build$VERSION", "SDK_INT", 19);
  S_str("android/os/Build$VERSION", "RELEASE", "4.4.4");
  S_str("android/os/Build$VERSION", "INCREMENTAL", "1");
  S_str("android/os/Build$VERSION", "SDK", "19");
  S_str("android/os/Environment", "MEDIA_MOUNTED", "mounted");
  jvm_method("android/os/Environment", "getExternalStorageState", "()Ljava/lang/String;", env_getExternalStorageState);
  jvm_method("android/os/Environment", "getExternalStorageDirectory", "()Ljava/io/File;", env_getExternalStorageDirectory);
  S_str("android/content/Intent", "ACTION_VIEW", "android.intent.action.VIEW");
  S_int("android/content/DialogInterface", "BUTTON_POSITIVE", -1);
  S_int("android/content/DialogInterface", "BUTTON_NEGATIVE", -2);
  S_int("android/net/ConnectivityManager", "TYPE_MOBILE", 0);
  S_int("android/net/ConnectivityManager", "TYPE_WIFI", 1);
  S_int("android/media/AudioManager", "STREAM_MUSIC", 3);
  S_int("android/media/AudioManager", "AUDIOFOCUS_GAIN", 1);
  S_int("android/media/AudioManager", "AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK", 3);
  S_int("android/media/AudioManager", "AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK", -3);
  S_int("android/media/AudioManager", "AUDIOFOCUS_REQUEST_GRANTED", 1);
  S_str("android/media/AudioManager", "PROPERTY_OUTPUT_FRAMES_PER_BUFFER", "android.media.property.OUTPUT_FRAMES_PER_BUFFER");
  S_str("android/media/AudioManager", "PROPERTY_OUTPUT_SAMPLE_RATE", "android.media.property.OUTPUT_SAMPLE_RATE");
  S_str("android/provider/Settings$Secure", "ANDROID_ID", "android_id");
  const char *AI = "android/content/pm/ActivityInfo";
  S_int(AI, "SCREEN_ORIENTATION_UNSPECIFIED", -1);
  S_int(AI, "SCREEN_ORIENTATION_LANDSCAPE", 0);
  S_int(AI, "SCREEN_ORIENTATION_PORTRAIT", 1);
  S_int(AI, "SCREEN_ORIENTATION_SENSOR", 4);
  S_int(AI, "SCREEN_ORIENTATION_SENSOR_LANDSCAPE", 6);
  S_int(AI, "SCREEN_ORIENTATION_SENSOR_PORTRAIT", 7);
  S_int(AI, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE", 8);
  S_int(AI, "SCREEN_ORIENTATION_REVERSE_PORTRAIT", 9);
  S_int(AI, "SCREEN_ORIENTATION_FULL_SENSOR", 10);
  jvm_method("android/provider/Settings$Secure", "getString",
             "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", settings_getString);
  jvm_method("android/provider/Settings$System", "getInt",
             "(Landroid/content/ContentResolver;Ljava/lang/String;I)I", settings_getInt);

  locale = jvm_pin(jvm_new("java/util/Locale"));
  jvm_method("java/util/Locale", "getDefault", "()Ljava/util/Locale;", locale_getDefault);
  jvm_method("java/util/Locale", "getLanguage", "()Ljava/lang/String;", locale_getLanguage);
  jvm_method("java/util/Locale", "getCountry", "()Ljava/lang/String;", locale_getCountry);
  jvm_method("java/util/Locale", "toString", "()Ljava/lang/String;", locale_toString);
  jvm_set_field(configuration, "locale", "Ljava/util/Locale;", jv_obj(locale));
  jvm_method("java/lang/System", "currentTimeMillis", "()J", system_currentTimeMillis);
  jvm_method("java/lang/System", "load", "(Ljava/lang/String;)V", ret_void);
  jvm_method("java/lang/System", "nanoTime", "()J", system_nanoTime);
  jvm_method("java/lang/System", "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", system_getProperty);
  jvm_method("java/lang/Thread", "currentThread", "()Ljava/lang/Thread;", thread_currentThread);
  jvm_method("android/net/Uri", "encode", "(Ljava/lang/String;)Ljava/lang/String;", uri_encode);
  jvm_method("android/net/Uri", "encode", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", uri_encode);
  jvm_method("android/net/Uri", "decode", "(Ljava/lang/String;)Ljava/lang/String;", uri_decode);

  /* the activity and the UnityPlayer view */
  java_activity = jvm_pin(jvm_new(ACTIVITY_CLASS));
  java_unity_player = jvm_pin(jvm_new("com/unity3d/player/UnityPlayer"));
  jvm_static("com/unity3d/player/UnityPlayer", "currentActivity", "Landroid/app/Activity;", jv_obj(java_activity));
  jvm_set_field(java_activity, "mUnityPlayer", "Lcom/unity3d/player/UnityPlayer;", jv_obj(java_unity_player));

  java_runtime_init();
  java_prefs_init();
  java_unity_init();
  java_fmod_init();
  java_www_init();
  LOG("[java] framework ready");
}

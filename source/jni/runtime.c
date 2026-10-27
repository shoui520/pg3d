/*
 * Java runtime pieces libunity leans on: native interface proxies
 * (bitter.jnibridge), Looper/Handler with a UI-thread queue drained by the
 * UnityMain loop, ClassLoader.findLibrary, java.util maps/iterators,
 * reflected methods, and AlertDialog (logged; error dialogs are fatal).
 */
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>

#include "jni/java.h"
#include "util/log.h"

#define LIB_DIR "/data/app/com.pixel.gun3d-1/lib/arm"
#define PROXY_CLASS "bitter/jnibridge/JNIBridge$Proxy"

/* ---------- interface proxies ---------- */

typedef struct {
  int64_t ptr;
  jobj *ifaces; /* Class[] */
} proxy;

static jvalue bridge_newInterfaceProxy(jobj *thiz, const jvalue *args, jmeth *m) {
  jobj *o = jvm_new(PROXY_CLASS);
  proxy *p = calloc(1, sizeof(*p));
  p->ptr = args[0].j;
  p->ifaces = jvm_retain(ARG_OBJ(1));
  o->native = p;
  o->native_free = free;
  return jv_obj(o);
}

#define RH "com/unity3d/player/ReflectionHelper"
#define RH_PROXY RH "$Proxy"

int java_is_proxy(jobj *o) {
  return o && (o->cls == jvm_class(PROXY_CLASS) || o->cls == jvm_class(RH_PROXY));
}

/* ReflectionHelper.newProxyInstance(int delegate, Class iface): C# AndroidJavaProxy */
static jvalue rh_newProxyInstance(jobj *thiz, const jvalue *args, jmeth *m) {
  jobj *o = jvm_new(RH_PROXY);
  proxy *p = calloc(1, sizeof(*p));
  p->ptr = args[0].i;
  jobj *arr = jvm_array('L', 1);
  ((jobj **)arr->arr.data)[0] = jvm_retain(ARG_OBJ(1));
  p->ifaces = arr;
  o->native = p;
  o->native_free = free;
  return jv_obj(o);
}

static jobj *rh_proxy_call(jobj *o, const char *name, jobj *argv) {
  proxy *p = o->native;
  typedef jobject (*invoke_fn)(JNIEnv *, jclass, jint, jstring, jobjectArray);
  invoke_fn invoke = jvm_native(RH, "nativeProxyInvoke", "(ILjava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object;");
  jobj *n = jvm_string(name);
  jobj *a = argv ? jvm_retain(argv) : jvm_array('L', 0);
  jvm_frame_push();
  jobj *r = jvm_frame_pop((jobj *)invoke(jvm_env, (jclass)jvm_class(RH)->obj, (jint)p->ptr, (jstring)n, (jobjectArray)a));
  jvm_release(n);
  jvm_release(a);
  if ((*jvm_env)->ExceptionCheck(jvm_env)) {
    (*jvm_env)->ExceptionDescribe(jvm_env);
    (*jvm_env)->ExceptionClear(jvm_env);
    LOG("[proxy] %s threw; result dropped", name);
    jvm_release(r);
    return NULL;
  }
  return (jobj *)r;
}

static jcls *arg_class(const jvalue *args, int i) {
  jobj *c = ARG_OBJ(i);
  return c && c->kind == OBJ_CLASS ? c->klass : NULL;
}

static jvalue rh_getMethodID(jobj *thiz, const jvalue *args, jmeth *m) {
  jcls *c = arg_class(args, 0);
  if (!c)
    return jv_obj(NULL);
  jmeth *r = jvm_reflect_method(c, ARG_STR(1), ARG_STR(2), args[3].z);
  return jv_obj(jvm_method_object(r));
}

static jvalue rh_getConstructorID(jobj *thiz, const jvalue *args, jmeth *m) {
  jcls *c = arg_class(args, 0);
  if (!c)
    return jv_obj(NULL);
  jmeth *r = jvm_reflect_method(c, "<init>", ARG_STR(1), 0);
  return jv_obj(jvm_method_object(r));
}

static jvalue rh_getFieldID(jobj *thiz, const jvalue *args, jmeth *m) {
  jcls *c = arg_class(args, 0);
  if (!c)
    return jv_obj(NULL);
  jfld *f = jvm_reflect_field(c, ARG_STR(1), ARG_STR(2), args[3].z);
  return jv_obj(jvm_field_object(f));
}

static jvalue class_forName(jobj *thiz, const jvalue *args, jmeth *m) {
  char n[256];
  strncpy(n, ARG_STR(0), sizeof(n) - 1);
  n[sizeof(n) - 1] = 0;
  for (char *p = n; *p; p++)
    if (*p == '.')
      *p = '/';
  return jv_obj(jvm_retain(jvm_class(n)->obj));
}

/* Invokes method name/sig of interface iface_name (NULL: the first one) on a
 * native proxy with boxed args. A Java exception from the C# side is logged
 * and cleared; the result is then NULL. */
jobj *java_proxy_call(jobj *o, const char *iface_name, const char *name, const char *sig, jobj *argv) {
  if (o->cls == jvm_class(RH_PROXY))
    return rh_proxy_call(o, name, argv);
  proxy *p = o->native;
  typedef jobject (*invoke_fn)(JNIEnv *, jclass, jlong, jclass, jobject, jobjectArray);
  invoke_fn invoke = jvm_native("bitter/jnibridge/JNIBridge", "invoke",
                                "(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;");
  jobj *iface = NULL;
  int n = p->ifaces ? p->ifaces->arr.len : 0;
  for (int i = 0; i < n; i++) {
    jobj *c = ((jobj **)p->ifaces->arr.data)[i];
    if (!iface || (iface_name && c->kind == OBJ_CLASS && strcmp(c->klass->name, iface_name) == 0))
      iface = c;
  }
  if (!iface) {
    LOG("[proxy] %s%s on proxy without interfaces", name, sig);
    return NULL;
  }
  jmethodID mid = (*jvm_env)->GetMethodID(jvm_env, (jclass)iface, name, sig);
  jobj *method = jvm_method_object((jmeth *)mid);
  jvm_frame_push();
  jobj *r = jvm_frame_pop((jobj *)invoke(jvm_env, (jclass)jvm_class("bitter/jnibridge/JNIBridge")->obj, p->ptr,
                                          (jclass)iface, (jobject)method, (jobjectArray)argv));
  jvm_release(method);
  if ((*jvm_env)->ExceptionCheck(jvm_env)) {
    (*jvm_env)->ExceptionDescribe(jvm_env);
    (*jvm_env)->ExceptionClear(jvm_env);
    LOG("[proxy] %s%s threw; result dropped", name, sig);
    jvm_release(r);
    return NULL;
  }
  return r;
}

/* Runs a Runnable: native proxy or one of our modelled classes. */
void java_run(jobj *r) {
  if (!r)
    return;
  if (java_is_proxy(r)) {
    jvm_release(java_proxy_call(r, "java/lang/Runnable", "run", "()V", NULL));
    return;
  }
  jmeth *run = jvm_find_method(r->cls, "run", "()V");
  if (run && run->fn)
    run->fn(r, NULL, run);
  else
    LOG("[java] Runnable %s has no run()", r->cls->name);
}

/* ---------- Looper / Handler: the UI thread is the UnityMain loop ---------- */

typedef struct job {
  jobj *runnable;
  int64_t due_us;
  struct job *next;
} job;

static job *queue;
static SceKernelLwMutexWork qlock;
static jobj *main_looper;

int64_t bionic_monotonic_us(void);

static void enqueue(jobj *r, int64_t delay_ms) {
  job *j = calloc(1, sizeof(*j));
  j->runnable = jvm_retain(r);
  j->due_us = bionic_monotonic_us() + delay_ms * 1000;
  sceKernelLockLwMutex(&qlock, 1, NULL);
  job **pp = &queue;
  while (*pp)
    pp = &(*pp)->next;
  *pp = j;
  sceKernelUnlockLwMutex(&qlock, 1);
}

void java_ui_drain(void) {
  int64_t now = bionic_monotonic_us();
  for (;;) {
    job *found = NULL;
    sceKernelLockLwMutex(&qlock, 1, NULL);
    for (job **pp = &queue; *pp; pp = &(*pp)->next) {
      if ((*pp)->due_us <= now) {
        found = *pp;
        *pp = found->next;
        break;
      }
    }
    sceKernelUnlockLwMutex(&qlock, 1);
    if (!found)
      return;
    java_run(found->runnable);
    jvm_release(found->runnable);
    free(found);
  }
}

static jvalue looper_getMainLooper(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(main_looper));
}

static jvalue handler_post(jobj *thiz, const jvalue *args, jmeth *m) {
  enqueue(ARG_OBJ(0), 0);
  return jv_bool(1);
}

static jvalue handler_postDelayed(jobj *thiz, const jvalue *args, jmeth *m) {
  enqueue(ARG_OBJ(0), args[1].j);
  return jv_bool(1);
}

static jvalue act_runOnUiThread(jobj *thiz, const jvalue *args, jmeth *m) {
  enqueue(ARG_OBJ(0), 0);
  return jv_void();
}

static jvalue ret_void(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_void();
}

/* ---------- ClassLoader ---------- */

static jobj *class_loader;

static jvalue class_getClassLoader(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(class_loader));
}

static jvalue loader_findLibrary(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_stringf(LIB_DIR "/lib%s.so", ARG_STR(0)));
}

static jvalue loader_loadClass(jobj *thiz, const jvalue *args, jmeth *m) {
  jclass c = (*jvm_env)->FindClass(jvm_env, ARG_STR(0));
  return jv_obj(jvm_retain((jobj *)c));
}

/* ---------- reflection ---------- */

static jvalue method_getName(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_string(thiz->kind == OBJ_METHOD && thiz->meth ? thiz->meth->name : ""));
}

/* ---------- java.util maps, sets, iterators ---------- */

typedef struct {
  int n, cap;
  jobj **k, **v;
} map;

static void map_free(void *p) {
  map *mp = p;
  for (int i = 0; i < mp->n; i++) {
    jvm_release(mp->k[i]);
    jvm_release(mp->v[i]);
  }
  free(mp->k);
  free(mp->v);
  free(mp);
}

static map *map_of(jobj *o) {
  if (!o->native) {
    o->native = calloc(1, sizeof(map));
    o->native_free = map_free;
  }
  return o->native;
}

static int key_eq(jobj *a, jobj *b) {
  if (a == b)
    return 1;
  if (a && b && a->kind == OBJ_STRING && b->kind == OBJ_STRING)
    return strcmp(a->str.utf8, b->str.utf8) == 0;
  return 0;
}

void java_map_put(jobj *o, jobj *k, jobj *v) {
  map *mp = map_of(o);
  for (int i = 0; i < mp->n; i++) {
    if (key_eq(mp->k[i], k)) {
      jvm_release(mp->v[i]);
      mp->v[i] = jvm_retain(v);
      return;
    }
  }
  if (mp->n == mp->cap) {
    mp->cap = mp->cap ? mp->cap * 2 : 16;
    mp->k = realloc(mp->k, mp->cap * sizeof(jobj *));
    mp->v = realloc(mp->v, mp->cap * sizeof(jobj *));
  }
  mp->k[mp->n] = jvm_retain(k);
  mp->v[mp->n] = jvm_retain(v);
  mp->n++;
}

int java_map_size(jobj *o) {
  return o ? map_of(o)->n : 0;
}

jobj *java_map_key(jobj *o, int i) {
  return map_of(o)->k[i];
}

jobj *java_map_value(jobj *o, int i) {
  return map_of(o)->v[i];
}

static jvalue map_put(jobj *thiz, const jvalue *args, jmeth *m) {
  java_map_put(thiz, ARG_OBJ(0), ARG_OBJ(1));
  return jv_obj(NULL);
}

static jvalue map_get(jobj *thiz, const jvalue *args, jmeth *m) {
  map *mp = map_of(thiz);
  for (int i = 0; i < mp->n; i++)
    if (key_eq(mp->k[i], ARG_OBJ(0)))
      return jv_obj(jvm_retain(mp->v[i]));
  return jv_obj(NULL);
}

static jvalue map_size(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(map_of(thiz)->n);
}

/* An iterator over a snapshot of objects. */
typedef struct {
  int pos, n;
  jobj **items;
} iter;

static void iter_free(void *p) {
  iter *it = p;
  for (int i = 0; i < it->n; i++)
    jvm_release(it->items[i]);
  free(it->items);
  free(it);
}

static jobj *collection(const char *cls, int n) {
  jobj *o = jvm_new(cls);
  iter *it = calloc(1, sizeof(*it));
  it->n = n;
  it->items = calloc(n ? n : 1, sizeof(jobj *));
  o->native = it;
  o->native_free = iter_free;
  return o;
}

static jvalue map_entrySet(jobj *thiz, const jvalue *args, jmeth *m) {
  map *mp = map_of(thiz);
  jobj *set = collection("java/util/HashSet", mp->n);
  iter *it = set->native;
  for (int i = 0; i < mp->n; i++) {
    jobj *e = jvm_new("java/util/HashMap$Entry");
    jvm_set_field(e, "key", "Ljava/lang/Object;", jv_obj(mp->k[i]));
    jvm_set_field(e, "value", "Ljava/lang/Object;", jv_obj(mp->v[i]));
    it->items[i] = e;
  }
  return jv_obj(set);
}

static jvalue map_keySet(jobj *thiz, const jvalue *args, jmeth *m) {
  map *mp = map_of(thiz);
  jobj *set = collection("java/util/HashSet", mp->n);
  iter *it = set->native;
  for (int i = 0; i < mp->n; i++)
    it->items[i] = jvm_retain(mp->k[i]);
  return jv_obj(set);
}

static jvalue entry_getKey(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain((jobj *)jvm_get_field(thiz, "key").l));
}

static jvalue entry_getValue(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain((jobj *)jvm_get_field(thiz, "value").l));
}

static jvalue coll_iterator(jobj *thiz, const jvalue *args, jmeth *m) {
  iter *src = thiz->native;
  jobj *o = collection("java/util/Iterator$Impl", src ? src->n : 0);
  iter *it = o->native;
  for (int i = 0; src && i < src->n; i++)
    it->items[i] = jvm_retain(src->items[i]);
  return jv_obj(o);
}

static jvalue coll_size(jobj *thiz, const jvalue *args, jmeth *m) {
  iter *it = thiz->native;
  return jv_int(it ? it->n : 0);
}

static jvalue coll_get(jobj *thiz, const jvalue *args, jmeth *m) {
  iter *it = thiz->native;
  int i = args[0].i;
  return jv_obj(it && i >= 0 && i < it->n ? jvm_retain(it->items[i]) : NULL);
}

static jvalue iter_hasNext(jobj *thiz, const jvalue *args, jmeth *m) {
  iter *it = thiz->native;
  return jv_bool(it->pos < it->n);
}

static jvalue iter_next(jobj *thiz, const jvalue *args, jmeth *m) {
  iter *it = thiz->native;
  if (it->pos >= it->n) {
    jvm_throw("java/util/NoSuchElementException", "");
    return jv_obj(NULL);
  }
  return jv_obj(jvm_retain(it->items[it->pos++]));
}

/* ---------- AlertDialog ---------- */

static jvalue builder_self(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(thiz));
}

static jvalue builder_setTitle(jobj *thiz, const jvalue *args, jmeth *m) {
  jvm_set_field(thiz, "title", "Ljava/lang/String;", args[0]);
  return builder_self(thiz, args, m);
}

static jvalue builder_setMessage(jobj *thiz, const jvalue *args, jmeth *m) {
  jvm_set_field(thiz, "message", "Ljava/lang/String;", args[0]);
  return builder_self(thiz, args, m);
}

static jvalue builder_show(jobj *thiz, const jvalue *args, jmeth *m) {
  const char *t = jvm_str((jobj *)jvm_get_field(thiz, "title").l);
  const char *msg = jvm_str((jobj *)jvm_get_field(thiz, "message").l);
  LOG("[dialog] %s: %s", t, msg);
  if (!strcmp(t, "Error") || strstr(msg, "Failed"))
    fatal("[dialog] Unity reported a fatal error: %s", msg);
  return jv_obj(jvm_new("android/app/AlertDialog"));
}

void java_runtime_init(void) {
  jvm_method(RH, "getMethodID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;",
             rh_getMethodID);
  jvm_method(RH, "getConstructorID", "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Constructor;",
             rh_getConstructorID);
  jvm_method(RH, "getFieldID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;",
             rh_getFieldID);
  jvm_method(RH, "newProxyInstance", "(ILjava/lang/Class;)Ljava/lang/Object;", rh_newProxyInstance);
  jvm_method("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;", class_forName);
  sceKernelCreateLwMutex(&qlock, "ui_queue", 0, 0, NULL);
  jvm_declare(PROXY_CLASS, NULL);
  jvm_method("bitter/jnibridge/JNIBridge", "newInterfaceProxy", "(J[Ljava/lang/Class;)Ljava/lang/Object;",
             bridge_newInterfaceProxy);
  jvm_method("java/lang/Object", "<init>", "()V", ret_void);

  main_looper = jvm_pin(jvm_new("android/os/Looper"));
  jvm_method("android/os/Looper", "getMainLooper", "()Landroid/os/Looper;", looper_getMainLooper);
  jvm_method("android/os/Looper", "myLooper", "()Landroid/os/Looper;", looper_getMainLooper);
  jvm_method("android/os/Handler", "<init>", "(Landroid/os/Looper;)V", ret_void);
  jvm_method("android/os/Handler", "<init>", "()V", ret_void);
  jvm_method("android/os/Handler", "post", "(Ljava/lang/Runnable;)Z", handler_post);
  jvm_method("android/os/Handler", "postDelayed", "(Ljava/lang/Runnable;J)Z", handler_postDelayed);
  jvm_method("android/app/Activity", "runOnUiThread", "(Ljava/lang/Runnable;)V", act_runOnUiThread);

  class_loader = jvm_pin(jvm_new("dalvik/system/PathClassLoader"));
  jvm_declare("java/lang/ClassLoader", NULL);
  jvm_declare("dalvik/system/PathClassLoader", "java/lang/ClassLoader");
  jvm_method("java/lang/Class", "getClassLoader", "()Ljava/lang/ClassLoader;", class_getClassLoader);
  jvm_method("android/content/Context", "getClassLoader", "()Ljava/lang/ClassLoader;", class_getClassLoader);
  jvm_method("java/lang/ClassLoader", "findLibrary", "(Ljava/lang/String;)Ljava/lang/String;", loader_findLibrary);
  jvm_method("java/lang/ClassLoader", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", loader_loadClass);
  jvm_method("java/lang/reflect/Method", "getName", "()Ljava/lang/String;", method_getName);

  jvm_declare("java/util/Map", NULL);
  jvm_declare("java/util/AbstractMap", "java/util/Map");
  jvm_declare("java/util/HashMap", "java/util/AbstractMap");
  jvm_declare("java/util/Collection", NULL);
  jvm_declare("java/util/Set", "java/util/Collection");
  jvm_declare("java/util/List", "java/util/Collection");
  jvm_declare("java/util/AbstractCollection", "java/util/Collection");
  jvm_declare("java/util/HashSet", "java/util/Set");
  jvm_declare("java/util/ArrayList", "java/util/List");
  jvm_declare("java/util/Iterator", NULL);
  jvm_declare("java/util/Iterator$Impl", "java/util/Iterator");
  jvm_declare("java/util/Map$Entry", NULL);
  jvm_declare("java/util/HashMap$Entry", "java/util/Map$Entry");
  jvm_method("java/util/HashMap", "<init>", "()V", ret_void);
  jvm_method("java/util/HashMap", "<init>", "(I)V", ret_void);
  jvm_method("java/util/Map", "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", map_put);
  jvm_method("java/util/Map", "get", "(Ljava/lang/Object;)Ljava/lang/Object;", map_get);
  jvm_method("java/util/Map", "size", "()I", map_size);
  jvm_method("java/util/Map", "entrySet", "()Ljava/util/Set;", map_entrySet);
  jvm_method("java/util/Map", "keySet", "()Ljava/util/Set;", map_keySet);
  jvm_method("java/util/Map$Entry", "getKey", "()Ljava/lang/Object;", entry_getKey);
  jvm_method("java/util/Map$Entry", "getValue", "()Ljava/lang/Object;", entry_getValue);
  jvm_method("java/util/Collection", "iterator", "()Ljava/util/Iterator;", coll_iterator);
  jvm_method("java/util/Collection", "size", "()I", coll_size);
  jvm_method("java/util/List", "get", "(I)Ljava/lang/Object;", coll_get);
  jvm_method("java/util/Iterator", "hasNext", "()Z", iter_hasNext);
  jvm_method("java/util/Iterator", "next", "()Ljava/lang/Object;", iter_next);

  const char *B = "android/app/AlertDialog$Builder";
  jvm_method(B, "<init>", "(Landroid/content/Context;)V", ret_void);
  jvm_method(B, "setTitle", "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", builder_setTitle);
  jvm_method(B, "setMessage", "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", builder_setMessage);
  jvm_method(B, "setPositiveButton",
             "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;",
             builder_self);
  jvm_method(B, "setNegativeButton",
             "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;",
             builder_self);
  jvm_method(B, "setOnCancelListener",
             "(Landroid/content/DialogInterface$OnCancelListener;)Landroid/app/AlertDialog$Builder;", builder_self);
  jvm_method(B, "setView", "(Landroid/view/View;)Landroid/app/AlertDialog$Builder;", builder_self);
  jvm_method(B, "show", "()Landroid/app/AlertDialog;", builder_show);
}

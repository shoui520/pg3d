/*
 * JNI function table over the jvm.h object model.
 *
 * Unknown classes, methods and fields are created on demand so the game's
 * lookups succeed; calling a method without a handler logs it (first calls
 * only) and returns zero/null. That is the signal to model it.
 */
#include "jni/jvm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/bthread.h"
#include "util/log.h"

#define JOBJ_MAGIC 0x4a4f424a
#define MISSING_LOG_CALLS 3

static SceKernelLwMutexWork lock;
static jcls *classes;
static jcls *class_class;   /* java/lang/Class */
static jcls *string_class;  /* java/lang/String */

/* ---------- strings ---------- */

static int utf8_to_utf16(const char *s, int len, uint16_t *out) {
  int n = 0;
  const unsigned char *u = (const unsigned char *)s, *end = u + len;
  while (u < end) {
    unsigned c = *u++;
    if (c >= 0xc0) {
      int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
      c &= 0x3f >> extra;
      while (extra-- && u < end)
        c = (c << 6) | (*u++ & 0x3f);
    }
    if (c >= 0x10000) {
      c -= 0x10000;
      if (out) {
        out[n] = 0xd800 | (c >> 10);
        out[n + 1] = 0xdc00 | (c & 0x3ff);
      }
      n += 2;
    } else {
      if (out)
        out[n] = c;
      n++;
    }
  }
  return n;
}

static int utf16_to_utf8(const uint16_t *s, int len, char *out) {
  int n = 0;
  for (int i = 0; i < len; i++) {
    unsigned c = s[i];
    if (c >= 0xd800 && c < 0xdc00 && i + 1 < len && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
      c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
      i++;
    }
    char b[4];
    int k;
    if (c < 0x80) {
      b[0] = c;
      k = 1;
    } else if (c < 0x800) {
      b[0] = 0xc0 | (c >> 6);
      b[1] = 0x80 | (c & 0x3f);
      k = 2;
    } else if (c < 0x10000) {
      b[0] = 0xe0 | (c >> 12);
      b[1] = 0x80 | ((c >> 6) & 0x3f);
      b[2] = 0x80 | (c & 0x3f);
      k = 3;
    } else {
      b[0] = 0xf0 | (c >> 18);
      b[1] = 0x80 | ((c >> 12) & 0x3f);
      b[2] = 0x80 | ((c >> 6) & 0x3f);
      b[3] = 0x80 | (c & 0x3f);
      k = 4;
    }
    if (out)
      memcpy(out + n, b, k);
    n += k;
  }
  return n;
}

/* ---------- classes ---------- */

static jcls *find_class_locked(const char *name) {
  for (jcls *c = classes; c; c = c->next)
    if (strcmp(c->name, name) == 0)
      return c;
  return NULL;
}

static jobj *obj_alloc(jcls *c, int kind);

static jcls *class_create_locked(const char *name) {
  jcls *c = calloc(1, sizeof(*c));
  c->name = strdup(name);
  c->next = classes;
  classes = c;
  c->obj = obj_alloc(class_class, OBJ_CLASS);
  c->obj->klass = c;
  c->obj->pinned = 1;
  if (strcmp(name, "java/lang/Object") != 0)
    c->super = find_class_locked("java/lang/Object");
  return c;
}

jcls *jvm_class(const char *name) {
  sceKernelLockLwMutex(&lock, 1, NULL);
  jcls *c = find_class_locked(name);
  if (!c)
    c = class_create_locked(name);
  sceKernelUnlockLwMutex(&lock, 1);
  return c;
}

jcls *jvm_declare(const char *name, const char *super) {
  jcls *c = jvm_class(name);
  c->declared = 1;
  if (super)
    c->super = jvm_class(super);
  return c;
}

int jvm_is_a(jcls *c, jcls *ancestor) {
  for (; c; c = c->super)
    if (c == ancestor)
      return 1;
  return 0;
}

static void parse_sig(jmeth *m) {
  const char *p = m->sig;
  m->nargs = 0;
  if (*p == '(')
    p++;
  while (*p && *p != ')') {
    char t = *p;
    while (*p == '[')
      p++;
    if (*p == 'L')
      p = strchr(p, ';');
    if (!p)
      break;
    p++;
    if (m->nargs < (int)sizeof(m->argt))
      m->argt[m->nargs++] = t == '[' ? 'L' : t;
  }
  m->ret = (p && *p == ')') ? (p[1] == '[' ? 'L' : p[1]) : 'V';
}

static jmeth *method_find_local(jcls *c, const char *name, const char *sig) {
  for (jmeth *m = c->methods; m; m = m->next)
    if (strcmp(m->name, name) == 0 && strcmp(m->sig, sig) == 0)
      return m;
  return NULL;
}

jmeth *jvm_find_method(jcls *c, const char *name, const char *sig) {
  for (; c; c = c->super) {
    jmeth *m = method_find_local(c, name, sig);
    if (m && (m->fn || m->native))
      return m;
  }
  return NULL;
}

static jmeth *method_add(jcls *c, const char *name, const char *sig) {
  jmeth *m = method_find_local(c, name, sig);
  if (m)
    return m;
  m = calloc(1, sizeof(*m));
  m->cls = c;
  m->name = strdup(name);
  m->sig = strdup(sig);
  parse_sig(m);
  m->next = c->methods;
  c->methods = m;
  return m;
}

void jvm_method(const char *cls, const char *name, const char *sig, jvm_handler fn) {
  jcls *c = jvm_class(cls);
  sceKernelLockLwMutex(&lock, 1, NULL);
  method_add(c, name, sig)->fn = fn;
  sceKernelUnlockLwMutex(&lock, 1);
}

static jfld *field_find(jcls *c, const char *name) {
  for (; c; c = c->super)
    for (jfld *f = c->fields; f; f = f->next)
      if (strcmp(f->name, name) == 0)
        return f;
  return NULL;
}

static jfld *field_add(jcls *c, const char *name, const char *sig) {
  for (jfld *f = c->fields; f; f = f->next)
    if (strcmp(f->name, name) == 0)
      return f;
  jfld *f = calloc(1, sizeof(*f));
  f->cls = c;
  f->name = strdup(name);
  f->sig = strdup(sig ? sig : "?");
  f->next = c->fields;
  c->fields = f;
  return f;
}

void jvm_field(const char *cls, const char *name, const char *sig, jvm_getter get) {
  jcls *c = jvm_class(cls);
  sceKernelLockLwMutex(&lock, 1, NULL);
  field_add(c, name, sig)->get = get;
  sceKernelUnlockLwMutex(&lock, 1);
}

void jvm_static(const char *cls, const char *name, const char *sig, jvalue v) {
  jcls *c = jvm_class(cls);
  sceKernelLockLwMutex(&lock, 1, NULL);
  jfld *f = field_add(c, name, sig);
  f->static_val = v;
  f->has_static = 1;
  if (sig[0] == 'L' || sig[0] == '[')
    jvm_pin((jobj *)v.l);
  sceKernelUnlockLwMutex(&lock, 1);
}

void *jvm_native(const char *cls, const char *name, const char *sig) {
  jcls *c = jvm_class(cls);
  jmeth *m = method_find_local(c, name, sig);
  if (!m || !m->native)
    fatal("[jni] native %s.%s%s not registered", cls, name, sig);
  return m->native;
}

/* ---------- objects ---------- */

static jobj *obj_alloc(jcls *c, int kind) {
  jobj *o = calloc(1, sizeof(*o));
  o->magic = JOBJ_MAGIC;
  o->cls = c;
  o->refs = 1;
  o->kind = kind;
  return o;
}

static inline jobj *O(const void *p) {
  jobj *o = (jobj *)p;
  if (o && o->magic != JOBJ_MAGIC)
    fatal("[jni] bad object %p (magic 0x%08X) lr=%p", o, o->magic, __builtin_return_address(0));
  return o;
}

static inline jcls *C(const void *p) {
  jobj *o = O(p);
  if (!o)
    return NULL;
  if (o->kind != OBJ_CLASS)
    fatal("[jni] %p is a %s, not a class", o, o->cls ? o->cls->name : "?");
  return o->klass;
}

jobj *jvm_new(const char *cls) {
  return obj_alloc(jvm_class(cls), OBJ_PLAIN);
}

jobj *jvm_retain(jobj *o) {
  if (O(o))
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
  return o;
}

jobj *jvm_pin(jobj *o) {
  if (O(o))
    o->pinned = 1;
  return o;
}

static void obj_free(jobj *o) {
  switch (o->kind) {
  case OBJ_STRING:
    free(o->str.utf8);
    free(o->str.utf16);
    break;
  case OBJ_ARRAY:
    if (o->arr.etype == 'L')
      for (int i = 0; i < o->arr.len; i++)
        jvm_release(((jobj **)o->arr.data)[i]);
    free(o->arr.data);
    break;
  default:
    break;
  }
  for (jfieldval *v = o->fields, *n; v; v = n) {
    n = v->next;
    if (v->f->sig[0] == 'L' || v->f->sig[0] == '[')
      jvm_release((jobj *)v->v.l);
    free(v);
  }
  if (o->native_free)
    o->native_free(o->native);
  o->magic = 0xdeadbeef;
  free(o);
}

void jvm_release(jobj *o) {
  if (!O(o) || o->pinned)
    return;
  if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) == 0)
    obj_free(o);
}

static jobj *string_new(const char *utf8, int len) {
  jobj *o = obj_alloc(string_class, OBJ_STRING);
  o->str.utf8 = malloc(len + 1);
  memcpy(o->str.utf8, utf8, len);
  o->str.utf8[len] = 0;
  o->str.len = len;
  o->str.ulen = utf8_to_utf16(utf8, len, NULL);
  return o;
}

jobj *jvm_string(const char *utf8) {
  return utf8 ? string_new(utf8, strlen(utf8)) : NULL;
}

jobj *jvm_stringf(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return jvm_string(buf);
}

const char *jvm_str(jobj *s) {
  if (!O(s))
    return "";
  if (s->kind != OBJ_STRING) {
    LOG("[jni] jvm_str on a %s", s->cls->name);
    return "";
  }
  return s->str.utf8;
}

static const uint16_t *string_utf16(jobj *s) {
  if (!s->str.utf16) {
    uint16_t *u = malloc((s->str.ulen + 1) * 2);
    utf8_to_utf16(s->str.utf8, s->str.len, u);
    u[s->str.ulen] = 0;
    s->str.utf16 = u;
  }
  return s->str.utf16;
}

static int esize_of(char t) {
  switch (t) {
  case 'Z': case 'B': return 1;
  case 'C': case 'S': return 2;
  case 'J': case 'D': return 8;
  default: return 4;
  }
}

jobj *jvm_array(char etype, int len) {
  char name[8] = { '[', etype, 0 };
  jobj *o = obj_alloc(jvm_class(etype == 'L' ? "[Ljava/lang/Object;" : name), OBJ_ARRAY);
  o->arr.etype = etype;
  o->arr.esize = esize_of(etype);
  o->arr.len = len < 0 ? 0 : len;
  o->arr.data = calloc(o->arr.len ? o->arr.len : 1, o->arr.esize);
  return o;
}

jobj *jvm_box_int(int v) {
  jobj *o = jvm_new("java/lang/Integer");
  jvm_set_field(o, "value", "I", jv_int(v));
  return o;
}

jobj *jvm_box_bool(int v) {
  jobj *o = jvm_new("java/lang/Boolean");
  jvm_set_field(o, "value", "Z", jv_bool(v));
  return o;
}

jobj *jvm_box_float(float v) {
  jobj *o = jvm_new("java/lang/Float");
  jvm_set_field(o, "value", "F", jv_float(v));
  return o;
}

static jfieldval *fieldval(jobj *o, jfld *f, int create) {
  for (jfieldval *v = o->fields; v; v = v->next)
    if (v->f == f)
      return v;
  if (!create)
    return NULL;
  jfieldval *v = calloc(1, sizeof(*v));
  v->f = f;
  v->next = o->fields;
  o->fields = v;
  return v;
}

static void field_store(jobj *o, jfld *f, jvalue val) {
  jfieldval *v = fieldval(o, f, 1);
  if (f->sig[0] == 'L' || f->sig[0] == '[') {
    jvm_retain((jobj *)val.l);
    jvm_release((jobj *)v->v.l);
  }
  v->v = val;
}

jvalue jvm_get_field(jobj *o, const char *name) {
  jfld *f = field_find(o->cls, name);
  if (!f)
    return jv_void();
  if (f->get)
    return f->get(o, f);
  jfieldval *v = fieldval(o, f, 0);
  return v ? v->v : jv_void();
}

void jvm_set_field(jobj *o, const char *name, const char *sig, jvalue val) {
  sceKernelLockLwMutex(&lock, 1, NULL);
  jfld *f = field_find(o->cls, name);
  if (!f)
    f = field_add(o->cls, name, sig);
  sceKernelUnlockLwMutex(&lock, 1);
  field_store(o, f, val);
}

/* ---------- exceptions ---------- */

static jobj **pending(void) {
  return (jobj **)&bthread_self()->jni_exception;
}

static void set_pending(jobj *e) {
  jobj **p = pending();
  jvm_release(*p);
  *p = e;
}

void jvm_throw(const char *cls, const char *msg) {
  jobj *e = jvm_new(cls);
  e->native = strdup(msg ? msg : "");
  e->native_free = free;
  LOG("[jni] throwing %s: %s", cls, msg ? msg : "");
  set_pending(e);
}

/* ---------- invocation ---------- */

static void decode_va(jmeth *m, va_list ap, jvalue *out) {
  for (int i = 0; i < m->nargs; i++) {
    out[i].j = 0;
    switch (m->argt[i]) {
    case 'J': out[i].j = va_arg(ap, int64_t); break;
    case 'D': out[i].d = va_arg(ap, double); break;
    case 'F': out[i].f = (float)va_arg(ap, double); break;
    case 'L': out[i].l = va_arg(ap, jobject); break;
    case 'Z': out[i].z = (jboolean)va_arg(ap, int); break;
    case 'B': out[i].b = (jbyte)va_arg(ap, int); break;
    case 'C': out[i].c = (jchar)va_arg(ap, int); break;
    case 'S': out[i].s = (jshort)va_arg(ap, int); break;
    default: out[i].i = va_arg(ap, int); break;
    }
  }
}

static void describe_args(jmeth *m, const jvalue *a, char *buf, size_t n) {
  size_t o = 0;
  buf[0] = 0;
  for (int i = 0; i < m->nargs && o + 48 < n; i++) {
    jobj *ob;
    switch (m->argt[i]) {
    case 'L':
      ob = (jobj *)a[i].l;
      if (ob && ob->magic == JOBJ_MAGIC && ob->kind == OBJ_STRING)
        o += snprintf(buf + o, n - o, "%s\"%.40s\"", i ? ", " : "", ob->str.utf8);
      else
        o += snprintf(buf + o, n - o, "%s%s", i ? ", " : "", ob ? (ob->magic == JOBJ_MAGIC ? ob->cls->name : "?") : "null");
      break;
    case 'J': o += snprintf(buf + o, n - o, "%s%lld", i ? ", " : "", a[i].j); break;
    case 'F': o += snprintf(buf + o, n - o, "%s%g", i ? ", " : "", a[i].f); break;
    case 'D': o += snprintf(buf + o, n - o, "%s%g", i ? ", " : "", a[i].d); break;
    case 'Z': o += snprintf(buf + o, n - o, "%s%s", i ? ", " : "", a[i].z ? "true" : "false"); break;
    default: o += snprintf(buf + o, n - o, "%s%d", i ? ", " : "", a[i].i); break;
    }
  }
}

static jvalue invoke(jobj *thiz, jmeth *m, const jvalue *args) {
  if (!m) {
    LOG("[jni] call through null method id lr=%p", __builtin_return_address(0));
    return jv_void();
  }
  m->calls++;
  /* virtual dispatch: a handler on the object's own class wins */
  if (thiz && thiz->cls != m->cls) {
    jmeth *v = jvm_find_method(thiz->cls, m->name, m->sig);
    if (v && v->fn)
      return v->fn(thiz, args, v);
  }
  if (m->fn)
    return m->fn(thiz, args, m);
  jmeth *h = jvm_find_method(m->cls, m->name, m->sig);
  if (h && h->fn)
    return h->fn(thiz, args, h);
  if (m->calls <= MISSING_LOG_CALLS) {
    char a[256];
    describe_args(m, args, a, sizeof(a));
    LOG("[jni] MISSING %s.%s%s (%s) on %s", m->cls->name, m->name, m->sig, a,
        thiz ? thiz->cls->name : "static");
  }
  return jv_void();
}

static jvalue invoke_va(jobj *thiz, jmeth *m, va_list ap) {
  jvalue a[32];
  if (!m)
    return invoke(thiz, m, NULL);
  decode_va(m, ap, a);
  return invoke(thiz, m, a);
}

/* ---------- local frames ----------
 * A reference handed to native code while a frame is open belongs to the
 * frame and is released when it pops: Unity makes strings inside
 * PushLocalFrame/PopLocalFrame many times a frame and leaves them to the
 * pop. Outside any frame a local is the caller's to delete. One stack per
 * thread holds the references and, as odd values, the frame marks. */
typedef struct {
  int n, cap, top; /* top: the innermost mark's index, -1 for none */
  uintptr_t slot[];
} jframes;

static void frame_put(bthread *t, uintptr_t v) {
  jframes *f = t->jni_frames;
  if (f->n == f->cap) {
    f->cap *= 2;
    f = realloc(f, sizeof(*f) + f->cap * sizeof(f->slot[0]));
    t->jni_frames = f;
  }
  f->slot[f->n++] = v;
}

/* o is a new reference for native code: the open frame owns it */
static jobject local(jobj *o) {
  if (!o || o->pinned)
    return (jobject)o;
  bthread *t = bthread_self();
  jframes *f = t->jni_frames;
  if (f && f->top >= 0)
    frame_put(t, (uintptr_t)o);
  return (jobject)o;
}

void jvm_frame_push(void) {
  bthread *t = bthread_self();
  jframes *f = t->jni_frames;
  if (!f) {
    f = malloc(sizeof(*f) + 64 * sizeof(f->slot[0]));
    f->n = 0;
    f->cap = 64;
    f->top = -1;
    t->jni_frames = f;
  }
  uintptr_t mark = (uintptr_t)(f->top + 1) << 1 | 1;
  f->top = f->n;
  frame_put(t, mark);
}

jobj *jvm_frame_pop(jobj *keep) {
  jframes *f = bthread_self()->jni_frames;
  if (!f || f->top < 0)
    return keep; /* unbalanced pop */
  jvm_retain(keep);
  while (f->n > f->top + 1) {
    uintptr_t v = f->slot[--f->n];
    if (v)
      jvm_release((jobj *)v);
  }
  f->n = f->top;
  f->top = (int)(f->slot[f->top] >> 1) - 1;
  return keep;
}

/* ---------- JNIEnv implementation ---------- */

#define ENV JNIEnv *env

static jint GetVersion(ENV) { return 0x00010006; }

static jclass DefineClass(ENV, const char *name, jobject loader, const jbyte *buf, jsize len) {
  LOG("[jni] DefineClass(%s) unsupported", name);
  return NULL;
}

static jclass FindClass(ENV, const char *name) {
  /* accept dotted names too */
  char n[256];
  snprintf(n, sizeof(n), "%s", name);
  for (char *p = n; *p; p++)
    if (*p == '.')
      *p = '/';
  sceKernelLockLwMutex(&lock, 1, NULL);
  jcls *c = find_class_locked(n);
  int created = !c;
  if (!c)
    c = class_create_locked(n);
  sceKernelUnlockLwMutex(&lock, 1);
  if (created)
    DLOG("[jni] FindClass(%s) -> new undeclared class", n);
  return (jclass)c->obj;
}

static jmethodID FromReflectedMethod(ENV, jobject method) {
  jobj *o = O(method);
  return o && o->kind == OBJ_METHOD ? (jmethodID)o->meth : NULL;
}

static jfieldID FromReflectedField(ENV, jobject field) {
  jobj *o = O(field);
  return o && o->kind == OBJ_FIELD ? (jfieldID)o->fld : NULL;
}

jobj *jvm_method_object(jmeth *m) {
  jobj *o = obj_alloc(jvm_class("java/lang/reflect/Method"), OBJ_METHOD);
  o->meth = m;
  return o;
}

jobj *jvm_field_object(jfld *f) {
  jobj *o = obj_alloc(jvm_class("java/lang/reflect/Field"), OBJ_FIELD);
  o->fld = f;
  return o;
}

static jobject ToReflectedMethod(ENV, jclass cls, jmethodID mid, jboolean isStatic) {
  return local(jvm_method_object((jmeth *)mid));
}

static jclass GetSuperclass(ENV, jclass cls) {
  jcls *c = C(cls);
  return c && c->super ? (jclass)c->super->obj : NULL;
}

static jboolean IsAssignableFrom(ENV, jclass a, jclass b) {
  return jvm_is_a(C(a), C(b));
}

static jobject ToReflectedField(ENV, jclass cls, jfieldID fid, jboolean isStatic) {
  return local(jvm_field_object((jfld *)fid));
}

static jint Throw(ENV, jthrowable t) {
  set_pending(jvm_retain(O(t)));
  return 0;
}

static jint ThrowNew(ENV, jclass cls, const char *msg) {
  jvm_throw(C(cls)->name, msg);
  return 0;
}

static jthrowable ExceptionOccurred(ENV) {
  return (jthrowable)local(jvm_retain(*pending()));
}

static void ExceptionDescribe(ENV) {
  jobj *e = *pending();
  if (e)
    LOG("[jni] pending exception %s: %s", e->cls->name, e->native ? (char *)e->native : "");
}

static void ExceptionClear(ENV) {
  set_pending(NULL);
}

static void FatalError(ENV, const char *msg) {
  fatal("[jni] FatalError: %s", msg);
}

static jint PushLocalFrame(ENV, jint cap) {
  jvm_frame_push();
  return 0;
}
static jobject PopLocalFrame(ENV, jobject result) {
  return local(jvm_frame_pop(O(result)));
}
static jobject NewGlobalRef(ENV, jobject o) { return (jobject)jvm_retain(O(o)); }
static void DeleteGlobalRef(ENV, jobject o) { jvm_release(O(o)); }
static void DeleteLocalRef(ENV, jobject obj) {
  jobj *o = O(obj);
  jframes *f = o && !o->pinned ? bthread_self()->jni_frames : NULL;
  for (int i = f ? f->n - 1 : -1; i >= 0; i--) {
    if (f->slot[i] == (uintptr_t)o) {
      f->slot[i] = 0;
      while (f->n > f->top + 1 && !f->slot[f->n - 1])
        f->n--;
      break;
    }
  }
  jvm_release(o);
}
static jboolean IsSameObject(ENV, jobject a, jobject b) { return a == b; }
static jobject NewLocalRef(ENV, jobject o) { return local(jvm_retain(O(o))); }
static jint EnsureLocalCapacity(ENV, jint cap) { return 0; }

static jobject AllocObject(ENV, jclass cls) {
  return local(obj_alloc(C(cls), OBJ_PLAIN));
}

static jobject NewObjectA(ENV, jclass cls, jmethodID mid, const jvalue *args) {
  jobj *o = obj_alloc(C(cls), OBJ_PLAIN);
  invoke(o, (jmeth *)mid, args);
  return local(o);
}

static jobject NewObjectV(ENV, jclass cls, jmethodID mid, va_list ap) {
  jobj *o = obj_alloc(C(cls), OBJ_PLAIN);
  invoke_va(o, (jmeth *)mid, ap);
  return local(o);
}

static jobject NewObject(ENV, jclass cls, jmethodID mid, ...) {
  va_list ap;
  va_start(ap, mid);
  jobject o = NewObjectV(env, cls, mid, ap);
  va_end(ap);
  return o;
}

static jclass GetObjectClass(ENV, jobject obj) {
  jobj *o = O(obj);
  if (!o) {
    LOG("[jni] GetObjectClass(null) lr=%p", __builtin_return_address(0));
    return NULL;
  }
  return (jclass)o->cls->obj;
}

static jboolean IsInstanceOf(ENV, jobject obj, jclass cls) {
  jobj *o = O(obj);
  if (!o)
    return 1;
  jcls *c = C(cls);
  if (jvm_is_a(o->cls, c))
    return 1;
  if (!c->declared || !o->cls->declared)
    DLOG("[jni] IsInstanceOf(%s, %s) -> false (undeclared hierarchy)", o->cls->name, c->name);
  return 0;
}

static jmethodID get_method(jclass cls, const char *name, const char *sig, int is_static) {
  jcls *c = C(cls);
  if (!c)
    return NULL;
  jmeth *m = jvm_find_method(c, name, sig);
  if (m)
    return (jmethodID)m;
  sceKernelLockLwMutex(&lock, 1, NULL);
  m = method_find_local(c, name, sig);
  int created = !m;
  if (!m)
    m = method_add(c, name, sig);
  sceKernelUnlockLwMutex(&lock, 1);
  if (created)
    LOG("[jni] Get%sMethodID %s.%s%s -> stub", is_static ? "Static" : "", c->name, name, sig);
  return (jmethodID)m;
}

static jfieldID get_field(jclass cls, const char *name, const char *sig, int is_static);

/* Reflection-style lookup (Unity's ReflectionHelper): callers build sig from
 * C# argument types, so the return type is often just Object. Prefer an exact
 * match, then a modelled method with the same parameter list, then one with
 * the same name and arity; otherwise a stub with the given signature. */
jmeth *jvm_reflect_method(jcls *c, const char *name, const char *sig, int is_static) {
  jmeth *m = jvm_find_method(c, name, sig);
  if (m)
    return m;
  const char *close = strchr(sig, ')');
  size_t plen = close ? (size_t)(close - sig + 1) : strlen(sig);
  jmeth tmp;
  memset(&tmp, 0, sizeof(tmp));
  tmp.sig = (char *)sig;
  parse_sig(&tmp);
  jmeth *arity = NULL;
  sceKernelLockLwMutex(&lock, 1, NULL);
  for (jcls *k = c; k && !m; k = k->super)
    for (jmeth *x = k->methods; x; x = x->next) {
      if (!(x->fn || x->native) || strcmp(x->name, name) != 0)
        continue;
      if (strncmp(x->sig, sig, plen) == 0) {
        m = x;
        break;
      }
      if (!arity && x->nargs == tmp.nargs)
        arity = x;
    }
  sceKernelUnlockLwMutex(&lock, 1);
  if (!m)
    m = arity;
  if (m) {
    DLOG("[jni] reflect %s.%s%s -> %s", c->name, name, sig, m->sig);
    return m;
  }
  return (jmeth *)get_method((jclass)c->obj, name, sig, is_static);
}

jfld *jvm_reflect_field(jcls *c, const char *name, const char *sig, int is_static) {
  return (jfld *)get_field((jclass)c->obj, name, sig, is_static);
}

static jmethodID GetMethodID(ENV, jclass cls, const char *name, const char *sig) {
  return get_method(cls, name, sig, 0);
}

static jmethodID GetStaticMethodID(ENV, jclass cls, const char *name, const char *sig) {
  return get_method(cls, name, sig, 1);
}

/* Call<T>Method / CallNonvirtual<T>Method / CallStatic<T>Method families */
/* a call's result for native code: an object result is a new local */
static inline jvalue ret(jvalue v, char kind) {
  if (kind == 'l')
    local((jobj *)v.l);
  return v;
}

#define CALLS(T, R, FIELD)                                                                                   \
  static R Call##T##MethodA(ENV, jobject o, jmethodID m, const jvalue *a) {                                \
    return (R)ret(invoke(O(o), (jmeth *)m, a), #FIELD[0]).FIELD;                                                             \
  }                                                                                                          \
  static R Call##T##MethodV(ENV, jobject o, jmethodID m, va_list ap) {                                     \
    return (R)ret(invoke_va(O(o), (jmeth *)m, ap), #FIELD[0]).FIELD;                                                         \
  }                                                                                                          \
  static R Call##T##Method(ENV, jobject o, jmethodID m, ...) {                                             \
    va_list ap;                                                                                              \
    va_start(ap, m);                                                                                         \
    R r = (R)ret(invoke_va(O(o), (jmeth *)m, ap), #FIELD[0]).FIELD;                                                          \
    va_end(ap);                                                                                              \
    return r;                                                                                                \
  }                                                                                                          \
  static R CallNonvirtual##T##MethodA(ENV, jobject o, jclass c, jmethodID m, const jvalue *a) {            \
    return (R)ret(invoke(O(o), (jmeth *)m, a), #FIELD[0]).FIELD;                                                             \
  }                                                                                                          \
  static R CallNonvirtual##T##MethodV(ENV, jobject o, jclass c, jmethodID m, va_list ap) {                 \
    return (R)ret(invoke_va(O(o), (jmeth *)m, ap), #FIELD[0]).FIELD;                                                         \
  }                                                                                                          \
  static R CallNonvirtual##T##Method(ENV, jobject o, jclass c, jmethodID m, ...) {                         \
    va_list ap;                                                                                              \
    va_start(ap, m);                                                                                         \
    R r = (R)ret(invoke_va(O(o), (jmeth *)m, ap), #FIELD[0]).FIELD;                                                          \
    va_end(ap);                                                                                              \
    return r;                                                                                                \
  }                                                                                                          \
  static R CallStatic##T##MethodA(ENV, jclass c, jmethodID m, const jvalue *a) {                           \
    return (R)ret(invoke(NULL, (jmeth *)m, a), #FIELD[0]).FIELD;                                                             \
  }                                                                                                          \
  static R CallStatic##T##MethodV(ENV, jclass c, jmethodID m, va_list ap) {                                \
    return (R)ret(invoke_va(NULL, (jmeth *)m, ap), #FIELD[0]).FIELD;                                                         \
  }                                                                                                          \
  static R CallStatic##T##Method(ENV, jclass c, jmethodID m, ...) {                                        \
    va_list ap;                                                                                              \
    va_start(ap, m);                                                                                         \
    R r = (R)ret(invoke_va(NULL, (jmeth *)m, ap), #FIELD[0]).FIELD;                                                          \
    va_end(ap);                                                                                              \
    return r;                                                                                                \
  }

CALLS(Object, jobject, l)
CALLS(Boolean, jboolean, z)
CALLS(Byte, jbyte, b)
CALLS(Char, jchar, c)
CALLS(Short, jshort, s)
CALLS(Int, jint, i)
CALLS(Long, jlong, j)
CALLS(Float, jfloat, f)
CALLS(Double, jdouble, d)

static void CallVoidMethodA(ENV, jobject o, jmethodID m, const jvalue *a) { invoke(O(o), (jmeth *)m, a); }
static void CallVoidMethodV(ENV, jobject o, jmethodID m, va_list ap) { invoke_va(O(o), (jmeth *)m, ap); }
static void CallVoidMethod(ENV, jobject o, jmethodID m, ...) {
  va_list ap;
  va_start(ap, m);
  invoke_va(O(o), (jmeth *)m, ap);
  va_end(ap);
}
static void CallNonvirtualVoidMethodA(ENV, jobject o, jclass c, jmethodID m, const jvalue *a) { invoke(O(o), (jmeth *)m, a); }
static void CallNonvirtualVoidMethodV(ENV, jobject o, jclass c, jmethodID m, va_list ap) { invoke_va(O(o), (jmeth *)m, ap); }
static void CallNonvirtualVoidMethod(ENV, jobject o, jclass c, jmethodID m, ...) {
  va_list ap;
  va_start(ap, m);
  invoke_va(O(o), (jmeth *)m, ap);
  va_end(ap);
}
static void CallStaticVoidMethodA(ENV, jclass c, jmethodID m, const jvalue *a) { invoke(NULL, (jmeth *)m, a); }
static void CallStaticVoidMethodV(ENV, jclass c, jmethodID m, va_list ap) { invoke_va(NULL, (jmeth *)m, ap); }
static void CallStaticVoidMethod(ENV, jclass c, jmethodID m, ...) {
  va_list ap;
  va_start(ap, m);
  invoke_va(NULL, (jmeth *)m, ap);
  va_end(ap);
}

/* fields */
static jfieldID get_field(jclass cls, const char *name, const char *sig, int is_static) {
  jcls *c = C(cls);
  sceKernelLockLwMutex(&lock, 1, NULL);
  jfld *f = field_find(c, name);
  int created = !f;
  if (!f)
    f = field_add(c, name, sig);
  sceKernelUnlockLwMutex(&lock, 1);
  if (created && is_static)
    LOG("[jni] GetStaticFieldID %s.%s %s -> unmodelled", c->name, name, sig);
  else if (created)
    DLOG("[jni] GetFieldID %s.%s %s -> per-object storage", c->name, name, sig);
  return (jfieldID)f;
}

static jfieldID GetFieldID(ENV, jclass c, const char *n, const char *s) { return get_field(c, n, s, 0); }
static jfieldID GetStaticFieldID(ENV, jclass c, const char *n, const char *s) { return get_field(c, n, s, 1); }

static jvalue field_get(jobj *o, jfld *f) {
  if (f->get)
    return f->get(o, f);
  if (!o) {
    if (!f->has_static)
      LOG("[jni] read of unmodelled static %s.%s", f->cls->name, f->name);
    return f->static_val;
  }
  jfieldval *v = fieldval(o, f, 0);
  return v ? v->v : jv_void();
}

static void field_set(jobj *o, jfld *f, jvalue v) {
  if (!o) {
    f->static_val = v;
    f->has_static = 1;
    return;
  }
  field_store(o, f, v);
}

#define FIELDS(T, R, FIELD)                                                                                  \
  static R Get##T##Field(ENV, jobject o, jfieldID f) {                                                     \
    jvalue v = field_get(O(o), (jfld *)f);                                                                   \
    if (#FIELD[0] == 'l')                                                                                    \
      local(jvm_retain((jobj *)v.l));                                                                        \
    return (R)v.FIELD;                                                                                       \
  }                                                                                                          \
  static void Set##T##Field(ENV, jobject o, jfieldID f, R val) {                                           \
    jvalue v;                                                                                                \
    v.j = 0;                                                                                                 \
    v.FIELD = val;                                                                                           \
    field_set(O(o), (jfld *)f, v);                                                                           \
  }                                                                                                          \
  static R GetStatic##T##Field(ENV, jclass c, jfieldID f) {                                                \
    jvalue v = field_get(NULL, (jfld *)f);                                                                   \
    if (#FIELD[0] == 'l')                                                                                    \
      local(jvm_retain((jobj *)v.l));                                                                        \
    return (R)v.FIELD;                                                                                       \
  }                                                                                                          \
  static void SetStatic##T##Field(ENV, jclass c, jfieldID f, R val) {                                      \
    jvalue v;                                                                                                \
    v.j = 0;                                                                                                 \
    v.FIELD = val;                                                                                           \
    field_set(NULL, (jfld *)f, v);                                                                           \
  }

FIELDS(Object, jobject, l)
FIELDS(Boolean, jboolean, z)
FIELDS(Byte, jbyte, b)
FIELDS(Char, jchar, c)
FIELDS(Short, jshort, s)
FIELDS(Int, jint, i)
FIELDS(Long, jlong, j)
FIELDS(Float, jfloat, f)
FIELDS(Double, jdouble, d)

/* strings */
static jstring NewString(ENV, const jchar *u, jsize len) {
  int n = utf16_to_utf8(u, len, NULL);
  char *tmp = malloc(n + 1);
  utf16_to_utf8(u, len, tmp);
  jobj *s = string_new(tmp, n);
  free(tmp);
  return (jstring)local(s);
}

static jsize GetStringLength(ENV, jstring s) { return O(s) ? ((jobj *)s)->str.ulen : 0; }

static const jchar *GetStringChars(ENV, jstring s, jboolean *isCopy) {
  if (isCopy)
    *isCopy = 0;
  return O(s) ? string_utf16((jobj *)s) : NULL;
}

static void ReleaseStringChars(ENV, jstring s, const jchar *c) {}

static jstring NewStringUTF(ENV, const char *utf) { return (jstring)local(jvm_string(utf)); }
static jsize GetStringUTFLength(ENV, jstring s) { return O(s) ? ((jobj *)s)->str.len : 0; }

static const char *GetStringUTFChars(ENV, jstring s, jboolean *isCopy) {
  if (isCopy)
    *isCopy = 0;
  if (!O(s)) {
    LOG("[jni] GetStringUTFChars(null) lr=%p", __builtin_return_address(0));
    return NULL;
  }
  return ((jobj *)s)->str.utf8;
}

static void ReleaseStringUTFChars(ENV, jstring s, const char *c) {}

static void GetStringRegion(ENV, jstring s, jsize start, jsize len, jchar *buf) {
  memcpy(buf, string_utf16(O(s)) + start, len * 2);
}

static void GetStringUTFRegion(ENV, jstring s, jsize start, jsize len, char *buf) {
  int n = utf16_to_utf8(string_utf16(O(s)) + start, len, buf);
  buf[n] = 0;
}

static const jchar *GetStringCritical(ENV, jstring s, jboolean *isCopy) { return GetStringChars(env, s, isCopy); }
static void ReleaseStringCritical(ENV, jstring s, const jchar *c) {}

/* arrays */
static jsize GetArrayLength(ENV, jarray a) { return O(a) ? ((jobj *)a)->arr.len : 0; }

static jobjectArray NewObjectArray(ENV, jsize len, jclass cls, jobject init) {
  jobj *a = jvm_array('L', len);
  for (int i = 0; i < len; i++)
    ((jobj **)a->arr.data)[i] = jvm_retain(O(init));
  return (jobjectArray)local(a);
}

static jobject GetObjectArrayElement(ENV, jobjectArray a, jsize i) {
  jobj *o = O(a);
  if (!o || i < 0 || i >= o->arr.len) {
    jvm_throw("java/lang/ArrayIndexOutOfBoundsException", "index");
    return NULL;
  }
  return local(jvm_retain(((jobj **)o->arr.data)[i]));
}

static void SetObjectArrayElement(ENV, jobjectArray a, jsize i, jobject v) {
  jobj *o = O(a);
  if (!o || i < 0 || i >= o->arr.len) {
    jvm_throw("java/lang/ArrayIndexOutOfBoundsException", "index");
    return;
  }
  jobj **slot = &((jobj **)o->arr.data)[i];
  jvm_retain(O(v));
  jvm_release(*slot);
  *slot = (jobj *)v;
}

#define ARRAYS(T, R, C)                                                                                      \
  static R##Array New##T##Array(ENV, jsize len) { return (R##Array)local(jvm_array(C, len)); }             \
  static R *Get##T##ArrayElements(ENV, R##Array a, jboolean *isCopy) {                                     \
    if (isCopy)                                                                                              \
      *isCopy = 0;                                                                                           \
    return O(a) ? (R *)((jobj *)a)->arr.data : NULL;                                                         \
  }                                                                                                          \
  static void Release##T##ArrayElements(ENV, R##Array a, R *e, jint mode) {}                               \
  static void Get##T##ArrayRegion(ENV, R##Array a, jsize s, jsize n, R *buf) {                             \
    memcpy(buf, (R *)((jobj *)a)->arr.data + s, n * sizeof(R));                                              \
  }                                                                                                          \
  static void Set##T##ArrayRegion(ENV, R##Array a, jsize s, jsize n, const R *buf) {                       \
    memcpy((R *)((jobj *)a)->arr.data + s, buf, n * sizeof(R));                                              \
  }

ARRAYS(Boolean, jboolean, 'Z')
ARRAYS(Byte, jbyte, 'B')
ARRAYS(Char, jchar, 'C')
ARRAYS(Short, jshort, 'S')
ARRAYS(Int, jint, 'I')
ARRAYS(Long, jlong, 'J')
ARRAYS(Float, jfloat, 'F')
ARRAYS(Double, jdouble, 'D')

static void *GetPrimitiveArrayCritical(ENV, jarray a, jboolean *isCopy) {
  if (isCopy)
    *isCopy = 0;
  return O(a) ? ((jobj *)a)->arr.data : NULL;
}

static void ReleasePrimitiveArrayCritical(ENV, jarray a, void *p, jint mode) {}

/* natives */
static jint RegisterNatives(ENV, jclass cls, const JNINativeMethod *methods, jint n) {
  jcls *c = C(cls);
  sceKernelLockLwMutex(&lock, 1, NULL);
  for (int i = 0; i < n; i++) {
    jmeth *m = method_add(c, methods[i].name, methods[i].signature);
    m->native = methods[i].fnPtr;
    DLOG("[jni] native %s.%s%s = %p", c->name, methods[i].name, methods[i].signature, methods[i].fnPtr);
  }
  sceKernelUnlockLwMutex(&lock, 1);
  LOG("[jni] RegisterNatives(%s, %d)", c->name, n);
  return 0;
}

static jint UnregisterNatives(ENV, jclass cls) { return 0; }
static jint MonitorEnter(ENV, jobject o) { return 0; }
static jint MonitorExit(ENV, jobject o) { return 0; }

static jint GetJavaVM(ENV, JavaVM **vm) {
  *vm = jvm_vm;
  return 0;
}

static jweak NewWeakGlobalRef(ENV, jobject o) { return (jweak)jvm_retain(O(o)); }
static void DeleteWeakGlobalRef(ENV, jweak o) { jvm_release(O(o)); }
static jboolean ExceptionCheck(ENV) { return *pending() != NULL; }

static jobject NewDirectByteBuffer(ENV, void *addr, jlong cap) {
  jobj *o = obj_alloc(jvm_class("java/nio/DirectByteBuffer"), OBJ_BUFFER);
  o->buf.addr = addr;
  o->buf.cap = cap;
  return local(o);
}

static void *GetDirectBufferAddress(ENV, jobject b) {
  jobj *o = O(b);
  return o && o->kind == OBJ_BUFFER ? o->buf.addr : NULL;
}

static jlong GetDirectBufferCapacity(ENV, jobject b) {
  jobj *o = O(b);
  return o && o->kind == OBJ_BUFFER ? o->buf.cap : -1;
}

static jobjectRefType GetObjectRefType(ENV, jobject o) { return o ? JNIGlobalRefType : JNIInvalidRefType; }

#define F(x) .x = x
#define FT(T) F(Call##T##Method), F(Call##T##MethodV), F(Call##T##MethodA), F(CallNonvirtual##T##Method),           \
              F(CallNonvirtual##T##MethodV), F(CallNonvirtual##T##MethodA), F(CallStatic##T##Method),                \
              F(CallStatic##T##MethodV), F(CallStatic##T##MethodA)
#define FF(T) F(Get##T##Field), F(Set##T##Field), F(GetStatic##T##Field), F(SetStatic##T##Field)
#define FA(T) F(New##T##Array), F(Get##T##ArrayElements), F(Release##T##ArrayElements), F(Get##T##ArrayRegion), \
              F(Set##T##ArrayRegion)

static const struct JNINativeInterface env_table = {
  F(GetVersion), F(DefineClass), F(FindClass), F(FromReflectedMethod), F(FromReflectedField),
  F(ToReflectedMethod), F(GetSuperclass), F(IsAssignableFrom), F(ToReflectedField), F(Throw), F(ThrowNew),
  F(ExceptionOccurred), F(ExceptionDescribe), F(ExceptionClear), F(FatalError), F(PushLocalFrame),
  F(PopLocalFrame), F(NewGlobalRef), F(DeleteGlobalRef), F(DeleteLocalRef), F(IsSameObject), F(NewLocalRef),
  F(EnsureLocalCapacity), F(AllocObject), F(NewObject), F(NewObjectV), F(NewObjectA), F(GetObjectClass),
  F(IsInstanceOf), F(GetMethodID), F(GetStaticMethodID), F(GetFieldID), F(GetStaticFieldID),
  FT(Object), FT(Boolean), FT(Byte), FT(Char), FT(Short), FT(Int), FT(Long), FT(Float), FT(Double),
  F(CallVoidMethod), F(CallVoidMethodV), F(CallVoidMethodA), F(CallNonvirtualVoidMethod),
  F(CallNonvirtualVoidMethodV), F(CallNonvirtualVoidMethodA), F(CallStaticVoidMethod), F(CallStaticVoidMethodV),
  F(CallStaticVoidMethodA),
  FF(Object), FF(Boolean), FF(Byte), FF(Char), FF(Short), FF(Int), FF(Long), FF(Float), FF(Double),
  F(NewString), F(GetStringLength), F(GetStringChars), F(ReleaseStringChars), F(NewStringUTF),
  F(GetStringUTFLength), F(GetStringUTFChars), F(ReleaseStringUTFChars), F(GetArrayLength), F(NewObjectArray),
  F(GetObjectArrayElement), F(SetObjectArrayElement),
  FA(Boolean), FA(Byte), FA(Char), FA(Short), FA(Int), FA(Long), FA(Float), FA(Double),
  F(RegisterNatives), F(UnregisterNatives), F(MonitorEnter), F(MonitorExit), F(GetJavaVM), F(GetStringRegion),
  F(GetStringUTFRegion), F(GetPrimitiveArrayCritical), F(ReleasePrimitiveArrayCritical), F(GetStringCritical),
  F(ReleaseStringCritical), F(NewWeakGlobalRef), F(DeleteWeakGlobalRef), F(ExceptionCheck),
  F(NewDirectByteBuffer), F(GetDirectBufferAddress), F(GetDirectBufferCapacity), F(GetObjectRefType),
};

/* ---------- JavaVM ---------- */

static const struct JNINativeInterface *env_ptr = &env_table;
JNIEnv *jvm_env = (JNIEnv *)&env_ptr;

static jint DestroyJavaVM(JavaVM *vm) { return 0; }
static jint AttachCurrentThread(JavaVM *vm, JNIEnv **env, void *args) {
  *env = jvm_env;
  return 0;
}
static jint DetachCurrentThread(JavaVM *vm) { return 0; }
static jint GetEnv(JavaVM *vm, void **env, jint version) {
  *env = jvm_env;
  return 0;
}

static const struct JNIInvokeInterface vm_table = {
  .DestroyJavaVM = DestroyJavaVM,
  .AttachCurrentThread = AttachCurrentThread,
  .DetachCurrentThread = DetachCurrentThread,
  .GetEnv = GetEnv,
  .AttachCurrentThreadAsDaemon = AttachCurrentThread,
};

static const struct JNIInvokeInterface *vm_ptr = &vm_table;
JavaVM *jvm_vm = (JavaVM *)&vm_ptr;

/* ---------- core java.lang ---------- */

static void java_name(const char *in, char *out, size_t n) {
  snprintf(out, n, "%s", in);
  for (char *p = out; *p; p++)
    if (*p == '/')
      *p = '.';
}

static jvalue class_getName(jobj *thiz, const jvalue *args, jmeth *m) {
  char n[256];
  java_name(thiz->klass->name, n, sizeof(n));
  return jv_obj(jvm_string(n));
}

static jvalue object_getClass(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(thiz->cls->obj));
}

static jvalue object_hashCode(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int((int32_t)(uintptr_t)thiz);
}

static jvalue object_equals(jobj *thiz, const jvalue *args, jmeth *m) {
  jobj *o = ARG_OBJ(0);
  if (thiz->kind == OBJ_STRING && o && o->kind == OBJ_STRING)
    return jv_bool(strcmp(thiz->str.utf8, o->str.utf8) == 0);
  return jv_bool(thiz == o);
}

static jvalue object_toString(jobj *thiz, const jvalue *args, jmeth *m) {
  if (thiz->kind == OBJ_STRING)
    return jv_obj(jvm_retain(thiz));
  char n[256];
  java_name(thiz->cls->name, n, sizeof(n));
  return jv_obj(jvm_stringf("%s@%x", n, (unsigned)(uintptr_t)thiz));
}

static jvalue throwable_getMessage(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_string(thiz->native ? (const char *)thiz->native : ""));
}

static jvalue box_intValue(jobj *thiz, const jvalue *args, jmeth *m) {
  return jvm_get_field(thiz, "value");
}

static jvalue string_length(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(thiz->str.ulen);
}

static jvalue string_getBytes(jobj *thiz, const jvalue *args, jmeth *m) {
  jobj *a = jvm_array('B', thiz->str.len);
  memcpy(a->arr.data, thiz->str.utf8, thiz->str.len);
  return jv_obj(a);
}

void jvm_init(void) {
  sceKernelCreateLwMutex(&lock, "jvm", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
  /* bootstrap Object and Class by hand: class objects need class_class */
  jcls *object = calloc(1, sizeof(*object));
  object->name = strdup("java/lang/Object");
  class_class = calloc(1, sizeof(*class_class));
  class_class->name = strdup("java/lang/Class");
  class_class->super = object;
  object->next = class_class;
  classes = object;
  object->obj = obj_alloc(class_class, OBJ_CLASS);
  object->obj->klass = object;
  object->obj->pinned = 1;
  class_class->obj = obj_alloc(class_class, OBJ_CLASS);
  class_class->obj->klass = class_class;
  class_class->obj->pinned = 1;
  object->declared = class_class->declared = 1;
  string_class = jvm_declare("java/lang/String", "java/lang/Object");

  jvm_method("java/lang/Class", "getName", "()Ljava/lang/String;", class_getName);
  jvm_method("java/lang/Class", "getSimpleName", "()Ljava/lang/String;", class_getName);
  jvm_method("java/lang/Object", "getClass", "()Ljava/lang/Class;", object_getClass);
  jvm_method("java/lang/Object", "hashCode", "()I", object_hashCode);
  jvm_method("java/lang/Object", "equals", "(Ljava/lang/Object;)Z", object_equals);
  jvm_method("java/lang/Object", "toString", "()Ljava/lang/String;", object_toString);
  jvm_method("java/lang/String", "length", "()I", string_length);
  jvm_method("java/lang/String", "getBytes", "()[B", string_getBytes);
  jvm_method("java/lang/String", "getBytes", "(Ljava/lang/String;)[B", string_getBytes);

  jvm_declare("java/lang/Throwable", "java/lang/Object");
  jvm_declare("java/lang/Exception", "java/lang/Throwable");
  jvm_declare("java/lang/Error", "java/lang/Throwable");
  jvm_declare("java/lang/RuntimeException", "java/lang/Exception");
  jvm_declare("java/lang/ClassNotFoundException", "java/lang/Exception");
  jvm_declare("java/lang/NoSuchMethodError", "java/lang/Error");
  jvm_declare("java/lang/NoSuchFieldError", "java/lang/Error");
  jvm_declare("java/lang/ArrayIndexOutOfBoundsException", "java/lang/RuntimeException");
  jvm_method("java/lang/Throwable", "getMessage", "()Ljava/lang/String;", throwable_getMessage);
  jvm_method("java/lang/Throwable", "getLocalizedMessage", "()Ljava/lang/String;", throwable_getMessage);
  jvm_method("java/lang/Throwable", "toString", "()Ljava/lang/String;", throwable_getMessage);

  jvm_declare("java/lang/Number", "java/lang/Object");
  jvm_declare("java/lang/Integer", "java/lang/Number");
  jvm_declare("java/lang/Boolean", "java/lang/Object");
  jvm_declare("java/lang/Float", "java/lang/Number");
  jvm_method("java/lang/Integer", "intValue", "()I", box_intValue);
  jvm_method("java/lang/Boolean", "booleanValue", "()Z", box_intValue);
  jvm_method("java/lang/Float", "floatValue", "()F", box_intValue);
  LOG("[jni] VM ready");
}

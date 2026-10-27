#ifndef PG3D_JVM_H
#define PG3D_JVM_H

/*
 * A small JVM model behind the JNI function table: named classes with
 * superclasses, objects with refcounts, strings, arrays, Class objects and
 * direct buffers. Java methods the game calls are implemented in C as
 * handlers; RegisterNatives records the game's native methods so we can call
 * into them.
 */
#include <stdint.h>

#include "jni/jni.h"

enum { OBJ_PLAIN = 0, OBJ_STRING, OBJ_ARRAY, OBJ_CLASS, OBJ_BUFFER, OBJ_METHOD, OBJ_FIELD };

typedef struct jcls jcls;
typedef struct jmeth jmeth;
typedef struct jfld jfld;

typedef struct jfieldval {
  jfld *f;
  jvalue v;
  struct jfieldval *next;
} jfieldval;

typedef struct jobj {
  uint32_t magic;
  jcls *cls;
  volatile int refs;
  int kind;
  int pinned;
  union {
    struct {
      char *utf8;
      int len;        /* bytes */
      uint16_t *utf16;
      int ulen;       /* UTF-16 units */
    } str;
    struct {
      int len;
      int esize;
      char etype;     /* signature char of the element: Z B C S I J F D L */
      void *data;
    } arr;
    jcls *klass;      /* OBJ_CLASS */
    struct {
      void *addr;
      int64_t cap;
    } buf;
    jmeth *meth;      /* OBJ_METHOD */
    jfld *fld;        /* OBJ_FIELD */
  };
  void *native;       /* per-class instance data */
  void (*native_free)(void *);
  jfieldval *fields;  /* instance field storage */
} jobj;

typedef jvalue (*jvm_handler)(jobj *thiz, const jvalue *args, jmeth *m);
typedef jvalue (*jvm_getter)(jobj *thiz, jfld *f);

struct jmeth {
  jcls *cls;
  char *name, *sig;
  jvm_handler fn;
  void *native;       /* RegisterNatives function pointer */
  char ret;           /* return type char */
  int nargs;
  char argt[32];
  unsigned calls;
  jmeth *next;
};

struct jfld {
  jcls *cls;
  char *name, *sig;
  jvm_getter get;
  jvalue static_val;
  int has_static;
  jfld *next;
};

struct jcls {
  char *name;
  jcls *super;
  jobj *obj;          /* the java.lang.Class instance */
  jmeth *methods;
  jfld *fields;
  int declared;       /* modelled by us (vs. created on demand) */
  jcls *next;
};

extern JNIEnv *jvm_env;
extern JavaVM *jvm_vm;

void jvm_init(void);

/* classes and members */
jcls *jvm_class(const char *name);                         /* find or create */
jcls *jvm_declare(const char *name, const char *super);    /* model a class */
void jvm_method(const char *cls, const char *name, const char *sig, jvm_handler fn);
void jvm_field(const char *cls, const char *name, const char *sig, jvm_getter get);
void jvm_static(const char *cls, const char *name, const char *sig, jvalue v);
jmeth *jvm_find_method(jcls *c, const char *name, const char *sig);
jmeth *jvm_reflect_method(jcls *c, const char *name, const char *sig, int is_static);
jfld *jvm_reflect_field(jcls *c, const char *name, const char *sig, int is_static);
int jvm_is_a(jcls *c, jcls *ancestor);

/* objects */
jobj *jvm_new(const char *cls);
jobj *jvm_retain(jobj *o);
void jvm_release(jobj *o);
jobj *jvm_pin(jobj *o);
jobj *jvm_string(const char *utf8);
jobj *jvm_stringf(const char *fmt, ...);
const char *jvm_str(jobj *s);                              /* NULL-safe, "" for null */
jobj *jvm_array(char etype, int len);
jobj *jvm_box_int(int v);
jobj *jvm_box_bool(int v);
jobj *jvm_box_float(float v);
jvalue jvm_get_field(jobj *o, const char *name);
void jvm_set_field(jobj *o, const char *name, const char *sig, jvalue v);

/* exceptions */
void jvm_throw(const char *cls, const char *msg);

/* calling the game's registered natives */
void *jvm_native(const char *cls, const char *name, const char *sig);
/* A call from our Java side into a native runs inside a local frame, as on a
 * real VM: what the native leaves behind is released when it returns. Pop
 * gives `keep` (the native's result) a reference of the caller's own. */
void jvm_frame_push(void);
jobj *jvm_frame_pop(jobj *keep);

/* java.lang.reflect.Method / Field objects for an id */
jobj *jvm_method_object(jmeth *m);
jobj *jvm_field_object(jfld *f);

/* handler return helpers */
static inline jvalue jv_obj(jobj *o) { jvalue v; v.j = 0; v.l = (jobject)o; return v; }
static inline jvalue jv_int(int32_t i) { jvalue v; v.j = 0; v.i = i; return v; }
static inline jvalue jv_long(int64_t j) { jvalue v; v.j = j; return v; }
static inline jvalue jv_bool(int z) { jvalue v; v.j = 0; v.z = z ? 1 : 0; return v; }
static inline jvalue jv_float(float f) { jvalue v; v.j = 0; v.f = f; return v; }
static inline jvalue jv_void(void) { jvalue v; v.j = 0; return v; }
#define ARG_OBJ(i) ((jobj *)args[i].l)
#define ARG_STR(i) jvm_str((jobj *)args[i].l)

#endif

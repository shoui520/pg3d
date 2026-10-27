/*
 * SharedPreferences (Unity PlayerPrefs live here) persisted as one text file
 * per preferences name under shared_prefs/. Editors write through to the
 * in-memory table; commit/apply only mark it dirty, and java_prefs_tick
 * snapshots dirty tables at most once a second for the writer thread, which
 * writes them out (write + rename). PG3D commits large JSON blobs several
 * times per purchase, and a save is ~400 KB, so neither the commit nor the
 * file write happens on the game's thread.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/bionic.h"
#include "jni/java.h"
#include "util/log.h"

#define PREFS_CLASS "android/app/SharedPreferencesImpl"
#define EDITOR_CLASS "android/app/SharedPreferencesImpl$EditorImpl"

typedef struct entry {
  char *key;
  char type; /* I F S Z J */
  union {
    int32_t i;
    float f;
    int64_t j;
    char *s;
  };
  struct entry *next;
} entry;

#define BUCKETS 1024

typedef struct prefs {
  char name[96];
  entry *b[BUCKETS];
  int count;
  int dirty;
  char *snap; /* file contents waiting for the writer */
  size_t snap_len, hint;
  SceKernelLwMutexWork lock;
  jobj *obj, *editor;
  struct prefs *next;
} prefs;

static prefs *all;
static SceKernelLwMutexWork all_lock;
static SceKernelLwMutexWork write_lock; /* held from taking a snapshot until it is on disk */
static SceUID write_sema = -1;

static unsigned hash(const char *s) {
  unsigned h = 2166136261u;
  while (*s)
    h = (h ^ (unsigned char)*s++) * 16777619u;
  return h % BUCKETS;
}

static entry *find(prefs *p, const char *key) {
  for (entry *e = p->b[hash(key)]; e; e = e->next)
    if (strcmp(e->key, key) == 0)
      return e;
  return NULL;
}

static entry *put(prefs *p, const char *key, char type) {
  entry *e = find(p, key);
  if (!e) {
    e = calloc(1, sizeof(*e));
    e->key = strdup(key);
    unsigned h = hash(key);
    e->next = p->b[h];
    p->b[h] = e;
    p->count++;
  } else if (e->type == 'S') {
    free(e->s);
    e->s = NULL;
  }
  e->type = type;
  p->dirty = 1;
  return e;
}

static void erase(prefs *p, const char *key) {
  unsigned h = hash(key);
  for (entry **pp = &p->b[h]; *pp; pp = &(*pp)->next) {
    entry *e = *pp;
    if (strcmp(e->key, key) == 0) {
      *pp = e->next;
      if (e->type == 'S')
        free(e->s);
      free(e->key);
      free(e);
      p->count--;
      p->dirty = 1;
      return;
    }
  }
}

static void clear(prefs *p) {
  for (int i = 0; i < BUCKETS; i++) {
    while (p->b[i]) {
      entry *e = p->b[i];
      p->b[i] = e->next;
      if (e->type == 'S')
        free(e->s);
      free(e->key);
      free(e);
    }
  }
  p->count = 0;
  p->dirty = 1;
}

/* ---------- file format: "T\tkey\tvalue\n" with \\ \t \n escaped ---------- */

typedef struct {
  char *p;
  size_t n, cap;
  int failed;
} text;

static void add(text *t, const char *s, size_t n) {
  if (t->failed || !n)
    return;
  if (t->n + n > t->cap) {
    size_t cap = t->cap ? t->cap : 64 * 1024;
    while (cap < t->n + n)
      cap *= 2;
    char *q = realloc(t->p, cap);
    if (!q) {
      t->failed = 1;
      return;
    }
    t->p = q;
    t->cap = cap;
  }
  memcpy(t->p + t->n, s, n);
  t->n += n;
}

static void add_escaped(text *t, const char *s) {
  const char *run = s;
  for (; *s; s++) {
    const char *esc = *s == '\\' ? "\\\\" : *s == '\t' ? "\\t" : *s == '\n' ? "\\n" : NULL;
    if (esc) {
      add(t, run, s - run);
      add(t, esc, 2);
      run = s + 1;
    }
  }
  add(t, run, s - run);
}

static void unescape(char *s) {
  char *o = s;
  for (; *s; s++) {
    if (*s == '\\' && s[1]) {
      s++;
      *o++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s;
    } else {
      *o++ = *s;
    }
  }
  *o = 0;
}

static void prefs_path(prefs *p, char *out, size_t n, const char *suffix) {
  char ap[256];
  snprintf(ap, sizeof(ap), "/data/data/com.pixel.gun3d/shared_prefs/%s.prefs%s", p->name, suffix);
  bionic_map_path(ap, out, n);
}

/* The table as file contents, for the writer; caller holds p->lock */
static void snapshot(prefs *p) {
  if (!p->dirty)
    return;
  text t = { 0 };
  t.p = malloc(t.cap = p->hint + 16 * 1024);
  if (!t.p)
    t.cap = 0;
  char num[32];
  for (int i = 0; i < BUCKETS; i++) {
    for (entry *e = p->b[i]; e; e = e->next) {
      char head[2] = { e->type, '\t' };
      add(&t, head, 2);
      add_escaped(&t, e->key);
      add(&t, "\t", 1);
      int n = 0;
      switch (e->type) {
      case 'I': n = snprintf(num, sizeof(num), "%d", e->i); break;
      case 'Z': n = snprintf(num, sizeof(num), "%d", e->i ? 1 : 0); break;
      case 'J': n = snprintf(num, sizeof(num), "%lld", e->j); break;
      case 'F': n = snprintf(num, sizeof(num), "%.9g", e->f); break;
      case 'S': add_escaped(&t, e->s); break;
      }
      add(&t, num, n);
      add(&t, "\n", 1);
    }
  }
  if (t.failed) {
    LOG("[prefs] %s: no memory for a %u KB snapshot; will retry", p->name, (unsigned)(t.cap / 1024));
    free(t.p);
    return;
  }
  free(p->snap);
  p->snap = t.p;
  p->snap_len = t.n;
  p->hint = t.n;
  p->dirty = 0;
}

static void write_file(prefs *p, const char *buf, size_t n) {
  char path[512], tmp[512];
  prefs_path(p, path, sizeof(path), "");
  prefs_path(p, tmp, sizeof(tmp), ".tmp");
  SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) {
    LOG("[prefs] cannot write %s: 0x%08X", tmp, fd);
    return;
  }
  size_t done = 0;
  while (done < n) {
    int r = sceIoWrite(fd, buf + done, n - done);
    if (r <= 0)
      break;
    done += r;
  }
  sceIoClose(fd);
  if (done < n) {
    LOG("[prefs] %s: wrote %u of %u bytes; keeping the old file", tmp, (unsigned)done, (unsigned)n);
    return;
  }
  sceIoRemove(path);
  int r = sceIoRename(tmp, path);
  if (r < 0)
    LOG("[prefs] rename %s failed: 0x%08X", tmp, r);
}

/* Write every queued snapshot; caller holds write_lock. The list only ever
 * gains entries at its head, so it is walked without all_lock. */
static void write_queued(void) {
  sceKernelLockLwMutex(&all_lock, 1, NULL);
  prefs *head = all;
  sceKernelUnlockLwMutex(&all_lock, 1);
  for (prefs *p = head; p; p = p->next) {
    sceKernelLockLwMutex(&p->lock, 1, NULL);
    char *buf = p->snap;
    size_t n = p->snap_len;
    p->snap = NULL;
    sceKernelUnlockLwMutex(&p->lock, 1);
    if (buf) {
      write_file(p, buf, n);
      free(buf);
    }
  }
}

static int writer(SceSize args, void *argp) {
  for (;;) {
    sceKernelWaitSema(write_sema, 1, NULL);
    sceKernelLockLwMutex(&write_lock, 1, NULL);
    write_queued();
    sceKernelUnlockLwMutex(&write_lock, 1);
  }
  return 0;
}

static void load(prefs *p) {
  char path[512];
  prefs_path(p, path, sizeof(path), "");
  FILE *f = fopen(path, "rb");
  if (!f)
    return;
  size_t cap = 4096;
  char *line = malloc(cap);
  int c;
  size_t n = 0;
  for (;;) {
    c = fgetc(f);
    if (c != '\n' && c != EOF) {
      if (n + 1 >= cap)
        line = realloc(line, cap *= 2);
      line[n++] = c;
      continue;
    }
    line[n] = 0;
    if (n > 2 && line[1] == '\t') {
      char *key = line + 2, *val = strchr(key, '\t');
      if (val) {
        *val++ = 0;
        unescape(key);
        entry *e = put(p, key, line[0]);
        switch (line[0]) {
        case 'I':
        case 'Z': e->i = atoi(val); break;
        case 'J': e->j = strtoll(val, NULL, 10); break;
        case 'F': e->f = strtof(val, NULL); break;
        default:
          unescape(val);
          e->type = 'S';
          e->s = strdup(val);
          break;
        }
      }
    }
    n = 0;
    if (c == EOF)
      break;
  }
  free(line);
  fclose(f);
  p->dirty = 0;
  LOG("[prefs] %s: loaded %d entries", p->name, p->count);
}

static prefs *of(jobj *o) {
  return o ? (prefs *)o->native : NULL;
}

static prefs *open_prefs(const char *name) {
  sceKernelLockLwMutex(&all_lock, 1, NULL);
  prefs *p;
  for (p = all; p; p = p->next)
    if (strcmp(p->name, name) == 0)
      break;
  if (!p) {
    p = calloc(1, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    sceKernelCreateLwMutex(&p->lock, "prefs", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    load(p);
    p->obj = jvm_pin(jvm_new(PREFS_CLASS));
    p->obj->native = p;
    p->editor = jvm_pin(jvm_new(EDITOR_CLASS));
    p->editor->native = p;
    p->next = all;
    all = p;
  }
  sceKernelUnlockLwMutex(&all_lock, 1);
  return p;
}

static int snapshot_all(void) {
  int queued = 0;
  sceKernelLockLwMutex(&all_lock, 1, NULL);
  for (prefs *p = all; p; p = p->next) {
    sceKernelLockLwMutex(&p->lock, 1, NULL);
    snapshot(p);
    queued |= p->snap != NULL;
    sceKernelUnlockLwMutex(&p->lock, 1);
  }
  sceKernelUnlockLwMutex(&all_lock, 1);
  return queued;
}

/* On the way out: everything on disk before returning */
void java_prefs_flush(void) {
  sceKernelLockLwMutex(&write_lock, 1, NULL);
  snapshot_all();
  write_queued();
  sceKernelUnlockLwMutex(&write_lock, 1);
}

void java_prefs_tick(void) {
  static int64_t last_us;
  int64_t now = bionic_monotonic_us();
  if (now - last_us < 1000000)
    return;
  last_us = now;
  if (snapshot_all()) {
    if (write_sema >= 0)
      sceKernelSignalSema(write_sema, 1);
    else
      java_prefs_flush();
  }
}

/* ---------- Context ---------- */

static jvalue ctx_getSharedPreferences(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(open_prefs(ARG_STR(0))->obj));
}

static jvalue act_getPreferences(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(open_prefs("UnityPlayerNativeActivity")->obj));
}

/* ---------- SharedPreferences ---------- */

#define LOCKED(p, body)                                                                                      \
  do {                                                                                                       \
    sceKernelLockLwMutex(&(p)->lock, 1, NULL);                                                               \
    body;                                                                                                    \
    sceKernelUnlockLwMutex(&(p)->lock, 1);                                                                   \
  } while (0)

static jvalue sp_contains(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  int r;
  LOCKED(p, r = find(p, ARG_STR(0)) != NULL);
  return jv_bool(r);
}

static jvalue sp_getInt(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  jvalue r = args[1];
  LOCKED(p, {
    entry *e = find(p, ARG_STR(0));
    if (e && (e->type == 'I' || e->type == 'Z'))
      r = jv_int(e->i);
  });
  return r;
}

static jvalue sp_getLong(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  jvalue r = args[1];
  LOCKED(p, {
    entry *e = find(p, ARG_STR(0));
    if (e && e->type == 'J')
      r = jv_long(e->j);
  });
  return r;
}

static jvalue sp_getFloat(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  jvalue r = args[1];
  LOCKED(p, {
    entry *e = find(p, ARG_STR(0));
    if (e && e->type == 'F')
      r = jv_float(e->f);
  });
  return r;
}

static jvalue sp_getBoolean(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  jvalue r = args[1];
  LOCKED(p, {
    entry *e = find(p, ARG_STR(0));
    if (e && (e->type == 'Z' || e->type == 'I'))
      r = jv_bool(e->i);
  });
  return r;
}

static jvalue sp_getString(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  jobj *r = NULL;
  int found = 0;
  LOCKED(p, {
    entry *e = find(p, ARG_STR(0));
    if (e && e->type == 'S') {
      r = jvm_string(e->s);
      found = 1;
    }
  });
  if (!found)
    r = jvm_retain(ARG_OBJ(1));
  return jv_obj(r);
}

static jvalue sp_edit(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_obj(jvm_retain(of(thiz)->editor));
}

/* getAll(): a java/util/HashMap of boxed values. */
static jvalue sp_getAll(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  jobj *map = jvm_new("java/util/HashMap");
  sceKernelLockLwMutex(&p->lock, 1, NULL);
  for (int i = 0; i < BUCKETS; i++) {
    for (entry *e = p->b[i]; e; e = e->next) {
      jobj *k = jvm_string(e->key), *v;
      switch (e->type) {
      case 'I': v = jvm_box_int(e->i); break;
      case 'Z': v = jvm_box_bool(e->i); break;
      case 'F': v = jvm_box_float(e->f); break;
      case 'J': v = jvm_box_int((int32_t)e->j); break;
      default: v = jvm_string(e->s); break;
      }
      java_map_put(map, k, v);
      jvm_release(k);
      jvm_release(v);
    }
  }
  sceKernelUnlockLwMutex(&p->lock, 1);
  return jv_obj(map);
}

/* ---------- Editor ---------- */

static jvalue ed_self(jobj *thiz) {
  return jv_obj(jvm_retain(thiz));
}

static jvalue ed_putInt(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  LOCKED(p, put(p, ARG_STR(0), 'I')->i = args[1].i);
  return ed_self(thiz);
}

static jvalue ed_putLong(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  LOCKED(p, put(p, ARG_STR(0), 'J')->j = args[1].j);
  return ed_self(thiz);
}

static jvalue ed_putFloat(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  LOCKED(p, put(p, ARG_STR(0), 'F')->f = args[1].f);
  return ed_self(thiz);
}

static jvalue ed_putBoolean(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  LOCKED(p, put(p, ARG_STR(0), 'Z')->i = args[1].z);
  return ed_self(thiz);
}

static jvalue ed_putString(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  if (!ARG_OBJ(1)) {
    LOCKED(p, erase(p, ARG_STR(0)));
    return ed_self(thiz);
  }
  LOCKED(p, put(p, ARG_STR(0), 'S')->s = strdup(ARG_STR(1)));
  return ed_self(thiz);
}

static jvalue ed_remove(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  LOCKED(p, erase(p, ARG_STR(0)));
  return ed_self(thiz);
}

static jvalue ed_clear(jobj *thiz, const jvalue *args, jmeth *m) {
  prefs *p = of(thiz);
  LOCKED(p, clear(p));
  return ed_self(thiz);
}

static jvalue ed_commit(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_bool(1);
}

void java_prefs_init(void) {
  sceKernelCreateLwMutex(&all_lock, "prefs_all", 0, 0, NULL);
  sceKernelCreateLwMutex(&write_lock, "prefs_write", 0, 0, NULL);
  write_sema = sceKernelCreateSema("prefs_write", 0, 0, 1, NULL);
  SceUID wt = write_sema >= 0 ? sceKernelCreateThread("pg3d_prefs", writer, 0x10000100, 0x4000, 0, 0, NULL) : -1;
  if (wt < 0 || sceKernelStartThread(wt, 0, NULL) < 0) {
    LOG("[prefs] no writer thread (0x%08X); saving on the game thread", wt);
    if (write_sema >= 0)
      sceKernelDeleteSema(write_sema);
    write_sema = -1;
  }
  extern int b_mkdir(const char *, int);
  b_mkdir("/data/data/com.pixel.gun3d/shared_prefs", 0777);

  jvm_declare("android/content/SharedPreferences", NULL);
  jvm_declare("android/content/SharedPreferences$Editor", NULL);
  jvm_declare(PREFS_CLASS, "android/content/SharedPreferences");
  jvm_declare(EDITOR_CLASS, "android/content/SharedPreferences$Editor");
  jvm_method("android/content/Context", "getSharedPreferences",
             "(Ljava/lang/String;I)Landroid/content/SharedPreferences;", ctx_getSharedPreferences);
  jvm_method("android/app/Activity", "getPreferences", "(I)Landroid/content/SharedPreferences;", act_getPreferences);

  const char *S = "android/content/SharedPreferences";
  jvm_method(S, "contains", "(Ljava/lang/String;)Z", sp_contains);
  jvm_method(S, "getInt", "(Ljava/lang/String;I)I", sp_getInt);
  jvm_method(S, "getLong", "(Ljava/lang/String;J)J", sp_getLong);
  jvm_method(S, "getFloat", "(Ljava/lang/String;F)F", sp_getFloat);
  jvm_method(S, "getBoolean", "(Ljava/lang/String;Z)Z", sp_getBoolean);
  jvm_method(S, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", sp_getString);
  jvm_method(S, "edit", "()Landroid/content/SharedPreferences$Editor;", sp_edit);
  jvm_method(S, "getAll", "()Ljava/util/Map;", sp_getAll);

  const char *E = "android/content/SharedPreferences$Editor";
  const char *R = "Landroid/content/SharedPreferences$Editor;";
  char sig[128];
  snprintf(sig, sizeof(sig), "(Ljava/lang/String;I)%s", R);
  jvm_method(E, "putInt", sig, ed_putInt);
  snprintf(sig, sizeof(sig), "(Ljava/lang/String;J)%s", R);
  jvm_method(E, "putLong", sig, ed_putLong);
  snprintf(sig, sizeof(sig), "(Ljava/lang/String;F)%s", R);
  jvm_method(E, "putFloat", sig, ed_putFloat);
  snprintf(sig, sizeof(sig), "(Ljava/lang/String;Z)%s", R);
  jvm_method(E, "putBoolean", sig, ed_putBoolean);
  snprintf(sig, sizeof(sig), "(Ljava/lang/String;Ljava/lang/String;)%s", R);
  jvm_method(E, "putString", sig, ed_putString);
  snprintf(sig, sizeof(sig), "(Ljava/lang/String;)%s", R);
  jvm_method(E, "remove", sig, ed_remove);
  snprintf(sig, sizeof(sig), "()%s", R);
  jvm_method(E, "clear", sig, ed_clear);
  jvm_method(E, "commit", "()Z", ed_commit);
  jvm_method(E, "apply", "()V", ed_commit);
}

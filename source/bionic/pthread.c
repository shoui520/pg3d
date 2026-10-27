/*
 * Bionic pthread ABI on Vita kernel primitives.
 *
 * Bionic objects are 4 bytes (mutex, cond, once, key, sem, mutexattr) and are
 * valid when zero-initialized, so each word lazily receives a pointer to a
 * heap object. Threads are created with the VM attribute so the GC can
 * suspend them (see signal.c).
 */
#include "bionic/bthread.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/bionic.h"
#include "util/log.h"

#define BTHREAD_MAGIC 0x42544852
#define THREAD_ATTR_VM 0x04000000
#define DEFAULT_STACK (128 * 1024) /* Unity's workers peak below 32 KB */
#define MIN_STACK (128 * 1024)

#define B_EBUSY 16
#define B_EINVAL 22
#define B_EDEADLK 35
#define B_ETIMEDOUT 110
#define B_EPERM 1
#define B_ESRCH 3

/* ---------- per-thread state ---------- */

static inline uint32_t *tls_slot(void) {
  uintptr_t user;
  __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(user));
  return (uint32_t *)(user - 0x800 + BTHREAD_TLS_SLOT * 4);
}

static void fill_stack_bounds(bthread *t) {
  SceKernelThreadInfo info;
  memset(&info, 0, sizeof(info));
  info.size = sizeof(info);
  if (sceKernelGetThreadInfo(t->uid, &info) == 0) {
    t->stack_lo = (uintptr_t)info.stack;
    t->stack_hi = t->stack_lo + info.stackSize;
  }
}

bthread *bthread_self(void) {
  uint32_t *slot = tls_slot();
  bthread *t = (bthread *)*slot;
  if (t)
    return t;
  t = calloc(1, sizeof(*t));
  t->magic = BTHREAD_MAGIC;
  t->uid = sceKernelGetThreadId();
  t->adopted = 1;
  t->park = -1;
  fill_stack_bounds(t);
  SceKernelThreadInfo info;
  memset(&info, 0, sizeof(info));
  info.size = sizeof(info);
  if (sceKernelGetThreadInfo(t->uid, &info) == 0) {
    strncpy(t->name, info.name, sizeof(t->name) - 1);
    t->vm_capable = (info.attr & THREAD_ATTR_VM) != 0;
  }
  *slot = (uint32_t)t;
  return t;
}

void bthread_check_tls(void) {
  uint32_t *a = tls_slot();
  uint32_t *b = sceKernelGetTLSAddr(BTHREAD_TLS_SLOT);
  if (a != b)
    fatal("[pthread] TLS slot mismatch %p vs %p", a, b);
}

/* ---------- keys ---------- */

static SceKernelLwMutexWork key_lock;
static uint8_t key_used[BIONIC_KEYS_MAX];
static void (*key_dtor[BIONIC_KEYS_MAX])(void *);

static void run_key_destructors(bthread *t) {
  for (int round = 0; round < 4; round++) {
    int any = 0;
    for (int k = 0; k < BIONIC_KEYS_MAX; k++) {
      void *v = t->tls[k];
      if (v && key_used[k] && key_dtor[k]) {
        t->tls[k] = NULL;
        key_dtor[k](v);
        any = 1;
      }
    }
    if (!any)
      break;
  }
}

int b_pthread_key_create(int *key, void (*dtor)(void *)) {
  sceKernelLockLwMutex(&key_lock, 1, NULL);
  for (int k = 0; k < BIONIC_KEYS_MAX; k++) {
    if (!key_used[k]) {
      key_used[k] = 1;
      key_dtor[k] = dtor;
      sceKernelUnlockLwMutex(&key_lock, 1);
      *key = k;
      return 0;
    }
  }
  sceKernelUnlockLwMutex(&key_lock, 1);
  LOG("[pthread] out of keys");
  return 11; /* EAGAIN */
}

int b_pthread_key_delete(int key) {
  if ((unsigned)key >= BIONIC_KEYS_MAX)
    return B_EINVAL;
  sceKernelLockLwMutex(&key_lock, 1, NULL);
  key_used[key] = 0;
  key_dtor[key] = NULL;
  sceKernelUnlockLwMutex(&key_lock, 1);
  return 0;
}

void *b_pthread_getspecific(int key) {
  if ((unsigned)key >= BIONIC_KEYS_MAX)
    return NULL;
  return bthread_self()->tls[key];
}

int b_pthread_setspecific(int key, const void *value) {
  if ((unsigned)key >= BIONIC_KEYS_MAX)
    return B_EINVAL;
  bthread_self()->tls[key] = (void *)value;
  return 0;
}

/* ---------- threads ---------- */

#define ST_DETACHED 1
#define ST_EXITED 2

typedef struct {
  bthread t;
  volatile int state;
} bthread_full;

static void bthread_free(bthread *t) {
  if (t->park >= 0)
    sceKernelDeleteSema(t->park);
  free(t->jni_frames);
  free(t);
}

static void forget_stack(SceUID uid);

static __attribute__((noreturn)) void bthread_finish(bthread *t, void *ret) {
  t->retval = ret;
  run_key_destructors(t);
  bthread_full *f = (bthread_full *)t;
  int old = __atomic_fetch_or(&f->state, ST_EXITED, __ATOMIC_ACQ_REL);
  if (old & ST_DETACHED) {
    *tls_slot() = 0;
    forget_stack(t->uid);
    bthread_free(t);
    sceKernelExitDeleteThread(0);
  } else {
    sceKernelExitThread(0);
  }
  for (;;)
    ;
}

static int bthread_entry(SceSize argc, void *argp) {
  bthread *t = *(bthread **)argp;
  *tls_slot() = (uint32_t)t;
  fill_stack_bounds(t);
  bthread_finish(t, t->start(t->arg));
  return 0;
}

/* every thread we created, for fault scanning and stack checks */
SceUID bthread_uids[256];
volatile int bthread_nuids;

/* ---------- stack use ----------
 * The kernel fills a new stack with 0xFF, so the lowest word that changed
 * marks the deepest a thread has gone. Stacks are sized from those marks;
 * the watchdog checks one thread per tick and says when one gets tight. A
 * stack is not freed while it is being read. */
static SceKernelLwMutexWork stacks_lock;
static uint32_t stack_used[256], stack_size[256];

static void forget_stack(SceUID uid) {
  sceKernelLockLwMutex(&stacks_lock, 1, NULL);
  for (int i = 0; i < bthread_nuids && i < 256; i++)
    if (bthread_uids[i] == uid)
      bthread_uids[i] = -1;
  sceKernelUnlockLwMutex(&stacks_lock, 1);
}

void bthread_check_stacks(void) {
  const char *thread_name(SceUID uid);
  static int next, fullest_pct_logged;
  static SceUID warned[256]; /* the thread in each slot already reported */
  int n = bthread_nuids < 256 ? bthread_nuids : 256;
  if (!n)
    return;
  int i = next++ % n;
  SceKernelThreadInfo ti;
  memset(&ti, 0, sizeof(ti));
  ti.size = sizeof(ti);
  sceKernelLockLwMutex(&stacks_lock, 1, NULL);
  SceUID uid = bthread_uids[i];
  if (uid > 0 && sceKernelGetThreadInfo(uid, &ti) == 0 && ti.stack && ti.stackSize >= 8) {
    const uint32_t *w = ti.stack;
    size_t words = ti.stackSize / 4, k = 1; /* word 0 is the kernel's */
    while (k < words && w[k] == 0xFFFFFFFF)
      k++;
    stack_used[i] = (words - k) * 4;
    stack_size[i] = ti.stackSize;
  } else {
    stack_used[i] = stack_size[i] = 0;
  }
  sceKernelUnlockLwMutex(&stacks_lock, 1);
  if (stack_size[i] && stack_used[i] * 4 > stack_size[i] * 3 && warned[i] != uid) {
    warned[i] = uid;
    const char *name = thread_name(uid);
    LOG("[mem] thread 0x%X (%s) has used %u of %u KB of stack", uid, name ? name : ti.name, stack_used[i] / 1024,
        stack_size[i] / 1024);
  }
  if (i != n - 1)
    return;
  /* a full round: report when the fullest stack is fuller than last time */
  int live = 0, fullest = -1, deepest = -1;
  uint32_t total = 0;
  for (int k = 0; k < n; k++) {
    if (!stack_size[k])
      continue;
    live++;
    total += stack_size[k];
    if (fullest < 0 || (uint64_t)stack_used[k] * stack_size[fullest] > (uint64_t)stack_used[fullest] * stack_size[k])
      fullest = k;
    if (deepest < 0 || stack_used[k] > stack_used[deepest])
      deepest = k;
  }
  if (fullest < 0)
    return;
  int pct = stack_used[fullest] * 100 / stack_size[fullest];
  if (pct <= fullest_pct_logged)
    return;
  fullest_pct_logged = pct;
  const char *fname = thread_name(bthread_uids[fullest]), *dname = thread_name(bthread_uids[deepest]);
  LOG("[mem] stacks: %d threads, %u KB; fullest 0x%X (%s) %u/%u KB, deepest 0x%X (%s) %u/%u KB", live, total / 1024,
      bthread_uids[fullest], fname ? fname : "?", stack_used[fullest] / 1024, stack_size[fullest] / 1024,
      bthread_uids[deepest], dname ? dname : "?", stack_used[deepest] / 1024, stack_size[deepest] / 1024);
}

int bthread_create(bthread **out, const char *name, void *(*start)(void *), void *arg, size_t stack_size,
                   int detached, int priority) {
  bthread_full *f = calloc(1, sizeof(*f));
  bthread *t = &f->t;
  t->magic = BTHREAD_MAGIC;
  t->start = start;
  t->arg = arg;
  t->park = -1;
  t->vm_capable = 1;
  f->state = detached ? ST_DETACHED : 0;
  strncpy(t->name, name ? name : "bthread", sizeof(t->name) - 1);
  if (stack_size == 0)
    stack_size = DEFAULT_STACK;
  if (stack_size < MIN_STACK)
    stack_size = MIN_STACK;
  stack_size = (stack_size + 0xfff) & ~0xfff;

  SceUID uid = sceKernelCreateThread(t->name, bthread_entry, priority ? priority : 0x10000100, stack_size,
                                     THREAD_ATTR_VM, 0, NULL);
  /* little user memory is kept back once the heaps have grown; caches give
   * theirs up for a stack */
  extern int alloc_reclaim(size_t want);
  if (uid < 0 && alloc_reclaim(stack_size))
    uid = sceKernelCreateThread(t->name, bthread_entry, priority ? priority : 0x10000100, stack_size, THREAD_ATTR_VM,
                                0, NULL);
  if (uid < 0) {
    LOG("[pthread] sceKernelCreateThread(%s, 0x%X) failed: 0x%08X", t->name, stack_size, uid);
    free(f);
    return 11;
  }
  t->uid = uid;
  /* reuse the slot of a thread that is gone */
  sceKernelLockLwMutex(&stacks_lock, 1, NULL);
  int slot = 0;
  while (slot < bthread_nuids && slot < 256 && bthread_uids[slot] != -1)
    slot++;
  if (slot < 256)
    bthread_uids[slot] = uid;
  if (slot >= bthread_nuids)
    bthread_nuids = slot + 1;
  sceKernelUnlockLwMutex(&stacks_lock, 1);
  *out = t;
  int r = sceKernelStartThread(uid, sizeof(t), &t);
  if (r < 0) {
    LOG("[pthread] sceKernelStartThread failed: 0x%08X", r);
    forget_stack(uid);
    sceKernelDeleteThread(uid);
    free(f);
    return 11;
  }
  return 0;
}

int bthread_join(bthread *t, void **ret) {
  if (!t || t->magic != BTHREAD_MAGIC || t->adopted)
    return B_ESRCH;
  int r = sceKernelWaitThreadEnd(t->uid, NULL, NULL);
  if (r < 0 && r != (int)SCE_KERNEL_ERROR_DORMANT)
    LOG("[pthread] join %s: 0x%08X", t->name, r);
  forget_stack(t->uid);
  sceKernelDeleteThread(t->uid);
  if (ret)
    *ret = t->retval;
  bthread_free(t);
  return 0;
}

/* bionic pthread_attr_t */
typedef struct {
  uint32_t flags;
  void *stack_base;
  size_t stack_size;
  size_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;
} battr_t;

#define BATTR_DETACHED 1

int b_pthread_attr_init(battr_t *a) {
  memset(a, 0, sizeof(*a));
  a->stack_size = 1024 * 1024;
  a->guard_size = 4096;
  return 0;
}

int b_pthread_attr_destroy(battr_t *a) {
  memset(a, 0x42, sizeof(*a));
  return 0;
}

int b_pthread_attr_setdetachstate(battr_t *a, int state) {
  if (state)
    a->flags |= BATTR_DETACHED;
  else
    a->flags &= ~BATTR_DETACHED;
  return 0;
}

int b_pthread_attr_setstacksize(battr_t *a, size_t size) {
  a->stack_size = size;
  return 0;
}

int b_pthread_attr_setschedparam(battr_t *a, const int *param) {
  a->sched_priority = *param;
  return 0;
}

int b_pthread_attr_setschedpolicy(battr_t *a, int policy) {
  a->sched_policy = policy;
  return 0;
}

int b_pthread_attr_getstack(const battr_t *a, void **addr, size_t *size) {
  *addr = a->stack_base;
  *size = a->stack_size;
  return 0;
}

int b_pthread_create(void **thread, const void *attr_, void *(*start)(void *), void *arg) {
  const battr_t *attr = attr_;
  size_t stack = attr ? attr->stack_size : 0;
  int detached = attr ? (attr->flags & BATTR_DETACHED) != 0 : 0;
  bthread *t;
  int r = bthread_create(&t, NULL, start, arg, stack, detached, 0);
  if (r == 0)
    *thread = t;
  DLOG("[pthread] create %p start=%p stack=0x%X detached=%d -> %d", r ? NULL : t, start, stack, detached, r);
  return r;
}

int b_pthread_getattr_np(bthread *t, battr_t *a) {
  b_pthread_attr_init(a);
  if (!t || t->magic != BTHREAD_MAGIC)
    return B_ESRCH;
  if (!t->stack_lo)
    fill_stack_bounds(t);
  a->stack_base = (void *)t->stack_lo;
  a->stack_size = t->stack_hi - t->stack_lo;
  return 0;
}

int b_pthread_detach(bthread *t) {
  if (!t || t->magic != BTHREAD_MAGIC)
    return B_ESRCH;
  if (t->adopted)
    return 0;
  bthread_full *f = (bthread_full *)t;
  int old = __atomic_fetch_or(&f->state, ST_DETACHED, __ATOMIC_ACQ_REL);
  if (old & ST_EXITED) {
    sceKernelWaitThreadEnd(t->uid, NULL, NULL);
    forget_stack(t->uid);
    sceKernelDeleteThread(t->uid);
    bthread_free(t);
  }
  return 0;
}

int b_pthread_join(bthread *t, void **ret) {
  return bthread_join(t, ret);
}

int b_pthread_equal(void *a, void *b) {
  return a == b;
}

void b_pthread_exit(void *ret) {
  bthread *t = bthread_self();
  if (t->adopted)
    fatal("[pthread] pthread_exit on adopted thread %s", t->name);
  bthread_finish(t, ret);
}

void *b_pthread_self(void) {
  return bthread_self();
}

int b_pthread_setname_np(bthread *t, const char *name) {
  if (t && t->magic == BTHREAD_MAGIC && name)
    strncpy(t->name, name, sizeof(t->name) - 1);
  return 0;
}

int b_pthread_getschedparam(bthread *t, int *policy, int *param) {
  if (policy)
    *policy = 0;
  if (param)
    *param = 0;
  return 0;
}

int b_pthread_setschedparam(bthread *t, int policy, const int *param) {
  return 0;
}

int b_sched_get_priority_max(int policy) {
  return policy == 0 ? 0 : 99;
}

int b_sched_get_priority_min(int policy) {
  return policy == 0 ? 0 : 1;
}

int b_sched_yield(void) {
  sceKernelDelayThread(0);
  return 0;
}

/* ---------- mutex ---------- */

#define BMUTEX_MAGIC 0x424d5458

typedef struct {
  SceKernelLwMutexWork lw; /* must stay first: 8-byte aligned by malloc */
  uint32_t magic;
  int type;
} bmutex;

#define MUTEX_NORMAL 0
#define MUTEX_RECURSIVE 1
#define MUTEX_ERRORCHECK 2

static bmutex *bmutex_new(int type) {
  bmutex *m = memalign(8, sizeof(*m));
  memset(m, 0, sizeof(*m));
  m->magic = BMUTEX_MAGIC;
  m->type = type;
  int r = sceKernelCreateLwMutex(&m->lw, "bmutex", type == MUTEX_RECURSIVE ? SCE_KERNEL_MUTEX_ATTR_RECURSIVE : 0, 0, NULL);
  if (r < 0)
    fatal("[pthread] sceKernelCreateLwMutex failed: 0x%08X", r);
  return m;
}

static void bmutex_free(bmutex *m) {
  sceKernelDeleteLwMutex(&m->lw);
  m->magic = 0;
  free(m);
}

static inline bmutex *mutex_get(volatile uint32_t *word) {
  uint32_t v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
  if (v >= 0x10000)
    return (bmutex *)v;
  int type = v == 0x4000 ? MUTEX_RECURSIVE : v == 0x8000 ? MUTEX_ERRORCHECK : MUTEX_NORMAL;
  bmutex *m = bmutex_new(type);
  if (__atomic_compare_exchange_n(word, &v, (uint32_t)m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return m;
  bmutex_free(m);
  return (bmutex *)__atomic_load_n(word, __ATOMIC_ACQUIRE);
}

int b_pthread_mutexattr_init(int *attr) {
  *attr = 0;
  return 0;
}

int b_pthread_mutexattr_destroy(int *attr) {
  *attr = -1;
  return 0;
}

int b_pthread_mutexattr_settype(int *attr, int type) {
  if (type < 0 || type > 2)
    return B_EINVAL;
  *attr = (*attr & ~3) | type;
  return 0;
}

int b_pthread_mutex_init(volatile uint32_t *word, const int *attr) {
  int type = attr ? (*attr & 3) : MUTEX_NORMAL;
  *word = (uint32_t)bmutex_new(type);
  return 0;
}

int b_pthread_mutex_destroy(volatile uint32_t *word) {
  uint32_t v = *word;
  if (v >= 0x10000) {
    bmutex *m = (bmutex *)v;
    if (m->magic == BMUTEX_MAGIC)
      bmutex_free(m);
  }
  *word = 0;
  return 0;
}

int b_pthread_mutex_lock(volatile uint32_t *word) {
  bmutex *m = mutex_get(word);
  int r = sceKernelLockLwMutex(&m->lw, 1, NULL);
  if (r < 0) {
    if (m->type == MUTEX_ERRORCHECK)
      return B_EDEADLK;
    fatal("[pthread] lock %p failed 0x%08X (type %d) lr=%p", word, r, m->type, __builtin_return_address(0));
  }
  return 0;
}

int b_pthread_mutex_trylock(volatile uint32_t *word) {
  bmutex *m = mutex_get(word);
  return sceKernelTryLockLwMutex(&m->lw, 1) < 0 ? B_EBUSY : 0;
}

int b_pthread_mutex_unlock(volatile uint32_t *word) {
  bmutex *m = mutex_get(word);
  return sceKernelUnlockLwMutex(&m->lw, 1) < 0 ? B_EPERM : 0;
}

/* ---------- condition variables ---------- */

typedef struct {
  SceKernelLwMutexWork lock;
  uint32_t magic;
  bthread *head, *tail;
} bcond;

#define BCOND_MAGIC 0x42434e44

static bcond *bcond_new(void) {
  bcond *c = memalign(8, sizeof(*c));
  memset(c, 0, sizeof(*c));
  c->magic = BCOND_MAGIC;
  sceKernelCreateLwMutex(&c->lock, "bcond", 0, 0, NULL);
  return c;
}

static inline bcond *cond_get(volatile uint32_t *word) {
  uint32_t v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
  if (v >= 0x10000)
    return (bcond *)v;
  bcond *c = bcond_new();
  if (__atomic_compare_exchange_n(word, &v, (uint32_t)c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return c;
  sceKernelDeleteLwMutex(&c->lock);
  free(c);
  return (bcond *)__atomic_load_n(word, __ATOMIC_ACQUIRE);
}

int b_pthread_cond_init(volatile uint32_t *word, const void *attr) {
  *word = (uint32_t)bcond_new();
  return 0;
}

int b_pthread_cond_destroy(volatile uint32_t *word) {
  uint32_t v = *word;
  if (v >= 0x10000) {
    bcond *c = (bcond *)v;
    if (c->magic == BCOND_MAGIC) {
      if (c->head)
        LOG("[pthread] destroying cond %p with waiters", word);
      sceKernelDeleteLwMutex(&c->lock);
      c->magic = 0;
      free(c);
    }
  }
  *word = 0;
  return 0;
}

static int cond_wait_us(volatile uint32_t *cword, volatile uint32_t *mword, int64_t timeout_us) {
  bcond *c = cond_get(cword);
  bthread *self = bthread_self();
  if (self->park < 0)
    self->park = sceKernelCreateSema("bpark", 0, 0, 0x7fffffff, NULL);

  sceKernelLockLwMutex(&c->lock, 1, NULL);
  self->cond_signaled = 0;
  self->cond_next = NULL;
  if (c->tail)
    c->tail->cond_next = self;
  else
    c->head = self;
  c->tail = self;
  sceKernelUnlockLwMutex(&c->lock, 1);

  b_pthread_mutex_unlock(mword);

  int r;
  if (timeout_us < 0) {
    r = sceKernelWaitSema(self->park, 1, NULL);
  } else {
    SceUInt t = timeout_us > 0x7fffffff ? 0x7fffffff : (SceUInt)timeout_us;
    r = sceKernelWaitSema(self->park, 1, &t);
  }

  int timed_out = 0;
  sceKernelLockLwMutex(&c->lock, 1, NULL);
  if (!self->cond_signaled) {
    /* still queued: remove ourselves */
    bthread *prev = NULL;
    for (bthread *w = c->head; w; prev = w, w = w->cond_next) {
      if (w == self) {
        if (prev)
          prev->cond_next = w->cond_next;
        else
          c->head = w->cond_next;
        if (c->tail == w)
          c->tail = prev;
        break;
      }
    }
    timed_out = 1;
  } else if (r < 0) {
    /* signaled while timing out: consume the pending token */
    sceKernelPollSema(self->park, 1);
  }
  sceKernelUnlockLwMutex(&c->lock, 1);

  b_pthread_mutex_lock(mword);
  return timed_out ? B_ETIMEDOUT : 0;
}

int b_pthread_cond_wait(volatile uint32_t *c, volatile uint32_t *m) {
  return cond_wait_us(c, m, -1);
}

int b_pthread_cond_timedwait(volatile uint32_t *c, volatile uint32_t *m, const struct bionic_timespec *abstime) {
  int64_t now = bionic_realtime_us();
  int64_t target = (int64_t)abstime->tv_sec * 1000000 + abstime->tv_nsec / 1000;
  int64_t rel = target - now;
  return cond_wait_us(c, m, rel < 0 ? 0 : rel);
}

int b_pthread_cond_timedwait_relative_np(volatile uint32_t *c, volatile uint32_t *m, const struct bionic_timespec *rel) {
  int64_t us = (int64_t)rel->tv_sec * 1000000 + rel->tv_nsec / 1000;
  return cond_wait_us(c, m, us < 0 ? 0 : us);
}

int b_pthread_cond_signal(volatile uint32_t *word) {
  bcond *c = cond_get(word);
  sceKernelLockLwMutex(&c->lock, 1, NULL);
  bthread *w = c->head;
  if (w) {
    c->head = w->cond_next;
    if (!c->head)
      c->tail = NULL;
    w->cond_signaled = 1;
    sceKernelSignalSema(w->park, 1);
  }
  sceKernelUnlockLwMutex(&c->lock, 1);
  return 0;
}

int b_pthread_cond_broadcast(volatile uint32_t *word) {
  bcond *c = cond_get(word);
  sceKernelLockLwMutex(&c->lock, 1, NULL);
  bthread *w = c->head;
  c->head = c->tail = NULL;
  while (w) {
    bthread *n = w->cond_next;
    w->cond_signaled = 1;
    sceKernelSignalSema(w->park, 1);
    w = n;
  }
  sceKernelUnlockLwMutex(&c->lock, 1);
  return 0;
}

/* ---------- once ---------- */

int b_pthread_once(volatile int *once, void (*fn)(void)) {
  int v = __atomic_load_n(once, __ATOMIC_ACQUIRE);
  if (v == 2)
    return 0;
  int expected = 0;
  if (__atomic_compare_exchange_n(once, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    fn();
    __atomic_store_n(once, 2, __ATOMIC_RELEASE);
    return 0;
  }
  while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != 2)
    sceKernelDelayThread(100);
  return 0;
}

/* ---------- semaphores (bionic sem_t is one word) ---------- */

int b_sem_init(volatile int *sem, int pshared, unsigned value) {
  SceUID s = sceKernelCreateSema("bsem", 0, value, 0x7fffffff, NULL);
  if (s < 0) {
    LOG("[pthread] sem_init failed 0x%08X", s);
    bionic_set_errno(28);
    return -1;
  }
  *sem = s;
  return 0;
}

int b_sem_destroy(volatile int *sem) {
  if (*sem > 0)
    sceKernelDeleteSema(*sem);
  *sem = 0;
  return 0;
}

int b_sem_post(volatile int *sem) {
  int r = sceKernelSignalSema(*sem, 1);
  if (r < 0) {
    bionic_set_errno(B_EINVAL);
    return -1;
  }
  return 0;
}

int b_sem_wait(volatile int *sem) {
  int r = sceKernelWaitSema(*sem, 1, NULL);
  if (r < 0) {
    bionic_set_errno(B_EINVAL);
    return -1;
  }
  return 0;
}

int b_sem_getvalue(volatile int *sem, int *value) {
  SceKernelSemaInfo info;
  memset(&info, 0, sizeof(info));
  info.size = sizeof(info);
  if (sceKernelGetSemaInfo(*sem, &info) < 0) {
    bionic_set_errno(B_EINVAL);
    return -1;
  }
  *value = info.currentCount;
  return 0;
}

void bthread_init(void) {
  sceKernelCreateLwMutex(&key_lock, "bkeys", 0, 0, NULL);
  sceKernelCreateLwMutex(&stacks_lock, "bstacks", 0, 0, NULL);
  bthread_check_tls();
}

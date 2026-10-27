#ifndef PG3D_BTHREAD_H
#define PG3D_BTHREAD_H

#include <stddef.h>
#include <stdint.h>
#include <psp2/types.h>

#define BIONIC_KEYS_MAX 128
/* Kernel TLS slot (sceKernelGetTLSAddr index) holding the current bthread. */
#define BTHREAD_TLS_SLOT 0x8E

typedef struct bthread {
  uint32_t magic;
  SceUID uid;
  void *(*start)(void *);
  void *arg;
  void *retval;
  int detached;
  int vm_capable;      /* created with the VM attribute: GC may suspend it */
  int adopted;         /* not created by bthread_create */
  uintptr_t stack_lo, stack_hi;
  SceUID park;         /* per-thread semaphore for condition waits */
  struct bthread *cond_next;
  volatile int cond_signaled;
  void *jni_exception; /* pending Java exception (jobj*) */
  void *jni_frames;    /* JNI local frames (jni/jvm.c) */
  void *tls[BIONIC_KEYS_MAX];
  char name[32];
} bthread;

bthread *bthread_self(void);
int bthread_create(bthread **out, const char *name, void *(*start)(void *), void *arg, size_t stack_size,
                   int detached, int priority);
int bthread_join(bthread *t, void **ret);
/* Watchdog tick: measures one thread's stack use, logs when one gets tight */
void bthread_check_stacks(void);

/* Bionic ABI entry points registered in the import table. */
int b_pthread_create(void **thread, const void *attr, void *(*start)(void *), void *arg);

#endif

/*
 * Signals. Vita has none; the only signal traffic that matters is Boehm GC's
 * stop-the-world inside libil2cpp, which sends tkill(kernel_id, SIG_SUSPEND)
 * and tkill(kernel_id, SIG_RESTART). We suspend/resume the target thread with
 * the VM thread API and do what GC_suspend_handler would have done:
 * spill the registers below SP, publish stack_ptr and last_stop_count in the
 * thread's GC_Thread record, and post the ack semaphore.
 */
#include <string.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/bionic.h"
#include "bionic/bthread.h"
#include "game/gamebin.h"
#include "loader/so_loader.h"
#include "util/log.h"

typedef struct {
  SceSize size;
  SceUInt32 cpsr;
  SceUInt32 reg[16];
  SceUInt32 tpidrurw, teehbr, sb, st;
} vm_cpu_ctx;

typedef struct {
  SceSize size;
  SceUInt32 fpscr;
  SceUInt32 reg[64];
} vm_vfp_ctx;

int sceKernelSuspendThreadForVM(SceUID tid);
int sceKernelResumeThreadForVM(SceUID tid);
int sceKernelGetThreadContextForVM(SceUID tid, vm_cpu_ctx *cpu, vm_vfp_ctx *vfp);

#define B_ESRCH 3
#define B_EINVAL 22
#define B_EINTR 4

/* bionic struct sigaction (arm32) */
typedef struct {
  void *handler;
  uint32_t mask;
  int flags;
  void *restorer;
} bsigaction;

static bsigaction handlers[65];
static int gc_sig_suspend = -1, gc_sig_restart = -1;

int b_sigaction(int sig, const bsigaction *act, bsigaction *old) {
  if (sig <= 0 || sig > 64) {
    bionic_set_errno(B_EINVAL);
    return -1;
  }
  if (old)
    *old = handlers[sig];
  if (act) {
    handlers[sig] = *act;
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    so_module *m = so_by_addr(lr);
    if (m && gamebin_il2cpp && m == gamebin_il2cpp) {
      uintptr_t off = lr - m->base;
      if (off == GB_IL2CPP_SIGACTION_SUSPEND_RET)
        gc_sig_suspend = sig;
      else if (off == GB_IL2CPP_SIGACTION_RESTART_RET)
        gc_sig_restart = sig;
    }
    DLOG("[signal] sigaction(%d, handler=%p flags=0x%X) from %s+0x%X", sig, act->handler, act->flags,
         m ? m->name : "?", m ? lr - m->base : lr);
  }
  return 0;
}

void *b_bsd_signal(int sig, void *handler) {
  if (sig <= 0 || sig > 64)
    return (void *)-1;
  void *old = handlers[sig].handler;
  handlers[sig].handler = handler;
  return old;
}

int b_gettid(void) {
  return sceKernelGetThreadId();
}

/* GC_Thread record layout in this libil2cpp (bdwgc GC_Thread_Rep, Android build) */
typedef struct gc_thread {
  struct gc_thread *next;
  uint32_t id;
  int32_t kernel_id;
  uint32_t last_stop_count;
  uintptr_t stack_ptr;
  uint8_t flags;
  uint8_t thread_blocked;
} gc_thread;

static gc_thread *gc_find(int32_t tid) {
  gc_thread **table = (gc_thread **)(gamebin_il2cpp->base + GB_IL2CPP_GC_THREADS);
  for (int i = 0; i < 256; i++)
    for (gc_thread *p = table[i]; p; p = p->next)
      if (p->kernel_id == tid)
        return p;
  return NULL;
}

static volatile int gc_nsuspend, gc_nresume;
static volatile int gc_stopped; /* threads currently held for the GC */

/* While the GC has threads stopped, one may hold a heap lock */
int gc_world_stopped(void) {
  return __atomic_load_n(&gc_stopped, __ATOMIC_SEQ_CST) > 0;
}

static int gc_suspend(SceUID tid) {
  __atomic_fetch_add(&gc_nsuspend, 1, __ATOMIC_SEQ_CST);
  int r = sceKernelSuspendThreadForVM(tid);
  if (r < 0) {
    LOG("[gc] suspend 0x%X failed: 0x%08X", tid, r);
    return B_ESRCH;
  }
  vm_cpu_ctx cpu;
  vm_vfp_ctx vfp;
  memset(&cpu, 0, sizeof(cpu));
  memset(&vfp, 0, sizeof(vfp));
  cpu.size = sizeof(cpu);
  vfp.size = sizeof(vfp);
  r = sceKernelGetThreadContextForVM(tid, &cpu, &vfp);
  if (r < 0) {
    LOG("[gc] getcontext 0x%X failed: 0x%08X", tid, r);
    sceKernelResumeThreadForVM(tid);
    return B_ESRCH;
  }
  gc_thread *me = gc_find(tid);
  if (!me) {
    LOG("[gc] thread 0x%X not in GC_threads", tid);
    sceKernelResumeThreadForVM(tid);
    return B_ESRCH;
  }
  /* spill integer + VFP registers below the interrupted SP so the
   * conservative stack scan sees them */
  uintptr_t sp = cpu.reg[13];
  uint32_t *frame = (uint32_t *)((sp - (16 + 64) * 4) & ~7u);
  memcpy(frame, cpu.reg, 16 * 4);
  memcpy(frame + 16, vfp.reg, 64 * 4);
  me->stack_ptr = (uintptr_t)frame;
  me->last_stop_count = *(volatile uint32_t *)(gamebin_il2cpp->base + GB_IL2CPP_GC_STOP_COUNT);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  extern int b_sem_post(volatile int *sem);
  __atomic_fetch_add(&gc_stopped, 1, __ATOMIC_SEQ_CST);
  b_sem_post((volatile int *)(gamebin_il2cpp->base + GB_IL2CPP_GC_SUSPEND_ACK_SEM));
  return 0;
}

static int gc_resume(SceUID tid) {
  __atomic_fetch_add(&gc_nresume, 1, __ATOMIC_SEQ_CST);
  int r = sceKernelResumeThreadForVM(tid);
  int held = __atomic_load_n(&gc_stopped, __ATOMIC_SEQ_CST);
  while (held > 0 && !__atomic_compare_exchange_n(&gc_stopped, &held, held - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    ;
  if (r < 0) {
    LOG("[gc] resume 0x%X failed: 0x%08X", tid, r);
    return B_ESRCH;
  }
  return 0;
}

int b_tkill(int tid, int sig) {
  int err;
  if (sig == gc_sig_suspend && sig > 0)
    err = gc_suspend(tid);
  else if (sig == gc_sig_restart && sig > 0)
    err = gc_resume(tid);
  else {
    LOG("[signal] tkill(0x%X, %d) ignored (lr=%p)", tid, sig, __builtin_return_address(0));
    err = 0;
  }
  if (err) {
    bionic_set_errno(err);
    return -1;
  }
  return 0;
}

int b_kill(int pid, int sig) {
  LOG("[signal] kill(%d, %d) lr=%p", pid, sig, __builtin_return_address(0));
  if (sig == 9 || sig == 15)
    fatal("[signal] process asked to kill itself");
  return 0;
}

int b_raise(int sig) {
  LOG("[signal] raise(%d) lr=%p", sig, __builtin_return_address(0));
  if (sig == 6 || sig == 11 || sig == 9)
    fatal("[signal] raise(%d)", sig);
  return 0;
}

int b_sigsuspend(const void *mask) {
  LOG("[signal] sigsuspend called unexpectedly lr=%p", __builtin_return_address(0));
  sceKernelDelayThread(1000);
  bionic_set_errno(B_EINTR);
  return -1;
}

static void log_addr(const char *what, uintptr_t a) {
  const char *mod, *sym;
  uintptr_t mb, sa;
  if (so_addr_info(a, &mod, &mb, &sym, &sa))
    LOG("[wd]   %s %08X %s+0x%X (%s+0x%X)", what, a, mod, a - mb, sym ? sym : "?", a - sa);
  else
    LOG("[wd]   %s %08X", what, a);
}

/* Debug: suspend a thread, copy PC/LR and code addresses found on its stack,
 * resume, then log (logging while it is suspended can deadlock on I/O). */
void thread_sample(SceUID tid, const char *tag) {
  vm_cpu_ctx cpu;
  vm_vfp_ctx vfp;
  uint32_t st[24];
  int n = 0;
  SceKernelThreadInfo ti;
  memset(&ti, 0, sizeof(ti));
  ti.size = sizeof(ti);
  if (sceKernelGetThreadInfo(tid, &ti) < 0)
    return;
  uintptr_t stack_top = (uintptr_t)ti.stack + ti.stackSize;
  if (sceKernelSuspendThreadForVM(tid) < 0)
    return;
  memset(&cpu, 0, sizeof(cpu));
  cpu.size = sizeof(cpu);
  vfp.size = sizeof(vfp);
  int ok = sceKernelGetThreadContextForVM(tid, &cpu, &vfp) >= 0;
  if (ok) {
    uint32_t *sp = (uint32_t *)cpu.reg[13];
    for (int i = 0; i < 1024 && n < 24 && (uintptr_t)&sp[i] < stack_top; i++) {
      so_module *m = so_by_addr(sp[i]);
      if (m && sp[i] >= m->text_start && sp[i] < m->text_end)
        st[n++] = sp[i];
    }
  }
  sceKernelResumeThreadForVM(tid);
  if (!ok)
    return;
  LOG("[wd] %s tid 0x%X status 0x%X sp %08X (thread_sample at %p) gc suspend/resume %d/%d", tag, tid, ti.status,
      cpu.reg[13], (void *)thread_sample, gc_nsuspend, gc_nresume);
  log_addr("pc", cpu.reg[15]);
  log_addr("lr", cpu.reg[14]);
  for (int i = 0; i < n; i++)
    log_addr("st", st[i]);
}

/* VM-attribute threads are suspended by the kernel when they fault instead of
 * taking the process down. Find one suspended outside a GC cycle, log it and
 * crash on purpose so a coredump with every thread is written. */
void thread_scan_faults(void) {
  extern SceUID bthread_uids[256];
  extern volatile int bthread_nuids;
  int n = bthread_nuids < 256 ? bthread_nuids : 256;
  for (int i = 0; i < n; i++) {
    SceKernelThreadInfo ti;
    memset(&ti, 0, sizeof(ti));
    ti.size = sizeof(ti);
    int before = gc_nsuspend;
    if (gc_nsuspend != gc_nresume || sceKernelGetThreadInfo(bthread_uids[i], &ti) < 0 || !(ti.status & 0x100))
      continue;
    sceKernelDelayThread(200 * 1000);
    memset(&ti, 0, sizeof(ti));
    ti.size = sizeof(ti);
    if (gc_nsuspend != before || gc_nsuspend != gc_nresume || sceKernelGetThreadInfo(bthread_uids[i], &ti) < 0 ||
        !(ti.status & 0x100))
      continue;
    vm_cpu_ctx cpu;
    vm_vfp_ctx vfp;
    memset(&cpu, 0, sizeof(cpu));
    cpu.size = sizeof(cpu);
    vfp.size = sizeof(vfp);
    sceKernelGetThreadContextForVM(bthread_uids[i], &cpu, &vfp);
    LOG("[fault] thread 0x%X (%s) faulted: pc %08X lr %08X sp %08X", bthread_uids[i], ti.name, cpu.reg[15], cpu.reg[14],
        cpu.reg[13]);
    for (int r = 0; r < 13; r += 4)
      LOG("[fault]   r%-2d %08X %08X %08X %08X", r, cpu.reg[r], cpu.reg[r + 1], cpu.reg[r + 2], r + 3 < 13 ? cpu.reg[r + 3] : 0);
    log_addr("pc", cpu.reg[15]);
    log_addr("lr", cpu.reg[14]);
    log_flush();
    *(volatile int *)0 = 0xDEAD; /* coredump */
  }
}

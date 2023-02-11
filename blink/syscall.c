/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2022 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "blink/ancillary.h"
#include "blink/assert.h"
#include "blink/bus.h"
#include "blink/case.h"
#include "blink/debug.h"
#include "blink/endian.h"
#include "blink/errno.h"
#include "blink/iovs.h"
#include "blink/limits.h"
#include "blink/linux.h"
#include "blink/loader.h"
#include "blink/lock.h"
#include "blink/log.h"
#include "blink/machine.h"
#include "blink/macros.h"
#include "blink/map.h"
#include "blink/overlays.h"
#include "blink/pml4t.h"
#include "blink/preadv.h"
#include "blink/random.h"
#include "blink/signal.h"
#include "blink/stats.h"
#include "blink/swap.h"
#include "blink/syscall.h"
#include "blink/timespec.h"
#include "blink/util.h"
#include "blink/xlat.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __HAIKU__
#include <OS.h>
#include <sys/sockio.h>
#endif

#ifdef __linux
#include <sched.h>
#include <sys/vfs.h>
#endif

#ifdef SO_LINGER_SEC
#define SO_LINGER_ SO_LINGER_SEC
#else
#define SO_LINGER_ SO_LINGER
#endif

#define SYSCALL0(magnum, func)                                \
  case magnum:                                                \
    ax = func(m);                                             \
    SYS_LOGF("%s() -> %s", #func, DescribeSyscallResult(ax)); \
    break

#define SYSCALL1(magnum, func)                                                \
  case magnum:                                                                \
    ax = func(m, di);                                                         \
    SYS_LOGF("%s(%#" PRIx64 ") -> %s", #func, di, DescribeSyscallResult(ax)); \
    break

#define SYSCALL2(magnum, func)                                      \
  case magnum:                                                      \
    ax = func(m, di, si);                                           \
    SYS_LOGF("%s(%#" PRIx64 ", %#" PRIx64 ") -> %s", #func, di, si, \
             DescribeSyscallResult(ax));                            \
    break

#define SYSCALL3(magnum, func)                                                \
  case magnum:                                                                \
    ax = func(m, di, si, dx);                                                 \
    SYS_LOGF("%s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ") -> %s", #func, di, \
             si, dx, DescribeSyscallResult(ax));                              \
    break

#define SYSCALL4(magnum, func)                                        \
  case magnum:                                                        \
    ax = func(m, di, si, dx, r0);                                     \
    SYS_LOGF("%s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 \
             ") -> %s",                                               \
             #func, di, si, dx, r0, DescribeSyscallResult(ax));       \
    break

#define SYSCALL5(magnum, func)                                        \
  case magnum:                                                        \
    ax = func(m, di, si, dx, r0, r8);                                 \
    SYS_LOGF("%s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 \
             ", %#" PRIx64 ") -> %s",                                 \
             #func, di, si, dx, r0, r8, DescribeSyscallResult(ax));   \
    break

#define SYSCALL6(magnum, func)                                          \
  case magnum:                                                          \
    ax = func(m, di, si, dx, r0, r8, r9);                               \
    SYS_LOGF("%s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64   \
             ", %#" PRIx64 ", %#" PRIx64 ") -> %s",                     \
             #func, di, si, dx, r0, r8, r9, DescribeSyscallResult(ax)); \
    break

char *g_blink_path;
bool FLAG_statistics;

// delegate to work around function pointer errors, b/c
// old musl toolchains using `int ioctl(int, int, ...)`
static int SystemIoctl(int fd, unsigned long request, ...) {
  va_list va;
  intptr_t arg;
  va_start(va, request);
  arg = va_arg(va, intptr_t);
  va_end(va);
  return ioctl(fd, request, (void *)arg);
}

#ifdef __EMSCRIPTEN__
// If this runs on the main thread, the browser is blocked until we return
// back to the main loop. Yield regularly when the process waits for some
// user input.

int em_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
  int ret = poll(fds, nfds, timeout);
  if (ret == 0) emscripten_sleep(50);
  return ret;
}

ssize_t em_readv(int fd, const struct iovec *iov, int iovcnt) {
  // Handle blocking reads by waiting for POLLIN
  if ((fcntl(fd, F_GETFL, 0) & O_NONBLOCK) == 0) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    while (em_poll(&pfd, 1, 50) == 0) {
    }
  }
  size_t ret = readv(fd, iov, iovcnt);
  if (ret == -1 && errno == EAGAIN) emscripten_sleep(50);
  return ret;
}
#endif

static int my_tcgetwinsize(int fd, struct winsize *ws) {
  return ioctl(fd, TIOCGWINSZ, (void *)ws);
}

static int my_tcsetwinsize(int fd, const struct winsize *ws) {
  return ioctl(fd, TIOCSWINSZ, (void *)ws);
}

const struct FdCb kFdCbHost = {
    .close = close,
#ifdef __EMSCRIPTEN__
    .readv = em_readv,
#else
    .readv = readv,
#endif
    .writev = writev,
#ifdef __EMSCRIPTEN__
    .poll = em_poll,
#else
    .poll = poll,
#endif
    .tcgetattr = tcgetattr,
    .tcsetattr = tcsetattr,
    .tcgetwinsize = my_tcgetwinsize,
    .tcsetwinsize = my_tcsetwinsize,
};

struct Fd *GetAndLockFd(struct Machine *m, int fildes) {
  struct Fd *fd;
  LOCK(&m->system->fds.lock);
  if ((fd = GetFd(&m->system->fds, fildes))) LockFd(fd);
  UNLOCK(&m->system->fds.lock);
  return fd;
}

int GetDirFildes(int fildes) {
  if (fildes == AT_FDCWD_LINUX) return AT_FDCWD;
  return fildes;
}

void SignalActor(struct Machine *mm) {
#ifdef __CYGWIN__
  // TODO: Why does JIT clobber %rbx on Cygwin?
  struct Machine *volatile m = mm;
#else
  struct Machine *m = mm;
#endif
  for (;;) {
#ifndef __CYGWIN__
    STATISTIC(++interps);
#endif
    ExecuteInstruction(m);
    if (m->restored) break;
    CheckForSignals(m);
  }
}

bool CheckInterrupt(struct Machine *m, bool restartable) {
  int sig, delivered;
  sigset_t unblock, oldmask;
  bool res, restart, issigsuspend;
  // an actual i/o call just received EINTR from the kernel
GetSome:
  // determine if there's any signals pending for our guest
  Put64(m->ax, -EINTR_LINUX);
  if ((sig = ConsumeSignal(m, &delivered, &restart))) {
    TerminateSignal(m, sig);
  }
  if (delivered) {
    if (m->sigdepth < kMaxSigDepth) {
      // we're officially calling a signal handler
      // run the signal handler code inside the i/o routine
      // it's very important that no locks are currently held
      // garbage may exist on the freelist for calls like sendmsg
      ++m->sigdepth;
      if ((issigsuspend = m->issigsuspend)) {
        m->issigsuspend = false;
        unassert(!sigemptyset(&unblock));
        unassert(!pthread_sigmask(SIG_BLOCK, &unblock, &oldmask));
      }
      m->restored = false;
      SignalActor(m);
      m->restored = false;
      if (issigsuspend) {
        unassert(!pthread_sigmask(SIG_SETMASK, &oldmask, 0));
        m->issigsuspend = true;
      }
      --m->sigdepth;
      if (restart && restartable) {
        // try to consume some more signals while we're here
        goto GetSome;
      } else {
        // let the i/o routine return eintr
        res = true;
      }
    } else {
      LOGF("exceeded max signal depth");
      res = true;
    }
  } else {
    // no signal is being delivered
    res = false;
  }
  m->interrupted = res;
  return res;
}

static struct Futex *FindFutex(struct Machine *m, i64 addr) {
  struct Dll *e;
  for (e = dll_first(g_bus->futexes.active); e;
       e = dll_next(g_bus->futexes.active, e)) {
    if (FUTEX_CONTAINER(e)->addr == addr) {
      return FUTEX_CONTAINER(e);
    }
  }
  return 0;
}

static int SysFutexWake(struct Machine *m, i64 uaddr, u32 count) {
  int rc;
  struct Futex *f;
  if (!count) return 0;
  LOCK(&g_bus->futexes.lock);
  if ((f = FindFutex(m, uaddr))) {
    LOCK(&f->lock);
  }
  UNLOCK(&g_bus->futexes.lock);
  if (f && f->waiters) {
    THR_LOGF("pid=%d tid=%d is waking %d waiters at address %#" PRIx64,
             m->system->pid, m->tid, f->waiters, uaddr);
    if (count == 1) {
      unassert(!pthread_cond_signal(&f->cond));
      rc = 1;
    } else {
      unassert(!pthread_cond_broadcast(&f->cond));
      rc = f->waiters;
    }
    UNLOCK(&f->lock);
  } else {
    if (f) UNLOCK(&f->lock);
    THR_LOGF("pid=%d tid=%d is waking no one at address %#" PRIx64,
             m->system->pid, m->tid, uaddr);
    rc = 0;
  }
  return rc;
}

static void ClearChildTid(struct Machine *m) {
  atomic_int *ctid;
  if (m->ctid) {
    THR_LOGF("ClearChildTid(%#" PRIx64 ")", m->ctid);
    if ((ctid = (atomic_int *)LookupAddress(m, m->ctid))) {
      atomic_store_explicit(ctid, 0, memory_order_seq_cst);
    } else {
      THR_LOGF("invalid clear child tid address %#" PRIx64, m->ctid);
    }
  }
  SysFutexWake(m, m->ctid, INT_MAX);
}

_Noreturn void SysExitGroup(struct Machine *m, int rc) {
  THR_LOGF("pid=%d tid=%d SysExitGroup", m->system->pid, m->tid);
#ifndef NDEBUG
  if (FLAG_statistics) {
    PrintStats();
  }
#endif
  if (m->system->isfork) {
    THR_LOGF("calling _Exit(%d)", rc);
    _Exit(rc);
  } else {
    THR_LOGF("calling exit(%d)", rc);
    KillOtherThreads(m->system);
    FreeMachine(m);
    ShutdownJit();
    exit(rc);
  }
}

_Noreturn void SysExit(struct Machine *m, int rc) {
  THR_LOGF("pid=%d tid=%d SysExit", m->system->pid, m->tid);
  if (IsOrphan(m)) {
    SysExitGroup(m, rc);
  } else {
    ClearChildTid(m);
    FreeMachine(m);
    pthread_exit(0);
  }
}

static int Fork(struct Machine *m, u64 flags, u64 stack, u64 ctid) {
  int pid, newpid = 0;
  atomic_int *ctid_ptr;
  unassert(!m->path.jb);
  if ((flags & CLONE_CHILD_SETTID_LINUX) &&
      ((ctid & (sizeof(int) - 1)) ||
       !(ctid_ptr = (atomic_int *)LookupAddress(m, ctid)))) {
    LOGF("bad clone() ptid / ctid pointers: %#" PRIx64, flags);
    return efault();
  }
  // NOTES ON LOCKING HIERARCHY
  // exec_lock must come before sig_lock (see dup3)
  // exec_lock must come before fds.lock (see dup3)
  // exec_lock must come before fds.lock (see execve)
  // mmap_lock must come before fds.lock (see GetOflags)
  LOCK(&m->system->exec_lock);
  LOCK(&m->system->sig_lock);
  LOCK(&m->system->mmap_lock);
  LOCK(&m->system->fds.lock);
  LOCK(&m->system->machines_lock);
#if !CAN_PSHARE
  LOCK(&g_bus->futexes.lock);
#endif
  pid = fork();
#ifdef __HAIKU__
  // haiku wipes tls after fork() in child
  // https://dev.haiku-os.org/ticket/17896
  if (!pid) g_machine = m;
#endif
#if !CAN_PSHARE
  UNLOCK(&g_bus->futexes.lock);
#endif
  UNLOCK(&m->system->machines_lock);
  UNLOCK(&m->system->fds.lock);
  UNLOCK(&m->system->mmap_lock);
  UNLOCK(&m->system->sig_lock);
  UNLOCK(&m->system->exec_lock);
  if (!pid) {
    newpid = getpid();
    if (flags & CLONE_CHILD_SETTID_LINUX) {
      atomic_store_explicit(ctid_ptr, Little32(newpid), memory_order_release);
    }
    if (flags & CLONE_CHILD_CLEARTID_LINUX) {
      m->ctid = ctid;
    }
    if (stack) {
      Put64(m->sp, stack);
    }
#if !CAN_PSHARE
    InitBus();
#endif
    THR_LOGF("pid=%d tid=%d SysFork -> pid=%d tid=%d",  //
             m->system->pid, m->tid, newpid, newpid);
    m->tid = m->system->pid = newpid;
    m->system->isfork = true;
    RemoveOtherThreads(m->system);
#ifdef __CYGWIN__
    // Cygwin doesn't seem to properly set the PROT_EXEC
    // protection for JIT blocks after forking.
    FixJitProtection(&m->system->jit);
#endif
  }
  return pid;
}

static int SysFork(struct Machine *m) {
  return Fork(m, 0, 0, 0);
}

static int SysVfork(struct Machine *m) {
  // TODO: Parent should be stopped while child is running.
  return SysFork(m);
}

static void *OnSpawn(void *arg) {
  int rc;
  struct Machine *m = (struct Machine *)arg;
  THR_LOGF("pid=%d tid=%d OnSpawn", m->system->pid, m->tid);
  m->thread = pthread_self();
  g_machine = m;
  if (!(rc = sigsetjmp(m->onhalt, 1))) {
    unassert(!pthread_sigmask(SIG_SETMASK, &m->spawn_sigmask, 0));
    Actor(m);
  } else {
    LOGF("halting machine from thread: %d", rc);
    SysExitGroup(m, rc);
  }
}

static int SysSpawn(struct Machine *m, u64 flags, u64 stack, u64 ptid, u64 ctid,
                    u64 tls, u64 func) {
  int tid;
  int err;
  int ignored;
  int supported;
  int mandatory;
  pthread_t thread;
  sigset_t ss, oldss;
  pthread_attr_t attr;
  atomic_int *ptid_ptr;
  atomic_int *ctid_ptr;
  struct Machine *m2 = 0;
  THR_LOGF("pid=%d tid=%d SysSpawn", m->system->pid, m->tid);
  if ((flags & 255) != 0 && (flags & 255) != SIGCHLD_LINUX) {
    LOGF("unsupported clone() signal: %" PRId64, flags & 255);
    return einval();
  }
  flags &= ~255;
  supported = CLONE_THREAD_LINUX | CLONE_VM_LINUX | CLONE_FS_LINUX |
              CLONE_FILES_LINUX | CLONE_SIGHAND_LINUX | CLONE_SETTLS_LINUX |
              CLONE_PARENT_SETTID_LINUX | CLONE_CHILD_CLEARTID_LINUX |
              CLONE_CHILD_SETTID_LINUX | CLONE_SYSVSEM_LINUX;
  mandatory = CLONE_THREAD_LINUX | CLONE_VM_LINUX | CLONE_FS_LINUX |
              CLONE_FILES_LINUX | CLONE_SIGHAND_LINUX;
  ignored = CLONE_DETACHED_LINUX;
  flags &= ~ignored;
  if (flags & ~supported) {
    LOGF("unsupported clone() flags: %#" PRIx64, flags & ~supported);
    return einval();
  }
  if ((flags & mandatory) != mandatory) {
    LOGF("missing mandatory clone() thread flags: %#" PRIx64
         " out of %#" PRIx64,
         (flags & mandatory) ^ mandatory, flags);
    return einval();
  }
  if (((flags & CLONE_PARENT_SETTID_LINUX) &&
       ((ptid & (sizeof(int) - 1)) ||
        !(ptid_ptr = (atomic_int *)LookupAddress(m, ptid)))) ||
      ((flags & CLONE_CHILD_SETTID_LINUX) &&
       ((ctid & (sizeof(int) - 1)) ||
        !(ctid_ptr = (atomic_int *)LookupAddress(m, ctid))))) {
    LOGF("bad clone() ptid / ctid pointers: %#" PRIx64, flags);
    return efault();
  }
  if (!(m2 = NewMachine(m->system, m))) {
    return eagain();
  }
  sigfillset(&ss);
  unassert(!pthread_sigmask(SIG_SETMASK, &ss, &oldss));
  tid = m2->tid;
  if (flags & CLONE_SETTLS_LINUX) {
    m2->fs.base = tls;
  }
  if (flags & CLONE_CHILD_CLEARTID_LINUX) {
    m2->ctid = ctid;
  }
  if (flags & CLONE_CHILD_SETTID_LINUX) {
    atomic_store_explicit(ctid_ptr, Little32(tid), memory_order_release);
  }
  Put64(m2->ax, 0);
  Put64(m2->sp, stack);
  m2->spawn_sigmask = oldss;
  unassert(!pthread_attr_init(&attr));
  unassert(!pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));
  err = pthread_create(&thread, &attr, OnSpawn, m2);
  unassert(!pthread_attr_destroy(&attr));
  if (err) {
    FreeMachine(m2);
    unassert(!pthread_sigmask(SIG_SETMASK, &oldss, 0));
    return eagain();
  }
  if (flags & CLONE_PARENT_SETTID_LINUX) {
    atomic_store_explicit(ptid_ptr, Little32(tid), memory_order_release);
  }
  unassert(!pthread_sigmask(SIG_SETMASK, &oldss, 0));
  return tid;
}

static bool IsForkOrVfork(u64 flags) {
  u64 supported = CLONE_CHILD_SETTID_LINUX | CLONE_CHILD_CLEARTID_LINUX;
  return (flags & ~supported) == SIGCHLD_LINUX ||
         (flags & ~supported) ==
             (CLONE_VM_LINUX | CLONE_VFORK_LINUX | SIGCHLD_LINUX);
}

static int SysClone(struct Machine *m, u64 flags, u64 stack, u64 ptid, u64 ctid,
                    u64 tls, u64 func) {
  if (IsForkOrVfork(flags)) {
    return Fork(m, flags, stack, ctid);
  } else {
    return SysSpawn(m, flags, stack, ptid, ctid, tls, func);
  }
}

static struct Futex *NewFutex(i64 addr) {
  struct Dll *e;
  struct Futex *f;
  if (!(e = dll_first(g_bus->futexes.free))) {
    LOG_ONCE(LOGF("ran out of futexes"));
    enomem();
    return 0;
  }
  dll_remove(&g_bus->futexes.free, e);
  f = FUTEX_CONTAINER(e);
  f->waiters = 1;
  f->addr = addr;
  return f;
}

static void FreeFutex(struct Futex *f) {
  dll_make_first(&g_bus->futexes.free, &f->elem);
}

static int SysFutexWait(struct Machine *m,  //
                        i64 uaddr,          //
                        i32 op,             //
                        u32 expect,         //
                        i64 timeout_addr) {
  int rc;
  u8 *mem;
  struct Futex *f;
  const struct timespec_linux *gtimeout;
  struct timespec now, tick, timeout, deadline;
  now = tick = GetTime();
  if (timeout_addr) {
    if (!(gtimeout = (const struct timespec_linux *)SchlepR(
              m, timeout_addr, sizeof(*gtimeout)))) {
      return -1;
    }
    timeout.tv_sec = Read64(gtimeout->sec);
    timeout.tv_nsec = Read64(gtimeout->nsec);
    if (!(0 <= timeout.tv_nsec && timeout.tv_nsec < 1000000000)) {
      return einval();
    }
    deadline = AddTime(now, timeout);
  } else {
    deadline = GetMaxTime();
  }
  if (!(mem = GetAddress(m, uaddr))) return efault();
  LOCK(&g_bus->futexes.lock);
  if (Load32(mem) != expect) {
    UNLOCK(&g_bus->futexes.lock);
    return eagain();
  }
  if ((f = FindFutex(m, uaddr))) {
    LOCK(&f->lock);
    ++f->waiters;
    UNLOCK(&f->lock);
  }
  if (!f) {
    if ((f = NewFutex(uaddr))) {
      dll_make_first(&g_bus->futexes.active, &f->elem);
    } else {
      UNLOCK(&g_bus->futexes.lock);
      return -1;
    }
  }
  UNLOCK(&g_bus->futexes.lock);
  THR_LOGF("pid=%d tid=%d is waiting at address %#" PRIx64, m->system->pid,
           m->tid, uaddr);
  do {
    if (m->killed) {
      rc = EAGAIN;
      break;
    }
    if (CheckInterrupt(m, true)) {
      rc = EINTR;
      break;
    }
    if (!(mem = GetAddress(m, uaddr))) {
      rc = EFAULT;
      break;
    }
    LOCK(&f->lock);
    if (Load32(mem) != expect) {
      rc = 0;
    } else {
      tick = AddTime(tick, FromMilliseconds(kPollingMs));
      if (CompareTime(tick, deadline) > 0) tick = deadline;
      rc = pthread_cond_timedwait(&f->cond, &f->lock, &tick);
      if (rc == ETIMEDOUT) {
        THR_LOGF("futex wait timed out");
      } else {
        THR_LOGF("futex wait returned %s", DescribeHostErrno(rc));
      }
    }
    UNLOCK(&f->lock);
  } while (rc == ETIMEDOUT && CompareTime(tick, deadline) < 0);
  LOCK(&g_bus->futexes.lock);
  LOCK(&f->lock);
  if (!--f->waiters) {
    dll_remove(&g_bus->futexes.active, &f->elem);
    UNLOCK(&f->lock);
    FreeFutex(f);
    UNLOCK(&g_bus->futexes.lock);
  } else {
    UNLOCK(&f->lock);
    UNLOCK(&g_bus->futexes.lock);
  }
  if (rc) {
    errno = rc;
    rc = -1;
  }
  return rc;
}

static int SysFutex(struct Machine *m,  //
                    i64 uaddr,          //
                    i32 op,             //
                    u32 val,            //
                    i64 timeout_addr,   //
                    i64 uaddr2,         //
                    u32 val3) {
  if (uaddr & 3) return efault();
  op &= ~FUTEX_PRIVATE_FLAG_LINUX;
  switch (op) {
    case FUTEX_WAIT_LINUX:
      return SysFutexWait(m, uaddr, op, val, timeout_addr);
    case FUTEX_WAKE_LINUX:
      return SysFutexWake(m, uaddr, val);
    case FUTEX_WAIT_BITSET_LINUX:
    case FUTEX_WAIT_BITSET_LINUX | FUTEX_CLOCK_REALTIME_LINUX:
      // will be supported soon
      // avoid logging when cosmo feature checks this
      return einval();
    default:
      LOGF("unsupported futex op %#x", op);
      return einval();
  }
}

static void UnlockRobustFutex(struct Machine *m, u64 futex_addr,
                              bool ispending) {
  int owner;
  u32 value, replace;
  _Atomic(u32) *futex;
  if (futex_addr & 3) {
    LOGF("robust futex isn't aligned");
    return;
  }
  if (!(futex = (_Atomic(u32) *)SchlepR(m, futex_addr, 4))) {
    LOGF("encountered efault in robust futex list");
    return;
  }
  for (value = atomic_load_explicit(futex, memory_order_acquire);;) {
    owner = value & FUTEX_TID_MASK_LINUX;
    if (ispending && !owner) {
      THR_LOGF("unlocking pending ownerless futex");
      SysFutexWake(m, futex_addr, 1);
      return;
    }
    if (owner && owner != m->tid) {
      THR_LOGF("robust futex 0x%08" PRIx32
               " was owned by %d but we're tid=%d pid=%d",
               value, owner, m->tid, m->system->pid);
      return;
    }
    replace = FUTEX_OWNER_DIED_LINUX | (value & FUTEX_WAITERS_LINUX);
    if (atomic_compare_exchange_weak_explicit(futex, &value, replace,
                                              memory_order_release,
                                              memory_order_acquire)) {
      THR_LOGF("successfully unlocked robust futex");
      if (value & FUTEX_WAITERS_LINUX) {
        THR_LOGF("waking robust futex waiters");
        SysFutexWake(m, futex_addr, 1);
      }
      return;
    } else {
      THR_LOGF("robust futex cas failed");
    }
  }
}

void UnlockRobustFutexes(struct Machine *m) {
  if (1) return;  // TODO: Figure out how these work.
  int limit = 1000;
  bool once = false;
  u64 list, item, pending;
  struct robust_list_linux *data;
  if (!(item = list = m->robust_list)) return;
  m->robust_list = 0;
  pending = 0;
  do {
    if (!(data = (struct robust_list_linux *)SchlepR(m, item, sizeof(*data)))) {
      LOGF("encountered efault in robust futex list");
      break;
    }
    THR_LOGF("unlocking robust futex %#" PRIx64 " {.next=%#" PRIx64
             " .offset=%" PRId64 " .pending=%#" PRIx64 "}",
             item, Read64(data->next), Read64(data->offset),
             Read64(data->pending));
    if (!once) {
      pending = Read64(data->pending);
      once = true;
    }
    if (!--limit) {
      LOGF("encountered cycle or limit in robust futex list");
      break;
    }
    if (item != pending) {
      UnlockRobustFutex(m, item + Read64(data->offset), false);
    }
    item = Read64(data->next);
  } while (item != list);
  if (!pending) return;
  if (!(data =
            (struct robust_list_linux *)SchlepR(m, pending, sizeof(*data)))) {
    LOGF("encountered efault in robust futex list");
    return;
  }
  THR_LOGF("unlocking pending robust futex %#" PRIx64 " {.next=%#" PRIx64
           " .offset=%" PRId64 " .pending=%#" PRIx64 "}",
           item, Read64(data->next), Read64(data->offset),
           Read64(data->pending));
  UnlockRobustFutex(m, pending + Read64(data->offset), true);
}

static i32 ReturnRobustList(struct Machine *m, i64 head_ptr_addr,
                            i64 len_ptr_addr) {
  u8 buf[8];
  Write64(buf, m->robust_list);
  CopyToUserWrite(m, head_ptr_addr, buf, sizeof(buf));
  Write64(buf, sizeof(struct robust_list_linux));
  CopyToUserWrite(m, len_ptr_addr, buf, sizeof(buf));
  return 0;
}

static i32 SysGetRobustList(struct Machine *m, int pid, i64 head_ptr_addr,
                            i64 len_ptr_addr) {
  int rc;
  struct Dll *e;
  struct Machine *m2;
  if (!pid || pid == m->tid) {
    rc = ReturnRobustList(m, head_ptr_addr, len_ptr_addr);
  } else {
    rc = -1;
    errno = ESRCH;
    LOCK(&m->system->machines_lock);
    for (e = dll_first(m->system->machines); e;
         e = dll_next(m->system->machines, e)) {
      m2 = MACHINE_CONTAINER(e);
      if (m2->tid == pid) {
        rc = ReturnRobustList(m2, head_ptr_addr, len_ptr_addr);
        break;
      }
    }
    UNLOCK(&m->system->machines_lock);
  }
  return rc;
}

static i32 SysSetRobustList(struct Machine *m, i64 head_addr, u64 len) {
  if (len != sizeof(struct robust_list_linux)) {
    return einval();
  }
  if (!IsValidMemory(m, head_addr, len, PROT_READ | PROT_WRITE)) {
    return efault();
  }
  m->robust_list = head_addr;
  return 0;
}

static int ValidateAffinityPid(struct Machine *m, int pid) {
  if (pid < 0) return esrch();
  if (pid && pid != m->tid && pid != m->system->pid) return eperm();
  return 0;
}

static int SysSchedSetaffinity(struct Machine *m,  //
                               i32 pid,            //
                               u64 cpusetsize,     //
                               i64 maskaddr) {
  if (ValidateAffinityPid(m, pid) == -1) return -1;
#ifdef __linux
  int rc;
  u8 *mask;
  size_t i, n;
  cpu_set_t sysmask;
  GetCpuCount();  // call for effect
  n = MIN(cpusetsize, CPU_SETSIZE / 8) * 8;
  if (!(mask = (u8 *)malloc(n / 8))) return -1;
  if (CopyFromUserRead(m, mask, maskaddr, n / 8) == -1) {
    free(mask);
    return -1;
  }
  CPU_ZERO(&sysmask);
  for (i = 0; i < n; ++i) {
    if (mask[i / 8] & (1 << (i % 8))) {
      CPU_SET(i, &sysmask);
    }
  }
  rc = sched_setaffinity(pid, sizeof(sysmask), &sysmask);
  free(mask);
  return rc;
#else
  return 0;  // do nothing
#endif
}

static int SysSchedGetaffinity(struct Machine *m,  //
                               i32 pid,            //
                               u64 cpusetsize,     //
                               i64 maskaddr) {
  if (ValidateAffinityPid(m, pid) == -1) return -1;
#ifdef __linux
  int rc;
  u8 *mask;
  size_t i, n;
  cpu_set_t sysmask;
  n = MIN(cpusetsize, CPU_SETSIZE / 8) * 8;
  if (!(mask = (u8 *)malloc(n / 8))) return -1;
  rc = sched_getaffinity(pid, sizeof(sysmask), &sysmask);
  unassert(rc == 0 || rc == -1);
  if (!rc) {
    rc = n / 8;
    memset(mask, 0, n / 8);
    for (i = 0; i < n; ++i) {
      if (CPU_ISSET(i, &sysmask)) {
        mask[i / 8] |= 1 << (i % 8);
      }
    }
    if (CopyToUserWrite(m, maskaddr, mask, n / 8) == -1) rc = -1;
  }
  free(mask);
  return rc;
#else
  u8 *mask;
  unsigned i, rc, count;
  count = GetCpuCount();
  rc = ROUNDUP(count, 64) / 8;
  if (cpusetsize < rc) return einval();
  if (!(mask = (u8 *)calloc(1, rc))) return -1;
  count = MIN(count, rc * 8);
  for (i = 0; i < count; ++i) {
    mask[i / 8] |= 1 << (i % 8);
  }
  if (CopyToUserWrite(m, maskaddr, mask, rc) == -1) rc = -1;
  free(mask);
  return rc;
#endif
}

static int SysPrctl(struct Machine *m, int op, i64 a, i64 b, i64 c, i64 d) {
  return einval();
}

static int SysArchPrctl(struct Machine *m, int code, i64 addr) {
  u8 buf[8];
  switch (code) {
    case ARCH_SET_GS_LINUX:
      m->gs.base = addr;
      return 0;
    case ARCH_SET_FS_LINUX:
      m->fs.base = addr;
      return 0;
    case ARCH_GET_GS_LINUX:
      Write64(buf, m->gs.base);
      return CopyToUserWrite(m, addr, buf, 8);
    case ARCH_GET_FS_LINUX:
      Write64(buf, m->fs.base);
      return CopyToUserWrite(m, addr, buf, 8);
    default:
      return einval();
  }
}

static u64 Prot2Page(int prot) {
  u64 key = 0;
  if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return einval();
  if (prot & PROT_READ) key |= PAGE_U;
  if (prot & PROT_WRITE) key |= PAGE_RW;
  if (~prot & PROT_EXEC) key |= PAGE_XD;
  return key;
}

static int SysMprotect(struct Machine *m, i64 addr, u64 size, int prot) {
  _Static_assert(PROT_READ == 1, "");
  _Static_assert(PROT_WRITE == 2, "");
  _Static_assert(PROT_EXEC == 4, "");
  int rc;
  int unsupported;
  if (size > NUMERIC_MAX(size_t)) return eoverflow();
  if (!IsValidAddrSize(addr, size)) return einval();
  if ((unsupported = prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
    LOGF("unsupported mprotect() protection: %#x", unsupported);
    return einval();
  }
  LOCK(&m->system->mmap_lock);
  rc = ProtectVirtual(m->system, addr, size, prot);
  if (rc != -1 && (prot & PROT_EXEC)) {
    LOGF("resetting jit hooks");
    ClearJitHooks(&m->system->jit);
  }
  UNLOCK(&m->system->mmap_lock);
  InvalidateSystem(m->system, false, true);
  return rc;
}

static int SysMadvise(struct Machine *m, i64 addr, u64 len, int advice) {
  return 0;
}

static i64 SysBrk(struct Machine *m, i64 addr) {
  i64 rc, size;
  long pagesize;
  LOCK(&m->system->mmap_lock);
  MEM_LOGF("brk(%#" PRIx64 ") currently %#" PRIx64, addr, m->system->brk);
  pagesize = GetSystemPageSize();
  addr = ROUNDUP(addr, pagesize);
  if (addr >= kMinBrk) {
    if (addr > m->system->brk) {
      size = addr - m->system->brk;
      CleanseMemory(m->system, size);
      if (m->system->rss < GetMaxRss(m->system)) {
        if (size / 4096 + m->system->vss < GetMaxVss(m->system)) {
          if (ReserveVirtual(m->system, m->system->brk, addr - m->system->brk,
                             PAGE_RW | PAGE_U, -1, 0, false) != -1) {
            MEM_LOGF("increased break %" PRIx64 " -> %" PRIx64, m->system->brk,
                     addr);
            m->system->brk = addr;
          }
        } else {
          LOGF("not enough virtual memory (%#" PRIx64 " / %#" PRIx64
               " pages) to map size %#" PRIx64,
               m->system->vss, GetMaxVss(m->system), size);
        }
      } else {
        LOGF("ran out of resident memory (%#" PRIx64 " / %#" PRIx64 " pages)",
             m->system->rss, GetMaxRss(m->system));
      }
    } else if (addr < m->system->brk) {
      if (FreeVirtual(m->system, addr, m->system->brk - addr) != -1) {
        m->system->brk = addr;
      }
    }
  }
  rc = m->system->brk;
  UNLOCK(&m->system->mmap_lock);
  return rc;
}

static int SysMunmap(struct Machine *m, i64 virt, u64 size) {
  int rc;
  LOCK(&m->system->mmap_lock);
  rc = FreeVirtual(m->system, virt, size);
  UNLOCK(&m->system->mmap_lock);
  return rc;
}

int GetOflags(struct Machine *m, int fildes) {
  int oflags;
  struct Fd *fd;
  LOCK(&m->system->fds.lock);
  if ((fd = GetFd(&m->system->fds, fildes))) {
    oflags = fd->oflags;
  } else {
    oflags = -1;
  }
  UNLOCK(&m->system->fds.lock);
  return oflags;
}

static i64 SysMmapImpl(struct Machine *m, i64 virt, u64 size, int prot,
                       int flags, int fildes, i64 offset) {
  u64 key;
  int oflags;
  ssize_t rc;
  long pagesize;
  i64 newautomap;
  if (!IsValidAddrSize(virt, size)) return einval();
  if (flags & MAP_GROWSDOWN_LINUX) return enotsup();
  if ((key = Prot2Page(prot)) == -1) return einval();
  if (flags & MAP_FIXED_NOREPLACE_LINUX) return enotsup();
  CleanseMemory(m->system, size);
  if (m->system->rss > GetMaxRss(m->system)) {
    LOGF("ran out of resident memory (%" PRIx64 " / %" PRIx64 " pages)",
         m->system->rss, GetMaxRss(m->system));
    return enomem();
  }
  if (size / 4096 + m->system->vss > GetMaxVss(m->system)) {
    LOGF("not enough virtual memory (%" PRIx64 " / %" PRIx64
         " pages) to map size %" PRIx64,
         m->system->vss, GetMaxVss(m->system), size);
    return enomem();
  }
  if (flags & MAP_ANONYMOUS_LINUX) {
    fildes = -1;
    if ((flags & MAP_TYPE_LINUX) == MAP_FILE_LINUX) {
      return einval();
    }
  } else if (offset < 0) {
    return einval();
  } else if (offset > NUMERIC_MAX(off_t)) {
    return eoverflow();
  }
  if (fildes != -1) {
    if ((oflags = GetOflags(m, fildes)) == -1) return -1;
    if ((oflags & O_ACCMODE) == O_WRONLY ||  //
        ((prot & PROT_WRITE) &&              //
         (oflags & O_APPEND)) ||             //
        ((prot & PROT_WRITE) &&              //
         (flags & MAP_SHARED_LINUX) &&       //
         (oflags & O_ACCMODE) != O_RDWR)) {  //
      errno = EACCES;
      return -1;
    }
  }
  if (!(flags & MAP_FIXED_LINUX) &&
      (!virt || !IsFullyUnmapped(m->system, virt, size))) {
    if ((virt = FindVirtual(m->system, m->system->automap, size)) == -1) {
      goto Finished;
    }
    pagesize = GetSystemPageSize();
    newautomap = ROUNDUP(virt + size, pagesize);
    if (newautomap >= kAutomapEnd) {
      newautomap = kAutomapStart;
    }
  } else {
    newautomap = -1;
  }
  rc = ReserveVirtual(m->system, virt, size, key, fildes, offset,
                      !!(flags & MAP_SHARED_LINUX));
  if (rc != -1 && newautomap != -1) {
    m->system->automap = newautomap;
  }
  if (rc == -1) virt = -1;
Finished:
  return virt;
}

static i64 SysMmap(struct Machine *m, i64 virt, u64 size, int prot, int flags,
                   int fildes, i64 offset) {
  i64 res;
  LOCK(&m->system->mmap_lock);
  res = SysMmapImpl(m, virt, size, prot, flags, fildes, offset);
  UNLOCK(&m->system->mmap_lock);
  return res;
}

static i64 SysMremap(struct Machine *m, i64 old_address, u64 old_size,
                     u64 new_size, int flags, i64 new_address) {
  // would be nice to have
  // avoid being noisy in the logs
  // hope program has fallback for failure
  LOG_ONCE(MEM_LOGF("mremap() not supported yet"));
  return enomem();
}

static int XlatMsyncFlags(int flags) {
  int sysflags;
  if (flags & ~(MS_ASYNC_LINUX | MS_SYNC_LINUX | MS_INVALIDATE_LINUX)) {
    LOGF("unsupported msync() flags %#x", flags);
    return einval();
  }
  // According to POSIX, either MS_SYNC or MS_ASYNC must be specified
  // in flags, and indeed failure to include one of these flags will
  // cause msync() to fail on some systems. However, Linux permits a
  // call to msync() that specifies neither of these flags, with
  // semantics that are (currently) equivalent to specifying MS_ASYNC.
  // ──Quoth msync(2) of Linux Programmer's Manual
  if (flags & MS_ASYNC_LINUX) {
    sysflags = MS_ASYNC;
  } else if (flags & MS_SYNC_LINUX) {
    sysflags = MS_SYNC;
  } else {
    sysflags = MS_ASYNC;
  }
  if (flags & MS_INVALIDATE_LINUX) {
    sysflags |= MS_INVALIDATE;
  }
#ifdef __FreeBSD__
  // FreeBSD's manual says "The flags argument was both MS_ASYNC and
  // MS_INVALIDATE. Only one of these flags is allowed." which makes
  // following the POSIX recommendation somewhat difficult.
  if (sysflags == (MS_ASYNC | MS_INVALIDATE)) {
    sysflags = MS_INVALIDATE;
  }
#endif
  return sysflags;
}

static int SysMsync(struct Machine *m, i64 virt, u64 size, int flags) {
  if (size > NUMERIC_MAX(size_t)) return eoverflow();
  if ((flags = XlatMsyncFlags(flags)) == -1) return -1;
  return SyncVirtual(m->system, virt, size, flags);
}

static int SysDup1(struct Machine *m, i32 fildes) {
  int oflags;
  int newfildes;
  struct Fd *fd;
  if (fildes < 0) return ebadf();
  if ((newfildes = dup(fildes)) != -1) {
    LOCK(&m->system->fds.lock);
    unassert(fd = GetFd(&m->system->fds, fildes));
    oflags = fd->oflags & ~O_CLOEXEC;
    unassert(ForkFd(&m->system->fds, fd, newfildes, oflags));
    UNLOCK(&m->system->fds.lock);
  }
  return newfildes;
}

static int Dup2(struct Machine *m, int fildes, int newfildes) {
  int rc;
  // POSIX.1-2007 lists dup2() as raising EINTR which seems impossible
  // so it'd be wonderful to learn what kernel(s) actually return this
  // noting Linux reproduces that in both its dup(2) and dup(3) manual
  RESTARTABLE(rc = dup2(fildes, newfildes));
  return rc;
}

static int SysDup2(struct Machine *m, i32 fildes, i32 newfildes) {
  int rc;
  int oflags;
  struct Fd *fd;
  if (newfildes < 0) {
    LOGF("dup2() ebadf");
    return ebadf();
  }
  if (fildes == newfildes) {
    // no-op system call, but still must validate
    LOCK(&m->system->fds.lock);
    if (GetFd(&m->system->fds, fildes)) {
      rc = fildes;
    } else {
      rc = -1;
    }
    UNLOCK(&m->system->fds.lock);
  } else if ((rc = Dup2(m, fildes, newfildes)) != -1) {
    LOCK(&m->system->fds.lock);
    if ((fd = GetFd(&m->system->fds, newfildes))) {
      dll_remove(&m->system->fds.list, &fd->elem);
      FreeFd(fd);
    }
    unassert(fd = GetFd(&m->system->fds, fildes));
    oflags = fd->oflags & ~O_CLOEXEC;
    unassert(ForkFd(&m->system->fds, fd, newfildes, oflags));
    UNLOCK(&m->system->fds.lock);
  }
  return rc;
}

static int SysDup3(struct Machine *m, i32 fildes, i32 newfildes, i32 flags) {
  int rc;
  int oflags;
  struct Fd *fd;
  if (newfildes < 0) return ebadf();
  if (fildes == newfildes) return einval();
  if (flags & ~O_CLOEXEC_LINUX) return einval();
  if ((rc = Dup2(m, fildes, newfildes)) != -1) {
    if (flags & O_CLOEXEC_LINUX) {
      unassert(!fcntl(newfildes, F_SETFD, FD_CLOEXEC));
    }
    LOCK(&m->system->fds.lock);
    if ((fd = GetFd(&m->system->fds, newfildes))) {
      dll_remove(&m->system->fds.list, &fd->elem);
      FreeFd(fd);
    }
    unassert(fd = GetFd(&m->system->fds, fildes));
    oflags = fd->oflags & ~O_CLOEXEC;
    if (flags & O_CLOEXEC_LINUX) {
      oflags |= O_CLOEXEC;
    }
    unassert(ForkFd(&m->system->fds, fd, newfildes, oflags));
    UNLOCK(&m->system->fds.lock);
  }
  return rc;
}

static int SysDupf(struct Machine *m, i32 fildes, i32 minfildes, int cmd) {
  int oflags;
  int newfildes;
  struct Fd *fd;
  if ((newfildes = fcntl(fildes, cmd, minfildes)) != -1) {
    LOCK(&m->system->fds.lock);
    unassert(fd = GetFd(&m->system->fds, fildes));
    oflags = fd->oflags & ~O_CLOEXEC;
    if (cmd == F_DUPFD_CLOEXEC) {
      oflags |= O_CLOEXEC;
    }
    unassert(ForkFd(&m->system->fds, fd, newfildes, oflags));
    UNLOCK(&m->system->fds.lock);
  }
  return newfildes;
}

static void FixupSock(int fd, int flags) {
  if (flags & SOCK_CLOEXEC_LINUX) {
    unassert(!fcntl(fd, F_SETFD, FD_CLOEXEC));
  }
  if (flags & SOCK_NONBLOCK_LINUX) {
    unassert(!fcntl(fd, F_SETFL, O_NDELAY));
  }
}

static int SysUname(struct Machine *m, i64 utsaddr) {
  // glibc binaries won't run unless we report blink as a
  // modern linux kernel on top of genuine intel hardware
  struct utsname_linux uts = {
      .sysname = "Linux",
      .release = "4.5",
      .version = "blink-1.0",
      .machine = "x86_64",
  };
  union {
    char host[sizeof(uts.nodename)];
    char domain[sizeof(uts.domainname)];
  } u;
  memset(u.host, 0, sizeof(u.host));
  gethostname(u.host, sizeof(u.host) - 1);
  strcpy(uts.nodename, u.host);
  memset(u.domain, 0, sizeof(u.domain));
#ifndef __HAIKU__
  getdomainname(u.domain, sizeof(u.domain) - 1);
#endif
  strcpy(uts.domainname, u.domain);
  return CopyToUser(m, utsaddr, &uts, sizeof(uts));
}

static int SysSocket(struct Machine *m, i32 family, i32 type, i32 protocol) {
  struct Fd *fd;
  int flags, fildes;
  flags = type & (SOCK_NONBLOCK_LINUX | SOCK_CLOEXEC_LINUX);
  type &= ~(SOCK_NONBLOCK_LINUX | SOCK_CLOEXEC_LINUX);
  if ((type = XlatSocketType(type)) == -1) return -1;
  if ((family = XlatSocketFamily(family)) == -1) return -1;
  if ((protocol = XlatSocketProtocol(protocol)) == -1) return -1;
  if (flags) LOCK(&m->system->exec_lock);
  if ((fildes = socket(family, type, protocol)) != -1) {
    FixupSock(fildes, flags);
    LOCK(&m->system->fds.lock);
    fd = AddFd(&m->system->fds, fildes,
               O_RDWR | (flags & SOCK_CLOEXEC_LINUX ? O_CLOEXEC : 0) |
                   (flags & SOCK_NONBLOCK_LINUX ? O_NDELAY : 0));
    fd->socktype = type;
    UNLOCK(&m->system->fds.lock);
  }
  if (flags) UNLOCK(&m->system->exec_lock);
  return fildes;
}

static int SysSocketpair(struct Machine *m, i32 family, i32 type, i32 protocol,
                         i64 pipefds_addr) {
  struct Fd *fd;
  u8 fds_linux[2][4];
  int rc, flags, sysflags, fds[2];
  flags = type & (SOCK_NONBLOCK_LINUX | SOCK_CLOEXEC_LINUX);
  type &= ~(SOCK_NONBLOCK_LINUX | SOCK_CLOEXEC_LINUX);
  if ((type = XlatSocketType(type)) == -1) return -1;
  if ((family = XlatSocketFamily(family)) == -1) return -1;
  if ((protocol = XlatSocketProtocol(protocol)) == -1) return -1;
  if (!IsValidMemory(m, pipefds_addr, sizeof(fds_linux), PROT_WRITE)) {
    return efault();
  }
  if (flags) LOCK(&m->system->exec_lock);
  if ((rc = socketpair(family, type, protocol, fds)) != -1) {
    FixupSock(fds[0], flags);
    FixupSock(fds[1], flags);
    LOCK(&m->system->fds.lock);
    sysflags = O_RDWR;
    if (flags & SOCK_CLOEXEC_LINUX) sysflags |= O_CLOEXEC;
    if (flags & SOCK_NONBLOCK_LINUX) sysflags |= O_NDELAY;
    unassert(fd = AddFd(&m->system->fds, fds[0], sysflags));
    fd->socktype = type;
    unassert(fd = AddFd(&m->system->fds, fds[1], sysflags));
    fd->socktype = type;
    UNLOCK(&m->system->fds.lock);
    Write32(fds_linux[0], fds[0]);
    Write32(fds_linux[1], fds[1]);
    unassert(!CopyToUserWrite(m, pipefds_addr, fds_linux, sizeof(fds_linux)));
  }
  if (flags) UNLOCK(&m->system->exec_lock);
  return rc;
}

static u32 LoadAddrSize(struct Machine *m, i64 asa) {
  const u8 *p;
  if (asa && (p = (const u8 *)SchlepR(m, asa, 4))) {
    return Read32(p);
  } else {
    return 0;
  }
}

static int StoreAddrSize(struct Machine *m, i64 asa, socklen_t len) {
  u8 buf[4];
  if (!asa) return 0;
  Write32(buf, len);
  return CopyToUserWrite(m, asa, buf, sizeof(buf));
}

static int LoadSockaddr(struct Machine *m, i64 sockaddr_addr, u32 sockaddr_size,
                        struct sockaddr_storage *out_sockaddr) {
  const struct sockaddr_linux *sockaddr_linux;
  if ((sockaddr_linux = (const struct sockaddr_linux *)SchlepR(
           m, sockaddr_addr, sockaddr_size))) {
    return XlatSockaddrToHost(out_sockaddr, sockaddr_linux, sockaddr_size);
  } else {
    return -1;
  }
}

static int StoreSockaddr(struct Machine *m, i64 sockaddr_addr,
                         i64 sockaddr_size_addr, const struct sockaddr *sa,
                         socklen_t salen) {
  int got;
  u32 avail;
  struct sockaddr_storage_linux ss;
  if (!sockaddr_addr) return 0;
  if (!sockaddr_size_addr) return 0;
  avail = LoadAddrSize(m, sockaddr_size_addr);
  if ((got = XlatSockaddrToLinux(&ss, sa, salen)) == -1) return -1;
  if (StoreAddrSize(m, sockaddr_size_addr, got) == -1) return -1;
  return CopyToUserWrite(m, sockaddr_addr, &ss, MIN(got, avail));
}

static int SysSocketName(struct Machine *m, i32 fildes, i64 sockaddr_addr,
                         i64 sockaddr_size_addr,
                         int SocketName(int, struct sockaddr *, socklen_t *)) {
  int rc;
  socklen_t addrlen;
  struct sockaddr_storage addr;
  addrlen = sizeof(addr);
  rc = SocketName(fildes, (struct sockaddr *)&addr, &addrlen);
  if (rc != -1) {
    if (StoreSockaddr(m, sockaddr_addr, sockaddr_size_addr,
                      (struct sockaddr *)&addr, addrlen) == -1) {
      rc = -1;
    }
  }
  return rc;
}

static int SysGetsockname(struct Machine *m, int fd, i64 aa, i64 asa) {
  return SysSocketName(m, fd, aa, asa, getsockname);
}

static int SysGetpeername(struct Machine *m, int fd, i64 aa, i64 asa) {
  return SysSocketName(m, fd, aa, asa, getpeername);
}

static int GetNoRestart(struct Machine *m, int fildes, bool *norestart) {
  struct Fd *fd;
  LOCK(&m->system->fds.lock);
  if ((fd = GetFd(&m->system->fds, fildes))) {
    *norestart = fd->norestart;
  }
  UNLOCK(&m->system->fds.lock);
  if (!fd) return ebadf();
  return 0;
}

static int SysAccept4(struct Machine *m, i32 fildes, i64 sockaddr_addr,
                      i64 sockaddr_size_addr, i32 flags) {
  struct Fd *fd;
  socklen_t addrlen;
  int newfd, socktype;
  bool restartable = false;
  struct sockaddr_storage addr;
  if (flags & ~(SOCK_CLOEXEC_LINUX | SOCK_NONBLOCK_LINUX)) return einval();
  LOCK(&m->system->fds.lock);
  if ((fd = GetFd(&m->system->fds, fildes))) {
    socktype = fd->socktype;
    restartable = !fd->norestart;
  }
  UNLOCK(&m->system->fds.lock);
  if (!fd) {
    return -1;
  }
  if (!socktype) {
    errno = ENOTSOCK;
    return -1;
  }
  if (socktype != SOCK_STREAM) {
    // POSIX.1 and Linux require EOPNOTSUPP when called on a file
    // descriptor that doesn't support accepting, i.e. SOCK_STREAM,
    // but FreeBSD incorrectly returns EINVAL.
    return eopnotsupp();
  }
  addrlen = sizeof(addr);
  INTERRUPTIBLE(restartable,
                newfd = accept(fildes, (struct sockaddr *)&addr, &addrlen));
  if (newfd != -1) {
    FixupSock(newfd, flags);
    LOCK(&m->system->fds.lock);
    if (!(fd = GetFd(&m->system->fds, fildes)) ||
        !ForkFd(&m->system->fds, fd, newfd,
                O_RDWR | (flags & SOCK_CLOEXEC_LINUX ? O_CLOEXEC : 0) |
                    (flags & SOCK_NONBLOCK_LINUX ? O_NDELAY : 0))) {
      close(newfd);
      newfd = -1;
    }
    UNLOCK(&m->system->fds.lock);
    if (newfd != -1) {
      StoreSockaddr(m, sockaddr_addr, sockaddr_size_addr,
                    (struct sockaddr *)&addr, addrlen);
    }
  }
  return newfd;
}

static int XlatSendFlags(int flags, int socktype) {
  int supported, hostflags;
  supported = MSG_OOB_LINUX |        //
              MSG_DONTROUTE_LINUX |  //
#ifdef MSG_NOSIGNAL
              MSG_NOSIGNAL_LINUX |  //
#endif
#ifdef MSG_EOR
              MSG_EOR_LINUX |
#endif
              MSG_DONTWAIT_LINUX;
  if (flags & ~supported) {
    LOGF("unsupported %s flags %#x", "send", flags & ~supported);
    return einval();
  }
  hostflags = 0;
  if (flags & MSG_OOB_LINUX) {
    if (socktype != SOCK_STREAM) {
      return eopnotsupp();
    }
    hostflags |= MSG_OOB;
  }
  if (flags & MSG_DONTROUTE_LINUX) hostflags |= MSG_DONTROUTE;
  if (flags & MSG_DONTWAIT_LINUX) hostflags |= MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
  if (flags & MSG_NOSIGNAL_LINUX) hostflags |= MSG_NOSIGNAL;
#endif
#ifdef MSG_EOR
  if (flags & MSG_EOR_LINUX) hostflags |= MSG_EOR;
#endif
  return hostflags;
}

static int XlatRecvFlags(int flags) {
  int supported, hostflags;
  supported = MSG_OOB_LINUX |       //
              MSG_PEEK_LINUX |      //
              MSG_TRUNC_LINUX |     //
              MSG_WAITALL_LINUX |   //
              MSG_DONTWAIT_LINUX |  //
              MSG_CMSG_CLOEXEC_LINUX;
  if (flags & ~supported) {
    LOGF("unsupported %s flags %#x", "recv", flags & ~supported);
    return einval();
  }
  hostflags = 0;
  if (flags & MSG_OOB_LINUX) hostflags |= MSG_OOB;
  if (flags & MSG_PEEK_LINUX) hostflags |= MSG_PEEK;
  if (flags & MSG_TRUNC_LINUX) hostflags |= MSG_TRUNC;
  if (flags & MSG_WAITALL_LINUX) hostflags |= MSG_WAITALL;
  if (flags & MSG_DONTWAIT_LINUX) hostflags |= MSG_DONTWAIT;
#ifdef MSG_CMSG_CLOEXEC
  if (flags & MSG_CMSG_CLOEXEC_LINUX) hostflags |= MSG_CMSG_CLOEXEC;
#endif
  return hostflags;
}

static int UnXlatMsgFlags(int flags) {
  int guestflags = 0;
  if (flags & MSG_OOB) {
    guestflags |= MSG_OOB_LINUX;
    flags &= ~MSG_OOB;
  }
  if (flags & MSG_PEEK) {
    guestflags |= MSG_PEEK_LINUX;
    flags &= ~MSG_PEEK;
  }
  if (flags & MSG_TRUNC) {
    guestflags |= MSG_TRUNC_LINUX;
    flags &= ~MSG_TRUNC;
  }
  if (flags & MSG_CTRUNC) {
    guestflags |= MSG_CTRUNC_LINUX;
    flags &= ~MSG_CTRUNC;
  }
  if (flags & MSG_WAITALL) {
    guestflags |= MSG_WAITALL_LINUX;
    flags &= ~MSG_WAITALL;
  }
  if (flags & MSG_DONTROUTE) {
    guestflags |= MSG_DONTROUTE_LINUX;
    flags &= ~MSG_DONTROUTE;
  }
#ifdef MSG_EOR
  if (flags & MSG_EOR) {
    guestflags |= MSG_EOR_LINUX;
    flags &= ~MSG_EOR;
  }
#endif
#ifdef MSG_NOSIGNAL
  if (flags & MSG_NOSIGNAL) {
    guestflags |= MSG_NOSIGNAL_LINUX;
    flags &= ~MSG_NOSIGNAL;
  }
#endif
#ifdef MSG_CMSG_CLOEXEC
  if (flags & MSG_CMSG_CLOEXEC) {
    guestflags |= MSG_CMSG_CLOEXEC_LINUX;
    flags &= ~MSG_CMSG_CLOEXEC;
  }
#endif
  if (flags) {
    LOGF("unsupported %s flags %#x", "msg", flags);
  }
  return guestflags;
}

static i64 ConvertEnotconnToSigpipe(struct Machine *m, i64 rc) {
#ifdef __linux
  return rc;
#else
  if (!(rc == -1 && errno == ENOTCONN)) return rc;
  LOCK(&m->system->sig_lock);
  if ((m->sigmask & (1ull << (SIGPIPE_LINUX - 1))) ||
      Read64(m->system->hands[SIGPIPE_LINUX - 1].handler) == SIG_IGN_LINUX) {
    errno = EPIPE;
  } else if (Read64(m->system->hands[SIGPIPE_LINUX - 1].handler) ==
             SIG_DFL_LINUX) {
    TerminateSignal(m, SIGPIPE_LINUX);
    errno = EPIPE;
  } else {
    m->interrupted = true;
    Put64(m->ax, -EPIPE_LINUX);
    DeliverSignal(m, SIGPIPE_LINUX, SI_KERNEL_LINUX);
  }
  UNLOCK(&m->system->sig_lock);
  return rc;
#endif
}

static bool IsSockAddrEmpty(const struct sockaddr *sa) {
  return (sa->sa_family == AF_INET &&
          !((const struct sockaddr_in *)sa)->sin_addr.s_addr) ||
         (sa->sa_family == AF_INET6 &&
          !((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr32[0] &&
          !((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr32[1] &&
          !((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr32[2] &&
          !((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr32[3]);
}

// Operating systems like Linux let you sendmsg(buf, dest=0.0.0.0) where
// the empty destination address implies the source address. Other OSes,
// e.g. OpenBSD, do not implement this behavior. See also this Stack
// Exchange question: https://unix.stackexchange.com/a/419881/451352
static void EnsureSockAddrHasDestination(struct Machine *m, int fildes,
                                         struct sockaddr_storage *ss) {
#ifndef __linux
  struct Fd *fd;
  if (!IsSockAddrEmpty((const struct sockaddr *)ss)) return;
  LOCK(&m->system->fds.lock);
  if ((fd = GetFd(&m->system->fds, fildes))) {
    if (ss->ss_family == fd->saddr.sa.sa_family) {
      if (ss->ss_family == AF_INET) {
        memcpy(&((struct sockaddr_in *)ss)->sin_addr, &fd->saddr.sin.sin_addr,
               sizeof(fd->saddr.sin.sin_addr));
      } else if (ss->ss_family == AF_INET6) {
        memcpy(&((struct sockaddr_in6 *)ss)->sin6_addr,
               &fd->saddr.sin6.sin6_addr, sizeof(fd->saddr.sin6.sin6_addr));
      } else {
        unassert(!"inconsistent code");
        __builtin_unreachable();
      }
    } else if (ss->ss_family == AF_INET) {
      // if we do socket() followed by connect(0.0.0.0) assume 127.0.0.1
      ((struct sockaddr_in *)ss)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
  }
  UNLOCK(&m->system->fds.lock);
#endif
}

static i64 SysSendto(struct Machine *m,  //
                     i32 fildes,         //
                     i64 bufaddr,        //
                     u64 buflen,         //
                     i32 flags,          //
                     i64 sockaddr_addr,  //
                     i32 sockaddr_size) {
  ssize_t rc;
  int socktype;
  struct Fd *fd;
  struct Iovs iv;
  bool norestart;
  struct msghdr msg;
  int len, hostflags;
  struct sockaddr_storage ss;
  LOCK(&m->system->fds.lock);
  if ((fd = GetFd(&m->system->fds, fildes))) {
    socktype = fd->socktype;
    norestart = fd->norestart;
  } else {
    socktype = 0;
    norestart = false;
  }
  UNLOCK(&m->system->fds.lock);
  if (!fd) return ebadf();
  if (sockaddr_size < 0) return einval();
  if ((hostflags = XlatSendFlags(flags, socktype)) == -1) return -1;
  memset(&msg, 0, sizeof(msg));
  // "If sendto() is used on a connection-mode socket, the arguments
  // dest_addr and addrlen are ignored." ──Quoth the Linux Programmer's
  // Manual § send(2). However FreeBSD behaves differently.
  if (socktype != SOCK_STREAM && sockaddr_size) {
    if ((len = LoadSockaddr(m, sockaddr_addr, sockaddr_size, &ss)) != -1) {
      msg.msg_namelen = len;
      EnsureSockAddrHasDestination(m, fildes, &ss);
    } else {
      return -1;
    }
    msg.msg_name = &ss;
  }
  InitIovs(&iv);
  if ((rc = AppendIovsReal(m, &iv, bufaddr, buflen, PROT_READ)) != -1) {
    msg.msg_iov = iv.p;
    msg.msg_iovlen = iv.i;
    INTERRUPTIBLE(!norestart, rc = sendmsg(fildes, &msg, hostflags));
  }
  FreeIovs(&iv);
  return ConvertEnotconnToSigpipe(m, rc);
}

static i64 SysRecvfrom(struct Machine *m,  //
                       i32 fildes,         //
                       i64 bufaddr,        //
                       u64 buflen,         //
                       i32 flags,          //
                       i64 sockaddr_addr,  //
                       i64 sockaddr_size_addr) {
  ssize_t rc;
  int hostflags;
  struct Iovs iv;
  struct msghdr msg;
  bool norestart = false;
  struct sockaddr_storage addr;
  if ((hostflags = XlatRecvFlags(flags)) == -1) return -1;
  if (GetNoRestart(m, fildes, &norestart) == -1) return -1;
  memset(&msg, 0, sizeof(msg));
  if (sockaddr_addr && sockaddr_size_addr) {
    msg.msg_name = &addr;
    msg.msg_namelen = sizeof(addr);
  }
  InitIovs(&iv);
  if ((rc = AppendIovsReal(m, &iv, bufaddr, buflen, PROT_WRITE)) != -1) {
    msg.msg_iov = iv.p;
    msg.msg_iovlen = iv.i;
    INTERRUPTIBLE(!norestart, rc = recvmsg(fildes, &msg, hostflags));
    if (rc != -1) {
      unassert(!StoreSockaddr(m, sockaddr_addr, sockaddr_size_addr,
                              (struct sockaddr *)msg.msg_name,
                              msg.msg_namelen));
    }
  }
  FreeIovs(&iv);
  return rc;
}

static i64 SysSendmsg(struct Machine *m, i32 fildes, i64 msgaddr, i32 flags) {
  i32 len;
  u64 iovlen;
  ssize_t rc;
  i64 iovaddr;
  int socktype;
  struct Fd *fd;
  struct Iovs iv;
  bool norestart;
  struct msghdr msg;
  struct sockaddr_storage ss;
  const struct msghdr_linux *gm;
  LOCK(&m->system->fds.lock);
  if ((fd = GetFd(&m->system->fds, fildes))) {
    socktype = fd->socktype;
    norestart = fd->norestart;
  } else {
    socktype = 0;
    norestart = false;
  }
  UNLOCK(&m->system->fds.lock);
  if (!fd) return ebadf();
  if ((flags = XlatSendFlags(flags, socktype)) == -1) return -1;
  if (!(gm = (const struct msghdr_linux *)SchlepR(m, msgaddr, sizeof(*gm)))) {
    return -1;
  }
  memset(&msg, 0, sizeof(msg));
  if (socktype != SOCK_STREAM && (len = Read32(gm->namelen)) > 0) {
    if ((len = LoadSockaddr(m, Read64(gm->name), len, &ss)) == -1) {
      return -1;
    }
    EnsureSockAddrHasDestination(m, fildes, &ss);
    msg.msg_name = &ss;
    msg.msg_namelen = len;
  }
  if (SendAncillary(m, &msg, gm) == -1) {
    return -1;
  }
  iovaddr = Read64(gm->iov);
  iovlen = Read64(gm->iovlen);
  if (!iovlen || iovlen > IOV_MAX_LINUX) {
    errno = EMSGSIZE;
    return -1;
  }
  InitIovs(&iv);
  if ((rc = AppendIovsGuest(m, &iv, iovaddr, iovlen, PROT_READ)) != -1) {
    msg.msg_iov = iv.p;
    msg.msg_iovlen = iv.i;
    INTERRUPTIBLE(!norestart, rc = sendmsg(fildes, &msg, flags));
  }
  FreeIovs(&iv);
  return ConvertEnotconnToSigpipe(m, rc);
}

static i64 SysRecvmsg(struct Machine *m, i32 fildes, i64 msgaddr, i32 flags) {
  ssize_t rc;
  u64 iovlen;
  i64 iovaddr;
  struct Iovs iv;
  struct msghdr msg;
  bool norestart = false;
  struct msghdr_linux gm;
  struct sockaddr_storage addr;
  if ((flags = XlatRecvFlags(flags)) == -1) return -1;
  if (GetNoRestart(m, fildes, &norestart) == -1) return -1;
  if (CopyFromUserRead(m, &gm, msgaddr, sizeof(gm)) == -1) return -1;
  memset(&msg, 0, sizeof(msg));
  iovaddr = Read64(gm.iov);
  iovlen = Read64(gm.iovlen);
  if (!iovlen || iovlen > IOV_MAX_LINUX) {
    errno = EMSGSIZE;
    return -1;
  }
  if (Read64(gm.controllen)) {
    msg.msg_control = AddToFreeList(m, calloc(1, kMaxAncillary));
    msg.msg_controllen = kMaxAncillary;
  }
  InitIovs(&iv);
  if ((rc = AppendIovsGuest(m, &iv, iovaddr, iovlen, PROT_WRITE)) != -1) {
    msg.msg_iov = iv.p;
    msg.msg_iovlen = iv.i;
    if (Read64(gm.name)) {
      memset(&addr, 0, sizeof(addr));
      msg.msg_name = &addr;
      msg.msg_namelen = sizeof(addr);
    }
    INTERRUPTIBLE(!norestart, rc = recvmsg(fildes, &msg, flags));
    if (rc != -1) {
      Write32(gm.flags, UnXlatMsgFlags(msg.msg_flags));
      unassert(CopyToUserWrite(m, msgaddr, &gm, sizeof(gm)) != -1);
      if (ReceiveAncillary(m, &gm, &msg, flags) == -1) {
        return -1;
      }
      if (Read64(gm.name)) {
        StoreSockaddr(m, Read64(gm.name),
                      msgaddr + offsetof(struct msghdr_linux, namelen),
                      (struct sockaddr *)&addr, msg.msg_namelen);
      }
    }
  }
  FreeIovs(&iv);
  return rc;
}

static int SysConnectBind(struct Machine *m, i32 fildes, i64 sockaddr_addr,
                          u32 sockaddr_size,
                          int impl(int, const struct sockaddr *, socklen_t)) {
  int rc, len;
  struct Fd *fd;
  socklen_t addrlen;
  bool norestart = false;
  struct sockaddr_storage addr;
  if (GetNoRestart(m, fildes, &norestart) == -1) return -1;
  if ((len = LoadSockaddr(m, sockaddr_addr, sockaddr_size, &addr)) != -1) {
    addrlen = len;
    if (impl == connect) {
      EnsureSockAddrHasDestination(m, fildes, &addr);
    }
  } else {
    return -1;
  }
  INTERRUPTIBLE(!norestart,
                (rc = impl(fildes, (const struct sockaddr *)&addr, addrlen)));
  if (rc != -1 && impl == bind) {
    LOCK(&m->system->fds.lock);
    if ((fd = GetFd(&m->system->fds, fildes))) {
      memcpy(&fd->saddr, &addr, sizeof(fd->saddr));
    }
    UNLOCK(&m->system->fds.lock);
  }
  return rc;
}

static int SysBind(struct Machine *m, int fd, i64 aa, u32 as) {
  return SysConnectBind(m, fd, aa, as, bind);
}

static int SysConnect(struct Machine *m, int fd, i64 aa, u32 as) {
  return SysConnectBind(m, fd, aa, as, connect);
}

static int UnXlatSocketType(int x) {
  if (x == SOCK_STREAM) return SOCK_STREAM_LINUX;
  if (x == SOCK_DGRAM) return SOCK_DGRAM_LINUX;
  LOGF("%s %d not supported yet", "socket type", x);
  return einval();
}

static int GetsockoptInt32(struct Machine *m, i32 fd, int level, int optname,
                           i64 optvaladdr, i64 optvalsizeaddr, int xlat(int)) {
  u8 *psize;
  u8 gval[4];
  int rc, val;
  socklen_t optvalsize;
  u32 optvalsize_linux;
  if (!(psize = (u8 *)SchlepRW(m, optvalsizeaddr, 4))) return -1;
  optvalsize_linux = Read32(psize);
  if (!IsValidMemory(m, optvaladdr, optvalsize_linux, PROT_WRITE)) {
    return efault();
  }
  optvalsize = sizeof(val);
  if ((rc = getsockopt(fd, level, optname, &val, &optvalsize)) != -1) {
    if ((val = xlat(val)) == -1) return -1;
    Write32(gval, val);
    CopyToUserWrite(m, optvaladdr, &gval, MIN(sizeof(gval), optvalsize_linux));
    Write32(psize, sizeof(gval));
  }
  return rc;
}

static int SetsockoptLinger(struct Machine *m, i32 fildes, i64 optvaladdr,
                            u32 optvalsize) {
  struct linger hl;
  const struct linger_linux *gl;
  if (optvalsize < sizeof(*gl)) return einval();
  if (!(gl =
            (const struct linger_linux *)SchlepR(m, optvaladdr, sizeof(*gl)))) {
    return -1;
  }
  hl.l_onoff = (i32)Read32(gl->onoff);
  hl.l_linger = (i32)Read32(gl->linger);
  return setsockopt(fildes, SOL_SOCKET, SO_LINGER_, &hl, sizeof(hl));
}

static int GetsockoptLinger(struct Machine *m, i32 fd, i64 optvaladdr,
                            i64 optvalsizeaddr) {
  int rc;
  u8 *psize;
  struct linger hl;
  socklen_t optvalsize;
  u32 optvalsize_linux;
  struct linger_linux gl;
  if (!(psize = (u8 *)SchlepRW(m, optvalsizeaddr, 4))) return -1;
  optvalsize_linux = Read32(psize);
  if (!IsValidMemory(m, optvaladdr, optvalsize_linux, PROT_WRITE)) {
    return efault();
  }
  optvalsize = sizeof(hl);
  if ((rc = getsockopt(fd, SOL_SOCKET, SO_LINGER_, &hl, &optvalsize)) != -1) {
    Write32(gl.onoff, hl.l_onoff);
    Write32(gl.linger, hl.l_linger);
    CopyToUserWrite(m, optvaladdr, &gl, MIN(sizeof(gl), optvalsize_linux));
    Write32(psize, sizeof(gl));
  }
  return rc;
}

static int SysSetsockopt(struct Machine *m, i32 fildes, i32 level, i32 optname,
                         i64 optvaladdr, u32 optvalsize) {
  int rc;
  void *optval;
  struct Fd *fd;
  int syslevel, sysoptname;
  switch (level) {
    case SOL_SOCKET_LINUX:
      switch (optname) {
        case SO_LINGER_LINUX:
          return SetsockoptLinger(m, fildes, optvaladdr, optvalsize);
        default:
          break;
      }
      break;
    default:
      break;
  }
  if (optvalsize > 256) return einval();
  if (XlatSocketLevel(level, &syslevel) == -1) return -1;
  if ((sysoptname = XlatSocketOptname(level, optname)) == -1) return -1;
  if (!(optval = SchlepR(m, optvaladdr, optvalsize))) return -1;
  rc = setsockopt(fildes, syslevel, sysoptname, optval, optvalsize);
  if (rc != -1 &&                      //
      level == SOL_SOCKET_LINUX &&     //
      optname == SO_RCVTIMEO_LINUX &&  //
      optvalsize >= 4) {
    LOCK(&m->system->fds.lock);
    if ((fd = GetFd(&m->system->fds, fildes))) {
      fd->norestart = !!Read32((u8 *)optval);
    }
    UNLOCK(&m->system->fds.lock);
  }
  return rc;
}

static int SysGetsockopt(struct Machine *m, i32 fildes, i32 level, i32 optname,
                         i64 optvaladdr, i64 optvalsizeaddr) {
  int rc;
  void *optval;
  socklen_t optvalsize;
  u8 optvalsize_linux[4];
  int syslevel, sysoptname;
  switch (level) {
    case SOL_SOCKET_LINUX:
      switch (optname) {
        case SO_TYPE_LINUX:
          return GetsockoptInt32(m, fildes, SOL_SOCKET, SO_TYPE, optvaladdr,
                                 optvalsizeaddr, UnXlatSocketType);
        case SO_ERROR_LINUX:
          return GetsockoptInt32(m, fildes, SOL_SOCKET, SO_ERROR, optvaladdr,
                                 optvalsizeaddr, XlatErrno);
        case SO_LINGER_LINUX:
          return GetsockoptLinger(m, fildes, optvaladdr, optvalsizeaddr);
        default:
          break;
      }
      break;
    default:
      break;
  }
  if (XlatSocketLevel(level, &syslevel) == -1) return -1;
  if ((sysoptname = XlatSocketOptname(level, optname)) == -1) return -1;
  if (CopyFromUserRead(m, optvalsize_linux, optvalsizeaddr,
                       sizeof(optvalsize_linux)) == -1) {
    return -1;
  }
  optvalsize = Read32(optvalsize_linux);
  if (optvalsize > 256) return einval();
  if (!(optval = calloc(1, optvalsize))) return -1;
  rc = getsockopt(fildes, syslevel, sysoptname, optval, &optvalsize);
  Write32(optvalsize_linux, optvalsize);
  CopyToUserWrite(m, optvaladdr, optval, optvalsize);
  CopyToUserWrite(m, optvalsizeaddr, optvalsize_linux,
                  sizeof(optvalsize_linux));
  free(optval);
  return rc;
}

static i64 SysRead(struct Machine *m, i32 fildes, i64 addr, u64 size) {
  i64 rc;
  int oflags;
  struct Fd *fd;
  struct Iovs iv;
  ssize_t (*readv_impl)(int, const struct iovec *, int);
  if (size > NUMERIC_MAX(size_t)) return eoverflow();
  LOCK(&m->system->fds.lock);
  if ((fd = GetFd(&m->system->fds, fildes))) {
    unassert(fd->cb);
    unassert(readv_impl = fd->cb->readv);
    oflags = fd->oflags;
  } else {
    readv_impl = 0;
    oflags = 0;
  }
  UNLOCK(&m->system->fds.lock);
  if (!fd) return -1;
  if ((oflags & O_ACCMODE) == O_WRONLY) return ebadf();
  if (size) {
    InitIovs(&iv);
    if ((rc = AppendIovsReal(m, &iv, addr, size, PROT_WRITE)) != -1) {
      RESTARTABLE(rc = readv_impl(fildes, iv.p, iv.i));
      if (rc != -1) SetWriteAddr(m, addr, rc);
    }
    FreeIovs(&iv);
  } else {
    rc = 0;
  }
  return rc;
}

static i64 SysWrite(struct Machine *m, i32 fildes, i64 addr, u64 size) {
  i64 rc;
  int oflags;
  struct Fd *fd;
  struct Iovs iv;
  ssize_t (*writev_impl)(int, const struct iovec *, int);
  if (size > NUMERIC_MAX(size_t)) return eoverflow();
  LOCK(&m->system->fds.lock);
  if ((fd = GetFd(&m->system->fds, fildes))) {
    unassert(fd->cb);
    unassert(writev_impl = fd->cb->writev);
    oflags = fd->oflags;
  } else {
    writev_impl = 0;
    oflags = 0;
  }
  UNLOCK(&m->system->fds.lock);
  if (!fd) return -1;
  if ((oflags & O_ACCMODE) == O_RDONLY) return ebadf();
  if (size) {
    InitIovs(&iv);
    if ((rc = AppendIovsReal(m, &iv, addr, size, PROT_READ)) != -1) {
      RESTARTABLE(rc = writev_impl(fildes, iv.p, iv.i));
      if (rc != -1) SetReadAddr(m, addr, rc);
    }
    FreeIovs(&iv);
  } else {
    rc = 0;
  }
  return ConvertEnotconnToSigpipe(m, rc);
}

// FreeBSD doesn't do access mode check on read/write to pipes.
// Cygwin generally doesn't do access checks or is inconsistent.
static long CheckFdAccess(struct Machine *m, i32 fildes, bool writable,
                          int errno_if_check_fails) {
  int oflags;
  struct Fd *fd;
  LOCK(&m->system->fds.lock);
  if ((fd = GetFd(&m->system->fds, fildes))) {
    oflags = fd->oflags;
  } else {
    oflags = 0;
  }
  UNLOCK(&m->system->fds.lock);
  if (!fd) return -1;
  if ((writable && ((oflags & O_ACCMODE) == O_RDONLY)) ||
      (!writable && ((oflags & O_ACCMODE) == O_WRONLY))) {
    errno = errno_if_check_fails;
    return -1;
  }
  return 0;
}

static i64 SysPread(struct Machine *m, i32 fildes, i64 addr, u64 size,
                    u64 offset) {
  ssize_t rc;
  struct Iovs iv;
  if (size > NUMERIC_MAX(size_t)) return eoverflow();
  if (CheckFdAccess(m, fildes, false, EBADF) == -1) return -1;
  if (size) {
    InitIovs(&iv);
    if ((rc = AppendIovsReal(m, &iv, addr, size, PROT_WRITE)) != -1) {
      RESTARTABLE(rc = preadv(fildes, iv.p, iv.i, offset));
      if (rc != -1) SetWriteAddr(m, addr, rc);
    }
    FreeIovs(&iv);
  } else {
    rc = 0;
  }
  return rc;
}

static i64 SysPwrite(struct Machine *m, i32 fildes, i64 addr, u64 size,
                     u64 offset) {
  ssize_t rc;
  struct Iovs iv;
  if (size > NUMERIC_MAX(size_t)) return eoverflow();
  if (CheckFdAccess(m, fildes, true, EBADF) == -1) return -1;
  if (size) {
    InitIovs(&iv);
    if ((rc = AppendIovsReal(m, &iv, addr, size, PROT_READ)) != -1) {
      RESTARTABLE(rc = pwritev(fildes, iv.p, iv.i, offset));
      if (rc != -1) SetReadAddr(m, addr, rc);
    }
    FreeIovs(&iv);
  } else {
    rc = 0;
  }
  return rc;
}

static i64 SysPreadv2(struct Machine *m, i32 fildes, i64 iovaddr, u32 iovlen,
                      i64 offset, i32 flags) {
  i64 rc;
  int oflags;
  struct Fd *fd;
  struct Iovs iv;
  ssize_t (*readv_impl)(int, const struct iovec *, int);
  if (flags) {
    LOGF("%s flags not supported yet: %#" PRIx32, "preadv2", flags);
    return einval();
  }
  if (iovlen > IOV_MAX_LINUX) return einval();
  LOCK(&m->system->fds.lock);
  if ((fd = GetFd(&m->system->fds, fildes))) {
    unassert(fd->cb);
    unassert(readv_impl = fd->cb->readv);
    oflags = fd->oflags;
  } else {
    readv_impl = 0;
    oflags = 0;
  }
  UNLOCK(&m->system->fds.lock);
  if (!fd) return -1;
  if ((oflags & O_ACCMODE) == O_WRONLY) return ebadf();
  if (iovlen) {
    InitIovs(&iv);
    if ((rc = AppendIovsGuest(m, &iv, iovaddr, iovlen, PROT_WRITE)) != -1) {
      if (iv.i) {
        if (offset == -1) {
          RESTARTABLE(rc = readv_impl(fildes, iv.p, iv.i));
        } else if (offset < 0) {
          return einval();
        } else if (offset > NUMERIC_MAX(off_t)) {
          return eoverflow();
        } else {
          RESTARTABLE(rc = preadv(fildes, iv.p, iv.i, offset));
        }
      } else {
        rc = 0;
      }
    }
    FreeIovs(&iv);
  } else {
    rc = 0;
  }
  return rc;
}

static i64 SysPwritev2(struct Machine *m, i32 fildes, i64 iovaddr, u32 iovlen,
                       i64 offset, i32 flags) {
  i64 rc;
  int oflags;
  struct Fd *fd;
  struct Iovs iv;
  ssize_t (*writev_impl)(int, const struct iovec *, int);
  if (flags) {
    LOGF("%s flags not supported yet: %#" PRIx32, "pwritev2", flags);
    return einval();
  }
  if (iovlen > IOV_MAX_LINUX) return einval();
  LOCK(&m->system->fds.lock);
  if ((fd = GetFd(&m->system->fds, fildes))) {
    unassert(fd->cb);
    unassert(writev_impl = fd->cb->writev);
    oflags = fd->oflags;
  } else {
    writev_impl = 0;
    oflags = 0;
  }
  UNLOCK(&m->system->fds.lock);
  if (!fd) return -1;
  if ((oflags & O_ACCMODE) == O_RDONLY) return ebadf();
  if (iovlen) {
    InitIovs(&iv);
    if ((rc = AppendIovsGuest(m, &iv, iovaddr, iovlen, PROT_READ)) != -1) {
      if (iv.i) {
        if (offset == -1) {
          RESTARTABLE(rc = writev_impl(fildes, iv.p, iv.i));
          rc = ConvertEnotconnToSigpipe(m, rc);
        } else if (offset < 0) {
          return einval();
        } else if (offset > NUMERIC_MAX(off_t)) {
          return eoverflow();
        } else {
          RESTARTABLE(rc = pwritev(fildes, iv.p, iv.i, offset));
        }
      } else {
        rc = 0;
      }
    }
    FreeIovs(&iv);
  } else {
    rc = 0;
  }
  return rc;
}

static i64 SysReadv(struct Machine *m, i32 fildes, i64 iovaddr, u32 iovlen) {
  return SysPreadv2(m, fildes, iovaddr, iovlen, -1, 0);
}

static i64 SysWritev(struct Machine *m, i32 fildes, i64 iovaddr, u32 iovlen) {
  return SysPwritev2(m, fildes, iovaddr, iovlen, -1, 0);
}

static i64 SysPreadv(struct Machine *m, i32 fildes, i64 iovaddr, u32 iovlen,
                     i64 offset) {
  if (offset < 0) return einval();
  return SysPreadv2(m, fildes, iovaddr, iovlen, offset, 0);
}

static i64 SysPwritev(struct Machine *m, i32 fildes, i64 iovaddr, u32 iovlen,
                      i64 offset) {
  if (offset < 0) return einval();
  return SysPwritev2(m, fildes, iovaddr, iovlen, offset, 0);
}

static int UnXlatDt(int x) {
#ifndef DT_UNKNOWN
  return DT_UNKNOWN_LINUX;
#else
  switch (x) {
    XLAT(DT_UNKNOWN, DT_UNKNOWN_LINUX);
    XLAT(DT_FIFO, DT_FIFO_LINUX);
    XLAT(DT_CHR, DT_CHR_LINUX);
    XLAT(DT_DIR, DT_DIR_LINUX);
    XLAT(DT_BLK, DT_BLK_LINUX);
    XLAT(DT_REG, DT_REG_LINUX);
    XLAT(DT_LNK, DT_LNK_LINUX);
    XLAT(DT_SOCK, DT_SOCK_LINUX);
    default:
      __builtin_unreachable();
  }
#endif
}

static i64 Getdents(struct Machine *m, i32 fildes, i64 addr, i64 size,
                    struct Fd *fd) {
  i64 i;
  int type;
  off_t off;
  long tell;
  int reclen;
  size_t len;
  struct stat st;
  struct dirent *ent;
  struct dirent_linux rec;
  if (size < sizeof(rec) - sizeof(rec.name)) return einval();
  if ((fd->oflags & O_DIRECTORY) != O_DIRECTORY) return enotdir();
  if (!IsValidMemory(m, addr, size, PROT_WRITE)) return efault();
  if (fstat(fildes, &st) || !st.st_nlink) return enoent();
  if (!fd->dirstream && !(fd->dirstream = fdopendir(fd->fildes))) {
    return -1;
  }
  for (i = 0; i + sizeof(rec) <= size; i += reclen) {
    // telldir() can actually return negative on ARM/MIPS/i386
    errno = 0;
    tell = telldir(fd->dirstream);
    unassert(tell != -1 || errno == 0);
    off = tell;
    if (!(ent = readdir(fd->dirstream))) break;
    len = strlen(ent->d_name);
    if (len + 1 > sizeof(rec.name)) {
      LOGF("ignoring %zu byte d_name: %s", len, ent->d_name);
      reclen = 0;
      continue;
    }
#ifdef DT_UNKNOWN
    type = UnXlatDt(ent->d_type);
#else
    struct stat st;
    if (fstatat(fd->fildes, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
      LOGF("getdents() fstatat(%d, %s) failed: %s", fd->fildes, ent->d_name,
           DescribeHostErrno(errno));
      type = DT_UNKNOWN_LINUX;
    } else {
      if (S_ISDIR(st.st_mode)) {
        type = DT_DIR_LINUX;
      } else if (S_ISCHR(st.st_mode)) {
        type = DT_CHR_LINUX;
      } else if (S_ISBLK(st.st_mode)) {
        type = DT_BLK_LINUX;
      } else if (S_ISFIFO(st.st_mode)) {
        type = DT_FIFO_LINUX;
      } else if (S_ISLNK(st.st_mode)) {
        type = DT_LNK_LINUX;
      } else if (S_ISSOCK(st.st_mode)) {
        type = DT_SOCK_LINUX;
      } else if (S_ISREG(st.st_mode)) {
        type = DT_REG_LINUX;
      } else {
        LOGF("unknown st_mode %d", st.st_mode);
        type = DT_UNKNOWN_LINUX;
      }
    }
#endif
    reclen = ROUNDUP(8 + 8 + 2 + 1 + len + 1, 8);
    memset(&rec, 0, sizeof(rec));
    Write64(rec.ino, ent->d_ino);
    Write64(rec.off, off);
    Write16(rec.reclen, reclen);
    Write8(rec.type, type);
    strcpy(rec.name, ent->d_name);
    CopyToUserWrite(m, addr + i, &rec, reclen);
  }
  return i;
}

static i64 SysGetdents(struct Machine *m, i32 fildes, i64 addr, i64 size) {
  i64 rc;
  struct Fd *fd;
  if (!(fd = GetAndLockFd(m, fildes))) return -1;
  rc = Getdents(m, fildes, addr, size, fd);
  UnlockFd(fd);
  return rc;
}

static int SysSetTidAddress(struct Machine *m, i64 ctid) {
  m->ctid = ctid;
  return m->tid;
}

static int SysFadvise(struct Machine *m, u32 fd, u64 offset, u64 len,
                      i32 advice) {
  return 0;
}

static i64 SysLseek(struct Machine *m, i32 fildes, i64 offset, int whence) {
  i64 rc;
  struct Fd *fd;
  if (offset > NUMERIC_MAX(off_t)) return eoverflow();
  if (offset < -NUMERIC_MAX(off_t) - 1) return eoverflow();
  if (!(fd = GetAndLockFd(m, fildes))) return -1;
  if (!fd->dirstream) {
    rc = lseek(fd->fildes, offset, XlatWhence(whence));
  } else if (whence == SEEK_SET_LINUX) {
    if (!offset) {
      rewinddir(fd->dirstream);
    } else {
      seekdir(fd->dirstream, offset);
    }
    rc = 0;
  } else {
    rc = einval();
  }
  UnlockFd(fd);
  return rc;
}

static i64 SysFtruncate(struct Machine *m, i32 fildes, i64 length) {
  i64 rc;
  if (length < 0) return einval();
  if (length > NUMERIC_MAX(off_t)) return eoverflow();
  if (CheckFdAccess(m, fildes, true, EINVAL) == -1) return -1;
  RESTARTABLE(rc = ftruncate(fildes, length));
  return rc;
}

static int XlatFaccessatFlags(int x) {
  int res = 0;
  if (x & AT_EACCESS_LINUX) {
#if !defined(__EMSCRIPTEN__) && !defined(__CYGWIN__)
    res |= AT_EACCESS;
#endif
    x &= ~AT_EACCESS_LINUX;
  }
  if (x & AT_SYMLINK_FOLLOW_LINUX) {
    x &= ~AT_SYMLINK_FOLLOW_LINUX;  // default behavior
  }
  if (x & AT_SYMLINK_NOFOLLOW_LINUX) {
    res |= AT_SYMLINK_NOFOLLOW;
    x &= ~AT_SYMLINK_NOFOLLOW_LINUX;
  }
  if (x) {
    LOGF("%s() flags %d not supported", "faccessat", x);
    return -1;
  }
  return res;
}

static int SysFaccessat2(struct Machine *m, i32 dirfd, i64 path, i32 mode,
                         i32 flags) {
  return OverlaysAccess(GetDirFildes(dirfd), LoadStr(m, path), XlatAccess(mode),
                        XlatFaccessatFlags(flags));
}

static int SysFaccessat(struct Machine *m, i32 dirfd, i64 path, i32 mode) {
  return SysFaccessat2(m, dirfd, path, mode, 0);
}

static int SysFstat(struct Machine *m, i32 fd, i64 staddr) {
  int rc;
  struct stat st;
  struct stat_linux gst;
  if ((rc = fstat(fd, &st)) != -1) {
    XlatStatToLinux(&gst, &st);
    if (CopyToUserWrite(m, staddr, &gst, sizeof(gst)) == -1) rc = -1;
  }
  return rc;
}

static int XlatFstatatFlags(int x) {
  int res = 0;
  if (x & AT_SYMLINK_FOLLOW_LINUX) {
    x &= ~AT_SYMLINK_FOLLOW_LINUX;  // default behavior
  }
  if (x & AT_SYMLINK_NOFOLLOW_LINUX) {
    res |= AT_SYMLINK_NOFOLLOW;
    x &= ~AT_SYMLINK_NOFOLLOW_LINUX;
  }
#ifdef AT_EMPTY_PATH
  if (x & AT_EMPTY_PATH_LINUX) {
    res |= AT_EMPTY_PATH;
    x &= ~AT_EMPTY_PATH_LINUX;
  }
#endif
  if (x) {
    LOGF("%s() flags %d not supported", "fstatat", x);
    return -1;
  }
  return res;
}

static int SysFstatat(struct Machine *m, i32 dirfd, i64 pathaddr, i64 staddr,
                      i32 flags) {
  int rc;
  struct stat st;
  const char *path;
  struct stat_linux gst;
  if (!(path = LoadStr(m, pathaddr))) return -1;
#ifndef AT_EMPTY_PATH
  if (flags & AT_EMPTY_PATH_LINUX) {
    flags &= AT_EMPTY_PATH_LINUX;
    if (!*path) {
      if (flags) {
        LOGF("%s() flags %d not supported", "fstatat(AT_EMPTY_PATH)", flags);
        return -1;
      }
      return SysFstat(m, dirfd, staddr);
    }
  }
#endif
  if ((rc = OverlaysStat(GetDirFildes(dirfd), path, &st,
                         XlatFstatatFlags(flags))) != -1) {
    XlatStatToLinux(&gst, &st);
    if (CopyToUserWrite(m, staddr, &gst, sizeof(gst)) == -1) rc = -1;
  }
  return rc;
}

static int XlatFchownatFlags(int x) {
  int res = 0;
  if (x & AT_SYMLINK_FOLLOW_LINUX) {
    x &= ~AT_SYMLINK_FOLLOW_LINUX;  // default behavior
  }
  if (x & AT_SYMLINK_NOFOLLOW_LINUX) {
    res |= AT_SYMLINK_NOFOLLOW;
    x &= ~AT_SYMLINK_NOFOLLOW_LINUX;
  }
#ifdef AT_EMPTY_PATH
  if (x & AT_EMPTY_PATH_LINUX) {
    res |= AT_EMPTY_PATH;
    x &= ~AT_EMPTY_PATH_LINUX;
  }
#endif
  if (x) {
    LOGF("%s() flags %#x not supported", "fchownat", x);
    return -1;
  }
  return res;
}

static int SysFchown(struct Machine *m, i32 fildes, u32 uid, u32 gid) {
  return fchown(fildes, uid, gid);
}

static int SysFchownat(struct Machine *m, i32 dirfd, i64 pathaddr, u32 uid,
                       u32 gid, i32 flags) {
  const char *path;
  if (!(path = LoadStr(m, pathaddr))) return -1;
#ifndef AT_EMPTY_PATH
  if (flags & AT_EMPTY_PATH_LINUX) {
    flags &= AT_EMPTY_PATH_LINUX;
    if (!*path) {
      if (flags) {
        LOGF("%s() flags %d not supported", "fchownat(AT_EMPTY_PATH)", flags);
        return -1;
      }
      return SysFchown(m, dirfd, uid, gid);
    }
  }
#endif
  return OverlaysChown(GetDirFildes(dirfd), path, uid, gid,
                       XlatFchownatFlags(flags));
}

static int SysChown(struct Machine *m, i64 pathaddr, u32 uid, u32 gid) {
  return SysFchownat(m, AT_FDCWD_LINUX, pathaddr, uid, gid, 0);
}

static int SysLchown(struct Machine *m, i64 pathaddr, u32 uid, u32 gid) {
  return SysFchownat(m, AT_FDCWD_LINUX, pathaddr, uid, gid,
                     AT_SYMLINK_NOFOLLOW_LINUX);
}

static int SysChroot(struct Machine *m, i64 path) {
  return SetOverlays(LoadStr(m, path));
}

static int SysSync(struct Machine *m) {
  sync();
  return 0;
}

static int CheckSyncable(int fildes) {
#ifdef __FreeBSD__
  // FreeBSD doesn't return EINVAL like Linux does when trying to
  // synchronize character devices, e.g. /dev/null. An unresolved
  // question though is if FreeBSD actually does something here.
  struct stat st;
  if (!fstat(fildes, &st) &&    //
      (S_ISCHR(st.st_mode) ||   //
       S_ISFIFO(st.st_mode) ||  //
       S_ISLNK(st.st_mode) ||   //
       S_ISSOCK(st.st_mode))) {
    return einval();
  }
#endif
  return 0;
}

static int SysFsync(struct Machine *m, i32 fildes) {
  if (CheckSyncable(fildes) == -1) return -1;
#ifdef F_FULLSYNC
  int rc;
  // macOS fsync() provides weaker guarantees than Linux fsync()
  // https://mjtsai.com/blog/2022/02/17/apple-ssd-benchmarks-and-f_fullsync/
  if ((rc = fcntl(fildes, F_FULLFSYNC, 0))) {
    // If the FULLFSYNC failed, fall back to attempting an fsync(). It
    // shouldn't be possible for fullfsync to fail on the local file
    // system (on OSX), so failure indicates that FULLFSYNC isn't
    // supported for this file system. So, attempt an fsync and (for
    // now) ignore the overhead of a superfluous fcntl call. It'd be
    // better to detect fullfsync support once and avoid the fcntl call
    // every time sync is called. ──Quoth SQLite (os_unix.c) It's also
    // possible for F_FULLFSYNC to fail on Cosmopolitan Libc when our
    // binary isn't running on macOS.
    rc = fsync(fildes);
  }
  return rc;
#else
  return fsync(fildes);
#endif
}

static int SysFdatasync(struct Machine *m, i32 fildes) {
  if (CheckSyncable(fildes) == -1) return -1;
#ifdef F_FULLSYNC
  int rc;
  if ((rc = fcntl(fildes, F_FULLFSYNC, 0))) {
    rc = fsync(fildes);
  }
  return rc;
#elif defined(__APPLE__)
  // fdatasync() on HFS+ doesn't yet flush the file size if it changed
  // correctly so currently we default to the macro that redefines
  // fdatasync() to fsync(). ──Quoth SQLite (os_unix.c)
  return fsync(fildes);
#elif defined(__HAIKU__)
  // Haiku doesn't have fdatasync() yet
  return fsync(fildes);
#else
  return fdatasync(fildes);
#endif
}

static int SysChdir(struct Machine *m, i64 path) {
  return OverlaysChdir(LoadStr(m, path));
}

static int SysFchdir(struct Machine *m, i32 fildes) {
  return fchdir(fildes);
}

static int XlatLock(int x) {
  switch (x) {
    case LOCK_UN_LINUX:
      return LOCK_UN;
    case LOCK_SH_LINUX:
      return LOCK_SH;
    case LOCK_SH_LINUX | LOCK_NB_LINUX:
      return LOCK_SH | LOCK_NB;
    case LOCK_EX_LINUX:
      return LOCK_EX;
    case LOCK_EX_LINUX | LOCK_NB_LINUX:
      return LOCK_EX | LOCK_NB;
    default:
      LOGF("bad flock() type: %#x", x);
      return einval();
  }
}

static int SysFlock(struct Machine *m, i32 fd, i32 lock) {
  if ((lock = XlatLock(lock)) == -1) return -1;
  return flock(fd, lock);
}

static int SysShutdown(struct Machine *m, i32 fd, i32 how) {
  return shutdown(fd, XlatShutdown(how));
}

static int SysListen(struct Machine *m, i32 fd, i32 backlog) {
  return listen(fd, backlog);
}

static int SysMkdirat(struct Machine *m, i32 dirfd, i64 path, i32 mode) {
  return OverlaysMkdir(GetDirFildes(dirfd), LoadStr(m, path), mode);
}

static int SysMkdir(struct Machine *m, i64 path, i32 mode) {
  return SysMkdirat(m, AT_FDCWD_LINUX, path, mode);
}

static int SysFchmod(struct Machine *m, i32 fd, u32 mode) {
  return fchmod(fd, mode);
}

static int SysFchmodat(struct Machine *m, i32 dirfd, i64 path, u32 mode) {
  return OverlaysChmod(GetDirFildes(dirfd), LoadStr(m, path), mode, 0);
}

static int SysFcntlLock(struct Machine *m, int systemfd, int cmd, i64 arg) {
  int rc;
  int whence;
  int syscmd;
  struct flock flock;
  struct flock_linux flock_linux;
  if (CopyFromUserRead(m, &flock_linux, arg, sizeof(flock_linux))) {
    return -1;
  }
  if (cmd == F_SETLK_LINUX) {
    syscmd = F_SETLK;
  } else if (cmd == F_SETLKW_LINUX) {
    syscmd = F_SETLKW;
  } else {
    syscmd = F_GETLK;
  }
  memset(&flock, 0, sizeof(flock));
  if (Read16(flock_linux.type) == F_RDLCK_LINUX) {
    flock.l_type = F_RDLCK;
  } else if (Read16(flock_linux.type) == F_WRLCK_LINUX) {
    flock.l_type = F_WRLCK;
  } else if (Read16(flock_linux.type) == F_UNLCK_LINUX) {
    flock.l_type = F_UNLCK;
  } else {
    return einval();
  }
  if ((whence = XlatWhence(Read16(flock_linux.whence))) == -1) return -1;
  flock.l_whence = whence;
  flock.l_start = Read64(flock_linux.start);
  flock.l_len = Read64(flock_linux.len);
  RESTARTABLE(rc = fcntl(systemfd, syscmd, &flock));
  if (rc != -1 && syscmd == F_GETLK) {
    if (flock.l_type == F_RDLCK) {
      Write16(flock_linux.type, F_RDLCK_LINUX);
    } else if (flock.l_type == F_WRLCK) {
      Write16(flock_linux.type, F_WRLCK_LINUX);
    } else {
      Write16(flock_linux.type, F_UNLCK_LINUX);
    }
    if (flock.l_whence == SEEK_END) {
      Write16(flock_linux.whence, SEEK_END_LINUX);
    } else if (flock.l_whence == SEEK_CUR) {
      Write16(flock_linux.whence, SEEK_CUR_LINUX);
    } else {
      Write16(flock_linux.whence, SEEK_SET_LINUX);
    }
    Write64(flock_linux.start, flock.l_start);
    Write64(flock_linux.len, flock.l_len);
    Write32(flock_linux.pid, flock.l_pid);
    CopyToUserWrite(m, arg, &flock_linux, sizeof(flock_linux));
  }
  return rc;
}

#ifdef F_GETOWN_EX
static int UnxlatFownerType(int type) {
  if (type == F_OWNER_TID) return F_OWNER_TID_LINUX;
  if (type == F_OWNER_PID) return F_OWNER_PID_LINUX;
  if (type == F_OWNER_PGRP) return F_OWNER_PGRP_LINUX;
  LOGF("unknown f_owner_ex::type %d", type);
  return einval();
}
#endif

#ifdef F_SETOWN
static int SysFcntlSetownEx(struct Machine *m, i32 fildes, i64 addr) {
  const struct f_owner_ex_linux *gowner;
  if (!(gowner = (const struct f_owner_ex_linux *)SchlepR(m, addr,
                                                          sizeof(*gowner)))) {
    return -1;
  }
#ifdef F_SETOWN_EX
  struct f_owner_ex howner;
  switch (Read32(gowner->type)) {
    case F_OWNER_TID_LINUX:
      howner.type = F_OWNER_TID;
      howner.pid = (i32)Read32(gowner->pid);
      break;
    case F_OWNER_PID_LINUX:
      howner.type = F_OWNER_PID;
      howner.pid = (i32)Read32(gowner->pid);
      break;
    case F_OWNER_PGRP_LINUX:
      howner.type = F_OWNER_PGRP;
      howner.pid = (i32)Read32(gowner->pid);
      break;
    default:
      LOGF("unknown f_owner_ex::type %" PRId32, Read32(gowner->type));
      return einval();
  }
  return fcntl(fildes, F_SETOWN_EX, &howner);
#else
  pid_t pid;
  switch (Read32(gowner->type)) {
    case F_OWNER_PID_LINUX:
      pid = (i32)Read32(gowner->pid);
      break;
    case F_OWNER_PGRP_LINUX:
      pid = (i32)-Read32(gowner->pid);
      break;
    case F_OWNER_TID_LINUX:
      // POSIX doesn't specify fcntl() as applying per-thread
      if (!(pid = (i32)Read32(gowner->pid)) || pid == m->system->pid) {
        break;
      }
      // fallthrough
    default:
      LOGF("unknown f_owner_ex::type %" PRId32, Read32(gowner->type));
      return einval();
  }
  return fcntl(fildes, F_SETOWN, pid);
#endif
}
#endif

#ifdef F_GETOWN
static int SysFcntlGetownEx(struct Machine *m, i32 fildes, i64 addr) {
  int rc;
  struct f_owner_ex_linux gowner;
  if (!IsValidMemory(m, addr, sizeof(gowner), PROT_WRITE)) return efault();
#ifdef F_GETOWN_EX
  int type;
  struct f_owner_ex howner;
  if ((rc = fcntl(fildes, F_GETOWN_EX, &howner)) != -1) {
    if ((type = UnxlatFownerType(howner.type)) != -1) {
      Write32(gowner.type, type);
      Write32(gowner.pid, howner.pid);
      CopyToUserWrite(m, addr, &gowner, sizeof(gowner));
    } else {
      rc = -1;
    }
  }
#else
  if ((rc = fcntl(fildes, F_GETOWN)) != -1) {
    if (rc >= 0) {
      Write32(gowner.type, F_OWNER_PID_LINUX);
      Write32(gowner.pid, rc);
    } else {
      Write32(gowner.type, F_OWNER_PGRP_LINUX);
      Write32(gowner.pid, -rc);
    }
    CopyToUserWrite(m, addr, &gowner, sizeof(gowner));
  }
#endif
  return rc;
}
#endif

static int SysFcntl(struct Machine *m, i32 fildes, i32 cmd, i64 arg) {
  int rc, fl;
  struct Fd *fd;
  if (cmd == F_DUPFD_LINUX) {
    return SysDupf(m, fildes, arg, F_DUPFD);
  } else if (cmd == F_DUPFD_CLOEXEC_LINUX) {
    return SysDupf(m, fildes, arg, F_DUPFD_CLOEXEC);
  }
  if (!(fd = GetAndLockFd(m, fildes))) return -1;
  if (cmd == F_GETFD_LINUX) {
    rc = (fd->oflags & O_CLOEXEC) ? FD_CLOEXEC_LINUX : 0;
  } else if (cmd == F_GETFL_LINUX) {
    rc = UnXlatOpenFlags(fd->oflags);
  } else if (cmd == F_SETFD_LINUX) {
    if (!(arg & ~FD_CLOEXEC_LINUX)) {
      if (fcntl(fd->fildes, F_SETFD, arg ? FD_CLOEXEC : 0) != -1) {
        fd->oflags &= ~O_CLOEXEC;
        if (arg) fd->oflags |= O_CLOEXEC;
        rc = 0;
      } else {
        rc = -1;
      }
    } else {
      rc = einval();
    }
  } else if (cmd == F_SETFL_LINUX) {
    fl = XlatOpenFlags(arg & (O_APPEND_LINUX | O_ASYNC_LINUX | O_DIRECT_LINUX |
                              O_NOATIME_LINUX | O_NDELAY_LINUX));
    if (fcntl(fd->fildes, F_SETFL, fl) != -1) {
      fd->oflags &= ~SETFL_FLAGS;
      fd->oflags |= fl;
      rc = 0;
    } else {
      rc = -1;
    }
  } else if (cmd == F_SETLK_LINUX ||   //
             cmd == F_SETLKW_LINUX ||  //
             cmd == F_GETLK_LINUX) {
    UnlockFd(fd);
    return SysFcntlLock(m, fildes, cmd, arg);
#ifdef F_SETOWN
  } else if (cmd == F_SETOWN_LINUX) {
    rc = fcntl(fd->fildes, F_SETOWN, arg);
  } else if (cmd == F_SETOWN_EX_LINUX) {
    rc = SysFcntlSetownEx(m, fd->fildes, arg);
#endif
#ifdef F_GETOWN
  } else if (cmd == F_GETOWN_LINUX) {
    rc = fcntl(fd->fildes, F_GETOWN);
  } else if (cmd == F_GETOWN_EX_LINUX) {
    rc = SysFcntlGetownEx(m, fd->fildes, arg);
#endif
  } else {
    LOGF("missing fcntl() command %" PRId32, cmd);
    rc = einval();
  }
  UnlockFd(fd);
  return rc;
}

static ssize_t SysReadlinkat(struct Machine *m, int dirfd, i64 path,
                             i64 bufaddr, i64 bufsiz) {
  char *buf;
  ssize_t rc;
  // This system call raises EINVAL when "bufsiz is not positive."
  // ──Quoth the Linux Programmer's Manual § readlink(2). Some libc
  // implementations (e.g. Musl) consider it to be posixly incorrect.
  if (bufsiz <= 0) return einval();
  if (!(buf = (char *)malloc(bufsiz))) return -1;
  if ((rc = OverlaysReadlink(GetDirFildes(dirfd), LoadStr(m, path), buf,
                             bufsiz)) != -1) {
    if (CopyToUserWrite(m, bufaddr, buf, rc) == -1) rc = -1;
  }
  free(buf);
  return rc;
}

static int SysChmod(struct Machine *m, i64 path, u32 mode) {
  return SysFchmodat(m, AT_FDCWD_LINUX, path, mode);
}

static int SysTruncate(struct Machine *m, i64 pathaddr, i64 length) {
  int rc, fd;
  const char *path;
  if (length < 0) return einval();
  if (length > NUMERIC_MAX(off_t)) return eoverflow();
  if (!(path = LoadStr(m, pathaddr))) return -1;
  RESTARTABLE(fd = OverlaysOpen(AT_FDCWD, path, O_RDWR | O_CLOEXEC, 0));
  if (fd == -1) return -1;
  rc = ftruncate(fd, length);
  close(fd);
  return rc;
}

static int SysSymlinkat(struct Machine *m, i64 targetpath, i32 newdirfd,
                        i64 linkpath) {
  return OverlaysSymlink(LoadStr(m, targetpath), GetDirFildes(newdirfd),
                         LoadStr(m, linkpath));
}

static int SysSymlink(struct Machine *m, i64 targetpath, i64 linkpath) {
  return SysSymlinkat(m, targetpath, AT_FDCWD_LINUX, linkpath);
}

static int SysReadlink(struct Machine *m, i64 path, i64 bufaddr, u64 size) {
  return SysReadlinkat(m, AT_FDCWD_LINUX, path, bufaddr, size);
}

static int SysMknodat(struct Machine *m, i32 dirfd, i64 path, i32 mode,
                      u64 dev) {
  _Static_assert(S_IFIFO == 0010000, "");   // pipe
  _Static_assert(S_IFCHR == 0020000, "");   // character device
  _Static_assert(S_IFDIR == 0040000, "");   // directory
  _Static_assert(S_IFBLK == 0060000, "");   // block device
  _Static_assert(S_IFREG == 0100000, "");   // regular file
  _Static_assert(S_IFLNK == 0120000, "");   // symbolic link
  _Static_assert(S_IFSOCK == 0140000, "");  // socket
  _Static_assert(S_IFMT == 0170000, "");    // mask of file types above
  if ((mode & S_IFMT) == S_IFIFO) {
    return OverlaysMkfifo(GetDirFildes(dirfd), LoadStr(m, path),
                          mode & ~S_IFMT);
  } else {
    LOGF("mknod mode %#o not supported yet", mode);
    return enosys();
  }
}

static int SysMknod(struct Machine *m, i64 path, i32 mode, u64 dev) {
  return SysMknodat(m, AT_FDCWD_LINUX, path, mode, dev);
}

static int XlatPrio(int x) {
  switch (x) {
    XLAT(0, PRIO_PROCESS);
    XLAT(1, PRIO_PGRP);
    XLAT(2, PRIO_USER);
    default:
      return -1;
  }
}

static int SysGetpriority(struct Machine *m, i32 which, u32 who) {
  int rc;
  errno = 0;
  rc = getpriority(XlatPrio(which), who);
  if (rc == -1 && errno) return -1;
  return MAX(-20, MIN(19, rc)) + 20;
}

static int SysSetpriority(struct Machine *m, i32 which, u32 who, int prio) {
  return setpriority(XlatPrio(which), who, prio);
}

static int XlatUnlinkatFlags(int x) {
  int res = 0;
  if (x & AT_REMOVEDIR_LINUX) {
    res |= AT_REMOVEDIR;
    x &= ~AT_REMOVEDIR_LINUX;
  }
  if (x) {
    LOGF("%s() flags %#x not supported", "unlinkat", x);
    return einval();
  }
  return res;
}

static int SysUnlinkat(struct Machine *m, i32 dirfd, i64 pathaddr, i32 flags) {
  int rc;
  const char *path;
  dirfd = GetDirFildes(dirfd);
  if ((flags = XlatUnlinkatFlags(flags)) == -1) return -1;
  if (!(path = LoadStr(m, pathaddr))) return -1;
  rc = OverlaysUnlink(dirfd, path, flags);
#ifndef __linux
  // POSIX.1 says unlink(directory) raises EPERM but on Linux
  // it always raises EISDIR, which is so much less ambiguous
  if (rc == -1 && !flags && errno == EPERM) {
    struct stat st;
    if (!fstatat(dirfd, path, &st, 0) && S_ISDIR(st.st_mode)) {
      errno = EISDIR;
    } else {
      errno = EPERM;
    }
  }
#endif
  return rc;
}

static int SysUnlink(struct Machine *m, i64 pathaddr) {
  return SysUnlinkat(m, AT_FDCWD_LINUX, pathaddr, 0);
}

static int SysRmdir(struct Machine *m, i64 path) {
  return SysUnlinkat(m, AT_FDCWD_LINUX, path, AT_REMOVEDIR_LINUX);
}

static int SysRenameat(struct Machine *m, int srcdirfd, i64 srcpath,
                       int dstdirfd, i64 dstpath) {
  return OverlaysRename(GetDirFildes(srcdirfd), LoadStr(m, srcpath),
                        GetDirFildes(dstdirfd), LoadStr(m, dstpath));
}

static int SysRename(struct Machine *m, i64 src, i64 dst) {
  return SysRenameat(m, AT_FDCWD_LINUX, src, AT_FDCWD_LINUX, dst);
}

static int XlatLinkatFlags(int x) {
  int res = 0;
  if (x & AT_SYMLINK_NOFOLLOW_LINUX) {
    x &= ~AT_SYMLINK_NOFOLLOW_LINUX;  // default behavior
  }
  if (x & AT_SYMLINK_FOLLOW_LINUX) {
    res |= AT_SYMLINK_FOLLOW;
    x &= ~AT_SYMLINK_FOLLOW_LINUX;
  }
  if (x) {
    LOGF("%s() flags %d not supported", "linkat", x);
    return -1;
  }
  return res;
}

static i32 SysLinkat(struct Machine *m,  //
                     i32 olddirfd,       //
                     i64 oldpath,        //
                     i32 newdirfd,       //
                     i64 newpath,        //
                     i32 flags) {
  return OverlaysLink(GetDirFildes(olddirfd), LoadStr(m, oldpath),
                      GetDirFildes(newdirfd), LoadStr(m, newpath),
                      XlatLinkatFlags(flags));
}

static int SysLink(struct Machine *m, i64 existingpath, i64 newpath) {
  return SysLinkat(m, AT_FDCWD_LINUX, existingpath, AT_FDCWD_LINUX, newpath, 0);
}

static bool CanEmulateExecutableImpl(const char *prog) {
  int fd;
  bool res;
  ssize_t rc;
  char hdr[64];
  struct stat st;
  if ((fd = OverlaysOpen(AT_FDCWD, prog, O_RDONLY | O_CLOEXEC, 0)) == -1) {
    return false;
  }
  unassert(!fstat(fd, &st));
  if (!(st.st_mode & 0111)) {
    LOGF("execve %s needs chmod +x", prog);
    close(fd);
    return false;
  }
#ifdef __HAIKU__
  if (IsHaikuExecutable(fd)) {
    close(fd);
    return false;
  }
#endif
  if ((rc = pread(fd, hdr, 64, 0)) == 64) {
    res = IsSupportedExecutable(prog, hdr);
  } else {
    res = false;
  }
  close(fd);
  return res;
}

static bool CanEmulateExecutable(const char *prog) {
  int err;
  bool res;
  err = errno;
  res = CanEmulateExecutableImpl(prog);
  errno = err;
  return res;
}

static bool IsBlinkSig(struct System *s, int sig) {
  unassert(1 <= sig && sig <= 64);
  return !!(s->blinksigs & (1ull << (sig - 1)));
}

static void ResetTimerDispositions(struct System *s) {
  struct itimerval it;
  memset(&it, 0, sizeof(it));
  setitimer(ITIMER_REAL, &it, 0);
}

static void ResetSignalDispositions(struct System *s) {
  int sig;
  int syssig;
  LOCK(&s->sig_lock);
  for (sig = 1; sig <= 64; ++sig) {
    if (!IsBlinkSig(s, sig) &&
        Read64(s->hands[sig - 1].handler) != SIG_IGN_LINUX &&
        Read64(s->hands[sig - 1].handler) != SIG_DFL_LINUX &&
        (syssig = XlatSignal(sig)) != -1) {
      Write64(s->hands[sig - 1].handler, SIG_DFL_LINUX);
      signal(syssig, SIG_DFL);
    }
  }
  UNLOCK(&s->sig_lock);
}

static void ExecveBlink(struct Machine *m, char *prog, char **argv,
                        char **envp) {
  sigset_t block;
  if (m->system->exec) {
    // it's worth blocking signals on the outside of the if statement
    // since open() during the executable check, might possibly EINTR
    // and the same could apply to calling close() on our cloexec fds
    sigfillset(&block);
    pthread_sigmask(SIG_BLOCK, &block, &m->system->exec_sigmask);
    if (CanEmulateExecutable(prog)) {
      // TODO(jart): Prevent possibility of stack overflow.
      SYS_LOGF("m->system->exec(%s)", prog);
      SysCloseExec(m->system);
      ResetTimerDispositions(m->system);
      ResetSignalDispositions(m->system);
      _Exit(m->system->exec(prog, argv, envp));
    }
    pthread_sigmask(SIG_SETMASK, &m->system->exec_sigmask, 0);
  }
}

static int SysExecve(struct Machine *m, i64 pa, i64 aa, i64 ea) {
  char *prog, **argv, **envp;
  if (!(prog = CopyStr(m, pa))) return -1;
  if (!(argv = CopyStrList(m, aa))) return -1;
  if (!(envp = CopyStrList(m, ea))) return -1;
  LOCK(&m->system->exec_lock);
  ExecveBlink(m, prog, argv, envp);
  SYS_LOGF("execve(%s)", prog);
  execve(prog, argv, envp);
  UNLOCK(&m->system->exec_lock);
  return -1;
}

static int SysWait4(struct Machine *m, int pid, i64 opt_out_wstatus_addr,
                    int options, i64 opt_out_rusage_addr) {
  int rc;
  int wstatus;
  i32 gwstatus;
  u8 gwstatusb[4];
  struct rusage hrusage;
  struct rusage_linux grusage;
  if ((options = XlatWait(options)) == -1) return -1;
  if ((opt_out_wstatus_addr && !IsValidMemory(m, opt_out_wstatus_addr,
                                              sizeof(gwstatusb), PROT_WRITE)) ||
      (opt_out_rusage_addr &&
       !IsValidMemory(m, opt_out_rusage_addr, sizeof(grusage), PROT_WRITE))) {
    return efault();
  }
#ifndef __EMSCRIPTEN__
  RESTARTABLE(rc = wait4(pid, &wstatus, options, &hrusage));
#else
  memset(&hrusage, 0, sizeof(hrusage));
  RESTARTABLE(rc = waitpid(pid, &wstatus, options));
#endif
  if (rc != -1 && rc != 0) {
    if (opt_out_wstatus_addr) {
#ifdef WIFCONTINUED
      if (WIFCONTINUED(wstatus)) {
        gwstatus = 0xffff;
        SYS_LOGF("pid %d continued", rc);
      } else
#endif
          if (WIFEXITED(wstatus)) {
        int exitcode;
        exitcode = WEXITSTATUS(wstatus) & 255;
        gwstatus = exitcode << 8;
        SYS_LOGF("pid %d exited %d", rc, exitcode);
      } else if (WIFSTOPPED(wstatus)) {
        int stopsig;
        stopsig = UnXlatSignal(WSTOPSIG(wstatus));
        gwstatus = stopsig << 8 | 127;
        SYS_LOGF("pid %d stopped %s", rc, DescribeSignal(stopsig));
      } else {
        int termsig;
        unassert(WIFSIGNALED(wstatus));
        termsig = UnXlatSignal(WTERMSIG(wstatus));
        SYS_LOGF("pid %d terminated %s", rc, DescribeSignal(termsig));
        gwstatus = termsig & 127;
#ifdef WCOREDUMP
        if (WCOREDUMP(wstatus)) {
          gwstatus |= 128;
        }
#endif
      }
      Write32(gwstatusb, gwstatus);
      CopyToUserWrite(m, opt_out_wstatus_addr, gwstatusb, sizeof(gwstatusb));
    }
    if (opt_out_rusage_addr) {
      XlatRusageToLinux(&grusage, &hrusage);
      CopyToUserWrite(m, opt_out_rusage_addr, &grusage, sizeof(grusage));
    }
  }
  return rc;
}

static int SysGetrusage(struct Machine *m, i32 resource, i64 rusageaddr) {
  int rc;
  struct rusage hrusage;
  struct rusage_linux grusage;
  if ((rc = getrusage(XlatRusage(resource), &hrusage)) != -1) {
    XlatRusageToLinux(&grusage, &hrusage);
    if (CopyToUserWrite(m, rusageaddr, &grusage, sizeof(grusage)) == -1) {
      rc = -1;
    }
  }
  return rc;
}

static void GetMemLimit(struct Machine *m, int resource,
                        struct rlimit_linux *lux) {
  LOCK(&m->system->mmap_lock);
  memcpy(lux, m->system->rlim + resource, sizeof(*lux));
  UNLOCK(&m->system->mmap_lock);
}

static int SetMemLimit(struct Machine *m, int resource,
                       const struct rlimit_linux *lux) {
  int rc;
  LOCK(&m->system->mmap_lock);
  if (Read64(lux->cur) <= Read64(m->system->rlim[resource].max) &&
      Read64(lux->max) <= Read64(m->system->rlim[resource].max)) {
    memcpy(m->system->rlim + resource, lux, sizeof(*lux));
    rc = 0;
  } else {
    rc = eperm();
  }
  UNLOCK(&m->system->mmap_lock);
  return rc;
}

static int SysGetrlimit(struct Machine *m, i32 resource, i64 rlimitaddr) {
  int rc;
  struct rlimit rlim;
  struct rlimit_linux lux;
  if (resource == RLIMIT_AS_LINUX || resource == RLIMIT_DATA_LINUX) {
    GetMemLimit(m, resource, &lux);
    return CopyToUserWrite(m, rlimitaddr, &lux, sizeof(lux));
  }
  if ((rc = getrlimit(XlatResource(resource), &rlim)) != -1) {
    XlatRlimitToLinux(&lux, &rlim);
    if (CopyToUserWrite(m, rlimitaddr, &lux, sizeof(lux)) == -1) rc = -1;
  }
  return rc;
}

static int SysSetrlimit(struct Machine *m, i32 resource, i64 rlimitaddr) {
  int sysresource;
  struct rlimit rlim;
  const struct rlimit_linux *lux;
  if (!(lux = (const struct rlimit_linux *)SchlepR(m, rlimitaddr,
                                                   sizeof(*lux)))) {
    return -1;
  }
  if (resource == RLIMIT_DATA_LINUX || resource == RLIMIT_AS_LINUX) {
    return SetMemLimit(m, resource, lux);
  }
  if ((sysresource = XlatResource(resource)) == -1) return -1;
  XlatLinuxToRlimit(sysresource, &rlim, lux);
  return setrlimit(sysresource, &rlim);
}

static int SysPrlimit(struct Machine *m, i32 pid, i32 resource,
                      i64 new_rlimit_addr, i64 old_rlimit_addr) {
  if (pid && pid != m->system->pid) {
    return eperm();
  }
#ifndef TINY
  if ((old_rlimit_addr &&
       !IsValidMemory(m, old_rlimit_addr, sizeof(struct rlimit_linux),
                      PROT_WRITE)) &&
      (new_rlimit_addr &&
       !IsValidMemory(m, new_rlimit_addr, sizeof(struct rlimit_linux),
                      PROT_READ))) {
    return efault();
  }
#endif
  if ((old_rlimit_addr && SysGetrlimit(m, resource, old_rlimit_addr) == -1) ||
      (new_rlimit_addr && SysSetrlimit(m, resource, new_rlimit_addr) == -1)) {
    return -1;
  }
  return 0;
}

static int SysSysinfo(struct Machine *m, i64 siaddr) {
  struct sysinfo_linux si;
  if (sysinfo_linux(&si) == -1) return -1;
  CopyToUserWrite(m, siaddr, &si, sizeof(si));
  return 0;
}

static i64 SysGetcwd(struct Machine *m, i64 bufaddr, i64 size) {
  i64 res;
  size_t n;
  char buf[PATH_MAX + 1];
  if (size < 0) return enomem();
  if (OverlaysGetcwd(buf, sizeof(buf))) {
    n = strlen(buf) + 1;
    if (size < n) {
      res = erange();
    } else if (CopyToUserWrite(m, bufaddr, buf, n) != -1) {
      res = n;
    } else {
      res = -1;
    }
  } else {
    res = -1;
  }
  return res;
}

static ssize_t SysGetrandom(struct Machine *m, i64 a, size_t n, int f) {
  char *p;
  ssize_t rc;
  int ignored;
  ignored = GRND_NONBLOCK_LINUX | GRND_RANDOM_LINUX;
  if ((f &= ~ignored)) {
    LOGF("%s() flags %d not supported", "getrandom", f);
    return einval();
  }
  if (n) {
    if (!(p = (char *)malloc(n))) return -1;
    RESTARTABLE(rc = GetRandom(p, n));
    if (rc != -1) {
      if (CopyToUserWrite(m, a, p, rc) == -1) {
        rc = -1;
      }
    }
    free(p);
  } else {
    rc = 0;
  }
  return rc;
}

void OnSignal(int sig, siginfo_t *si, void *uc) {
  EnqueueSignal(g_machine, UnXlatSignal(sig));
}

static int SysSigaction(struct Machine *m, int sig, i64 act, i64 old,
                        u64 sigsetsize) {
  int syssig;
  u64 flags = 0;
  i64 handler = 0;
  bool isignored = false;
  struct sigaction syshand;
  struct sigaction_linux hand;
  u32 supported = SA_SIGINFO_LINUX |    //
                  SA_RESTART_LINUX |    //
                  SA_ONSTACK_LINUX |    //
                  SA_NODEFER_LINUX |    //
                  SA_RESTORER_LINUX |   //
                  SA_RESETHAND_LINUX |  //
#ifdef SA_NOCLDWAIT
                  SA_NOCLDWAIT_LINUX |  //
#endif
                  SA_NOCLDSTOP_LINUX;
  if (sigsetsize != 8) return einval();
  if (!(1 <= sig && sig <= 64)) return einval();
  if (sig == SIGKILL_LINUX || sig == SIGSTOP_LINUX) return einval();
  memset(&hand, 0, sizeof(hand));
  if (old && !IsValidMemory(m, old, sizeof(hand), PROT_WRITE)) return efault();
  if (act) {
    if (CopyFromUserRead(m, &hand, act, sizeof(hand)) == -1) return -1;
    flags = Read64(hand.flags);
    handler = Read64(hand.handler);
    if (handler == SIG_IGN_LINUX) {
      flags &= ~SA_NOCLDWAIT_LINUX;
    }
    if (flags & ~supported) {
      LOGF("unrecognized sigaction() flags: %#" PRIx64, flags & ~supported);
      return einval();
    }
    switch (handler) {
      case SIG_DFL_LINUX:
        isignored = IsSignalIgnoredByDefault(sig);
        break;
      case SIG_IGN_LINUX:
        isignored = true;
        break;
      default:
        if (!(flags & SA_RESTORER_LINUX)) {
          LOGF("sigaction() flags missing SA_RESTORER");
          return einval();
        }
        if (!IsValidMemory(m, Read64(hand.restorer), 1, PROT_EXEC)) {
          LOGF("sigaction() SA_RESTORER at %" PRIx64 " isn't executable",
               Read64(hand.restorer));
          return efault();
        }
        break;
    }
  }
  LOCK(&m->system->sig_lock);
  if (old) {
    CopyToUserWrite(m, old, &m->system->hands[sig - 1], sizeof(hand));
  }
  if (act) {
    m->system->hands[sig - 1] = hand;
    if (isignored) {
      m->signals &= ~(1ull << (sig - 1));
    }
    if ((syssig = XlatSignal(sig)) != -1 && !IsBlinkSig(m->system, sig)) {
      sigfillset(&syshand.sa_mask);
      syshand.sa_flags = SA_SIGINFO;
      if (flags & SA_NOCLDSTOP_LINUX) syshand.sa_flags |= SA_NOCLDSTOP;
#ifdef SA_NOCLDWAIT
      if (flags & SA_NOCLDWAIT_LINUX) syshand.sa_flags |= SA_NOCLDWAIT;
#endif
      switch (handler) {
        case SIG_DFL_LINUX:
          syshand.sa_handler = SIG_DFL;
          break;
        case SIG_IGN_LINUX:
          syshand.sa_handler = SIG_IGN;
          break;
        default:
          syshand.sa_sigaction = OnSignal;
          break;
      }
      if (sigaction(syssig, &syshand, 0)) {
        LOGF("system sigaction(%s) returned %s", DescribeSignal(sig),
             DescribeHostErrno(errno));
      }
    }
  }
  UNLOCK(&m->system->sig_lock);
  return 0;
}

static int SysGetitimer(struct Machine *m, int which, i64 curvaladdr) {
  int rc;
  struct itimerval it;
  struct itimerval_linux git;
  if ((rc = getitimer(UnXlatItimer(which), &it)) != -1) {
    XlatItimervalToLinux(&git, &it);
    CopyToUserWrite(m, curvaladdr, &git, sizeof(git));
  }
  return rc;
}

static int SysSetitimer(struct Machine *m, int which, i64 neuaddr,
                        i64 oldaddr) {
  int rc;
  struct itimerval_linux gold;
  struct itimerval neu, *neup, old;
  const struct itimerval_linux *git = 0;
  if ((neuaddr && !(git = (const struct itimerval_linux *)SchlepR(
                        m, neuaddr, sizeof(*git)))) ||
      (oldaddr && !IsValidMemory(m, oldaddr, sizeof(gold), PROT_WRITE))) {
    return efault();
  }
  if (git) {
    XlatLinuxToItimerval(&neu, git);
    neup = &neu;
  } else {
    neup = 0;
  }
  if ((rc = setitimer(UnXlatItimer(which), neup, &old)) != -1) {
    if (oldaddr) {
      XlatItimervalToLinux(&gold, &old);
      CopyToUserWrite(m, oldaddr, &gold, sizeof(gold));
    }
  }
  return rc;
}

static int SysNanosleep(struct Machine *m, i64 req, i64 rem) {
  int rc;
  struct timespec hreq, hrem;
  struct timespec_linux gtimespec;
  if (CopyFromUserRead(m, &gtimespec, req, sizeof(gtimespec)) == -1) return -1;
  hreq.tv_sec = Read64(gtimespec.sec);
  hreq.tv_nsec = Read64(gtimespec.nsec);
TryAgain:
  rc = nanosleep(&hreq, &hrem);
  if (rc == -1 && errno == EINTR) {
    if (CheckInterrupt(m, false)) {
      if (rem) {
        Write64(gtimespec.sec, hrem.tv_sec);
        Write64(gtimespec.nsec, hrem.tv_nsec);
        CopyToUserWrite(m, rem, &gtimespec, sizeof(gtimespec));
      }
    } else {
      hreq = hrem;
      goto TryAgain;
    }
  }
  return rc;
}

static int SysClockNanosleep(struct Machine *m, int clock, int flags,
                             i64 reqaddr, i64 remaddr) {
  int rc;
  clock_t sysclock;
  struct timespec req, rem;
  struct timespec_linux gtimespec;
  if (XlatClock(clock, &sysclock) == -1) return -1;
  if (flags & ~TIMER_ABSTIME_LINUX) return einval();
  if (CopyFromUserRead(m, &gtimespec, reqaddr, sizeof(gtimespec)) == -1) {
    return -1;
  }
  req.tv_sec = Read64(gtimespec.sec);
  req.tv_nsec = Read64(gtimespec.nsec);
TryAgain:
#if defined(TIMER_ABSTIME) && !defined(__OpenBSD__)
  flags = flags & TIMER_ABSTIME_LINUX ? TIMER_ABSTIME : 0;
  if ((rc = clock_nanosleep(sysclock, flags, &req, &rem))) {
    errno = rc;
    rc = -1;
  }
#else
  if (!flags) {
    if (sysclock == CLOCK_REALTIME) {
      rc = nanosleep(&req, &rem);
    } else {
      rc = einval();
    }
  } else {
    struct timespec now;
    if (!(rc = clock_gettime(sysclock, &now))) {
      if (CompareTime(now, req) < 0) {
        req = SubtractTime(req, now);
        rc = nanosleep(&req, &rem);
      } else {
        rc = 0;
      }
    }
  }
#endif
  if (rc == -1 && errno == EINTR) {
    if (CheckInterrupt(m, false)) {
      if (!flags && remaddr) {
        Write64(gtimespec.sec, rem.tv_sec);
        Write64(gtimespec.nsec, rem.tv_nsec);
        CopyToUserWrite(m, remaddr, &gtimespec, sizeof(gtimespec));
      }
    } else {
      if (!flags) {
        req = rem;
      }
      goto TryAgain;
    }
  }
  return rc;
}

static int SigsuspendActual(struct Machine *m, u64 mask) {
  int rc;
  u64 oldmask;
  sigset_t block_host, oldmask_host;
  oldmask = m->sigmask;
  unassert(!sigfillset(&block_host));
  unassert(!pthread_sigmask(SIG_BLOCK, &block_host, &oldmask_host));
  assert(sigismember(&oldmask_host, SIGSYS) == 0);
  m->sigmask = mask;
  SIG_LOGF("sigmask push %" PRIx64, m->sigmask);
  m->issigsuspend = true;
  NORESTART(rc, sigsuspend(&oldmask_host));
  m->issigsuspend = false;
  unassert(!pthread_sigmask(SIG_SETMASK, &oldmask_host, 0));
  m->sigmask = oldmask;
  SIG_LOGF("sigmask pop %" PRIx64, m->sigmask);
  return rc;
}

static int SigsuspendPolyfill(struct Machine *m, u64 mask) {
  long nanos;
  u64 oldmask;
  struct timespec ts;
  oldmask = m->sigmask;
  m->sigmask = mask;
  nanos = 1;
  while (!CheckInterrupt(m, false)) {
    if (nanos > 256) {
      if (nanos < 10 * 1000) {
        sched_yield();
      } else {
        ts = FromNanoseconds(nanos);
        if (nanosleep(&ts, 0)) {
          unassert(errno == EINTR);
          continue;
        }
      }
    }
    if (nanos < 100 * 1000 * 1000) {
      nanos <<= 1;
    }
  }
  m->sigmask = oldmask;
  return -1;
}

static int SysSigsuspend(struct Machine *m, i64 maskaddr, i64 sigsetsize) {
  u8 word[8];
  if (sigsetsize != 8) return einval();
  if (CopyFromUserRead(m, word, maskaddr, 8) == -1) return -1;
#ifdef __EMSCRIPTEN__
  return SigsuspendPolyfill(m, Read64(word));
#else
  return SigsuspendActual(m, Read64(word));
#endif
}

static int SysSigaltstack(struct Machine *m, i64 newaddr, i64 oldaddr) {
  bool isonstack;
  int supported, unsupported;
  const struct sigaltstack_linux *ss = 0;
  supported = SS_ONSTACK_LINUX | SS_DISABLE_LINUX | SS_AUTODISARM_LINUX;
  isonstack =
      (~Read32(m->sigaltstack.flags) & SS_DISABLE_LINUX) &&
      Read64(m->sp) >= Read64(m->sigaltstack.sp) &&
      Read64(m->sp) <= Read64(m->sigaltstack.sp) + Read64(m->sigaltstack.size);
  if (newaddr) {
    if (isonstack) {
      LOGF("can't change sigaltstack whilst on sigaltstack");
      return eperm();
    }
    if (!(ss = (const struct sigaltstack_linux *)SchlepR(m, newaddr,
                                                         sizeof(*ss)))) {
      LOGF("couldn't schlep new sigaltstack: %#" PRIx64, newaddr);
      return -1;
    }
    if ((unsupported = Read32(ss->flags) & ~supported)) {
      LOGF("unsupported sigaltstack flags: %#x", unsupported);
      return einval();
    }
    if (~Read32(ss->flags) & SS_DISABLE_LINUX) {
      if (Read64(ss->size) < MINSIGSTKSZ_LINUX) {
        LOGF("sigaltstack ss_size=%#" PRIx64 " must be at least %#x",
             Read64(ss->size), MINSIGSTKSZ_LINUX);
        return enomem();
      }
      if (!IsValidMemory(m, Read64(ss->sp), Read64(ss->size),
                         PROT_READ | PROT_WRITE)) {
        LOGF("sigaltstack ss_sp=%#" PRIx64 " ss_size=%#" PRIx64
             " didn't exist with read+write permission",
             Read64(ss->sp), Read64(ss->size));
        return efault();
      }
    }
  }
  if (oldaddr) {
    Write32(m->sigaltstack.flags,
            Read32(m->sigaltstack.flags) & ~SS_ONSTACK_LINUX);
    if (isonstack) {
      Write32(m->sigaltstack.flags,
              Read32(m->sigaltstack.flags) | SS_ONSTACK_LINUX);
    }
    CopyToUserWrite(m, oldaddr, &m->sigaltstack, sizeof(m->sigaltstack));
  }
  if (ss) {
    memcpy(&m->sigaltstack, ss, sizeof(*ss));
  }
  return 0;
}

static int SysClockGettime(struct Machine *m, int clock, i64 ts) {
  int rc;
  clock_t sysclock;
  struct timespec htimespec;
  struct timespec_linux gtimespec;
  if (XlatClock(clock, &sysclock) == -1) return -1;
  if ((rc = clock_gettime(sysclock, &htimespec)) != -1) {
    if (ts) {
      Write64(gtimespec.sec, htimespec.tv_sec);
      Write64(gtimespec.nsec, htimespec.tv_nsec);
      CopyToUserWrite(m, ts, &gtimespec, sizeof(gtimespec));
    }
  }
  return rc;
}

static int SysClockSettime(struct Machine *m, int clock, i64 ts) {
  clock_t sysclock;
  struct timespec ht;
  const struct timespec_linux *gt;
  if (XlatClock(clock, &sysclock) == -1) return -1;
  if ((gt = (const struct timespec_linux *)SchlepR(m, ts, sizeof(*gt)))) {
    ht.tv_sec = Read64(gt->sec);
    ht.tv_nsec = Read64(gt->nsec);
  }
  return clock_settime(sysclock, &ht);
}

static int SysClockGetres(struct Machine *m, int clock, i64 ts) {
  int rc;
  clock_t sysclock;
  struct timespec htimespec;
  struct timespec_linux gtimespec;
  if (XlatClock(clock, &sysclock) == -1) return -1;
  if ((rc = clock_getres(sysclock, &htimespec)) != -1) {
    if (ts) {
      Write64(gtimespec.sec, htimespec.tv_sec);
      Write64(gtimespec.nsec, htimespec.tv_nsec);
      CopyToUserWrite(m, ts, &gtimespec, sizeof(gtimespec));
    }
  }
  return rc;
}

static int SysGettimeofday(struct Machine *m, i64 tv, i64 tz) {
  int rc;
  struct timeval htimeval;
  struct timezone htimezone;
  struct timeval_linux gtimeval;
  struct timezone_linux gtimezone;
  if ((rc = gettimeofday(&htimeval, tz ? &htimezone : 0)) != -1) {
    Write64(gtimeval.sec, htimeval.tv_sec);
    Write64(gtimeval.usec, htimeval.tv_usec);
    if (CopyToUserWrite(m, tv, &gtimeval, sizeof(gtimeval)) == -1) {
      return -1;
    }
    if (tz) {
      Write32(gtimezone.minuteswest, htimezone.tz_minuteswest);
      Write32(gtimezone.dsttime, htimezone.tz_dsttime);
      if (CopyToUserWrite(m, tz, &gtimezone, sizeof(gtimezone)) == -1) {
        return -1;
      }
    }
  }
  return rc;
}

static i64 SysTime(struct Machine *m, i64 addr) {
  u8 buf[8];
  time_t secs;
  if ((secs = time(0)) == (time_t)-1) return -1;
  if (addr) {
    Write64(buf, secs);
    if (CopyToUserWrite(m, addr, buf, sizeof(buf)) == -1) return -1;
  }
  return secs;
}

static i64 SysTimes(struct Machine *m, i64 bufaddr) {
  // no conversion needed thanks to getauxval(AT_CLKTCK)
  clock_t res;
  struct tms tms;
  struct tms_linux gtms;
  if ((res = times(&tms)) == (clock_t)-1) return -1;
  Write64(gtms.utime, tms.tms_utime);
  Write64(gtms.stime, tms.tms_stime);
  Write64(gtms.cutime, tms.tms_cutime);
  Write64(gtms.cstime, tms.tms_cstime);
  if (CopyToUserWrite(m, bufaddr, &gtms, sizeof(gtms)) == -1) return -1;
  return res;
}

static struct timespec ConvertUtimeTimespec(const struct timespec_linux *tv) {
  struct timespec ts;
  switch (Read64(tv->nsec)) {
    case UTIME_NOW_LINUX:
      ts.tv_sec = 0;
      ts.tv_nsec = UTIME_NOW;
      return ts;
    case UTIME_OMIT_LINUX:
      ts.tv_sec = 0;
      ts.tv_nsec = UTIME_OMIT;
      return ts;
    default:
      ts.tv_sec = Read64(tv->sec);
      ts.tv_nsec = Read64(tv->nsec);
      return ts;
  }
}

static struct timespec ConvertUtimeTimeval(const struct timeval_linux *tv) {
  i64 x;
  struct timespec ts;
  switch ((x = Read64(tv->usec))) {
    case UTIME_NOW_LINUX:
      ts.tv_sec = 0;
      ts.tv_nsec = UTIME_NOW;
      return ts;
    case UTIME_OMIT_LINUX:
      ts.tv_sec = 0;
      ts.tv_nsec = UTIME_OMIT;
      return ts;
    default:
      ts.tv_sec = Read64(tv->sec);
      if (0 <= x && x < 1000000) {
        ts.tv_nsec = x * 1000;
      } else {
        // make sure system call will einval
        // should not overlap with magnums above
        ts.tv_nsec = 1000000666;
      }
      return ts;
  }
}

static void ConvertUtimeTimespecs(struct timespec *ts,
                                  const struct timespec_linux *tv) {
  ts[0] = ConvertUtimeTimespec(tv + 0);
  ts[1] = ConvertUtimeTimespec(tv + 1);
}

static void ConvertUtimeTimevals(struct timespec *ts,
                                 const struct timeval_linux *tv) {
  ts[0] = ConvertUtimeTimeval(tv + 0);
  ts[1] = ConvertUtimeTimeval(tv + 1);
}

static int XlatUtimensatFlags(int x) {
  int res = 0;
  if (x & AT_SYMLINK_FOLLOW_LINUX) {
    x &= ~AT_SYMLINK_FOLLOW_LINUX;  // default behavior
  }
  if (x & AT_SYMLINK_NOFOLLOW_LINUX) {
    res |= AT_SYMLINK_NOFOLLOW;
    x &= ~AT_SYMLINK_NOFOLLOW_LINUX;
  }
  if (x) {
    LOGF("%s() flags %d not supported", "utimensat", x);
    return -1;
  }
  return res;
}

static int SysUtime(struct Machine *m, i64 pathaddr, i64 timesaddr) {
  const char *path;
  struct timespec ts[2];
  const struct utimbuf_linux *t;
  if (!(path = LoadStr(m, pathaddr))) return -1;
  if (!timesaddr) return OverlaysUtime(AT_FDCWD, path, 0, 0);
  if ((t = (const struct utimbuf_linux *)SchlepR(m, timesaddr, sizeof(*t)))) {
    ts[0].tv_sec = Read64(t->actime);
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = Read64(t->modtime);
    ts[1].tv_nsec = 0;
    return OverlaysUtime(AT_FDCWD, path, ts, 0);
  } else {
    return -1;
  }
}

static int SysUtimes(struct Machine *m, i64 pathaddr, i64 tvsaddr) {
  const char *path;
  struct timespec ts[2];
  const struct timeval_linux *tv;
  if (!(path = LoadStr(m, pathaddr))) return -1;
  if (!tvsaddr) return OverlaysUtime(AT_FDCWD, path, 0, 0);
  if ((tv = (const struct timeval_linux *)SchlepR(
           m, tvsaddr, sizeof(struct timeval_linux) * 2))) {
    ConvertUtimeTimevals(ts, tv);
    return OverlaysUtime(AT_FDCWD, path, ts, 0);
  } else {
    return -1;
  }
}

static int SysFutimesat(struct Machine *m, i32 dirfd, i64 pathaddr,
                        i64 tvsaddr) {
  const char *path;
  struct timespec ts[2];
  const struct timeval_linux *tv;
  if (!(path = LoadStr(m, pathaddr))) return -1;
  if (!tvsaddr) return OverlaysUtime(GetDirFildes(dirfd), path, 0, 0);
  if ((tv = (const struct timeval_linux *)SchlepR(
           m, tvsaddr, sizeof(struct timeval_linux) * 2))) {
    ConvertUtimeTimevals(ts, tv);
    return OverlaysUtime(GetDirFildes(dirfd), path, ts, 0);
  } else {
    return -1;
  }
}

static int SysUtimensat(struct Machine *m, i32 fd, i64 pathaddr, i64 tvsaddr,
                        i32 flags) {
  const char *path;
  struct timespec ts[2], *tsp;
  const struct timespec_linux *tv;
  if (!pathaddr) {
    path = 0;
  } else if (!(path = LoadStr(m, pathaddr))) {
    return -1;
  }
  if (tvsaddr) {
    if ((tv = (const struct timespec_linux *)SchlepR(
             m, tvsaddr, sizeof(struct timespec_linux) * 2))) {
      ConvertUtimeTimespecs(ts, tv);
      tsp = ts;
    } else {
      return -1;
    }
  } else {
    tsp = 0;
  }
  if ((flags = XlatUtimensatFlags(flags)) == -1) return -1;
  if (path) {
    return OverlaysUtime(GetDirFildes(fd), path, tsp, flags);
  } else {
    if (flags) {
      LOGF("%s() flags %d not supported", "utimensat(path=null)", flags);
      return einval();
    }
    return futimens(fd, tsp);
  }
}

static int LoadFdSet(struct Machine *m, int nfds, fd_set *fds, i64 addr) {
  int fd;
  const u8 *p;
  if ((p = (const u8 *)SchlepRW(m, addr, FD_SETSIZE_LINUX / 8))) {
    FD_ZERO(fds);
    for (fd = 0; fd < nfds; ++fd) {
      if (p[fd >> 3] & (1 << (fd & 7))) {
        FD_SET(fd, fds);
      }
    }
    return 0;
  } else {
    return -1;
  }
}

static int SaveFdSet(struct Machine *m, int nfds, const fd_set *fds, i64 addr) {
  u8 *p;
  int fd;
  if (!(p = (u8 *)calloc(1, FD_SETSIZE_LINUX / 8))) return -1;
  for (fd = 0; fd < nfds; ++fd) {
    if (FD_ISSET(fd, fds)) {
      p[fd >> 3] |= 1 << (fd & 7);
    }
  }
  CopyToUserWrite(m, addr, p, FD_SETSIZE_LINUX / 8);
  free(p);
  return 0;
}

static i32 Select(struct Machine *m,          //
                  i32 nfds,                   //
                  i64 readfds_addr,           //
                  i64 writefds_addr,          //
                  i64 exceptfds_addr,         //
                  struct timespec *timeoutp,  //
                  const u64 *sigmaskp_guest) {
  int rc;
  i32 setsize;
  u64 oldmask_guest = 0;
  sigset_t block, oldmask;
  fd_set readfds, writefds, exceptfds;
  fd_set *readfdsp, *writefdsp, *exceptfdsp;
  struct timespec timeout, started, elapsed;
  if (timeoutp) {
    started = GetTime();
    timeout = *timeoutp;
  } else {
    memset(&timeout, 0, sizeof(timeout));
  }
  setsize = MIN(FD_SETSIZE, FD_SETSIZE_LINUX);
  if (nfds < 0 || nfds > setsize) {
    LOGF("select() nfds=%d can't exceed %d on this platform", nfds, setsize);
    return einval();
  }
  if (readfds_addr) {
    if (LoadFdSet(m, nfds, &readfds, readfds_addr) != -1) {
      readfdsp = &readfds;
    } else {
      LOGF("select() bad %s memory", "readfds");
      return -1;
    }
  } else {
    readfdsp = 0;
  }
  if (writefds_addr) {
    if (LoadFdSet(m, nfds, &writefds, writefds_addr) != -1) {
      writefdsp = &writefds;
    } else {
      LOGF("select() bad %s memory", "writefds");
      return -1;
    }
  } else {
    writefdsp = 0;
  }
  if (exceptfds_addr) {
    if (LoadFdSet(m, nfds, &exceptfds, exceptfds_addr) != -1) {
      exceptfdsp = &exceptfds;
    } else {
      LOGF("select() bad %s memory", "exceptfds");
      return -1;
    }
  } else {
    exceptfdsp = 0;
  }
  unassert(!sigfillset(&block));
  unassert(!pthread_sigmask(SIG_BLOCK, &block, &oldmask));
  if (sigmaskp_guest) {
    oldmask_guest = m->sigmask;
    m->sigmask = *sigmaskp_guest;
    SIG_LOGF("sigmask push %" PRIx64, m->sigmask);
  }
  NORESTART(rc,
            pselect(nfds, readfdsp, writefdsp, exceptfdsp, timeoutp, &oldmask));
  if (sigmaskp_guest) {
    m->sigmask = oldmask_guest;
    SIG_LOGF("sigmask pop %" PRIx64, m->sigmask);
  }
  unassert(!pthread_sigmask(SIG_SETMASK, &oldmask, 0));
  if (rc != -1) {
    if ((readfds_addr && SaveFdSet(m, nfds, &readfds, readfds_addr) == -1) ||
        (writefds_addr && SaveFdSet(m, nfds, &writefds, writefds_addr) == -1) ||
        (exceptfds_addr &&
         SaveFdSet(m, nfds, &exceptfds, exceptfds_addr) == -1)) {
      return -1;
    }
  }
  if (timeoutp) {
    elapsed = SubtractTime(GetTime(), started);
    if (CompareTime(elapsed, timeout) < 0) {
      *timeoutp = SubtractTime(timeout, elapsed);
    } else {
      memset(timeoutp, 0, sizeof(*timeoutp));
    }
  }
  return rc;
}

static i32 SysSelect(struct Machine *m, i32 nfds, i64 readfds_addr,
                     i64 writefds_addr, i64 exceptfds_addr, i64 timeout_addr) {
  i32 rc;
  struct timespec timeout, *timeoutp;
  struct timeval_linux timeout_linux;
  const struct timeval_linux *timeoutp_linux;
  if (timeout_addr) {
    if ((timeoutp_linux = (const struct timeval_linux *)SchlepRW(
             m, timeout_addr, sizeof(*timeoutp_linux)))) {
      timeout.tv_sec = Read64(timeoutp_linux->sec);
      timeout.tv_nsec = Read64(timeoutp_linux->usec);
      if (0 <= timeout.tv_nsec && timeout.tv_nsec < 1000000) {
        timeout.tv_nsec *= 1000;
        timeoutp = &timeout;
      } else {
        return einval();
      }
    } else {
      return -1;
    }
  } else {
    timeoutp = 0;
    memset(&timeout, 0, sizeof(timeout));
  }
  rc =
      Select(m, nfds, readfds_addr, writefds_addr, exceptfds_addr, timeoutp, 0);
  if (timeout_addr && (rc != -1 || errno == EINTR)) {
    Write64(timeout_linux.sec, timeout.tv_sec);
    Write64(timeout_linux.usec, (timeout.tv_nsec + 999) / 1000);
    CopyToUserWrite(m, timeout_addr, &timeout_linux, sizeof(timeout_linux));
  }
  return rc;
}

static i32 SysPselect(struct Machine *m, i32 nfds, i64 readfds_addr,
                      i64 writefds_addr, i64 exceptfds_addr, i64 timeout_addr,
                      i64 pselect6_addr) {
  i32 rc;
  u64 sigmask, *sigmaskp;
  const struct sigset_linux *sm;
  const struct pselect6_linux *ps;
  struct timespec timeout, *timeoutp;
  struct timespec_linux timeout_linux;
  const struct timespec_linux *timeoutp_linux;
  if (timeout_addr) {
    if ((timeoutp_linux = (const struct timespec_linux *)SchlepRW(
             m, timeout_addr, sizeof(*timeoutp_linux)))) {
      timeout.tv_sec = Read64(timeoutp_linux->sec);
      timeout.tv_nsec = Read64(timeoutp_linux->nsec);
      timeoutp = &timeout;
    } else {
      return -1;
    }
  } else {
    timeoutp = 0;
    memset(&timeout, 0, sizeof(timeout));
  }
  if (pselect6_addr) {
    if ((ps = (const struct pselect6_linux *)SchlepR(m, pselect6_addr,
                                                     sizeof(*ps)))) {
      if (Read64(ps->sigmaskaddr)) {
        if (Read64(ps->sigmasksize) == 8) {
          if ((sm = (const struct sigset_linux *)SchlepR(
                   m, Read64(ps->sigmaskaddr), sizeof(*sm)))) {
            sigmask = Read64(sm->sigmask);
            sigmaskp = &sigmask;
          } else {
            return -1;
          }
        } else {
          return einval();
        }
      } else {
        sigmaskp = 0;
      }
    } else {
      return -1;
    }
  } else {
    sigmaskp = 0;
  }
  rc = Select(m, nfds, readfds_addr, writefds_addr, exceptfds_addr, timeoutp,
              sigmaskp);
  if (timeout_addr && (rc != -1 || errno == EINTR)) {
    Write64(timeout_linux.sec, timeout.tv_sec);
    Write64(timeout_linux.nsec, timeout.tv_nsec);
    CopyToUserWrite(m, timeout_addr, &timeout_linux, sizeof(timeout_linux));
  }
  return rc;
}

static int Poll(struct Machine *m, i64 fdsaddr, u64 nfds,
                struct timespec deadline) {
  long i;
  u64 gfdssize;
  struct Fd *fd;
  int fildes, rc, ev;
  struct pollfd hfds[1];
  struct pollfd_linux *gfds;
  struct timespec now, wait, remain;
  int (*poll_impl)(struct pollfd *, nfds_t, int);
  if (!mulo(nfds, sizeof(struct pollfd_linux), &gfdssize) &&
      gfdssize <= 0x7ffff000) {
    if ((gfds = (struct pollfd_linux *)malloc(gfdssize))) {
      rc = 0;
      CopyFromUserRead(m, gfds, fdsaddr, gfdssize);
      for (;;) {
        for (i = 0; i < nfds; ++i) {
        TryAgain:
          if (CheckInterrupt(m, false)) {
            rc = eintr();
            break;
          }
          fildes = Read32(gfds[i].fd);
          LOCK(&m->system->fds.lock);
          if ((fd = GetFd(&m->system->fds, fildes))) {
            unassert(fd->cb);
            unassert(poll_impl = fd->cb->poll);
          } else {
            poll_impl = 0;
          }
          UNLOCK(&m->system->fds.lock);
          if (fd) {
            hfds[0].fd = fildes;
            ev = Read16(gfds[i].events);
            hfds[0].events = (((ev & POLLIN_LINUX) ? POLLIN : 0) |
                              ((ev & POLLOUT_LINUX) ? POLLOUT : 0) |
                              ((ev & POLLPRI_LINUX) ? POLLPRI : 0));
            switch (poll_impl(hfds, 1, 0)) {
              case 0:
                Write16(gfds[i].revents, 0);
                break;
              case 1:
                ++rc;
                ev = 0;
                if (hfds[0].revents & POLLIN) ev |= POLLIN_LINUX;
                if (hfds[0].revents & POLLPRI) ev |= POLLPRI_LINUX;
                if (hfds[0].revents & POLLOUT) ev |= POLLOUT_LINUX;
                if (hfds[0].revents & POLLERR) ev |= POLLERR_LINUX;
                if (hfds[0].revents & POLLHUP) ev |= POLLHUP_LINUX;
                if (hfds[0].revents & POLLNVAL) ev |= POLLERR_LINUX;
                if (!ev) ev |= POLLERR_LINUX;
                Write16(gfds[i].revents, ev);
                break;
              case -1:
                if (errno == EINTR) {
                  goto TryAgain;
                }
                ++rc;
                Write16(gfds[i].revents, POLLERR_LINUX);
                break;
              default:
                break;
            }
          } else {
            Write16(gfds[i].revents, POLLNVAL_LINUX);
          }
        }
        if (rc || CompareTime((now = GetTime()), deadline) >= 0) {
          break;
        }
        wait = FromMilliseconds(kPollingMs);
        remain = SubtractTime(deadline, now);
        if (CompareTime(remain, wait) < 0) {
          wait = remain;
        }
        nanosleep(&wait, 0);
      }
      if (rc != -1) {
        CopyToUserWrite(m, fdsaddr, gfds, nfds * sizeof(*gfds));
      }
    } else {
      rc = enomem();
    }
    free(gfds);
    return rc;
  } else {
    return einval();
  }
}

static int SysPoll(struct Machine *m, i64 fdsaddr, u64 nfds, i32 timeout_ms) {
  struct timespec deadline;
  if (timeout_ms < 0) {
    deadline = GetMaxTime();
  } else {
    deadline = AddTime(GetTime(), FromMilliseconds(timeout_ms));
  }
  return Poll(m, fdsaddr, nfds, deadline);
}

static int SysPpoll(struct Machine *m, i64 fdsaddr, u64 nfds, i64 timeoutaddr,
                    i64 sigmaskaddr, u64 sigsetsize) {
  int rc;
  u64 oldmask = 0;
  const struct sigset_linux *sm;
  const struct timespec_linux *gt;
  struct timespec_linux timeout_linux;
  struct timespec now, timeout, remain, deadline;
  if (sigmaskaddr) {
    if (sigsetsize != 8) return einval();
    if ((sm = (const struct sigset_linux *)SchlepR(m, sigmaskaddr,
                                                   sizeof(*sm)))) {
      oldmask = m->sigmask;
      m->sigmask = Read64(sm->sigmask);
      SIG_LOGF("sigmask push %" PRIx64, m->sigmask);
    } else {
      return -1;
    }
  }
  if (!CheckInterrupt(m, false)) {
    if (timeoutaddr) {
      if ((gt = (const struct timespec_linux *)SchlepRW(m, timeoutaddr,
                                                        sizeof(*gt)))) {
        timeout.tv_sec = Read64(gt->sec);
        timeout.tv_nsec = Read64(gt->nsec);
        if (timeout.tv_nsec >= 1000000000) {
          return einval();
        }
      } else {
        return -1;
      }
      deadline = AddTime(GetTime(), timeout);
      rc = Poll(m, fdsaddr, nfds, deadline);
      if (rc != -1 || errno == EINTR) {
        now = GetTime();
        if (CompareTime(now, deadline) >= 0) {
          remain = FromMilliseconds(0);
        } else {
          remain = SubtractTime(deadline, now);
        }
        Write64(timeout_linux.sec, remain.tv_sec);
        Write64(timeout_linux.nsec, remain.tv_nsec);
        CopyToUserWrite(m, timeoutaddr, &timeout_linux, sizeof(timeout_linux));
      }
    } else {
      rc = Poll(m, fdsaddr, nfds, GetMaxTime());
    }
  } else {
    rc = -1;
  }
  if (sigmaskaddr) {
    m->sigmask = oldmask;
    SIG_LOGF("sigmask pop %" PRIx64, m->sigmask);
  }
  return rc;
}

static int SysSigprocmask(struct Machine *m, int how, i64 setaddr,
                          i64 oldsetaddr, u64 sigsetsize) {
  u64 set;
  u8 word[8];
  const u8 *neu;
  if (sigsetsize != 8) {
    return einval();
  }
  if (how != SIG_BLOCK_LINUX &&    //
      how != SIG_UNBLOCK_LINUX &&  //
      how != SIG_SETMASK_LINUX) {
    return einval();
  }
  if (setaddr) {
    if (!(neu = (const u8 *)SchlepR(m, setaddr, 8))) {
      return -1;
    }
  } else {
    neu = 0;
  }
  if (oldsetaddr) {
    SIG_LOGF("sigmask read %" PRIx64, m->sigmask);
    Write64(word, m->sigmask);
    if (CopyToUserWrite(m, oldsetaddr, word, 8) == -1) {
      return -1;
    }
  }
  if (setaddr) {
    set = Read64(neu);
    if (how == SIG_BLOCK_LINUX) {
      m->sigmask |= set;
    } else if (how == SIG_UNBLOCK_LINUX) {
      m->sigmask &= ~set;
    } else if (how == SIG_SETMASK_LINUX) {
      m->sigmask = set;
    } else {
      __builtin_unreachable();
    }
    SIG_LOGF("sigmask becomes %" PRIx64, m->sigmask);
  }
  return 0;
}

static int SysSigpending(struct Machine *m, i64 setaddr) {
  u8 word[8];
  Write64(word, m->signals);
  return CopyToUserWrite(m, setaddr, word, 8);
}

static int SysKill(struct Machine *m, int pid, int sig) {
  return kill(pid, sig ? XlatSignal(sig) : 0);
}

static bool IsValidThreadId(struct System *s, int tid) {
  return tid == s->pid ||
         (kMinThreadId <= tid && tid < kMinThreadId + kMaxThreadIds);
}

static int SysTkill(struct Machine *m, int tid, int sig) {
  int err;
  bool found;
  struct Dll *e;
  struct Machine *m2;
  if (tid < 0) return einval();
  if (!(0 <= sig && sig <= 64)) {
    LOGF("tkill(%d, %d) failed due to bogus signal", tid, sig);
    return einval();
  }
  // trigger signal immediately if possible
  if (tid == m->tid && (~m->sigmask & (1ull << (sig - 1)))) {
    switch (Read64(m->system->hands[sig - 1].handler)) {
      case SIG_DFL_LINUX:
        if (!IsSignalIgnoredByDefault(sig)) {
          TerminateSignal(m, sig);
          return 0;
        }
        // fallthrough
      case SIG_IGN_LINUX:
        return 0;
      default:
        Put64(m->ax, 0);
        m->interrupted = true;
        DeliverSignal(m, sig, SI_TKILL_LINUX);
        return -1;
    }
  }
  if (!IsValidThreadId(m->system, tid)) {
    LOGF("tkill(%d, %d) failed due to bogus thread id", tid, sig);
    return esrch();
  }
  err = 0;
  LOCK(&m->system->machines_lock);
  for (e = dll_first(m->system->machines); e;
       e = dll_next(m->system->machines, e)) {
    m2 = MACHINE_CONTAINER(e);
    if (m2->tid == tid) {
      if (sig) {
        EnqueueSignal(m2, sig);
        err = pthread_kill(m2->thread, SIGSYS);
      } else {
        err = pthread_kill(m2->thread, 0);
      }
      found = true;
      break;
    }
  }
  UNLOCK(&m->system->machines_lock);
  if (!sig && !found) err = ESRCH;
  if (!err) {
    return 0;
  } else {
    errno = err;
    return -1;
  }
}

static int SysTgkill(struct Machine *m, int pid, int tid, int sig) {
  if (pid < 1 || tid < 1) return einval();
  if (pid != m->system->pid) return eperm();
  return SysTkill(m, tid, sig);
}

static int SysPause(struct Machine *m) {
  int rc;
  NORESTART(rc, pause());
  return rc;
}

static int SysSetsid(struct Machine *m) {
  return setsid();
}

static i32 SysGetsid(struct Machine *m, i32 pid) {
  return getsid(pid);
}

static int SysGetpid(struct Machine *m) {
  return m->system->pid;
}

static int SysGettid(struct Machine *m) {
  return m->tid;
}

static int SysGetppid(struct Machine *m) {
  return getppid();
}

static int SysGetuid(struct Machine *m) {
  return getuid();
}

static int SysGetgid(struct Machine *m) {
  return getgid();
}

static int SysGeteuid(struct Machine *m) {
  return geteuid();
}

static int SysGetegid(struct Machine *m) {
  return getegid();
}

static i32 SysGetgroups(struct Machine *m, i32 size, i64 addr) {
  gid_t *group;
  u8 i32buf[4];
  int i, ngroups;
  long ngroups_max;
  if (!size) {
    return getgroups(0, 0);
  } else {
    // POSIX.1 recommends adding 1 to ngroups_max but the Linux manual
    // says this can result in EINVAL. Apple M1 says NGROUPS_MAX is 16
    // even though it usually returns 18 groups or more...
#ifdef __APPLE__
    ngroups_max = size;
#else
    ngroups_max = sysconf(_SC_NGROUPS_MAX);
#endif
    size = MIN(size, ngroups_max);
    if (!IsValidMemory(m, addr, (size_t)size * 4, PROT_WRITE)) return efault();
    if (!(group = (gid_t *)malloc(size * sizeof(gid_t)))) return -1;
    if ((ngroups = getgroups(size, group)) != -1) {
      for (i = 0; i < ngroups; ++i) {
        Write32(i32buf, group[i]);
        CopyToUserWrite(m, addr + (size_t)i * 4, i32buf, 4);
      }
    }
    free(group);
    return ngroups;
  }
}

static i32 SysSetgroups(struct Machine *m, i32 size, i64 addr) {
  int i, rc;
  gid_t *group;
  const u8 *group_linux;
  if (!(group_linux = (const u8 *)SchlepR(m, addr, (size_t)size * 4)) ||
      !(group = (gid_t *)malloc(size * sizeof(gid_t)))) {
    return -1;
  }
  for (i = 0; i < size; ++i) {
    group[i] = Read32(group_linux + (size_t)i * 4);
  }
  rc = setgroups(size, group);
  free(group);
  return rc;
}

static i32 SysSetresuid(struct Machine *m,  //
                        u32 real,           //
                        u32 effective,      //
                        u32 saved) {
#ifdef __linux
  return setresuid(real, effective, saved);
#else
  if (saved != (u32)-1) return enosys();
  return setreuid(real, effective);
#endif
}

static i32 SysSetresgid(struct Machine *m,  //
                        u32 real,           //
                        u32 effective,      //
                        u32 saved) {
#ifdef __linux
  return setresgid(real, effective, saved);
#else
  if (saved != (u32)-1) return enosys();
  return setregid(real, effective);
#endif
}

static i32 SysGetresuid(struct Machine *m,  //
                        i64 realaddr,       //
                        i64 effectiveaddr,  //
                        i64 savedaddr) {
  uid_t uid;
  gid_t euid;
  u8 *real = 0;
  u8 *effective = 0;
  if ((realaddr && !(real = (u8 *)SchlepW(m, realaddr, 4))) ||
      (effectiveaddr && !(effective = (u8 *)SchlepW(m, effectiveaddr, 4)))) {
    return -1;
  }
#ifdef __linux
  gid_t suid;
  u8 *saved = 0;
  if ((savedaddr && !(saved = (u8 *)SchlepW(m, savedaddr, 4)))) return -1;
  if (getresuid(&uid, &euid, &suid) == -1) return -1;
  Write32(saved, uid);
#else
  if (savedaddr) return enosys();
  uid = getuid();
  euid = geteuid();
#endif
  Write32(real, uid);
  Write32(effective, euid);
  return 0;
}

static i32 SysGetresgid(struct Machine *m,  //
                        i64 realaddr,       //
                        i64 effectiveaddr,  //
                        i64 savedaddr) {
  u8 *real = 0;
  u8 *effective = 0;
  if (savedaddr) return enosys();
  if ((realaddr && !(real = (u8 *)SchlepW(m, realaddr, 4))) ||
      (effectiveaddr && !(effective = (u8 *)SchlepW(m, effectiveaddr, 4)))) {
    return -1;
  }
  Write32(real, getgid());
  Write32(effective, getegid());
  return 0;
}

static int SysSchedYield(struct Machine *m) {
  return sched_yield();
}

static int SysUmask(struct Machine *m, int mask) {
  return umask(mask);
}

static int SysSetuid(struct Machine *m, int uid) {
  return setuid(uid);
}

static int SysSetgid(struct Machine *m, int gid) {
  return setgid(gid);
}

static int SysSetreuid(struct Machine *m, u32 uid, u32 euid) {
  return setreuid(uid, euid);
}

static int SysSetregid(struct Machine *m, u32 gid, u32 egid) {
  return setregid(gid, egid);
}

static int SysGetpgid(struct Machine *m, int pid) {
  return getpgid(pid);
}

static int SysGetpgrp(struct Machine *m) {
  return getpgid(0);
}

static int SysAlarm(struct Machine *m, unsigned seconds) {
  return alarm(seconds);
}

static int SysSetpgid(struct Machine *m, int pid, int gid) {
  return setpgid(pid, gid);
}

static int SysCreat(struct Machine *m, i64 path, int mode) {
  return SysOpenat(m, AT_FDCWD_LINUX, path,
                   O_WRONLY_LINUX | O_CREAT_LINUX | O_TRUNC_LINUX, mode);
}

static int SysAccess(struct Machine *m, i64 path, int mode) {
  return SysFaccessat(m, AT_FDCWD_LINUX, path, mode);
}

static int SysStat(struct Machine *m, i64 path, i64 st) {
  return SysFstatat(m, AT_FDCWD_LINUX, path, st, 0);
}

static int SysLstat(struct Machine *m, i64 path, i64 st) {
  return SysFstatat(m, AT_FDCWD_LINUX, path, st, AT_SYMLINK_NOFOLLOW_LINUX);
}

static int SysOpen(struct Machine *m, i64 path, int flags, int mode) {
  return SysOpenat(m, AT_FDCWD_LINUX, path, flags, mode);
}

static int SysAccept(struct Machine *m, int fd, i64 sa, i64 sas) {
  return SysAccept4(m, fd, sa, sas, 0);
}

static int SysSchedSetparam(struct Machine *m, int pid, i64 paramaddr) {
  if (pid < 0 || !paramaddr) return einval();
  return 0;
}

static int SysSchedGetparam(struct Machine *m, int pid, i64 paramaddr) {
  u8 param[8];
  if (pid < 0 || !paramaddr) return einval();
  Write32(param, 0);
  CopyToUserWrite(m, paramaddr, param, 8);
  return 0;
}

static int SysSchedSetscheduler(struct Machine *m, int pid, int policy,
                                i64 paramaddr) {
  if (pid < 0 || !paramaddr) return einval();
  return 0;
}

static int SysSchedGetscheduler(struct Machine *m, int pid) {
  if (pid < 0) return einval();
  return SCHED_OTHER_LINUX;
}

static int SysSchedGetPriorityMax(struct Machine *m, int policy) {
  return 0;
}

static int SysSchedGetPriorityMin(struct Machine *m, int policy) {
  return 0;
}

static int SysPipe(struct Machine *m, i64 pipefds_addr) {
  return SysPipe2(m, pipefds_addr, 0);
}

void OpSyscall(P) {
  size_t mark;
  u64 ax, di, si, dx, r0, r8, r9;
  STATISTIC(++syscalls);
  if (m->system->redraw) {
    m->system->redraw(true);
  }
  ax = Get64(m->ax);
  di = Get64(m->di);
  si = Get64(m->si);
  dx = Get64(m->dx);
  r0 = Get64(m->r10);
  r8 = Get64(m->r8);
  r9 = Get64(m->r9);
  mark = m->freelist.n;
  m->interrupted = false;
  switch (ax & 0xfff) {
    SYSCALL3(0x000, SysRead);
    SYSCALL3(0x001, SysWrite);
    SYSCALL3(0x002, SysOpen);
    SYSCALL1(0x003, SysClose);
    SYSCALL2(0x004, SysStat);
    SYSCALL2(0x005, SysFstat);
    SYSCALL2(0x006, SysLstat);
    SYSCALL3(0x007, SysPoll);
    SYSCALL3(0x008, SysLseek);
    SYSCALL6(0x009, SysMmap);
    SYSCALL4(0x011, SysPread);
    SYSCALL4(0x012, SysPwrite);
    SYSCALL5(0x017, SysSelect);
    SYSCALL5(0x019, SysMremap);
    SYSCALL6(0x10E, SysPselect);
    SYSCALL3(0x01A, SysMsync);
    SYSCALL3(0x00A, SysMprotect);
    SYSCALL2(0x00B, SysMunmap);
    SYSCALL1(0x00C, SysBrk);
    SYSCALL4(0x00D, SysSigaction);
    SYSCALL4(0x00E, SysSigprocmask);
    SYSCALL3(0x010, SysIoctl);
    SYSCALL3(0x013, SysReadv);
    SYSCALL3(0x014, SysWritev);
    SYSCALL2(0x015, SysAccess);
    SYSCALL1(0x016, SysPipe);
    SYSCALL2(0x125, SysPipe2);
    SYSCALL0(0x018, SysSchedYield);
    SYSCALL3(0x01C, SysMadvise);
    SYSCALL1(0x020, SysDup1);
    SYSCALL2(0x021, SysDup2);
    SYSCALL3(0x124, SysDup3);
    SYSCALL0(0x022, SysPause);
    SYSCALL2(0x023, SysNanosleep);
    SYSCALL2(0x024, SysGetitimer);
    SYSCALL1(0x025, SysAlarm);
    SYSCALL3(0x026, SysSetitimer);
    SYSCALL0(0x027, SysGetpid);
    SYSCALL0(0x0BA, SysGettid);
    SYSCALL3(0x029, SysSocket);
    SYSCALL3(0x02A, SysConnect);
    SYSCALL3(0x02B, SysAccept);
    SYSCALL6(0x02C, SysSendto);
    SYSCALL6(0x02D, SysRecvfrom);
    SYSCALL3(0x02E, SysSendmsg);
    SYSCALL3(0x02F, SysRecvmsg);
    SYSCALL2(0x030, SysShutdown);
    SYSCALL3(0x031, SysBind);
    SYSCALL2(0x032, SysListen);
    SYSCALL3(0x033, SysGetsockname);
    SYSCALL3(0x034, SysGetpeername);
    SYSCALL4(0x035, SysSocketpair);
    SYSCALL5(0x036, SysSetsockopt);
    SYSCALL5(0x037, SysGetsockopt);
    SYSCALL6(0x038, SysClone);
    SYSCALL0(0x039, SysFork);
    SYSCALL0(0x03A, SysVfork);
    SYSCALL3(0x03B, SysExecve);
    SYSCALL4(0x03D, SysWait4);
    SYSCALL2(0x03E, SysKill);
    SYSCALL1(0x03F, SysUname);
    SYSCALL3(0x048, SysFcntl);
    SYSCALL2(0x049, SysFlock);
    SYSCALL1(0x04A, SysFsync);
    SYSCALL1(0x04B, SysFdatasync);
    SYSCALL2(0x04C, SysTruncate);
    SYSCALL2(0x04D, SysFtruncate);
    SYSCALL2(0x04F, SysGetcwd);
    SYSCALL1(0x050, SysChdir);
    SYSCALL1(0x051, SysFchdir);
    SYSCALL2(0x052, SysRename);
    SYSCALL2(0x053, SysMkdir);
    SYSCALL1(0x054, SysRmdir);
    SYSCALL2(0x055, SysCreat);
    SYSCALL2(0x056, SysLink);
    SYSCALL1(0x057, SysUnlink);
    SYSCALL2(0x058, SysSymlink);
    SYSCALL3(0x059, SysReadlink);
    SYSCALL2(0x05A, SysChmod);
    SYSCALL2(0x05B, SysFchmod);
    SYSCALL3(0x05C, SysChown);
    SYSCALL3(0x05D, SysFchown);
    SYSCALL3(0x05E, SysLchown);
    SYSCALL5(0x104, SysFchownat);
    SYSCALL1(0x05F, SysUmask);
    SYSCALL2(0x060, SysGettimeofday);
    SYSCALL2(0x061, SysGetrlimit);
    SYSCALL2(0x062, SysGetrusage);
    SYSCALL1(0x063, SysSysinfo);
    SYSCALL1(0x064, SysTimes);
    SYSCALL0(0x06F, SysGetpgrp);
    SYSCALL0(0x070, SysSetsid);
    SYSCALL2(0x073, SysGetgroups);
    SYSCALL2(0x074, SysSetgroups);
    SYSCALL3(0x075, SysSetresuid);
    SYSCALL3(0x076, SysGetresuid);
    SYSCALL3(0x077, SysSetresgid);
    SYSCALL3(0x078, SysGetresgid);
    SYSCALL1(0x079, SysGetpgid);
    SYSCALL1(0x07C, SysGetsid);
    SYSCALL1(0x07F, SysSigpending);
    SYSCALL2(0x089, SysStatfs);
    SYSCALL2(0x08A, SysFstatfs);
    SYSCALL2(0x06D, SysSetpgid);
    SYSCALL0(0x066, SysGetuid);
    SYSCALL0(0x068, SysGetgid);
    SYSCALL1(0x069, SysSetuid);
    SYSCALL1(0x06A, SysSetgid);
    SYSCALL0(0x06B, SysGeteuid);
    SYSCALL0(0x06C, SysGetegid);
    SYSCALL0(0x06E, SysGetppid);
    SYSCALL2(0x071, SysSetreuid);
    SYSCALL2(0x072, SysSetregid);
    SYSCALL2(0x082, SysSigsuspend);
    SYSCALL2(0x083, SysSigaltstack);
    SYSCALL3(0x085, SysMknod);
    SYSCALL4(0x103, SysMknodat);
    SYSCALL2(0x08C, SysGetpriority);
    SYSCALL3(0x08D, SysSetpriority);
    SYSCALL2(0x08E, SysSchedSetparam);
    SYSCALL2(0x08F, SysSchedGetparam);
    SYSCALL3(0x090, SysSchedSetscheduler);
    SYSCALL1(0x091, SysSchedGetscheduler);
    SYSCALL1(0x092, SysSchedGetPriorityMax);
    SYSCALL1(0x093, SysSchedGetPriorityMin);
    SYSCALL5(0x09D, SysPrctl);
    SYSCALL2(0x09E, SysArchPrctl);
    SYSCALL2(0x0A0, SysSetrlimit);
    SYSCALL1(0x0A1, SysChroot);
    SYSCALL0(0x0A2, SysSync);
    SYSCALL2(0x0C8, SysTkill);
    SYSCALL6(0x0CA, SysFutex);
    SYSCALL3(0x0CB, SysSchedSetaffinity);
    SYSCALL3(0x0CC, SysSchedGetaffinity);
    SYSCALL3(0x0D9, SysGetdents);
    SYSCALL1(0x0DA, SysSetTidAddress);
    SYSCALL4(0x0DD, SysFadvise);
    SYSCALL2(0x0E3, SysClockSettime);
    SYSCALL2(0x0E5, SysClockGetres);
    SYSCALL4(0x0E6, SysClockNanosleep);
    SYSCALL3(0x0EA, SysTgkill);
    SYSCALL2(0x084, SysUtime);
    SYSCALL2(0x0EB, SysUtimes);
    SYSCALL3(0x105, SysFutimesat);
    SYSCALL4(0x118, SysUtimensat);
    SYSCALL4(0x101, SysOpenat);
    SYSCALL3(0x102, SysMkdirat);
    SYSCALL4(0x106, SysFstatat);
    SYSCALL3(0x107, SysUnlinkat);
    SYSCALL4(0x108, SysRenameat);
    SYSCALL5(0x109, SysLinkat);
    SYSCALL3(0x10A, SysSymlinkat);
    SYSCALL4(0x10B, SysReadlinkat);
    SYSCALL3(0x10C, SysFchmodat);
    SYSCALL3(0x10D, SysFaccessat);
    SYSCALL5(0x10F, SysPpoll);
    SYSCALL4(0x1b7, SysFaccessat2);
    SYSCALL4(0x120, SysAccept4);
    SYSCALL2(0x111, SysSetRobustList);
    SYSCALL3(0x112, SysGetRobustList);
    SYSCALL4(0x127, SysPreadv);
    SYSCALL4(0x128, SysPwritev);
    SYSCALL4(0x12E, SysPrlimit);
    SYSCALL3(0x13E, SysGetrandom);
    SYSCALL5(0x147, SysPreadv2);
    SYSCALL5(0x148, SysPwritev2);
    SYSCALL3(0x1B4, SysCloseRange);
    case 0x3C:
      SYS_LOGF("%s(%#" PRIx64 ")", "SysExit", di);
      SysExit(m, di);
    case 0xE7:
      SYS_LOGF("%s(%#" PRIx64 ")", "SysExitGroup", di);
      SysExitGroup(m, di);
    case 0x00F:
      SYS_LOGF("%s()", "SigRestore");
      SigRestore(m);
      return;
    case 0x500:
      // Cosmopolitan uses this number to trigger ENOSYS for testing.
      ax = enosys();
      break;
    case 0x0E4:
      // clock_gettime() is
      //   1) called frequently,
      //   2) latency sensitive, and
      //   3) usually implemented as a VDSO.
      // Therefore we exempt it from system call tracing.
      ax = SysClockGettime(m, di, si);
      break;
    case 0x0C9:
      // time() is also noisy in some environments.
      ax = SysTime(m, di);
      break;
    default:
      LOGF("missing syscall 0x%03" PRIx64, ax);
      ax = enosys();
      break;
  }
  if (!m->interrupted) {
    Put64(m->ax, ax != -1 ? ax : -(XlatErrno(errno) & 0xfff));
  }
  CollectGarbage(m, mark);
}

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
#include <errno.h>
#include <string.h>

#include "blink/address.h"
#include "blink/assert.h"
#include "blink/debug.h"
#include "blink/log.h"
#include "blink/machine.h"
#include "blink/macros.h"
#include "blink/modrm.h"
#include "blink/path.h"
#include "blink/stats.h"

void (*AddPath_StartOp_Hook)(P);

static void StartPath(struct Machine *m) {
  JIX_LOGF("%" PRIx64 " <path>", m->ip);
}

static void DebugOp(struct Machine *m, i64 expected_ip) {
  if (m->ip != expected_ip) {
    LOGF("IP was %" PRIx64 " but it should have been %" PRIx64, m->ip,
         expected_ip);
  }
  unassert(m->ip == expected_ip);
}

static void StartOp(struct Machine *m, long len) {
  JIX_LOGF("%" PRIx64 "   <op>", GetPc(m));
  JIX_LOGF("%" PRIx64 "     %s", GetPc(m), DescribeOp(m, GetPc(m)));
  unassert(!m->path.jb);
  m->oldip = m->ip;
  m->ip += len;
}

static void EndOp(struct Machine *m) {
  JIX_LOGF("%" PRIx64 "   </op>", GetPc(m));
  m->oldip = -1;
  if (m->stashaddr) {
    CommitStash(m);
  }
}

static void EndPath(struct Machine *m) {
  JIX_LOGF("%" PRIx64 "   %s", GetPc(m), DescribeOp(m, GetPc(m)));
  JIX_LOGF("%" PRIx64 " </path>", GetPc(m));
}

bool CreatePath(P) {
  i64 pc;
  bool res;
  unassert(!m->path.jb);
  if ((pc = GetPc(m))) {
    if ((m->path.jb = StartJit(&m->system->jit))) {
      JIT_LOGF("starting new path %" PRIxPTR " at %" PRIx64,
               GetJitPc(m->path.jb), pc);
      Jitter(A, "s4i", pc);
#if LOG_JIX
      Jitter(A, "c s0a0=", StartPath);
#endif
      m->path.start = pc;
      m->path.elements = 0;
      SetHook(m, pc, JitlessDispatch);
      res = true;
    } else {
      LOGF("jit failed: %s", strerror(errno));
      res = false;
    }
  } else {
    res = false;
  }
  return res;
}

void CommitPath(P, intptr_t splice) {
  unassert(m->path.jb);
#if LOG_JIX
  Jitter(A, "s0a0= c s0a0=", EndPath);
#endif
  STATISTIC(path_longest_bytes =
                MAX(path_longest_bytes, m->path.jb->index - m->path.jb->start));
  STATISTIC(path_longest = MAX(path_longest, m->path.elements));
  STATISTIC(AVERAGE(path_average_elements, m->path.elements));
  STATISTIC(AVERAGE(path_average_bytes, m->path.jb->index - m->path.jb->start));
  if (SpliceJit(&m->system->jit, m->path.jb, m->fun + m->path.start,
                (intptr_t)JitlessDispatch, splice)) {
    STATISTIC(++path_count);
    JIT_LOGF("staged path to %" PRIx64, m->path.start);
  } else {
    STATISTIC(++path_ooms);
    JIT_LOGF("path starting at %" PRIx64 " ran out of space", m->path.start);
  }
  m->path.jb = 0;
}

void AbandonPath(struct Machine *m) {
  unassert(m->path.jb);
  STATISTIC(++path_abandoned);
  AbandonJit(&m->system->jit, m->path.jb);
  m->path.jb = 0;
}

void AddPath_StartOp(P) {
  _Static_assert(offsetof(struct Machine, ip) < 128, "");
  _Static_assert(offsetof(struct Machine, oldip) < 128, "");
#ifndef NDEBUG
  Jitter(A, "a0i m", &instructions_jitted, CountOp);
#endif
  if (AddPath_StartOp_Hook) {
    AddPath_StartOp_Hook(A);
  }
  u8 len = Oplength(rde);
  // TODO(jart): We shouldn't need to modify m->ip on every op.
#if defined(DEBUG) || LOG_JIX
  Jitter(A, "a1i s0a0= c s0a0=", m->ip, DebugOp);
#endif
#if !LOG_JIX && defined(__x86_64__)
  AppendJitMovReg(m->path.jb, kJitArg0, kJitSav0);
  u8 ip = offsetof(struct Machine, ip);
  u8 oldip = offsetof(struct Machine, oldip);
  u8 code[] = {
      0x4c, 0x89, 0177, oldip,  // mov %r15,16(%rdi)
      0x49, 0x83, 0307, len,    // add $len,%r15
      0x4c, 0x89, 0177, ip,     // mov %r15,8(%rdi)
  };
  AppendJit(m->path.jb, code, sizeof(code));
  m->reserving = false;
#elif !LOG_JIX && defined(__aarch64__)
  AppendJitMovReg(m->path.jb, kJitArg0, kJitSav0);
  u8 ip = offsetof(struct Machine, ip);
  u8 oldip = offsetof(struct Machine, oldip);
  u32 code[] = {
      0xf9400001 | (ip / 8) << 10,     // ldr x1, [x0, #ip]
      0xf9000001 | (oldip / 8) << 10,  // str x1, [x0, #oldip]
      0x91000021 | len << 10,          // add x1, x1, #len
      0xf9000001 | (ip / 8) << 10,     // str x1, [x0, #ip]
  };
  AppendJit(m->path.jb, code, sizeof(code));
  m->reserving = false;
#else
  Jitter(A, "a1i s0a0= c s0a0=", len, StartOp);
#endif
}

void AddPath_EndOp(P) {
  _Static_assert(offsetof(struct Machine, stashaddr) < 128, "");
#if !LOG_JIX && defined(__x86_64__)
  if (m->reserving) {
    AppendJitMovReg(m->path.jb, kJitArg0, kJitSav0);
    u8 sa = offsetof(struct Machine, stashaddr);
    u8 code[] = {
        0x48, 0x83, 0177, sa, 0x00,  // cmpq $0x0,0x18(%rdi)
        0x74, 0x05,                  // jnz +5
    };
    AppendJit(m->path.jb, code, sizeof(code));
    AppendJitCall(m->path.jb, (void *)CommitStash);
  }
#elif !LOG_JIX && defined(__aarch64__)
  if (m->reserving) {
    AppendJitMovReg(m->path.jb, kJitArg0, kJitSav0);
    u32 sa = offsetof(struct Machine, stashaddr);
    u32 code[] = {
        0xf9400001 | (sa / 8) << 10,  // ldr x1, [x0, #stashaddr]
        0xb4000001 | 2 << 5,          // cbz x1, +2
    };
    AppendJit(m->path.jb, code, sizeof(code));
    AppendJitCall(m->path.jb, (void *)CommitStash);
  }
#else
  Jitter(A, "s0a0= c", EndOp);
#endif
}

bool AddPath(P) {
  unassert(m->path.jb);
  AppendJitSetReg(m->path.jb, kJitArg[kArgRde], rde);
  AppendJitSetReg(m->path.jb, kJitArg[kArgDisp], disp);
  AppendJitSetReg(m->path.jb, kJitArg[kArgUimm0], uimm0);
  AppendJitCall(m->path.jb, (void *)GetOp(Mopcode(rde)));
  return true;
}

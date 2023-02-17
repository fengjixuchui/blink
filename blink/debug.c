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
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "blink/assert.h"
#include "blink/builtin.h"
#include "blink/debug.h"
#include "blink/dis.h"
#include "blink/endian.h"
#include "blink/flags.h"
#include "blink/loader.h"
#include "blink/log.h"
#include "blink/machine.h"
#include "blink/macros.h"
#include "blink/map.h"
#include "blink/overlays.h"
#include "blink/stats.h"
#include "blink/thread.h"
#include "blink/tsan.h"
#include "blink/util.h"

#ifdef UNWIND
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

#define MAX_BACKTRACE_LINES 64

#define APPEND(...) o += snprintf(b + o, n - o, __VA_ARGS__)

int asan_backtrace_index;
int asan_backtrace_buffer[1];
int ubsan_backtrace_memory[2];
char *ubsan_backtrace_pointer = ((char *)ubsan_backtrace_memory) + 1;

void PrintBacktrace(void) {
#ifdef UNWIND
  int o = 0;
  char b[2048];
  char sym[256];
  int n = sizeof(b);
  unw_cursor_t cursor;
  unw_context_t context;
  unw_word_t offset, pc;
  unw_getcontext(&context);
  unw_init_local(&cursor, &context);
  while (unw_step(&cursor) > 0) {
    unw_get_reg(&cursor, UNW_REG_IP, &pc);
    if (!pc) break;
    APPEND("\n\t%lx ", pc);
    if (unw_get_proc_name(&cursor, sym, sizeof(sym), &offset) == 0) {
      APPEND("%s+%ld", sym, offset);
    } else {
      APPEND("<unknown>");
    }
  }
  ERRF("blink backtrace%s", b);
#elif defined(__SANITIZE_ADDRESS__)
  volatile int x;
  x = asan_backtrace_buffer[asan_backtrace_index + 1];
  (void)x;
#elif defined(__SANITIZE_UNDEFINED__)
  *(int *)ubsan_backtrace_pointer = 0;
#endif
}

static i64 ReadWord(struct Machine *m, u8 *p) {
  switch (2 << m->mode) {
    default:
    case 8:
      return Read64(p);
    case 4:
      return Read32(p);
    case 2:
      return Read16(p);
  }
}

int GetInstruction(struct Machine *m, i64 pc, struct XedDecodedInst *x) {
  int i, rc, err;
  u8 copy[15], *toil, *addr;
  BEGIN_NO_PAGE_FAULTS;
  if ((addr = LookupAddress(m, pc))) {
    if ((i = 4096 - (pc & 4095)) >= 15) {
      if (!DecodeInstruction(x, addr, 15, m->mode)) {
        rc = 0;
      } else {
        rc = kMachineDecodeError;
      }
    } else if ((toil = LookupAddress(m, pc + i))) {
      memcpy(copy, addr, i);
      memcpy(copy + i, toil, 15 - i);
      if (!DecodeInstruction(x, copy, 15, m->mode)) {
        rc = 0;
      } else {
        rc = kMachineDecodeError;
      }
    } else if (!(err = DecodeInstruction(x, addr, i, m->mode))) {
      rc = 0;
    } else if (err == XED_ERROR_BUFFER_TOO_SHORT) {
      rc = kMachineSegmentationFault;
    } else {
      rc = kMachineDecodeError;
    }
  } else {
    memset(x, 0, sizeof(*x));
    rc = kMachineSegmentationFault;
  }
  END_NO_PAGE_FAULTS;
  return rc;
}

const char *DescribeCpuFlags(int flags) {
  _Thread_local static char b[7];
  b[0] = (flags & OF) ? 'O' : '.';
  b[1] = (flags & SF) ? 'S' : '.';
  b[2] = (flags & ZF) ? 'Z' : '.';
  b[3] = (flags & AF) ? 'A' : '.';
  b[4] = (flags & PF) ? 'P' : '.';
  b[5] = (flags & CF) ? 'C' : '.';
  return b;
}

const char *DescribeOp(struct Machine *m, i64 pc) {
  _Thread_local static char b[256];
  int e, i, k, o = 0, n = sizeof(b);
  struct Dis d = {true};
  if (!(e = GetInstruction(m, pc, d.xedd))) {
#ifndef DISABLE_DISASSEMBLER
    char spec[64];
    o = DisInst(&d, b, DisSpec(d.xedd, spec)) - b;
#else
    APPEND(".byte");
#endif
  }
  if (e != kMachineSegmentationFault) {
    k = MAX(8, d.xedd->length);
    for (i = 0; i < k; ++i) {
      APPEND(" %02x", d.xedd->bytes[i]);
    }
  } else {
    APPEND("segfault");
  }
#ifndef DISABLE_DISASSEMBLER
  DisFree(&d);
#endif
  return b;
}

void PrintFds(struct Fds *fds) {
  struct Dll *e;
  LOGF("%-8s %-8s", "fildes", "oflags");
  for (e = dll_first(fds->list); e; e = dll_next(fds->list, e)) {
    LOGF("%-8d %-8x", FD_CONTAINER(e)->fildes, FD_CONTAINER(e)->oflags);
  }
}

const char *GetBacktrace(struct Machine *m) {
  _Thread_local static char b[4096];
  int o = 0;
  int n = sizeof(b);
#ifndef DISABLE_BACKTRACE
  struct Dis dis = {true};
  u8 *r;
  int i;
  i64 sym, sp, bp, rp;
  char kAlignmentMask[] = {3, 3, 15};
#endif

  BEGIN_NO_PAGE_FAULTS;

#ifndef DISABLE_BACKTRACE
  LoadDebugSymbols(&m->system->elf);
  DisLoadElf(&dis, &m->system->elf);
#endif

  APPEND(" PC %" PRIx64 " %s\n\t"
         " AX %016" PRIx64 " "
         " CX %016" PRIx64 " "
         " DX %016" PRIx64 " "
         " BX %016" PRIx64 "\n\t"
         " SP %016" PRIx64 " "
         " BP %016" PRIx64 " "
         " SI %016" PRIx64 " "
         " DI %016" PRIx64 "\n\t"
         " R8 %016" PRIx64 " "
         " R9 %016" PRIx64 " "
         "R10 %016" PRIx64 " "
         "R11 %016" PRIx64 "\n\t"
         "R12 %016" PRIx64 " "
         "R13 %016" PRIx64 " "
         "R14 %016" PRIx64 " "
         "R15 %016" PRIx64 "\n\t"
         " FS %016" PRIx64 " "
         " GS %016" PRIx64 " "
         "OPS %-16ld "
         "JIT %-16ld\n\t"
         "%s\n\t",
         m->cs.base + MaskAddress(m->mode, m->ip), DescribeOp(m, GetPc(m)),
         Get64(m->ax), Get64(m->cx), Get64(m->dx), Get64(m->bx), Get64(m->sp),
         Get64(m->bp), Get64(m->si), Get64(m->di), Get64(m->r8), Get64(m->r9),
         Get64(m->r10), Get64(m->r11), Get64(m->r12), Get64(m->r13),
         Get64(m->r14), Get64(m->r15), m->fs.base, m->gs.base,
         GET_COUNTER(instructions_decoded), GET_COUNTER(instructions_jitted),
         g_progname);

#ifndef DISABLE_BACKTRACE
  rp = m->ip;
  bp = Get64(m->bp);
  sp = Get64(m->sp);
  for (i = 0; i < MAX_BACKTRACE_LINES;) {
    if (i) APPEND("\n\t");
    sym = DisFindSym(&dis, rp);
    APPEND("%012" PRIx64 " %012" PRIx64 " %s", m->ss.base + bp, rp,
           sym != -1 ? dis.syms.stab + dis.syms.p[sym].name : "UNKNOWN");
    if (sym != -1 && rp != dis.syms.p[sym].addr) {
      APPEND("+%#" PRIx64 "", rp - dis.syms.p[sym].addr);
    }
    if (!bp) break;
    if (bp < sp) {
      APPEND(" [STRAY]");
    } else if (bp - sp <= 0x1000) {
      APPEND(" %" PRId64 " bytes", bp - sp);
    }
    if (bp & kAlignmentMask[m->mode] && i) {
      APPEND(" [MISALIGN]");
    }
    ++i;
    if (((m->ss.base + bp) & 0xfff) > 0xff0) break;
    if (!(r = LookupAddress(m, m->ss.base + bp))) {
      APPEND(" [CORRUPT FRAME POINTER]");
      break;
    }
    sp = bp;
    bp = ReadWord(m, r + 0);
    rp = ReadWord(m, r + 8);
  }
  DisFree(&dis);
#endif

  END_NO_PAGE_FAULTS;
  return b;
}

bool CheckMemoryInvariants(struct System *s) {
  if (s->rss == s->memstat.tables + s->memstat.committed &&
      s->vss == s->memstat.committed + s->memstat.reserved) {
    return true;
  } else {
    ERRF("%-10s = %ld vs. %ld", "rss", s->rss,
         s->memstat.tables + s->memstat.committed);
    ERRF("%-10s = %ld vs. %ld", "vss", s->vss,
         s->memstat.committed + s->memstat.reserved);
    ERRF("%-10s = %ld", "tables", s->memstat.tables);
    ERRF("%-10s = %ld", "reserved", s->memstat.reserved);
    ERRF("%-10s = %ld", "committed", s->memstat.committed);
    return false;
  }
}

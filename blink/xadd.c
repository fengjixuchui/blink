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
#include <limits.h>
#include <stdatomic.h>

#include "blink/alu.h"
#include "blink/assert.h"
#include "blink/endian.h"
#include "blink/machine.h"
#include "blink/modrm.h"
#include "blink/swap.h"

void OpXaddEbGb(struct Machine *m, u32 rde) {
  u8 x, y, z, *p, *q;
  p = GetModrmRegisterBytePointerWrite(m, rde);
  q = ByteRexrReg(m, rde);
  x = Read8(p);
  y = Read8(q);
#if !defined(__riscv) && !defined(__MICROBLAZE__)
  do {
    z = Add8(x, y, &m->flags);
    Write8(q, x);
    if (!Lock(rde)) {
      *p = z;
      break;
    }
  } while (!atomic_compare_exchange_weak_explicit(
      (atomic_uchar *)p, &x, *q, memory_order_acq_rel, memory_order_relaxed));
#else
  if (!Lock(rde)) {
    z = Add8(x, y, &m->flags);
    Write8(q, x);
    Write8(p, z);
  } else {
    OpUd(m, rde);
  }
#endif
}

void OpXaddEvqpGvqp(struct Machine *m, u32 rde) {
  u8 *p, *q;
  q = RegRexrReg(m, rde);
  p = GetModrmRegisterWordPointerWriteOszRexw(m, rde);
  if (Rexw(rde)) {
    u64 x, y, z;
    if (Lock(rde) && !((intptr_t)p & 7)) {
#if LONG_BIT == 64
      x = atomic_load_explicit((_Atomic(u64) *)p, memory_order_relaxed);
      y = atomic_load_explicit((_Atomic(u64) *)q, memory_order_relaxed);
      y = SWAP64LE(y);
      do {
        atomic_store_explicit((_Atomic(u64) *)q, x, memory_order_relaxed);
        z = kAlu[ALU_ADD][ALU_INT64](SWAP64LE(x), y, &m->flags);
        z = SWAP64LE(z);
      } while (!atomic_compare_exchange_weak_explicit((_Atomic(u64) *)p, &x, z,
                                                      memory_order_acq_rel,
                                                      memory_order_relaxed));
#else
      OpUd(m, rde);
#endif
    } else {
      x = Read64(p);
      y = Read64(q);
      z = kAlu[ALU_ADD][ALU_INT64](x, y, &m->flags);
      Write64(q, x);
      Write64(p, z);
    }
  } else if (!Osz(rde)) {
    u32 x, y, z;
    if (Lock(rde) && !((intptr_t)p & 3)) {
      x = atomic_load_explicit((_Atomic(u32) *)p, memory_order_relaxed);
      y = atomic_load_explicit((_Atomic(u32) *)q, memory_order_relaxed);
      y = SWAP32LE(y);
      do {
        atomic_store_explicit((_Atomic(u32) *)q, x, memory_order_relaxed);
        z = kAlu[ALU_ADD][ALU_INT32](SWAP32LE(x), y, &m->flags);
        z = SWAP32LE(z);
      } while (!atomic_compare_exchange_weak_explicit((_Atomic(u32) *)p, &x, z,
                                                      memory_order_acq_rel,
                                                      memory_order_relaxed));
    } else {
      x = Read32(p);
      y = Read32(q);
      z = kAlu[ALU_ADD][ALU_INT32](x, y, &m->flags);
      Write32(q, x);
      Write32(p, z);
    }
    if (IsModrmRegister(rde)) {
      Write32(p + 4, 0);
    }
  } else {
    u16 x, y, z;
    unassert(!Lock(rde));
    x = Read16(p);
    y = Read16(q);
    z = kAlu[ALU_ADD][ALU_INT16](x, y, &m->flags);
    Write16(q, x);
    Write16(p, z);
  }
}

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
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blink/address.h"
#include "blink/alu.h"
#include "blink/assert.h"
#include "blink/bitscan.h"
#include "blink/builtin.h"
#include "blink/case.h"
#include "blink/debug.h"
#include "blink/endian.h"
#include "blink/flags.h"
#include "blink/fpu.h"
#include "blink/likely.h"
#include "blink/lock.h"
#include "blink/log.h"
#include "blink/machine.h"
#include "blink/macros.h"
#include "blink/modrm.h"
#include "blink/mop.h"
#include "blink/path.h"
#include "blink/random.h"
#include "blink/signal.h"
#include "blink/sse.h"
#include "blink/ssefloat.h"
#include "blink/ssemov.h"
#include "blink/stats.h"
#include "blink/string.h"
#include "blink/swap.h"
#include "blink/syscall.h"
#include "blink/time.h"
#include "blink/util.h"

_Thread_local struct Machine *g_machine;

static void OpHintNopEv(P) {
}

static void OpCmc(P) {
  m->flags ^= CF;
}

static void OpClc(P) {
  m->flags = SetFlag(m->flags, FLAGS_CF, false);
}

static void OpStc(P) {
  m->flags = SetFlag(m->flags, FLAGS_CF, true);
}

static void OpCli(P) {
  m->flags = SetFlag(m->flags, FLAGS_IF, false);
}

static void OpSti(P) {
  m->flags = SetFlag(m->flags, FLAGS_IF, true);
}

static void OpCld(P) {
  m->flags = SetFlag(m->flags, FLAGS_DF, false);
}

static void OpStd(P) {
  m->flags = SetFlag(m->flags, FLAGS_DF, true);
}

static void OpPushf(P) {
  Push(A, ExportFlags(m->flags) & 0xFCFFFF);
}

static void OpPopf(P) {
  if (!Osz(rde)) {
    ImportFlags(m, Pop(A, 0));
  } else {
    ImportFlags(m, (m->flags & ~0xffff) | Pop(A, 0));
  }
}

static void OpLahf(P) {
  m->ah = ExportFlags(m->flags);
}

static void OpSahf(P) {
  ImportFlags(m, (m->flags & ~0xff) | m->ah);
}

static void OpLeaGvqpM(P) {
  WriteRegister(rde, RegRexrReg(m, rde), LoadEffectiveAddress(A).addr);
  Jitter(A, "L r0 C");
}

static relegated void OpPushSeg(P) {
  u8 seg = (Opcode(rde) & 070) >> 3;
  Push(A, *GetSegment(A, seg) >> 4);
}

static relegated void OpPopSeg(P) {
  u8 seg = (Opcode(rde) & 070) >> 3;
  *GetSegment(A, seg) = Pop(A, 0) << 4;
}

static relegated void OpMovEvqpSw(P) {
  WriteRegisterOrMemory(rde, GetModrmRegisterWordPointerWriteOszRexw(A),
                        *GetSegment(A, ModrmReg(rde)) >> 4);
}

static relegated int GetDescriptor(struct Machine *m, int selector,
                                   u64 *out_descriptor) {
  unassert(m->system->gdt_base + m->system->gdt_limit <= kRealSize);
  selector &= -8;
  if (8 <= selector && selector + 8 <= m->system->gdt_limit) {
    SetReadAddr(m, m->system->gdt_base + selector, 8);
    *out_descriptor = Load64(m->system->real + m->system->gdt_base + selector);
    return 0;
  } else {
    return -1;
  }
}

static relegated u64 GetDescriptorBase(u64 d) {
  return (d & 0xff00000000000000) >> 32 | (d & 0x000000ffffff0000) >> 16;
}

static relegated u64 GetDescriptorLimit(u64 d) {
  return (d & 0x000f000000000000) >> 32 | (d & 0xffff);
}

static relegated int GetDescriptorMode(u64 d) {
  u8 kMode[] = {XED_MODE_REAL, XED_MODE_LONG, XED_MODE_LEGACY, XED_MODE_LONG};
  return kMode[(d & 0x0060000000000000) >> 53];
}

static relegated bool IsProtectedMode(struct Machine *m) {
  return m->system->cr0 & 1;
}

static relegated void OpMovSwEvqp(P) {
  u64 x, d;
  x = ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(A));
  if (!IsProtectedMode(m)) {
    x <<= 4;
  } else if (GetDescriptor(m, x, &d) != -1) {
    x = GetDescriptorBase(d);
  } else {
    ThrowProtectionFault(m);
  }
  *GetSegment(A, ModrmReg(rde)) = x;
}

static relegated void OpLsl(P) {
  u64 descriptor;
  if (GetDescriptor(m, Load16(GetModrmRegisterWordPointerRead2(A)),
                    &descriptor) != -1) {
    WriteRegister(rde, RegRexrReg(m, rde), GetDescriptorLimit(descriptor));
    m->flags = SetFlag(m->flags, FLAGS_ZF, true);
  } else {
    m->flags = SetFlag(m->flags, FLAGS_ZF, false);
  }
}

void SetMachineMode(struct Machine *m, int mode) {
  m->mode = mode;
  m->system->mode = mode;
}

void ChangeMachineMode(struct Machine *m, int mode) {
  if (mode == m->mode) return;
  ResetInstructionCache(m);
  SetMachineMode(m, mode);
}

static relegated void OpJmpf(P) {
  u64 descriptor;
  if (!IsProtectedMode(m)) {
    m->cs = uimm0 << 4;
    m->ip = disp;
  } else if (GetDescriptor(m, uimm0, &descriptor) != -1) {
    m->cs = GetDescriptorBase(descriptor);
    m->ip = disp;
    ChangeMachineMode(m, GetDescriptorMode(descriptor));
  } else {
    ThrowProtectionFault(m);
  }
  if (m->system->onlongbranch) {
    m->system->onlongbranch(m);
  }
}

static relegated void OpXlatAlBbb(P) {
  i64 v;
  v = MaskAddress(Eamode(rde), Get64(m->bx) + Get8(m->ax));
  v = DataSegment(A, v);
  SetReadAddr(m, v, 1);
  m->al = Load8(ResolveAddress(m, v));
}

static void PutEaxAx(P, u32 x) {
  if (!Osz(rde)) {
    Put64(m->ax, x);
  } else {
    Put16(m->ax, x);
  }
}

static u32 GetEaxAx(P) {
  if (!Osz(rde)) {
    return Get32(m->ax);
  } else {
    return Get16(m->ax);
  }
}

static relegated void OpInAlImm(P) {
  Put8(m->ax, OpIn(m, uimm0));
}

static relegated void OpInAxImm(P) {
  PutEaxAx(A, OpIn(m, uimm0));
}

static relegated void OpInAlDx(P) {
  Put8(m->ax, OpIn(m, Get16(m->dx)));
}

static relegated void OpInAxDx(P) {
  PutEaxAx(A, OpIn(m, Get16(m->dx)));
}

static relegated void OpOutImmAl(P) {
  OpOut(m, uimm0, Get8(m->ax));
}

static relegated void OpOutImmAx(P) {
  OpOut(m, uimm0, GetEaxAx(A));
}

static relegated void OpOutDxAl(P) {
  OpOut(m, Get16(m->dx), Get8(m->ax));
}

static relegated void OpOutDxAx(P) {
  OpOut(m, Get16(m->dx), GetEaxAx(A));
}

static void OpXchgZvqp(P) {
  u64 x, y;
  x = Get64(m->ax);
  y = Get64(RegRexbSrm(m, rde));
  WriteRegister(rde, m->ax, y);
  WriteRegister(rde, RegRexbSrm(m, rde), x);
}

static void Op1c7(P) {
  bool ismem;
  ismem = !IsModrmRegister(rde);
  switch (ModrmReg(rde)) {
    case 6:
      if (!ismem) {
        OpRdrand(A);
      } else {
        OpUdImpl(m);
      }
      break;
    case 7:
      if (!ismem) {
        if (Rep(rde) == 3) {
          OpRdpid(A);
        } else {
          OpRdseed(A);
        }
      } else {
        OpUdImpl(m);
      }
      break;
    default:
      OpUdImpl(m);
  }
}

static u64 Bts(u64 x, u64 y) {
  return x | y;
}

static u64 Btr(u64 x, u64 y) {
  return x & ~y;
}

static u64 Btc(u64 x, u64 y) {
  return (x & ~y) | (~x & y);
}

static void OpBit(P) {
  u8 *p;
  int op;
  i64 bitdisp;
  unsigned bit;
  u64 v, x, y, z;
  u8 w, W[2][2] = {{2, 3}, {1, 3}};
  if (Lock(rde)) LOCK(&m->system->lock_lock);
  w = W[Osz(rde)][Rexw(rde)];
  if (Opcode(rde) == 0xBA) {
    op = ModrmReg(rde);
    bit = uimm0 & ((8 << w) - 1);
    bitdisp = 0;
  } else {
    op = (Opcode(rde) & 070) >> 3;
    bitdisp = ReadRegisterSigned(rde, RegRexrReg(m, rde));
    bit = bitdisp & ((8 << w) - 1);
    bitdisp &= -(8 << w);
    bitdisp >>= 3;
  }
  if (IsModrmRegister(rde)) {
    p = RegRexbRm(m, rde);
  } else {
    v = MaskAddress(Eamode(rde), ComputeAddress(A) + bitdisp);
    p = ReserveAddress(m, v, 1 << w, false);
    if (op == 4) {
      SetReadAddr(m, v, 1 << w);
    } else {
      SetWriteAddr(m, v, 1 << w);
    }
  }
  y = 1;
  y <<= bit;
  x = ReadMemory(rde, p);
  m->flags = SetFlag(m->flags, FLAGS_CF, !!(y & x));
  switch (op) {
    case 4:
      return;
    case 5:
      z = Bts(x, y);
      break;
    case 6:
      z = Btr(x, y);
      break;
    case 7:
      z = Btc(x, y);
      break;
    default:
      OpUdImpl(m);
  }
  WriteRegisterOrMemory(rde, p, z);
  if (Lock(rde)) UNLOCK(&m->system->lock_lock);
}

static void OpSax(P) {
  if (Rexw(rde)) {
    Put64(m->ax, (i32)Get32(m->ax));
  } else if (!Osz(rde)) {
    Put64(m->ax, (u32)(i16)Get16(m->ax));
  } else {
    Put16(m->ax, (i8)Get8(m->ax));
  }
}

static void OpConvert(P) {
  if (Rexw(rde)) {
    Put64(m->dx, Get64(m->ax) & 0x8000000000000000 ? 0xffffffffffffffff : 0);
  } else if (!Osz(rde)) {
    Put64(m->dx, Get32(m->ax) & 0x80000000 ? 0xffffffff : 0);
  } else {
    Put16(m->dx, Get16(m->ax) & 0x8000 ? 0xffff : 0);
  }
}

static void OpBswapZvqp(P) {
  u64 x = Get64(RegRexbSrm(m, rde));
  if (Rexw(rde)) {
    Put64(RegRexbSrm(m, rde), SWAP64(x));
  } else if (!Osz(rde)) {
    Put64(RegRexbSrm(m, rde), SWAP32(x));
  } else {
    Put16(RegRexbSrm(m, rde), SWAP16(x));
  }
}

static void OpMovEbIb(P) {
  Store8(GetModrmRegisterBytePointerWrite1(A), uimm0);
}

static void OpMovAlOb(P) {
  i64 addr = AddressOb(A);
  SetWriteAddr(m, addr, 1);
  Put8(m->ax, Load8(ResolveAddress(m, addr)));
}

static void OpMovObAl(P) {
  i64 addr = AddressOb(A);
  SetReadAddr(m, addr, 1);
  Store8(ResolveAddress(m, addr), Get8(m->ax));
}

static void OpMovRaxOvqp(P) {
  i64 v = DataSegment(A, disp);
  SetReadAddr(m, v, 1 << RegLog2(rde));
  WriteRegister(rde, m->ax, ReadMemory(rde, ResolveAddress(m, v)));
}

static void OpMovOvqpRax(P) {
  i64 v = DataSegment(A, disp);
  SetWriteAddr(m, v, 1 << RegLog2(rde));
  WriteMemory(rde, ResolveAddress(m, v), Get64(m->ax));
}

static void OpMovEbGb(P) {
  Store8(GetModrmRegisterBytePointerWrite1(A), Get8(ByteRexrReg(m, rde)));
  Jitter(A, "A r0 D");
}

static void OpMovGbEb(P) {
  Put8(ByteRexrReg(m, rde), Load8(GetModrmRegisterBytePointerRead1(A)));
  unassert(!RegLog2(rde));
  Jitter(A, "B r0 C");
}

static void OpMovZbIb(P) {
  Put8(ByteRexbSrm(m, rde), uimm0);
}

static void OpMovZvqpIvqp(P) {
  WriteRegister(rde, RegRexbSrm(m, rde), uimm0);
  Jitter(A, "a2iu F", uimm0);
}

static relegated void OpIncZv(P) {
  if (!Osz(rde)) {
    Put32(RegSrm(m, rde), Inc32(m, Get32(RegSrm(m, rde)), 0));
  } else {
    Put16(RegSrm(m, rde), Inc16(m, Get16(RegSrm(m, rde)), 0));
  }
}

static relegated void OpDecZv(P) {
  if (!Osz(rde)) {
    Put32(RegSrm(m, rde), Dec32(m, Get32(RegSrm(m, rde)), 0));
  } else {
    Put16(RegSrm(m, rde), Dec16(m, Get16(RegSrm(m, rde)), 0));
  }
}

static void OpMovEvqpIvds(P) {
  WriteRegisterOrMemory(rde, GetModrmRegisterWordPointerWriteOszRexw(A), uimm0);
  Jitter(A, "a3iu D", uimm0);
}

static void OpMovEvqpGvqp(P) {
  WriteRegisterOrMemory(rde, GetModrmRegisterWordPointerWriteOszRexw(A),
                        ReadRegister(rde, RegRexrReg(m, rde)));
  Jitter(A, "A r0 D");
}

static void OpMovGvqpEvqp(P) {
  WriteRegister(rde, RegRexrReg(m, rde),
                ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(A)));
  Jitter(A, "B r0 C");
}

static void OpMovzbGvqpEb(P) {
  WriteRegister(rde, RegRexrReg(m, rde),
                Load8(GetModrmRegisterBytePointerRead1(A)));
}

static void OpMovzwGvqpEw(P) {
  WriteRegister(rde, RegRexrReg(m, rde),
                Load16(GetModrmRegisterWordPointerRead2(A)));
}

static void OpMovsbGvqpEb(P) {
  WriteRegister(rde, RegRexrReg(m, rde),
                (i8)Load8(GetModrmRegisterBytePointerRead1(A)));
}

static void OpMovswGvqpEw(P) {
  WriteRegister(rde, RegRexrReg(m, rde),
                (i16)Load16(GetModrmRegisterWordPointerRead2(A)));
}

static void OpMovsxdGdqpEd(P) {
  Put64(RegRexrReg(m, rde), (i32)Load32(GetModrmRegisterWordPointerRead4(A)));
}

static void AlubRo(P, aluop_f op) {
  op(m, Load8(GetModrmRegisterBytePointerRead1(A)), Get8(ByteRexrReg(m, rde)));
  Jitter(A, "B r0s1= A r0a2= s1a1= s0a0= c", op);
}

static void OpAlubCmp(P) {
  AlubRo(A, Sub8);
}

static void OpAlubTest(P) {
  AlubRo(A, And8);
}

static void OpAlubFlip(P) {
  aluop_f op = kAlu[(Opcode(rde) & 070) >> 3][0];
  Put8(ByteRexrReg(m, rde), op(m, Get8(ByteRexrReg(m, rde)),
                               Load8(GetModrmRegisterBytePointerRead1(A))));
  Jitter(A, "A r0s1= B r0a2= s1a1= s0a0= c r0 C", op);
}

static void OpAlubFlipCmp(P) {
  Sub8(m, Get8(ByteRexrReg(m, rde)),
       Load8(GetModrmRegisterBytePointerRead1(A)));
  Jitter(A, "A r0s1= B r0a2= s1a1= s0a0= c", Sub8);
}

static void Alubi(P, aluop_f op) {
  u8 *a = GetModrmRegisterBytePointerWrite1(A);
  Store8(a, op(m, Load8(a), uimm0));
}

static void AlubiRo(P, aluop_f op) {
  op(m, Load8(GetModrmRegisterBytePointerRead1(A)), uimm0);
}

static void OpAlubiTest(P) {
  AlubiRo(A, And8);
}

static void OpAlubiReg(P) {
  if (ModrmReg(rde) == ALU_CMP) {
    AlubiRo(A, kAlu[ModrmReg(rde)][0]);
  } else {
    Alubi(A, kAlu[ModrmReg(rde)][0]);
  }
}

static void OpAluwCmpReg64(struct Machine *m, long rexb, long rexr) {
  Sub64(m, Get64(m->weg[rexb]), Get64(m->weg[rexr]));
}

static void AluwRo(P, const aluop_f ops[4]) {
  aluop_f op = ops[RegLog2(rde)];
  op(m, ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(A)),
     ReadRegister(rde, RegRexrReg(m, rde)));
  if (op == Sub64 && IsModrmRegister(rde)) {
    Jitter(A, "a1i a2i c", RexbRm(rde), RexrReg(rde), OpAluwCmpReg64);
  } else {
    Jitter(A, "B r0s1= A r0a2= s1a1= s0a0= c", op);
  }
}

static void OpAluwCmp(P) {
  AluwRo(A, kAlu[ALU_SUB]);
}

static void OpAluwTest(P) {
  AluwRo(A, kAlu[ALU_AND]);
}

static void OpAluwFlip(P) {
  aluop_f op = kAlu[(Opcode(rde) & 070) >> 3][RegLog2(rde)];
  WriteRegister(rde, RegRexrReg(m, rde),
                op(m, ReadRegister(rde, RegRexrReg(m, rde)),
                   ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(A))));
  Jitter(A, "B r0s1= A r0a1= s1a2= s0a0= c r0 C", op);
}

static void AluwFlipRo(P, const aluop_f ops[4]) {
  aluop_f op = ops[RegLog2(rde)];
  op(m, ReadRegister(rde, RegRexrReg(m, rde)),
     ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(A)));
  Jitter(A, "B r0s1= A r0a1= s1a2= s0a0= c", op);
}

static void OpAluwFlipCmp(P) {
  AluwFlipRo(A, kAlu[ALU_SUB]);
}

static void Aluwi(P) {
  aluop_f op = kAlu[ModrmReg(rde)][RegLog2(rde)];
  u8 *a = GetModrmRegisterWordPointerWriteOszRexw(A);
  WriteRegisterOrMemory(rde, a, op(m, ReadMemory(rde, a), uimm0));
  Jitter(A, "B r0a1= a2i", uimm0);
  if (CanSkipFlags(m, CF | ZF | SF | OF | AF | PF)) {
    if (GetFlagDeps(rde)) Jitter(A, "s0a0=");
    Jitter(A, "m r0 D", kJustAlu[ModrmReg(rde)]);
  } else {
    Jitter(A, "s0a0= c r0 D", op);
  }
}

static void AluwiRo(P, const aluop_f ops[4]) {
  aluop_f op = ops[RegLog2(rde)];
  op(m, ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(A)), uimm0);
  Jitter(A, "B r0a1= a2i s0a0= c", uimm0, op);
}

static void OpAluwiReg(P) {
  if (ModrmReg(rde) == ALU_CMP) {
    AluwiRo(A, kAlu[ModrmReg(rde)]);
  } else {
    Aluwi(A);
  }
}

static void AluAlIb(P, aluop_f op) {
  Put8(m->ax, op(m, Get8(m->ax), uimm0));
}

static void OpAluAlIbAdd(P) {
  AluAlIb(A, Add8);
}

static void OpAluAlIbOr(P) {
  AluAlIb(A, Or8);
}

static void OpAluAlIbAdc(P) {
  AluAlIb(A, Adc8);
}

static void OpAluAlIbSbb(P) {
  AluAlIb(A, Sbb8);
}

static void OpAluAlIbAnd(P) {
  AluAlIb(A, And8);
}

static void OpAluAlIbSub(P) {
  AluAlIb(A, Sub8);
}

static void OpAluAlIbXor(P) {
  AluAlIb(A, Xor8);
}

static void OpAluRaxIvds(P) {
  WriteRegister(rde, m->ax,
                kAlu[(Opcode(rde) & 070) >> 3][RegLog2(rde)](
                    m, ReadRegister(rde, m->ax), uimm0));
}

static void OpCmpAlIb(P) {
  Sub8(m, Get8(m->ax), uimm0);
}

static void OpCmpRaxIvds(P) {
  kAlu[ALU_SUB][RegLog2(rde)](m, ReadRegister(rde, m->ax), uimm0);
}

static void OpTestAlIb(P) {
  And8(m, Get8(m->ax), uimm0);
}

static void OpTestRaxIvds(P) {
  kAlu[ALU_AND][RegLog2(rde)](m, ReadRegister(rde, m->ax), uimm0);
}

static void OpBsuwiCl(P) {
  aluop_f op = kBsu[ModrmReg(rde)][RegLog2(rde)];
  u8 *p = GetModrmRegisterWordPointerWriteOszRexw(A);
  WriteRegisterOrMemory(rde, p, op(m, ReadMemory(rde, p), m->cl));
  Jitter(A, "B r0s1= %cl r0a2= s1a1= s0a0= c r0 D", op);
}

static void BsuwiConstant(P, u64 y) {
  aluop_f op = kBsu[ModrmReg(rde)][RegLog2(rde)];
  u8 *p = GetModrmRegisterWordPointerWriteOszRexw(A);
  WriteRegisterOrMemory(rde, p, op(m, ReadMemory(rde, p), y));
  Jitter(A, "B r0a1= s0a0=");
  switch (ModrmReg(rde)) {
    case BSU_ROL:
    case BSU_ROR:
    case BSU_SHL:
    case BSU_SHR:
    case BSU_SAL:
    case BSU_SAR:
      if (Rexw(rde) && (y &= 63) && CanSkipFlags(m, GetFlagClobbers(rde))) {
        Jitter(A, "a2i m r0 D", y, kJustBsu[ModrmReg(rde)]);
        return;
      }
      break;
    default:
      break;
  }
  Jitter(A, "a2i c r0 D", y, op);
}

static void OpBsuwi1(P) {
  BsuwiConstant(A, 1);
}

static void OpBsuwiImm(P) {
  BsuwiConstant(A, uimm0);
}

static aluop_f Bsubi(P, u64 y) {
  aluop_f op = kBsu[ModrmReg(rde)][RegLog2(rde)];
  u8 *a = GetModrmRegisterBytePointerWrite1(A);
  Store8(a, op(m, Load8(a), y));
  return op;
}

static void OpBsubiCl(P) {
  Jitter(A, "B r0s1= %cl r0a2= s1a1= s0a0= c r0 D", Bsubi(A, m->cl));
}

static void BsubiConstant(P, u64 y) {
  Jitter(A, "B r0a1= s0a0= a2i c r0 D", y, Bsubi(A, y));
}

static void OpBsubi1(P) {
  BsubiConstant(A, 1);
}

static void OpBsubiImm(P) {
  BsubiConstant(A, uimm0);
}

static void OpPushImm(P) {
  Push(A, uimm0);
}

static void OpInterruptImm(P) {
  HaltMachine(m, uimm0);
}

static void OpInterrupt1(P) {
  HaltMachine(m, 1);
}

static void OpInterrupt3(P) {
  HaltMachine(m, 3);
}

static void OpJmp(P) {
  m->ip += disp;
  Jitter(A, "a1i m", disp, FastJmp);
}

static void OpJae(P) {
  if (~m->flags & CF) {
    m->ip += disp;
  }
}

static void OpJne(P) {
  if (~m->flags & ZF) {
    m->ip += disp;
  }
}

static void OpJe(P) {
  if (m->flags & ZF) {
    m->ip += disp;
  }
}

static void OpJb(P) {
  if (m->flags & CF) {
    m->ip += disp;
  }
}

static void OpJbe(P) {
  if (IsBelowOrEqual(m)) {
    m->ip += disp;
  }
}

static void OpJo(P) {
  if (m->flags & OF) {
    m->ip += disp;
  }
}

static void OpJno(P) {
  if (~m->flags & OF) {
    m->ip += disp;
  }
}

static void OpJa(P) {
  if (IsAbove(m)) {
    m->ip += disp;
  }
}

static void OpJs(P) {
  if (m->flags & SF) {
    m->ip += disp;
  }
}

static void OpJns(P) {
  if (~m->flags & SF) {
    m->ip += disp;
  }
}

static void OpJp(P) {
  if (IsParity(m)) {
    m->ip += disp;
  }
}

static void OpJnp(P) {
  if (!IsParity(m)) {
    m->ip += disp;
  }
}

static void OpJl(P) {
  if (IsLess(m)) {
    m->ip += disp;
  }
}

static void OpJge(P) {
  if (IsGreaterOrEqual(m)) {
    m->ip += disp;
  }
}

static void OpJle(P) {
  if (IsLessOrEqual(m)) {
    m->ip += disp;
  }
}

static void OpJg(P) {
  if (IsGreater(m)) {
    m->ip += disp;
  }
}

static void OpCmov(P, bool taken) {
  u64 x;
  if (taken) {
    x = ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(A));
  } else {
    x = Get64(RegRexrReg(m, rde));
  }
  WriteRegister(rde, RegRexrReg(m, rde), x);
}

static void OpCmovo(P) {
  OpCmov(A, GetFlag(m->flags, FLAGS_OF));
}

static void OpCmovno(P) {
  OpCmov(A, !GetFlag(m->flags, FLAGS_OF));
}

static void OpCmovb(P) {
  OpCmov(A, GetFlag(m->flags, FLAGS_CF));
}

static void OpCmovae(P) {
  OpCmov(A, !GetFlag(m->flags, FLAGS_CF));
}

static void OpCmove(P) {
  OpCmov(A, GetFlag(m->flags, FLAGS_ZF));
}

static void OpCmovne(P) {
  OpCmov(A, !GetFlag(m->flags, FLAGS_ZF));
}

static void OpCmovbe(P) {
  OpCmov(A, IsBelowOrEqual(m));
}

static void OpCmova(P) {
  OpCmov(A, IsAbove(m));
}

static void OpCmovs(P) {
  OpCmov(A, GetFlag(m->flags, FLAGS_SF));
}

static void OpCmovns(P) {
  OpCmov(A, !GetFlag(m->flags, FLAGS_SF));
}

static void OpCmovp(P) {
  OpCmov(A, IsParity(m));
}

static void OpCmovnp(P) {
  OpCmov(A, !IsParity(m));
}

static void OpCmovl(P) {
  OpCmov(A, IsLess(m));
}

static void OpCmovge(P) {
  OpCmov(A, IsGreaterOrEqual(m));
}

static void OpCmovle(P) {
  OpCmov(A, IsLessOrEqual(m));
}

static void OpCmovg(P) {
  OpCmov(A, IsGreater(m));
}

static void SetEb(P, bool x) {
  Store8(GetModrmRegisterBytePointerWrite1(A), x);
}

static void OpSeto(P) {
  SetEb(A, GetFlag(m->flags, FLAGS_OF));
}

static void OpSetno(P) {
  SetEb(A, !GetFlag(m->flags, FLAGS_OF));
}

static void OpSetb(P) {
  SetEb(A, GetFlag(m->flags, FLAGS_CF));
}

static void OpSetae(P) {
  SetEb(A, !GetFlag(m->flags, FLAGS_CF));
}

static void OpSete(P) {
  SetEb(A, GetFlag(m->flags, FLAGS_ZF));
}

static void OpSetne(P) {
  SetEb(A, !GetFlag(m->flags, FLAGS_ZF));
}

static void OpSetbe(P) {
  SetEb(A, IsBelowOrEqual(m));
}

static void OpSeta(P) {
  SetEb(A, IsAbove(m));
}

static void OpSets(P) {
  SetEb(A, GetFlag(m->flags, FLAGS_SF));
}

static void OpSetns(P) {
  SetEb(A, !GetFlag(m->flags, FLAGS_SF));
}

static void OpSetp(P) {
  SetEb(A, IsParity(m));
}

static void OpSetnp(P) {
  SetEb(A, !IsParity(m));
}

static void OpSetl(P) {
  SetEb(A, IsLess(m));
}

static void OpSetge(P) {
  SetEb(A, IsGreaterOrEqual(m));
}

static void OpSetle(P) {
  SetEb(A, IsLessOrEqual(m));
}

static void OpSetg(P) {
  SetEb(A, IsGreater(m));
}

static void OpJcxz(P) {
  if (!MaskAddress(Eamode(rde), Get64(m->cx))) {
    m->ip += disp;
  }
}

static u64 AluPopcnt(P, u64 x) {
  m->flags = SetFlag(m->flags, FLAGS_ZF, !x);
  m->flags = SetFlag(m->flags, FLAGS_CF, false);
  m->flags = SetFlag(m->flags, FLAGS_SF, false);
  m->flags = SetFlag(m->flags, FLAGS_OF, false);
  m->flags = SetFlag(m->flags, FLAGS_PF, false);
  return popcount(x);
}

static u64 AluBsr(P, u64 x) {
  unsigned n;
  if (Rexw(rde)) {
    x &= 0xffffffffffffffff;
    n = 64;
  } else if (!Osz(rde)) {
    x &= 0xffffffff;
    n = 32;
  } else {
    x &= 0xffff;
    n = 16;
  }
  if (Rep(rde) == 3) {
    if (!x) {
      m->flags = SetFlag(m->flags, FLAGS_CF, true);
      m->flags = SetFlag(m->flags, FLAGS_ZF, false);
      return n;
    } else {
      m->flags = SetFlag(m->flags, FLAGS_CF, false);
      m->flags = SetFlag(m->flags, FLAGS_ZF, x == 1);
    }
  } else {
    m->flags = SetFlag(m->flags, FLAGS_ZF, !x);
    if (!x) return 0;
  }
  return bsr(x);
}

static u64 AluBsf(P, u64 x) {
  unsigned n;
  if (Rexw(rde)) {
    x &= 0xffffffffffffffff;
    n = 64;
  } else if (!Osz(rde)) {
    x &= 0xffffffff;
    n = 32;
  } else {
    x &= 0xffff;
    n = 16;
  }
  if (Rep(rde) == 3) {
    if (!x) {
      m->flags = SetFlag(m->flags, FLAGS_CF, true);
      m->flags = SetFlag(m->flags, FLAGS_ZF, false);
      return n;
    } else {
      m->flags = SetFlag(m->flags, FLAGS_CF, false);
      m->flags = SetFlag(m->flags, FLAGS_ZF, x & 1);
    }
  } else {
    m->flags = SetFlag(m->flags, FLAGS_ZF, !x);
    if (!x) return 0;
  }
  return bsf(x);
}

static void Bitscan(P, u64 op(P, u64)) {
  WriteRegister(
      rde, RegRexrReg(m, rde),
      op(A, ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(A))));
}

static void OpBsf(P) {
  Bitscan(A, AluBsf);
}

static void OpBsr(P) {
  Bitscan(A, AluBsr);
}

static void Op1b8(P) {
  if (Rep(rde) == 3) {
    Bitscan(A, AluPopcnt);
  } else {
    OpUdImpl(m);
  }
}

static relegated void LoadFarPointer(P, u64 *seg) {
  u32 fp = Load32(ComputeReserveAddressRead4(A));
  *seg = (fp & 0x0000ffff) << 4;
  Put16(RegRexrReg(m, rde), fp >> 16);
}

static relegated void OpLes(P) {
  LoadFarPointer(A, &m->es);
}

static relegated void OpLds(P) {
  LoadFarPointer(A, &m->ds);
}

static relegated void Loop(P, bool cond) {
  u64 cx;
  cx = Get64(m->cx) - 1;
  if (Eamode(rde) != XED_MODE_REAL) {
    if (Eamode(rde) == XED_MODE_LEGACY) {
      cx &= 0xffffffff;
    }
    Put64(m->cx, cx);
  } else {
    cx &= 0xffff;
    Put16(m->cx, cx);
  }
  if (cx && cond) {
    m->ip += disp;
  }
}

static relegated void OpLoope(P) {
  Loop(A, GetFlag(m->flags, FLAGS_ZF));
}

static relegated void OpLoopne(P) {
  Loop(A, !GetFlag(m->flags, FLAGS_ZF));
}

static relegated void OpLoop1(P) {
  Loop(A, true);
}

static const nexgen32e_f kOp0f6[] = {
    OpAlubiTest,
    OpAlubiTest,
    OpNotEb,
    OpNegEb,
    OpMulAxAlEbUnsigned,
    OpMulAxAlEbSigned,
    OpDivAlAhAxEbUnsigned,
    OpDivAlAhAxEbSigned,
};

static void Op0f6(P) {
  kOp0f6[ModrmReg(rde)](A);
}

static void OpTestEvqpIvds(P) {
  AluwiRo(A, kAlu[ALU_AND]);
}

static const nexgen32e_f kOp0f7[] = {
    OpTestEvqpIvds,
    OpTestEvqpIvds,
    OpNotEvqp,
    OpNegEvqp,
    OpMulRdxRaxEvqpUnsigned,
    OpMulRdxRaxEvqpSigned,
    OpDivRdxRaxEvqpUnsigned,
    OpDivRdxRaxEvqpSigned,
};

static void Op0f7(P) {
  kOp0f7[ModrmReg(rde)](A);
}

static const nexgen32e_f kOp0ff[] = {
    OpIncEvqp,  //
    OpDecEvqp,  //
    OpCallEq,   //
    OpUd,       //
    OpJmpEq,    //
    OpUd,       //
    OpPushEvq,  //
    OpUd,       //
};

static void Op0ff(P) {
  kOp0ff[ModrmReg(rde)](A);
  // Jitter(A, "a1i a2i c", rde, disp, kOp0ff[ModrmReg(rde)]);
}

static void OpDoubleShift(P) {
  u8 *p;
  u8 W[2][2] = {{2, 3}, {1, 3}};
  p = GetModrmRegisterWordPointerWriteOszRexw(A);
  WriteRegisterOrMemory(
      rde, p,
      BsuDoubleShift(m, W[Osz(rde)][Rexw(rde)], ReadMemory(rde, p),
                     ReadRegister(rde, RegRexrReg(m, rde)),
                     Opcode(rde) & 1 ? m->cl : uimm0, Opcode(rde) & 8));
}

static void OpFxsave(P) {
  i64 v;
  u8 buf[32];
  memset(buf, 0, 32);
  Write16(buf + 0, m->fpu.cw);
  Write16(buf + 2, m->fpu.sw);
  Write8(buf + 4, m->fpu.tw);
  Write16(buf + 6, m->fpu.op);
  Write32(buf + 8, m->fpu.ip);
  Write32(buf + 24, m->mxcsr);
  v = ComputeAddress(A);
  CopyToUser(m, v + 0, buf, 32);
  CopyToUser(m, v + 32, m->fpu.st, 128);
  CopyToUser(m, v + 160, m->xmm, 256);
  SetWriteAddr(m, v, 416);
}

static void OpFxrstor(P) {
  i64 v;
  u8 buf[32];
  v = ComputeAddress(A);
  SetReadAddr(m, v, 416);
  CopyFromUser(m, buf, v + 0, 32);
  CopyFromUser(m, m->fpu.st, v + 32, 128);
  CopyFromUser(m, m->xmm, v + 160, 256);
  m->fpu.cw = Load16(buf + 0);
  m->fpu.sw = Load16(buf + 2);
  m->fpu.tw = Load8(buf + 4);
  m->fpu.op = Load16(buf + 6);
  m->fpu.ip = Load32(buf + 8);
  m->mxcsr = Load32(buf + 24);
}

static void OpXsave(P) {
}

static void OpLdmxcsr(P) {
  m->mxcsr = Load32(ComputeReserveAddressRead4(A));
}

static void OpStmxcsr(P) {
  Store32(ComputeReserveAddressWrite4(A), m->mxcsr);
}

static void OpRdfsbase(P) {
  WriteRegister(rde, RegRexbRm(m, rde), m->fs);
}

static void OpRdgsbase(P) {
  WriteRegister(rde, RegRexbRm(m, rde), m->gs);
}

static void OpWrfsbase(P) {
  m->fs = ReadRegister(rde, RegRexbRm(m, rde));
}

static void OpWrgsbase(P) {
  m->gs = ReadRegister(rde, RegRexbRm(m, rde));
}

static void OpMfence(P) {
  atomic_thread_fence(memory_order_seq_cst);
}

static void OpLfence(P) {
  OpMfence(A);
}

static void OpSfence(P) {
  OpMfence(A);
}

static void OpClflush(P) {
  OpMfence(A);
}

static void Op1ae(P) {
  bool ismem;
  ismem = !IsModrmRegister(rde);
  switch (ModrmReg(rde)) {
    case 0:
      if (ismem) {
        OpFxsave(A);
      } else {
        OpRdfsbase(A);
      }
      break;
    case 1:
      if (ismem) {
        OpFxrstor(A);
      } else {
        OpRdgsbase(A);
      }
      break;
    case 2:
      if (ismem) {
        OpLdmxcsr(A);
      } else {
        OpWrfsbase(A);
      }
      break;
    case 3:
      if (ismem) {
        OpStmxcsr(A);
      } else {
        OpWrgsbase(A);
      }
      break;
    case 4:
      if (ismem) {
        OpXsave(A);
      } else {
        OpUdImpl(m);
      }
      break;
    case 5:
      OpLfence(A);
      break;
    case 6:
      OpMfence(A);
      break;
    case 7:
      if (ismem) {
        OpClflush(A);
      } else {
        OpSfence(A);
      }
      break;
    default:
      OpUdImpl(m);
  }
}

static relegated void OpSalc(P) {
  if (GetFlag(m->flags, FLAGS_CF)) {
    m->al = 255;
  } else {
    m->al = 0;
  }
}

static relegated void OpBofram(P) {
  if (disp) {
    m->bofram[0] = m->ip;
    m->bofram[1] = m->ip + (disp & 0xff);
  } else {
    m->bofram[0] = 0;
    m->bofram[1] = 0;
  }
}

static relegated void OpBinbase(P) {
  if (m->system->onbinbase) {
    m->system->onbinbase(m);
  }
}

static void OpNoop(P) {
  if (m->path.jb) {
    AppendJitNop(m->path.jb);
  }
}

static void OpNopEv(P) {
  switch (ModrmMod(rde) << 6 | ModrmReg(rde) << 3 | ModrmRm(rde)) {
    case 0105:
      OpBofram(A);
      break;
    case 0007:
    case 0107:
    case 0207:
      OpBinbase(A);
      break;
    default:
      OpNoop(A);
  }
}

static void OpNop(P) {
  if (Rexb(rde)) {
    OpXchgZvqp(A);
  } else if (Rep(rde) == 3) {
    OpPause(A);
  } else {
    OpNoop(A);
  }
}

static relegated void OpMovRqCq(P) {
  switch (ModrmReg(rde)) {
    case 0:
      Put64(RegRexbRm(m, rde), m->system->cr0);
      break;
    case 2:
      Put64(RegRexbRm(m, rde), m->system->cr2);
      break;
    case 3:
      Put64(RegRexbRm(m, rde), m->system->cr3);
      break;
    case 4:
      Put64(RegRexbRm(m, rde), m->system->cr4);
      break;
    default:
      OpUdImpl(m);
  }
}

static relegated void OpMovCqRq(P) {
  switch (ModrmReg(rde)) {
    case 0:
      m->system->cr0 = Get64(RegRexbRm(m, rde));
      break;
    case 2:
      m->system->cr2 = Get64(RegRexbRm(m, rde));
      break;
    case 3:
      m->system->cr3 = Get64(RegRexbRm(m, rde));
      break;
    case 4:
      m->system->cr4 = Get64(RegRexbRm(m, rde));
      break;
    default:
      OpUdImpl(m);
  }
}

static relegated void OpWrmsr(P) {
}

static relegated void OpRdmsr(P) {
  Put32(m->dx, 0);
  Put32(m->ax, 0);
}

static void OpEmms(P) {
  m->fpu.tw = -1;
}

int ClassifyOp(u64 rde) {
  switch (Mopcode(rde)) {
    default:
      return kOpNormal;
    case 0x070:  // OpJo
    case 0x071:  // OpJno
    case 0x072:  // OpJb
    case 0x073:  // OpJae
    case 0x074:  // OpJe
    case 0x075:  // OpJne
    case 0x076:  // OpJbe
    case 0x077:  // OpJa
    case 0x078:  // OpJs
    case 0x079:  // OpJns
    case 0x07A:  // OpJp
    case 0x07B:  // OpJnp
    case 0x07C:  // OpJl
    case 0x07D:  // OpJge
    case 0x07E:  // OpJle
    case 0x07F:  // OpJg
    case 0x09A:  // OpCallf
    case 0x0C2:  // OpRetIw
    case 0x0C3:  // OpRet
    case 0x0CA:  // OpRetf
    case 0x0CB:  // OpRetf
    case 0x0E0:  // OpLoopne
    case 0x0E1:  // OpLoope
    case 0x0E2:  // OpLoop1
    case 0x0E3:  // OpJcxz
    case 0x0E8:  // OpCallJvds
    case 0x0E9:  // OpJmp
    case 0x0EA:  // OpJmpf
    case 0x0EB:  // OpJmp
    case 0x0cf:  // OpIret
    case 0x180:  // OpJo
    case 0x181:  // OpJno
    case 0x182:  // OpJb
    case 0x183:  // OpJae
    case 0x184:  // OpJe
    case 0x185:  // OpJne
    case 0x186:  // OpJbe
    case 0x187:  // OpJa
    case 0x188:  // OpJs
    case 0x189:  // OpJns
    case 0x18A:  // OpJp
    case 0x18B:  // OpJnp
    case 0x18C:  // OpJl
    case 0x18D:  // OpJge
    case 0x18E:  // OpJle
    case 0x18F:  // OpJg
      return kOpBranching;
    case 0x0FF:  // Op0ff
      switch (ModrmReg(rde)) {
        case 2:  // call Ev
        case 4:  // jmp Ev
          return kOpBranching;
        default:
          return kOpNormal;
      }
    case 0x105:  // OpSyscall
      // We don't want clone() to fork JIT pathmaking.
      return kOpPrecious;
  }
}

static const nexgen32e_f kNexgen32e[] = {
    /*000*/ OpAlub,                  //
    /*001*/ OpAluw,                  // #8    (5.653689%)
    /*002*/ OpAlubFlip,              // #180  (0.000087%)
    /*003*/ OpAluwFlip,              // #7    (5.840835%)
    /*004*/ OpAluAlIbAdd,            //
    /*005*/ OpAluRaxIvds,            // #166  (0.000114%)
    /*006*/ OpPushSeg,               //
    /*007*/ OpPopSeg,                //
    /*008*/ OpAlub,                  // #154  (0.000207%)
    /*009*/ OpAluw,                  // #21   (0.520082%)
    /*00A*/ OpAlubFlip,              // #120  (0.001072%)
    /*00B*/ OpAluwFlip,              // #114  (0.001252%)
    /*00C*/ OpAluAlIbOr,             //
    /*00D*/ OpAluRaxIvds,            // #282  (0.000001%)
    /*00E*/ OpPushSeg,               //
    /*00F*/ OpPopSeg,                //
    /*010*/ OpAlub,                  //
    /*011*/ OpAluw,                  // #11   (5.307809%)
    /*012*/ OpAlubFlip,              //
    /*013*/ OpAluwFlip,              // #108  (0.001526%)
    /*014*/ OpAluAlIbAdc,            // #97   (0.002566%)
    /*015*/ OpAluRaxIvds,            //
    /*016*/ OpPushSeg,               //
    /*017*/ OpPopSeg,                //
    /*018*/ OpAlub,                  //
    /*019*/ OpAluw,                  // #65   (0.015300%)
    /*01A*/ OpAlubFlip,              //
    /*01B*/ OpAluwFlip,              // #44   (0.241806%)
    /*01C*/ OpAluAlIbSbb,            // #96   (0.002566%)
    /*01D*/ OpAluRaxIvds,            //
    /*01E*/ OpPushSeg,               //
    /*01F*/ OpPopSeg,                //
    /*020*/ OpAlub,                  // #165  (0.000130%)
    /*021*/ OpAluw,                  // #59   (0.019691%)
    /*022*/ OpAlubFlip,              //
    /*023*/ OpAluwFlip,              // #41   (0.279852%)
    /*024*/ OpAluAlIbAnd,            // #279  (0.000001%)
    /*025*/ OpAluRaxIvds,            // #43   (0.275823%)
    /*026*/ OpPushSeg,               //
    /*027*/ OpPopSeg,                //
    /*028*/ OpAlub,                  //
    /*029*/ OpAluw,                  // #29   (0.334693%)
    /*02A*/ OpAlubFlip,              // #179  (0.000087%)
    /*02B*/ OpAluwFlip,              // #71   (0.012465%)
    /*02C*/ OpAluAlIbSub,            //
    /*02D*/ OpAluRaxIvds,            // #112  (0.001317%)
    /*02E*/ OpUd,                    //
    /*02F*/ OpDas,                   //
    /*030*/ OpAlub,                  // #140  (0.000397%)
    /*031*/ OpAluw,                  // #3    (6.612252%)
    /*032*/ OpAlubFlip,              // #81   (0.007453%)
    /*033*/ OpAluwFlip,              // #47   (0.138021%)
    /*034*/ OpAluAlIbXor,            //
    /*035*/ OpAluRaxIvds,            // #295  (0.000000%)
    /*036*/ OpUd,                    //
    /*037*/ OpAaa,                   //
    /*038*/ OpAlubCmp,               // #98   (0.002454%)
    /*039*/ OpAluwCmp,               // #2    (6.687374%)
    /*03A*/ OpAlubFlipCmp,           // #103  (0.001846%)
    /*03B*/ OpAluwFlipCmp,           // #75   (0.010320%)
    /*03C*/ OpCmpAlIb,               // #85   (0.006267%)
    /*03D*/ OpCmpRaxIvds,            // #42   (0.279462%)
    /*03E*/ OpUd,                    //
    /*03F*/ OpAas,                   //
    /*040*/ OpIncZv,                 //
    /*041*/ OpIncZv,                 //
    /*042*/ OpIncZv,                 //
    /*043*/ OpIncZv,                 //
    /*044*/ OpIncZv,                 //
    /*045*/ OpIncZv,                 //
    /*046*/ OpIncZv,                 //
    /*047*/ OpIncZv,                 //
    /*048*/ OpDecZv,                 //
    /*049*/ OpDecZv,                 //
    /*04A*/ OpDecZv,                 //
    /*04B*/ OpDecZv,                 //
    /*04C*/ OpDecZv,                 //
    /*04D*/ OpDecZv,                 //
    /*04E*/ OpDecZv,                 //
    /*04F*/ OpDecZv,                 //
    /*050*/ OpPushZvq,               // #82   (0.007191%)
    /*051*/ OpPushZvq,               // #91   (0.003740%)
    /*052*/ OpPushZvq,               // #138  (0.000405%)
    /*053*/ OpPushZvq,               // #27   (0.343891%)
    /*054*/ OpPushZvq,               // #30   (0.332411%)
    /*055*/ OpPushZvq,               // #16   (0.661109%)
    /*056*/ OpPushZvq,               // #35   (0.297138%)
    /*057*/ OpPushZvq,               // #38   (0.289927%)
    /*058*/ OpPopZvq,                // #155  (0.000199%)
    /*059*/ OpPopZvq,                // #190  (0.000054%)
    /*05A*/ OpPopZvq,                // #74   (0.011075%)
    /*05B*/ OpPopZvq,                // #28   (0.343770%)
    /*05C*/ OpPopZvq,                // #31   (0.332403%)
    /*05D*/ OpPopZvq,                // #17   (0.659868%)
    /*05E*/ OpPopZvq,                // #36   (0.296997%)
    /*05F*/ OpPopZvq,                // #39   (0.289680%)
    /*060*/ OpPusha,                 //
    /*061*/ OpPopa,                  //
    /*062*/ OpUd,                    //
    /*063*/ OpMovsxdGdqpEd,          // #58   (0.026117%)
    /*064*/ OpUd,                    //
    /*065*/ OpUd,                    //
    /*066*/ OpUd,                    //
    /*067*/ OpUd,                    //
    /*068*/ OpPushImm,               // #168  (0.000112%)
    /*069*/ OpImulGvqpEvqpImm,       // #147  (0.000253%)
    /*06A*/ OpPushImm,               // #143  (0.000370%)
    /*06B*/ OpImulGvqpEvqpImm,       // #131  (0.000720%)
    /*06C*/ OpIns,                   //
    /*06D*/ OpIns,                   //
    /*06E*/ OpOuts,                  //
    /*06F*/ OpOuts,                  //
    /*070*/ OpJo,                    // #177  (0.000094%)
    /*071*/ OpJno,                   // #200  (0.000034%)
    /*072*/ OpJb,                    // #64   (0.015441%)
    /*073*/ OpJae,                   // #9    (5.615257%)
    /*074*/ OpJe,                    // #15   (0.713108%)
    /*075*/ OpJne,                   // #13   (0.825247%)
    /*076*/ OpJbe,                   // #23   (0.475584%)
    /*077*/ OpJa,                    // #48   (0.054677%)
    /*078*/ OpJs,                    // #66   (0.014096%)
    /*079*/ OpJns,                   // #84   (0.006506%)
    /*07A*/ OpJp,                    // #175  (0.000112%)
    /*07B*/ OpJnp,                   // #174  (0.000112%)
    /*07C*/ OpJl,                    // #223  (0.000008%)
    /*07D*/ OpJge,                   // #80   (0.007801%)
    /*07E*/ OpJle,                   // #70   (0.012536%)
    /*07F*/ OpJg,                    // #76   (0.010144%)
    /*080*/ OpAlubiReg,              // #53   (0.033021%)
    /*081*/ OpAluwiReg,              // #60   (0.018910%)
    /*082*/ OpAlubiReg,              //
    /*083*/ OpAluwiReg,              // #4    (6.518845%)
    /*084*/ OpAlubTest,              // #54   (0.030642%)
    /*085*/ OpAluwTest,              // #18   (0.628547%)
    /*086*/ OpXchgGbEb,              // #219  (0.000011%)
    /*087*/ OpXchgGvqpEvqp,          // #161  (0.000141%)
    /*088*/ OpMovEbGb,               // #49   (0.042510%)
    /*089*/ OpMovEvqpGvqp,           // #1    (22.226650%)
    /*08A*/ OpMovGbEb,               // #51   (0.038177%)
    /*08B*/ OpMovGvqpEvqp,           // #12   (2.903141%)
    /*08C*/ OpMovEvqpSw,             //
    /*08D*/ OpLeaGvqpM,              // #14   (0.800508%)
    /*08E*/ OpMovSwEvqp,             //
    /*08F*/ OpPopEvq,                // #288  (0.000000%)
    /*090*/ OpNop,                   // #218  (0.000011%)
    /*091*/ OpXchgZvqp,              // #278  (0.000001%)
    /*092*/ OpXchgZvqp,              // #284  (0.000001%)
    /*093*/ OpXchgZvqp,              // #213  (0.000018%)
    /*094*/ OpXchgZvqp,              //
    /*095*/ OpXchgZvqp,              //
    /*096*/ OpXchgZvqp,              //
    /*097*/ OpXchgZvqp,              // #286  (0.000001%)
    /*098*/ OpSax,                   // #83   (0.006728%)
    /*099*/ OpConvert,               // #163  (0.000137%)
    /*09A*/ OpCallf,                 //
    /*09B*/ OpFwait,                 //
    /*09C*/ OpPushf,                 //
    /*09D*/ OpPopf,                  //
    /*09E*/ OpSahf,                  //
    /*09F*/ OpLahf,                  //
    /*0A0*/ OpMovAlOb,               //
    /*0A1*/ OpMovRaxOvqp,            //
    /*0A2*/ OpMovObAl,               //
    /*0A3*/ OpMovOvqpRax,            //
    /*0A4*/ OpMovsb,                 // #73   (0.011594%)
    /*0A5*/ OpMovs,                  // #158  (0.000147%)
    /*0A6*/ OpCmps,                  //
    /*0A7*/ OpCmps,                  //
    /*0A8*/ OpTestAlIb,              // #115  (0.001247%)
    /*0A9*/ OpTestRaxIvds,           // #113  (0.001300%)
    /*0AA*/ OpStosb,                 // #67   (0.013327%)
    /*0AB*/ OpStos,                  // #194  (0.000044%)
    /*0AC*/ OpLods,                  // #198  (0.000035%)
    /*0AD*/ OpLods,                  // #296  (0.000000%)
    /*0AE*/ OpScas,                  // #157  (0.000152%)
    /*0AF*/ OpScas,                  // #292  (0.000000%)
    /*0B0*/ OpMovZbIb,               // #135  (0.000500%)
    /*0B1*/ OpMovZbIb,               // #178  (0.000093%)
    /*0B2*/ OpMovZbIb,               // #176  (0.000099%)
    /*0B3*/ OpMovZbIb,               // #202  (0.000028%)
    /*0B4*/ OpMovZbIb,               //
    /*0B5*/ OpMovZbIb,               // #220  (0.000010%)
    /*0B6*/ OpMovZbIb,               // #136  (0.000488%)
    /*0B7*/ OpMovZbIb,               // #216  (0.000014%)
    /*0B8*/ OpMovZvqpIvqp,           // #33   (0.315404%)
    /*0B9*/ OpMovZvqpIvqp,           // #50   (0.039176%)
    /*0BA*/ OpMovZvqpIvqp,           // #32   (0.315927%)
    /*0BB*/ OpMovZvqpIvqp,           // #93   (0.003549%)
    /*0BC*/ OpMovZvqpIvqp,           // #109  (0.001380%)
    /*0BD*/ OpMovZvqpIvqp,           // #101  (0.002061%)
    /*0BE*/ OpMovZvqpIvqp,           // #68   (0.013223%)
    /*0BF*/ OpMovZvqpIvqp,           // #79   (0.008900%)
    /*0C0*/ OpBsubiImm,              // #111  (0.001368%)
    /*0C1*/ OpBsuwiImm,              // #20   (0.536537%)
    /*0C2*/ OpRetIw,                 //
    /*0C3*/ OpRet,                   // #24   (0.422698%)
    /*0C4*/ OpLes,                   //
    /*0C5*/ OpLds,                   //
    /*0C6*/ OpMovEbIb,               // #90   (0.004525%)
    /*0C7*/ OpMovEvqpIvds,           // #45   (0.161349%)
    /*0C8*/ OpUd,                    //
    /*0C9*/ OpLeave,                 // #116  (0.001237%)
    /*0CA*/ OpRetf,                  //
    /*0CB*/ OpRetf,                  //
    /*0CC*/ OpInterrupt3,            //
    /*0CD*/ OpInterruptImm,          //
    /*0CE*/ OpUd,                    //
    /*0CF*/ OpUd,                    //
    /*0D0*/ OpBsubi1,                // #212  (0.000021%)
    /*0D1*/ OpBsuwi1,                // #61   (0.016958%)
    /*0D2*/ OpBsubiCl,               //
    /*0D3*/ OpBsuwiCl,               // #19   (0.621270%)
    /*0D4*/ OpAam,                   //
    /*0D5*/ OpAad,                   //
    /*0D6*/ OpSalc,                  //
    /*0D7*/ OpXlatAlBbb,             //
    /*0D8*/ OpFpu,                   // #258  (0.000005%)
    /*0D9*/ OpFpu,                   // #145  (0.000335%)
    /*0DA*/ OpFpu,                   // #290  (0.000000%)
    /*0DB*/ OpFpu,                   // #139  (0.000399%)
    /*0DC*/ OpFpu,                   // #283  (0.000001%)
    /*0DD*/ OpFpu,                   // #144  (0.000340%)
    /*0DE*/ OpFpu,                   // #193  (0.000046%)
    /*0DF*/ OpFpu,                   // #215  (0.000014%)
    /*0E0*/ OpLoopne,                //
    /*0E1*/ OpLoope,                 //
    /*0E2*/ OpLoop1,                 //
    /*0E3*/ OpJcxz,                  //
    /*0E4*/ OpInAlImm,               //
    /*0E5*/ OpInAxImm,               //
    /*0E6*/ OpOutImmAl,              //
    /*0E7*/ OpOutImmAx,              //
    /*0E8*/ OpCallJvds,              // #25   (0.403872%)
    /*0E9*/ OpJmp,                   // #22   (0.476546%)
    /*0EA*/ OpJmpf,                  //
    /*0EB*/ OpJmp,                   // #6    (6.012044%)
    /*0EC*/ OpInAlDx,                //
    /*0ED*/ OpInAxDx,                //
    /*0EE*/ OpOutDxAl,               //
    /*0EF*/ OpOutDxAx,               //
    /*0F0*/ OpUd,                    //
    /*0F1*/ OpInterrupt1,            //
    /*0F2*/ OpUd,                    //
    /*0F3*/ OpUd,                    //
    /*0F4*/ OpHlt,                   //
    /*0F5*/ OpCmc,                   //
    /*0F6*/ Op0f6,                   // #56   (0.028122%)
    /*0F7*/ Op0f7,                   // #10   (5.484639%)
    /*0F8*/ OpClc,                   // #156  (0.000187%)
    /*0F9*/ OpStc,                   //
    /*0FA*/ OpCli,                   //
    /*0FB*/ OpSti,                   //
    /*0FC*/ OpCld,                   // #142  (0.000379%)
    /*0FD*/ OpStd,                   // #141  (0.000379%)
    /*0FE*/ Op0fe,                   // #181  (0.000083%)
    /*0FF*/ Op0ff,                   // #5    (6.314024%)
    /*100*/ OpUd,                    //
    /*101*/ Op101,                   //
    /*102*/ OpUd,                    //
    /*103*/ OpLsl,                   //
    /*104*/ OpUd,                    //
    /*105*/ OpSyscall,               // #133  (0.000663%)
    /*106*/ OpUd,                    //
    /*107*/ OpUd,                    //
    /*108*/ OpUd,                    //
    /*109*/ OpUd,                    //
    /*10A*/ OpUd,                    //
    /*10B*/ OpUd,                    //
    /*10C*/ OpUd,                    //
    /*10D*/ OpHintNopEv,             //
    /*10E*/ OpUd,                    //
    /*10F*/ OpUd,                    //
    /*110*/ OpMov0f10,               // #89   (0.004629%)
    /*111*/ OpMovWpsVps,             // #104  (0.001831%)
    /*112*/ OpMov0f12,               //
    /*113*/ OpMov0f13,               //
    /*114*/ OpUnpcklpsd,             //
    /*115*/ OpUnpckhpsd,             //
    /*116*/ OpMov0f16,               //
    /*117*/ OpMov0f17,               //
    /*118*/ OpHintNopEv,             //
    /*119*/ OpHintNopEv,             //
    /*11A*/ OpHintNopEv,             //
    /*11B*/ OpHintNopEv,             //
    /*11C*/ OpHintNopEv,             //
    /*11D*/ OpHintNopEv,             //
    /*11E*/ OpHintNopEv,             //
    /*11F*/ OpNopEv,                 // #62   (0.016260%)
    /*120*/ OpMovRqCq,               //
    /*121*/ OpUd,                    //
    /*122*/ OpMovCqRq,               //
    /*123*/ OpUd,                    //
    /*124*/ OpUd,                    //
    /*125*/ OpUd,                    //
    /*126*/ OpUd,                    //
    /*127*/ OpUd,                    //
    /*128*/ OpMov0f28,               // #100  (0.002220%)
    /*129*/ OpMovWpsVps,             // #99   (0.002294%)
    /*12A*/ OpCvt0f2a,               // #173  (0.000112%)
    /*12B*/ OpMov0f2b,               //
    /*12C*/ OpCvtt0f2c,              // #172  (0.000112%)
    /*12D*/ OpCvt0f2d,               //
    /*12E*/ OpComissVsWs,            // #153  (0.000223%)
    /*12F*/ OpComissVsWs,            // #152  (0.000223%)
    /*130*/ OpWrmsr,                 //
    /*131*/ OpRdtsc,                 // #214  (0.000016%)
    /*132*/ OpRdmsr,                 //
    /*133*/ OpUd,                    //
    /*134*/ OpUd,                    //
    /*135*/ OpUd,                    //
    /*136*/ OpUd,                    //
    /*137*/ OpUd,                    //
    /*138*/ OpUd,                    //
    /*139*/ OpUd,                    //
    /*13A*/ OpUd,                    //
    /*13B*/ OpUd,                    //
    /*13C*/ OpUd,                    //
    /*13D*/ OpUd,                    //
    /*13E*/ OpUd,                    //
    /*13F*/ OpUd,                    //
    /*140*/ OpCmovo,                 //
    /*141*/ OpCmovno,                //
    /*142*/ OpCmovb,                 // #69   (0.012667%)
    /*143*/ OpCmovae,                // #276  (0.000002%)
    /*144*/ OpCmove,                 // #134  (0.000584%)
    /*145*/ OpCmovne,                // #132  (0.000700%)
    /*146*/ OpCmovbe,                // #125  (0.000945%)
    /*147*/ OpCmova,                 // #40   (0.289378%)
    /*148*/ OpCmovs,                 // #130  (0.000774%)
    /*149*/ OpCmovns,                // #149  (0.000228%)
    /*14A*/ OpCmovp,                 //
    /*14B*/ OpCmovnp,                //
    /*14C*/ OpCmovl,                 // #102  (0.002008%)
    /*14D*/ OpCmovge,                // #196  (0.000044%)
    /*14E*/ OpCmovle,                // #110  (0.001379%)
    /*14F*/ OpCmovg,                 // #121  (0.001029%)
    /*150*/ OpMovmskpsd,             //
    /*151*/ OpSqrtpsd,               //
    /*152*/ OpRsqrtps,               //
    /*153*/ OpRcpps,                 //
    /*154*/ OpAndpsd,                // #171  (0.000112%)
    /*155*/ OpAndnpsd,               //
    /*156*/ OpOrpsd,                 //
    /*157*/ OpXorpsd,                // #148  (0.000245%)
    /*158*/ OpAddpsd,                // #151  (0.000223%)
    /*159*/ OpMulpsd,                // #150  (0.000223%)
    /*15A*/ OpCvt0f5a,               //
    /*15B*/ OpCvt0f5b,               //
    /*15C*/ OpSubpsd,                // #146  (0.000335%)
    /*15D*/ OpMinpsd,                //
    /*15E*/ OpDivpsd,                // #170  (0.000112%)
    /*15F*/ OpMaxpsd,                //
    /*160*/ OpSsePunpcklbw,          // #259  (0.000003%)
    /*161*/ OpSsePunpcklwd,          // #221  (0.000009%)
    /*162*/ OpSsePunpckldq,          // #262  (0.000003%)
    /*163*/ OpSsePacksswb,           // #297  (0.000000%)
    /*164*/ OpSsePcmpgtb,            // #274  (0.000003%)
    /*165*/ OpSsePcmpgtw,            // #273  (0.000003%)
    /*166*/ OpSsePcmpgtd,            // #272  (0.000003%)
    /*167*/ OpSsePackuswb,           // #231  (0.000005%)
    /*168*/ OpSsePunpckhbw,          // #271  (0.000003%)
    /*169*/ OpSsePunpckhwd,          // #261  (0.000003%)
    /*16A*/ OpSsePunpckhdq,          // #260  (0.000003%)
    /*16B*/ OpSsePackssdw,           // #257  (0.000005%)
    /*16C*/ OpSsePunpcklqdq,         // #264  (0.000003%)
    /*16D*/ OpSsePunpckhqdq,         // #263  (0.000003%)
    /*16E*/ OpMov0f6e,               // #289  (0.000000%)
    /*16F*/ OpMov0f6f,               // #191  (0.000051%)
    /*170*/ OpShuffle,               // #164  (0.000131%)
    /*171*/ Op171,                   // #294  (0.000000%)
    /*172*/ Op172,                   // #293  (0.000000%)
    /*173*/ Op173,                   // #211  (0.000022%)
    /*174*/ OpSsePcmpeqb,            // #118  (0.001215%)
    /*175*/ OpSsePcmpeqw,            // #201  (0.000028%)
    /*176*/ OpSsePcmpeqd,            // #222  (0.000008%)
    /*177*/ OpEmms,                  //
    /*178*/ OpUd,                    //
    /*179*/ OpUd,                    //
    /*17A*/ OpUd,                    //
    /*17B*/ OpUd,                    //
    /*17C*/ OpHaddpsd,               //
    /*17D*/ OpHsubpsd,               //
    /*17E*/ OpMov0f7e,               // #122  (0.001005%)
    /*17F*/ OpMov0f7f,               // #192  (0.000048%)
    /*180*/ OpJo,                    //
    /*181*/ OpJno,                   //
    /*182*/ OpJb,                    // #107  (0.001532%)
    /*183*/ OpJae,                   // #72   (0.011761%)
    /*184*/ OpJe,                    // #55   (0.029121%)
    /*185*/ OpJne,                   // #57   (0.027593%)
    /*186*/ OpJbe,                   // #46   (0.147358%)
    /*187*/ OpJa,                    // #86   (0.005907%)
    /*188*/ OpJs,                    // #106  (0.001569%)
    /*189*/ OpJns,                   // #160  (0.000142%)
    /*18A*/ OpJp,                    //
    /*18B*/ OpJnp,                   //
    /*18C*/ OpJl,                    // #105  (0.001786%)
    /*18D*/ OpJge,                   // #281  (0.000001%)
    /*18E*/ OpJle,                   // #77   (0.009607%)
    /*18F*/ OpJg,                    // #126  (0.000890%)
    /*190*/ OpSeto,                  // #280  (0.000001%)
    /*191*/ OpSetno,                 //
    /*192*/ OpSetb,                  // #26   (0.364366%)
    /*193*/ OpSetae,                 // #183  (0.000063%)
    /*194*/ OpSete,                  // #78   (0.009363%)
    /*195*/ OpSetne,                 // #94   (0.003096%)
    /*196*/ OpSetbe,                 // #162  (0.000139%)
    /*197*/ OpSeta,                  // #92   (0.003559%)
    /*198*/ OpSets,                  //
    /*199*/ OpSetns,                 //
    /*19A*/ OpSetp,                  //
    /*19B*/ OpSetnp,                 //
    /*19C*/ OpSetl,                  // #119  (0.001079%)
    /*19D*/ OpSetge,                 // #275  (0.000002%)
    /*19E*/ OpSetle,                 // #167  (0.000112%)
    /*19F*/ OpSetg,                  // #95   (0.002688%)
    /*1A0*/ OpPushSeg,               //
    /*1A1*/ OpPopSeg,                //
    /*1A2*/ OpCpuid,                 // #285  (0.000001%)
    /*1A3*/ OpBit,                   //
    /*1A4*/ OpDoubleShift,           //
    /*1A5*/ OpDoubleShift,           //
    /*1A6*/ OpUd,                    //
    /*1A7*/ OpUd,                    //
    /*1A8*/ OpPushSeg,               //
    /*1A9*/ OpPopSeg,                //
    /*1AA*/ OpUd,                    //
    /*1AB*/ OpBit,                   // #291  (0.000000%)
    /*1AC*/ OpDoubleShift,           //
    /*1AD*/ OpDoubleShift,           //
    /*1AE*/ Op1ae,                   // #287  (0.000000%)
    /*1AF*/ OpImulGvqpEvqp,          // #34   (0.299503%)
    /*1B0*/ OpCmpxchgEbAlGb,         //
    /*1B1*/ OpCmpxchgEvqpRaxGvqp,    // #87   (0.005376%)
    /*1B2*/ OpUd,                    //
    /*1B3*/ OpBit,                   // #199  (0.000035%)
    /*1B4*/ OpUd,                    //
    /*1B5*/ OpUd,                    //
    /*1B6*/ OpMovzbGvqpEb,           // #37   (0.296523%)
    /*1B7*/ OpMovzwGvqpEw,           // #137  (0.000433%)
    /*1B8*/ Op1b8,                   //
    /*1B9*/ OpUd,                    //
    /*1BA*/ OpBit,                   // #127  (0.000879%)
    /*1BB*/ OpBit,                   //
    /*1BC*/ OpBsf,                   // #88   (0.005117%)
    /*1BD*/ OpBsr,                   // #123  (0.000985%)
    /*1BE*/ OpMovsbGvqpEb,           // #52   (0.035351%)
    /*1BF*/ OpMovswGvqpEw,           // #63   (0.015753%)
    /*1C0*/ OpXaddEbGb,              //
    /*1C1*/ OpXaddEvqpGvqp,          //
    /*1C2*/ OpCmppsd,                //
    /*1C3*/ OpMovntiMdqpGdqp,        //
    /*1C4*/ OpPinsrwVdqEwIb,         // #124  (0.000981%)
    /*1C5*/ OpPextrwGdqpUdqIb,       // #277  (0.000002%)
    /*1C6*/ OpShufpsd,               //
    /*1C7*/ Op1c7,                   // #189  (0.000054%)
    /*1C8*/ OpBswapZvqp,             // #159  (0.000145%)
    /*1C9*/ OpBswapZvqp,             // #182  (0.000069%)
    /*1CA*/ OpBswapZvqp,             // #197  (0.000039%)
    /*1CB*/ OpBswapZvqp,             // #217  (0.000012%)
    /*1CC*/ OpBswapZvqp,             //
    /*1CD*/ OpBswapZvqp,             //
    /*1CE*/ OpBswapZvqp,             // #129  (0.000863%)
    /*1CF*/ OpBswapZvqp,             // #128  (0.000863%)
    /*1D0*/ OpAddsubpsd,             //
    /*1D1*/ OpSsePsrlwv,             // #256  (0.000005%)
    /*1D2*/ OpSsePsrldv,             // #255  (0.000005%)
    /*1D3*/ OpSsePsrlqv,             // #254  (0.000005%)
    /*1D4*/ OpSsePaddq,              // #253  (0.000005%)
    /*1D5*/ OpSsePmullw,             // #188  (0.000054%)
    /*1D6*/ OpMov0fD6,               // #169  (0.000112%)
    /*1D7*/ OpPmovmskbGdqpNqUdq,     // #117  (0.001235%)
    /*1D8*/ OpSsePsubusb,            // #252  (0.000005%)
    /*1D9*/ OpSsePsubusw,            // #251  (0.000005%)
    /*1DA*/ OpSsePminub,             // #250  (0.000005%)
    /*1DB*/ OpSsePand,               // #249  (0.000005%)
    /*1DC*/ OpSsePaddusb,            // #248  (0.000005%)
    /*1DD*/ OpSsePaddusw,            // #247  (0.000005%)
    /*1DE*/ OpSsePmaxub,             // #246  (0.000005%)
    /*1DF*/ OpSsePandn,              // #245  (0.000005%)
    /*1E0*/ OpSsePavgb,              // #244  (0.000005%)
    /*1E1*/ OpSsePsrawv,             // #230  (0.000005%)
    /*1E2*/ OpSsePsradv,             // #224  (0.000008%)
    /*1E3*/ OpSsePavgw,              // #243  (0.000005%)
    /*1E4*/ OpSsePmulhuw,            // #229  (0.000005%)
    /*1E5*/ OpSsePmulhw,             // #187  (0.000054%)
    /*1E6*/ OpCvt0fE6,               //
    /*1E7*/ OpMov0fE7,               //
    /*1E8*/ OpSsePsubsb,             // #242  (0.000005%)
    /*1E9*/ OpSsePsubsw,             // #241  (0.000005%)
    /*1EA*/ OpSsePminsw,             // #240  (0.000005%)
    /*1EB*/ OpSsePor,                // #239  (0.000005%)
    /*1EC*/ OpSsePaddsb,             // #238  (0.000005%)
    /*1ED*/ OpSsePaddsw,             // #228  (0.000006%)
    /*1EE*/ OpSsePmaxsw,             // #237  (0.000005%)
    /*1EF*/ OpSsePxor,               // #195  (0.000044%)
    /*1F0*/ OpLddquVdqMdq,           //
    /*1F1*/ OpSsePsllwv,             // #226  (0.000006%)
    /*1F2*/ OpSsePslldv,             // #225  (0.000006%)
    /*1F3*/ OpSsePsllqv,             // #236  (0.000005%)
    /*1F4*/ OpSsePmuludq,            // #186  (0.000054%)
    /*1F5*/ OpSsePmaddwd,            // #185  (0.000054%)
    /*1F6*/ OpSsePsadbw,             // #270  (0.000003%)
    /*1F7*/ OpMaskMovDiXmmRegXmmRm,  //
    /*1F8*/ OpSsePsubb,              // #235  (0.000005%)
    /*1F9*/ OpSsePsubw,              // #234  (0.000005%)
    /*1FA*/ OpSsePsubd,              // #184  (0.000054%)
    /*1FB*/ OpSsePsubq,              // #269  (0.000003%)
    /*1FC*/ OpSsePaddb,              // #233  (0.000005%)
    /*1FD*/ OpSsePaddw,              // #227  (0.000006%)
    /*1FE*/ OpSsePaddd,              // #232  (0.000005%)
    /*1FF*/ OpUd,                    //
    /*200*/ OpSsePshufb,             // #268  (0.000003%)
    /*201*/ OpSsePhaddw,             // #204  (0.000027%)
    /*202*/ OpSsePhaddd,             // #210  (0.000027%)
    /*203*/ OpSsePhaddsw,            // #203  (0.000027%)
    /*204*/ OpSsePmaddubsw,          // #209  (0.000027%)
    /*205*/ OpSsePhsubw,             // #208  (0.000027%)
    /*206*/ OpSsePhsubd,             // #207  (0.000027%)
    /*207*/ OpSsePhsubsw,            // #206  (0.000027%)
    /*208*/ OpSsePsignb,             // #267  (0.000003%)
    /*209*/ OpSsePsignw,             // #266  (0.000003%)
    /*20A*/ OpSsePsignd,             // #265  (0.000003%)
    /*20B*/ OpSsePmulhrsw,           // #205  (0.000027%)
};

nexgen32e_f GetOp(long op) {
  if (op < ARRAYLEN(kNexgen32e)) {
    return kNexgen32e[op];
  } else {
    switch (op) {
      XLAT(0x21c, OpSsePabsb);
      XLAT(0x21d, OpSsePabsw);
      XLAT(0x21e, OpSsePabsd);
      XLAT(0x22a, OpMovntdqaVdqMdq);
      XLAT(0x240, OpSsePmulld);
      XLAT(0x30f, OpSsePalignr);
      XLAT(0x344, OpSsePclmulqdq);
      default:
        return OpUd;
    }
  }
}

static bool CanJit(struct Machine *m) {
  return !IsJitDisabled(&m->system->jit) && HasHook(m, m->ip);
}

bool HasHook(struct Machine *m, u64 pc) {
  return pc - m->codestart < m->codesize;
}

nexgen32e_f GetHook(struct Machine *m, u64 pc) {
  int off = atomic_load_explicit(m->fun + (uintptr_t)pc, memory_order_relaxed);
  return off ? (nexgen32e_f)(IMAGE_END + off) : GeneralDispatch;
}

void SetHook(struct Machine *m, u64 pc, nexgen32e_f func) {
  u8 *f;
  int off;
  if (func) {
    f = (u8 *)func;
    unassert(f - IMAGE_END);
    unassert(INT_MIN <= f - IMAGE_END && f - IMAGE_END <= INT_MAX);
    off = f - IMAGE_END;
  } else {
    off = 0;
  }
  atomic_store_explicit(m->fun + pc, off, memory_order_relaxed);
}

void JitlessDispatch(P) {
  ASM_LOGF("decoding [%s] at address %" PRIx64, DescribeOp(m, GetPc(m)),
           GetPc(m));
  STATISTIC(++instructions_dispatched);
  LoadInstruction(m, GetPc(m));
  m->oldip = m->ip;
  rde = m->xedd->op.rde;
  disp = m->xedd->op.disp;
  uimm0 = m->xedd->op.uimm0;
  m->ip += Oplength(rde);
  GetOp(Mopcode(rde))(A);
  if (m->stashaddr) CommitStash(m);
  m->oldip = -1;
}

void GeneralDispatch(P) {
#ifdef HAVE_JIT
  i64 newip;
  int opclass;
  intptr_t jitpc;
  ASM_LOGF("decoding [%s] at address %" PRIx64, DescribeOp(m, GetPc(m)),
           GetPc(m));
  LoadInstruction(m, GetPc(m));
  m->oldip = m->ip;
  rde = m->xedd->op.rde;
  disp = m->xedd->op.disp;
  uimm0 = m->xedd->op.uimm0;
  opclass = ClassifyOp(rde);
  if (m->path.jb || (opclass == kOpNormal && CanJit(m) && CreatePath(A))) {
    if (opclass == kOpNormal || opclass == kOpBranching) {
      ++m->path.elements;
      STATISTIC(++path_elements);
      AddPath_StartOp(A);
    }
    jitpc = GetJitPc(m->path.jb);
    JIT_LOGF("adding [%s] from address %" PRIx64
             " to path starting at %" PRIx64,
             DescribeOp(m, GetPc(m)), GetPc(m), m->path.start);
  } else {
    jitpc = 0;
  }
  m->ip += Oplength(rde);
  GetOp(Mopcode(rde))(A);
  if (m->stashaddr) {
    CommitStash(m);
  }
  if (jitpc) {
    newip = m->ip;
    m->ip = m->oldip;
    if (GetJitPc(m->path.jb) == jitpc) {
      if (opclass == kOpNormal || opclass == kOpBranching) {
        STATISTIC(++path_elements_auto);
        AddPath(A);
        AddPath_EndOp(A);
      } else {
        JIT_LOGF("won't add [%" PRIx64 " %s] so path started at %" PRIx64,
                 GetPc(m), DescribeOp(m, GetPc(m)), m->path.start);
      }
    } else {
      AddPath_EndOp(A);
    }
    if (opclass == kOpPrecious || opclass == kOpBranching) {
      CommitPath(A, 0);
    }
    m->ip = newip;
  }
  m->oldip = -1;
#endif
}

static void ExploreInstruction(struct Machine *m, nexgen32e_f func) {
  if (func == JitlessDispatch) {
    JIT_LOGF("abandoning path starting at %" PRIx64
             " due to running into staged path",
             m->path.start);
    AbandonPath(m);
    STATISTIC(++instructions_dispatched);
    func(DISPATCH_NOTHING);
    return;
  } else if (func != GeneralDispatch) {
    JIT_LOGF("splicing path starting at %" PRIx64
             " into previously created function %p",
             m->path.start, func);
    STATISTIC(++path_spliced);
    CommitPath(DISPATCH_NOTHING, (intptr_t)func);
    STATISTIC(++instructions_dispatched);
    func(DISPATCH_NOTHING);
    return;
  } else {
    GeneralDispatch(DISPATCH_NOTHING);
  }
}

void ExecuteInstruction(struct Machine *m) {
#ifdef HAVE_JIT
  nexgen32e_f func;
  if (HasHook(m, m->ip)) {
    func = GetHook(m, m->ip);
    if (!m->path.jb) {
      STATISTIC(++instructions_dispatched);
      func(DISPATCH_NOTHING);
    } else {
      ExploreInstruction(m, func);
    }
  } else {
    JitlessDispatch(DISPATCH_NOTHING);
  }
#else
  JitlessDispatch(DISPATCH_NOTHING);
#endif
}

static void CheckForSignals(struct Machine *m) {
  int sig;
  if (VERY_UNLIKELY(m->signals) && (sig = ConsumeSignal(m))) {
    TerminateSignal(m, sig);
  }
}

void Actor(struct Machine *m) {
  for (g_machine = m;;) {
    ExecuteInstruction(m);
    CheckForSignals(m);
    if (VERY_UNLIKELY(atomic_load_explicit(&m->killed, memory_order_relaxed))) {
      SysExit(m, 0);
    }
  }
}

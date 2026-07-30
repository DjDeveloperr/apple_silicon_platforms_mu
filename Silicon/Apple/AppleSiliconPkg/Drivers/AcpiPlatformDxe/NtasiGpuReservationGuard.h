/** @file
  Pure, freestanding, host-testable safety invariants for GPU preboot
  reservations.

  This header has ZERO dependencies -- no EDK2 headers, no <stdint.h>, no
  <stdbool.h> -- on purpose, so the exact same text compiles unmodified
  both into AcpiPlatform.c (under the EDK2/Clang AArch64 DXE toolchain)
  and into Tests/test_gpu_reservation_guard.c (compiled directly with the
  host cc, no EDK2 involved at all). There is exactly one copy of this
  logic; it is never reimplemented or duplicated by hand.

  WHY THIS EXISTS, AND WHY IT MOVED FROM PEI TO DXE: on 2026-07-30, a GPU
  preboot reservation computed from Mu's own PcdSystemMemoryBase+
  PcdSystemMemorySize placed "hw_data_a" directly on top of Mu's live PEI
  stack (SystemMemoryTop=0x103db29c000, computed hw_data_a=
  [0x103db2953cc, 0x103db29c000), live SP_EL1 observed at 0x103db29ba10 --
  squarely inside that range) and crashed the machine before a vector
  table even existed (ESR 0x82000007, ELR=FAR=0x200). That specific bug
  was fixed (hw_data_a/b/globals are no longer computed at all -- see
  NtasiResolveAndReserveGpuCarveouts() in AcpiPlatform.c), but a *second*
  independent hardware boot with the fix in place crashed again, in early
  PEI, at the *same* SP_EL1 value, with zero UART output either time --
  PEI has no exception vector table and no reliable way to report a fault
  no matter how carefully guarded the reservation logic is. So the GPU
  carveout resolution this header supports moved out of PEI entirely and
  now runs from AcpiPlatformDxe, late in DXE dispatch (after console,
  AIC2, and CpuDxe's exception vectors are all up) -- mirroring the same
  move that turned ANS's unreported hang into a one-line stage diagnosis.
  These two checks remain the hard backstop regardless of where the
  candidate address came from (ADT, a hardcoded constant, or anything
  else): a wrong GPU reservation must degrade the GPU, never touch memory
  Mu itself depends on.

  SPDX-License-Identifier: MIT
**/

#ifndef NTASI_GPU_RESERVATION_GUARD_H_
#define NTASI_GPU_RESERVATION_GUARD_H_

typedef unsigned long long  NTASI_GUARD_U64;
typedef int                 NTASI_GUARD_BOOL;

#define NTASI_GUARD_TRUE   1
#define NTASI_GUARD_FALSE  0

/**
  TRUE if the half-open range [Base, Base + Size) is non-empty and
  contains Point.

  A range that overflows the 64-bit address space cannot be reasoned
  about safely and is treated as containing every point (fail closed).
**/
static inline NTASI_GUARD_BOOL
NtasiRangeContainsPoint (
  NTASI_GUARD_U64  Base,
  NTASI_GUARD_U64  Size,
  NTASI_GUARD_U64  Point
  )
{
  NTASI_GUARD_U64  Top;

  if (Size == 0) {
    return NTASI_GUARD_FALSE;
  }

  Top = Base + (Size - 1);
  if (Top < Base) {
    return NTASI_GUARD_TRUE;
  }

  return ((Point >= Base) && (Point <= Top)) ? NTASI_GUARD_TRUE : NTASI_GUARD_FALSE;
}

/**
  TRUE if the half-open ranges [Base1, Base1 + Size1) and
  [Base2, Base2 + Size2) share at least one byte.

  Either range overflowing the 64-bit address space is treated as
  overlapping everything (fail closed), same rationale as above.
**/
static inline NTASI_GUARD_BOOL
NtasiRangesOverlap (
  NTASI_GUARD_U64  Base1,
  NTASI_GUARD_U64  Size1,
  NTASI_GUARD_U64  Base2,
  NTASI_GUARD_U64  Size2
  )
{
  NTASI_GUARD_U64  Top1;
  NTASI_GUARD_U64  Top2;

  if ((Size1 == 0) || (Size2 == 0)) {
    return NTASI_GUARD_FALSE;
  }

  Top1 = Base1 + (Size1 - 1);
  Top2 = Base2 + (Size2 - 1);
  if ((Top1 < Base1) || (Top2 < Base2)) {
    return NTASI_GUARD_TRUE;
  }

  return ((Base1 <= Top2) && (Base2 <= Top1)) ? NTASI_GUARD_TRUE : NTASI_GUARD_FALSE;
}

/**
  TRUE if [Base, Base + Size) lies entirely inside [WindowBase, WindowTop).

  This is the "real memory map" bound the 2026-07-30 GPU incident was
  missing. The two predicates above only answer "does this candidate collide
  with something Mu is using"; neither can tell a plausible-looking address
  that is simply not backed by DRAM on THIS machine from one that is. The
  original six hardcoded AGX carveouts included three (0x103fffb8000,
  0x103fff78000, 0x103fff70000) that sit above boot_args' mem_size ceiling,
  and the PEI code of the day tried to reserve them through
  ReserveMemoryRegion()/ReserveAllocatedSystemMemoryRegion(), which can only
  punch holes in an existing SystemMemory HOB -- so the reservation failed
  and MemoryInitPeiLib returned a fatal status before a console existed.

  The correct window is NOT boot_args' mem_size top (SystemMemoryTop): the
  UAT/GUAT carveouts legitimately live ABOVE it, in the pool iBoot and m1n1
  reserve for themselves. It is the machine's real installed-DRAM top,
  ALIGN_DOWN(phys_base, 4GiB) + mem_size_actual -- the same formula m1n1's
  own top_of_memory_alloc() uses, and the same one
  NtasiDeriveWirelessReservation() in MemoryInitPeiLib.c already derives the
  wireless reservation from. A candidate outside that window is not a
  carveout at all and must never be reserved, mapped, or published.

  Zero size, an empty/inverted window, or either range overflowing the
  64-bit address space all return FALSE (fail closed: "not provably inside"
  is treated as "outside").
**/
static inline NTASI_GUARD_BOOL
NtasiRangeWithinWindow (
  NTASI_GUARD_U64  Base,
  NTASI_GUARD_U64  Size,
  NTASI_GUARD_U64  WindowBase,
  NTASI_GUARD_U64  WindowTop
  )
{
  NTASI_GUARD_U64  Top;

  if ((Size == 0) || (WindowTop <= WindowBase)) {
    return NTASI_GUARD_FALSE;
  }

  Top = Base + (Size - 1);
  if (Top < Base) {
    return NTASI_GUARD_FALSE;
  }

  return ((Base >= WindowBase) && (Top <= (WindowTop - 1))) ? NTASI_GUARD_TRUE
                                                            : NTASI_GUARD_FALSE;
}

#endif // NTASI_GPU_RESERVATION_GUARD_H_

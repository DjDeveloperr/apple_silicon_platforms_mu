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

#endif // NTASI_GPU_RESERVATION_GUARD_H_

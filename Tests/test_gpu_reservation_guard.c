/*
 * Host-side, hardware-free regression test for the GPU preboot reservation
 * safety invariants in
 * Silicon/Apple/AppleSiliconPkg/Drivers/AcpiPlatformDxe/NtasiGpuReservationGuard.h.
 *
 * This file #includes that header directly (the exact same text compiled
 * into AcpiPlatform.c under the EDK2/Clang AArch64 DXE toolchain -- moved
 * there from PEI's MemoryInitPeiLib.c on 2026-07-30, see the header's own
 * comment for why) and compiles standalone with a plain host C compiler --
 * no EDK2, no cross-toolchain, no hardware, no proxy. Run it with:
 *
 *   cc -std=c99 -Wall -Wextra -o /tmp/test_gpu_reservation_guard \
 *      Tests/test_gpu_reservation_guard.c && /tmp/test_gpu_reservation_guard
 *
 * or via Tests/test_gpu_reservation_guard.py, which does exactly that.
 *
 * The case in test_StackOverlap_2026_07_30_regression() reproduces the
 * exact numbers from the 2026-07-30 hardware crash: a GPU "hw_data_a"
 * reservation computed as [SystemMemoryTop - 0x8000, SystemMemoryTop) with
 * SystemMemoryTop = 0x103db29c000 landed on the live PEI stack pointer
 * (observed SP_EL1 = 0x103db29ba10) and crashed the machine before a
 * vector table even existed. This test fails loudly if
 * NtasiRangeContainsPoint() would ever fail to flag that exact
 * computation again.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdio.h>

#include "../Silicon/Apple/AppleSiliconPkg/Drivers/AcpiPlatformDxe/NtasiGpuReservationGuard.h"

static int gFailures = 0;

#define CHECK(description, condition)                                          \
  do {                                                                          \
    if (condition) {                                                           \
      printf("PASS: %s\n", (description));                                     \
    } else {                                                                   \
      printf("FAIL: %s\n", (description));                                     \
      gFailures++;                                                             \
    }                                                                          \
  } while (0)

static void
test_StackOverlap_2026_07_30_regression (void)
{
  /* Exact figures from the coordinator's hardware report. */
  const NTASI_GUARD_U64 SystemMemoryTop = 0x103db29c000ULL;
  const NTASI_GUARD_U64 LivePeiStackPointer = 0x103db29ba10ULL;

  /* hw_data_a payload size 0x6C34 aligned up to 16 KiB = 0x8000. */
  const NTASI_GUARD_U64 HwDataASize = 0x8000ULL;
  const NTASI_GUARD_U64 HwDataABase = SystemMemoryTop - HwDataASize;

  CHECK(
    "2026-07-30 regression: the crashing hw_data_a range must be flagged as containing the live PEI stack pointer",
    NtasiRangeContainsPoint(HwDataABase, HwDataASize, LivePeiStackPointer) == NTASI_GUARD_TRUE
  );

  /* hw_data_b and globals, stacked further down in the same bad scheme,
   * should also be checked -- they happen to sit below the stack pointer
   * in this particular reproduction, so the *point* check alone would not
   * catch them, but the system-memory-window overlap check must. */
  {
    const NTASI_GUARD_U64 SystemMemoryBase = 0x1003e7f0000ULL;
    const NTASI_GUARD_U64 HwDataBSize = 0x4000ULL;
    const NTASI_GUARD_U64 HwDataBBase = HwDataABase - HwDataBSize;
    const NTASI_GUARD_U64 GlobalsSize = 0x18000ULL;
    const NTASI_GUARD_U64 GlobalsBase = HwDataBBase - GlobalsSize;
    const NTASI_GUARD_U64 WindowSize = SystemMemoryTop - SystemMemoryBase;

    CHECK(
      "2026-07-30 regression: the crashing hw_data_b range must overlap Mu's system-memory window",
      NtasiRangesOverlap(HwDataBBase, HwDataBSize, SystemMemoryBase, WindowSize) == NTASI_GUARD_TRUE
    );
    CHECK(
      "2026-07-30 regression: the crashing globals range must overlap Mu's system-memory window",
      NtasiRangesOverlap(GlobalsBase, GlobalsSize, SystemMemoryBase, WindowSize) == NTASI_GUARD_TRUE
    );
  }
}

static void
test_ConfirmedGoodAdtCarveouts_2026_07_30 (void)
{
  /* Hardware-confirmed live /arm-io/sgx ADT values, all three byte-exact
   * against the historical hardcoded constants. None of these may ever be
   * flagged as unsafe, or the guard is too aggressive to ship. */
  const NTASI_GUARD_U64 SystemMemoryBase = 0x1003e7f0000ULL;
  const NTASI_GUARD_U64 SystemMemoryTop  = 0x103db29c000ULL;
  const NTASI_GUARD_U64 WindowSize       = SystemMemoryTop - SystemMemoryBase;
  const NTASI_GUARD_U64 LivePeiStackPointer = 0x103db29ba10ULL;

  const struct {
    const char       *Label;
    NTASI_GUARD_U64  Base;
    NTASI_GUARD_U64  Size;
  } Carveouts[3] = {
    { "uat_ttbs (gpu-region)",         0x103fffb8000ULL, 0x4000ULL  },
    { "uat_pagetables (gfx-shared-region)", 0x103fff78000ULL, 0x40000ULL },
    { "uat_handoff (gfx-handoff)",     0x103fff70000ULL, 0x4000ULL  },
  };
  size_t i;
  char message[128];

  for (i = 0; i < sizeof(Carveouts) / sizeof(Carveouts[0]); i++) {
    snprintf(message, sizeof(message), "%s must not be flagged as containing the live PEI stack pointer", Carveouts[i].Label);
    CHECK(message, NtasiRangeContainsPoint(Carveouts[i].Base, Carveouts[i].Size, LivePeiStackPointer) == NTASI_GUARD_FALSE);

    snprintf(message, sizeof(message), "%s must not be flagged as overlapping Mu's system-memory window", Carveouts[i].Label);
    CHECK(message, NtasiRangesOverlap(Carveouts[i].Base, Carveouts[i].Size, SystemMemoryBase, WindowSize) == NTASI_GUARD_FALSE);
  }
}

static void
test_RangeContainsPoint_EdgeCases (void)
{
  CHECK("zero-size range contains nothing", NtasiRangeContainsPoint(0x1000, 0, 0x1000) == NTASI_GUARD_FALSE);
  CHECK("point exactly at base is contained", NtasiRangeContainsPoint(0x1000, 0x10, 0x1000) == NTASI_GUARD_TRUE);
  CHECK("point exactly at last byte is contained", NtasiRangeContainsPoint(0x1000, 0x10, 0x100f) == NTASI_GUARD_TRUE);
  CHECK("point one past the range is not contained", NtasiRangeContainsPoint(0x1000, 0x10, 0x1010) == NTASI_GUARD_FALSE);
  CHECK("point one before the range is not contained", NtasiRangeContainsPoint(0x1000, 0x10, 0x0fff) == NTASI_GUARD_FALSE);
  CHECK(
    "a range that wraps the address space is treated as containing everything (fail closed)",
    NtasiRangeContainsPoint(0xfffffffffffffff0ULL, 0x100, 0x1234) == NTASI_GUARD_TRUE
  );
}

static void
test_RangesOverlap_EdgeCases (void)
{
  CHECK("zero-size ranges never overlap", NtasiRangesOverlap(0x1000, 0, 0x1000, 0x10) == NTASI_GUARD_FALSE);
  CHECK("identical ranges overlap", NtasiRangesOverlap(0x1000, 0x10, 0x1000, 0x10) == NTASI_GUARD_TRUE);
  CHECK("adjacent-but-not-touching ranges do not overlap", NtasiRangesOverlap(0x1000, 0x10, 0x1010, 0x10) == NTASI_GUARD_FALSE);
  CHECK("ranges sharing exactly one byte overlap", NtasiRangesOverlap(0x1000, 0x11, 0x1010, 0x10) == NTASI_GUARD_TRUE);
  CHECK("a range straddling the top of a window overlaps it", NtasiRangesOverlap(0x1ff8, 0x10, 0x1000, 0x1000) == NTASI_GUARD_TRUE);
  CHECK(
    "a wrapping range is treated as overlapping everything (fail closed)",
    NtasiRangesOverlap(0xfffffffffffffff0ULL, 0x100, 0x1000, 0x10) == NTASI_GUARD_TRUE
  );
}

int
main (void)
{
  test_StackOverlap_2026_07_30_regression();
  test_ConfirmedGoodAdtCarveouts_2026_07_30();
  test_RangeContainsPoint_EdgeCases();
  test_RangesOverlap_EdgeCases();

  if (gFailures != 0) {
    printf("\n%d assertion(s) FAILED\n", gFailures);
    return 1;
  }

  printf("\nall assertions passed\n");
  return 0;
}

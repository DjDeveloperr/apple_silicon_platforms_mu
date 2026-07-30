/*
 * Host-side, hardware-free regression test for the m1n1 -> Mu wireless DART
 * handoff validator in
 * Silicon/Apple/AppleSiliconPkg/Include/IndustryStandard/J414sWirelessHandoff.h.
 *
 * WHY THIS MATTERS NOW. Until 2026-07-30 the reservation travelled from PEI
 * to DXE through PcdAppleWirelessDartPageTableBase/Size, which are
 * [PcdsPatchableInModule] -- a PER-MODULE copy. MemoryInitPeiLib's
 * PatchPcdSet64/32 wrote PrePi's copy; AcpiPlatformDxe's PcdGet64/32 read
 * their own never-patched copies and always saw zero, so DRT0 was withheld on
 * every single boot no matter what PEI derived and authenticated. That is now
 * a GUID HOB, and BOTH phases authenticate the descriptor with the one shared
 * copy of NtasiValidateWirelessHandoffV2() exercised here.
 *
 * Because the validator is what stands between "m1n1 installed a real SID-1
 * DART page table" and "Mu publishes a DART page-table base to Windows", it
 * has to be fail-closed on every field. Each mutation below must be rejected.
 *
 * The header under test is a wire-ABI header and legitimately uses EDK II
 * types and packing, so it cannot be dependency-free the way
 * NtasiGpuReservationGuard.h is. Tests/edk2stub/Base.h supplies exactly the
 * constructs it uses and nothing more.
 *
 *   cc -std=c99 -Wall -Wextra -I Tests/edk2stub \
 *      -o /tmp/t Tests/test_wireless_handoff_validate.c && /tmp/t
 *
 * or via Tests/test_wireless_handoff_validate.py.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../Silicon/Apple/AppleSiliconPkg/Include/IndustryStandard/J414sWirelessHandoff.h"

static int gFailures = 0;

#define CHECK(description, condition)                                          \
  do {                                                                         \
    if (condition) {                                                           \
      printf("PASS: %s\n", (description));                                     \
    } else {                                                                   \
      printf("FAIL: %s\n", (description));                                     \
      gFailures++;                                                             \
    }                                                                          \
  } while (0)

/*
 * Figures from the hardware-verified m1n1 handoff of 2026-07-30:
 *   reservation 0x103ffff0000 + 0x10000
 *   descriptor  0x103ffffc000
 *   L1          0x103ffff0000
 *   MSI L2      0x103ffff4000
 * The test allocates a real 16 KiB-aligned buffer and uses its address as the
 * base, because the validator dereferences the base directly -- but every
 * offset, size and fixed field below is the live contract.
 */
#define GUEST_MEMORY_TOP  0x103db29c000ULL
#define PHYSICAL_TOP      0x10400000000ULL

static void *
alloc_reservation (void)
{
  void *p = NULL;

  if (posix_memalign (&p, (size_t)NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE,
                      (size_t)NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE) != 0)
  {
    return NULL;
  }

  memset (p, 0, (size_t)NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE);
  return p;
}

/* Build a fully valid ABI v2 handoff at Base and return its descriptor. */
static NTASI_WIRELESS_HANDOFF_DESCRIPTOR_V2 *
build_valid (UINT64 Base)
{
  NTASI_WIRELESS_HANDOFF_DESCRIPTOR_V2  *d;
  UINT8                                 *l1;
  UINT8                                 *msi;
  NTASI_WIRELESS_HANDOFF_DESCRIPTOR_V2   copy;
  unsigned                               i;

  l1  = (UINT8 *)(UINTN)(Base + NTASI_WIRELESS_HANDOFF_V2_L1_OFFSET);
  msi = (UINT8 *)(UINTN)(Base + NTASI_WIRELESS_HANDOFF_V2_MSI_L2_OFFSET);
  d   = (NTASI_WIRELESS_HANDOFF_DESCRIPTOR_V2 *)(UINTN)
        (Base + NTASI_WIRELESS_HANDOFF_V2_DESCRIPTOR_OFFSET);

  /* Plausible page-table content: deterministic, non-uniform. */
  for (i = 0; i < NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE; i++) {
    l1[i]  = (UINT8)(i * 7u + 1u);
    msi[i] = (UINT8)(i * 13u + 5u);
  }

  memset (d, 0, sizeof (*d));
  d->Signature          = NTASI_WIRELESS_HANDOFF_V2_SIGNATURE;
  d->Version            = NTASI_WIRELESS_HANDOFF_V2_VERSION;
  d->StructureSize      = (UINT16)sizeof (*d);
  d->Flags              = NTASI_WIRELESS_HANDOFF_V2_FLAG_INSTALLED;
  d->Sid                = NTASI_WIRELESS_HANDOFF_V2_SID;
  d->PageShift          = NTASI_WIRELESS_HANDOFF_V2_PAGE_SHIFT;
  d->Reserved           = 0;
  d->ReservationBase    = Base;
  d->ReservationSize    = NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE;
  d->GuestMemoryTop     = GUEST_MEMORY_TOP;
  d->PhysicalMemoryTop  = PHYSICAL_TOP;
  d->DartBase           = NTASI_WIRELESS_HANDOFF_V2_DART_BASE;
  d->L1Physical         = Base + NTASI_WIRELESS_HANDOFF_V2_L1_OFFSET;
  d->MsiL2Physical      = Base + NTASI_WIRELESS_HANDOFF_V2_MSI_L2_OFFSET;
  d->DescriptorPhysical = Base + NTASI_WIRELESS_HANDOFF_V2_DESCRIPTOR_OFFSET;
  d->L1Crc32            = NtasiWirelessCrc32 (l1, NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE);
  d->MsiL2Crc32         = NtasiWirelessCrc32 (msi, NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE);

  copy                 = *d;
  copy.DescriptorCrc32 = 0;
  d->DescriptorCrc32   = NtasiWirelessCrc32 (&copy, sizeof (copy));
  return d;
}

static void
test_Crc32KnownVector (void)
{
  /* The canonical CRC-32/ISO-HDLC check value. If this drifts, every CRC in
   * the handoff drifts with it and m1n1 and Mu stop agreeing. */
  CHECK (
    "CRC-32 of \"123456789\" is 0xcbf43926",
    NtasiWirelessCrc32 ("123456789", 9) == 0xcbf43926U
    );
}

static void
test_ValidHandoffIsAccepted (void)
{
  void    *buffer = alloc_reservation ();
  UINT64   base;

  if (buffer == NULL) {
    CHECK ("could allocate an aligned reservation buffer", 0);
    return;
  }

  base = (UINT64)(UINTN)buffer;
  (void)build_valid (base);

  CHECK (
    "a fully valid ABI v2 handoff is accepted",
    NtasiValidateWirelessHandoffV2 (
      base,
      (UINT32)NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE,
      GUEST_MEMORY_TOP
      ) == TRUE
    );

  /* Both phases must reach the same verdict from the same bytes -- this is
   * the whole point of PEI and DXE sharing one validator now. */
  CHECK (
    "the same bytes validate identically on a second call (no hidden state)",
    NtasiValidateWirelessHandoffV2 (
      base,
      (UINT32)NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE,
      GUEST_MEMORY_TOP
      ) == TRUE
    );

  free (buffer);
}

#define MUTATION_CASE(description, mutation)                                   \
  do {                                                                         \
    void    *buffer = alloc_reservation ();                                    \
    UINT64   base;                                                             \
    NTASI_WIRELESS_HANDOFF_DESCRIPTOR_V2 *d;                                   \
    if (buffer == NULL) {                                                      \
      CHECK ("could allocate an aligned reservation buffer", 0);               \
      return;                                                                  \
    }                                                                          \
    base = (UINT64)(UINTN)buffer;                                              \
    d = build_valid (base);                                                    \
    (void)d;                                                                   \
    { mutation }                                                               \
    CHECK (                                                                    \
      description,                                                             \
      NtasiValidateWirelessHandoffV2 (                                         \
        base,                                                                  \
        (UINT32)NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE,                    \
        GUEST_MEMORY_TOP                                                       \
        ) == FALSE                                                             \
      );                                                                       \
    free (buffer);                                                             \
  } while (0)

static void
test_EveryFieldIsFailClosed (void)
{
  MUTATION_CASE ("a wrong signature is rejected", d->Signature ^= 1u;);
  MUTATION_CASE ("a wrong version is rejected", d->Version = 3;);
  MUTATION_CASE ("a wrong structure size is rejected", d->StructureSize = 64;);
  MUTATION_CASE ("a cleared INSTALLED flag is rejected", d->Flags = 0;);
  MUTATION_CASE ("a wrong SID is rejected", d->Sid = 2;);
  MUTATION_CASE ("a wrong page shift is rejected", d->PageShift = 12;);
  MUTATION_CASE ("a non-zero reserved field is rejected", d->Reserved = 1;);
  MUTATION_CASE ("a mismatched reservation base is rejected", d->ReservationBase += 0x4000ULL;);
  MUTATION_CASE ("a mismatched reservation size is rejected", d->ReservationSize = 0x8000ULL;);
  MUTATION_CASE ("a mismatched guest memory top is rejected", d->GuestMemoryTop += 0x4000ULL;);
  MUTATION_CASE ("a physical top below the reservation is rejected", d->PhysicalMemoryTop = 0x1000ULL;);
  MUTATION_CASE ("a wrong DART base is rejected", d->DartBase = 0x594008000ULL;);
  MUTATION_CASE ("a wrong L1 physical address is rejected", d->L1Physical += 0x4000ULL;);
  MUTATION_CASE ("a wrong MSI L2 physical address is rejected", d->MsiL2Physical += 0x4000ULL;);
  MUTATION_CASE ("a wrong descriptor physical address is rejected", d->DescriptorPhysical += 0x4000ULL;);
  MUTATION_CASE ("a zero descriptor CRC is rejected", d->DescriptorCrc32 = 0;);
  MUTATION_CASE ("a corrupted descriptor CRC is rejected", d->DescriptorCrc32 ^= 0x1000u;);

  /* Content corruption -- the case a field-only check would miss entirely,
   * and the one that proves the reservation survived PEI and DXE intact. */
  MUTATION_CASE (
    "a single flipped bit in the L1 page table is rejected",
    ((UINT8 *)(UINTN)(base + NTASI_WIRELESS_HANDOFF_V2_L1_OFFSET))[1234] ^= 0x01;
    );
  MUTATION_CASE (
    "a single flipped bit in the MSI L2 page table is rejected",
    ((UINT8 *)(UINTN)(base + NTASI_WIRELESS_HANDOFF_V2_MSI_L2_OFFSET))[4095] ^= 0x80;
    );
  MUTATION_CASE (
    "a wholly zeroed reservation is rejected",
    memset ((void *)(UINTN)base, 0, (size_t)NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE);
    );
}

static void
test_CallerSuppliedBoundsAreChecked (void)
{
  void   *buffer = alloc_reservation ();
  UINT64  base;

  if (buffer == NULL) {
    CHECK ("could allocate an aligned reservation buffer", 0);
    return;
  }

  base = (UINT64)(UINTN)buffer;
  (void)build_valid (base);

  CHECK (
    "a size other than the ABI reservation size is rejected",
    NtasiValidateWirelessHandoffV2 (base, 0x8000, GUEST_MEMORY_TOP) == FALSE
    );
  CHECK (
    "a zero size is rejected",
    NtasiValidateWirelessHandoffV2 (base, 0, GUEST_MEMORY_TOP) == FALSE
    );
  CHECK (
    "a base that is not 16 KiB aligned is rejected",
    NtasiValidateWirelessHandoffV2 (
      base + 0x1000,
      (UINT32)NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE,
      GUEST_MEMORY_TOP
      ) == FALSE
    );
  CHECK (
    "a guest_top the descriptor was not built for is rejected",
    NtasiValidateWirelessHandoffV2 (
      base,
      (UINT32)NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE,
      GUEST_MEMORY_TOP + 0x4000ULL
      ) == FALSE
    );

  free (buffer);
}

static void
test_AbiConstantsAreStable (void)
{
  /* m1n1 writes these; a silent change here desynchronises the two sides. */
  CHECK ("reservation size is 64 KiB", NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE == 0x10000ULL);
  CHECK ("page size is 16 KiB", NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE == 0x4000ULL);
  CHECK ("L1 is at offset 0", NTASI_WIRELESS_HANDOFF_V2_L1_OFFSET == 0x0ULL);
  CHECK ("MSI L2 is at offset 16 KiB", NTASI_WIRELESS_HANDOFF_V2_MSI_L2_OFFSET == 0x4000ULL);
  CHECK ("descriptor is at offset 48 KiB", NTASI_WIRELESS_HANDOFF_V2_DESCRIPTOR_OFFSET == 0xc000ULL);
  CHECK ("DART base is 0x594000000", NTASI_WIRELESS_HANDOFF_V2_DART_BASE == 0x594000000ULL);
  CHECK ("SID is 1", NTASI_WIRELESS_HANDOFF_V2_SID == 1);
  CHECK ("page shift is 14", NTASI_WIRELESS_HANDOFF_V2_PAGE_SHIFT == 14);
  CHECK ("descriptor is 96 bytes", sizeof (NTASI_WIRELESS_HANDOFF_DESCRIPTOR_V2) == 96);
  CHECK ("PEI->DXE reservation HOB is 32 bytes", sizeof (NTASI_WIRELESS_DART_RESERVATION_HOB) == 32);
}

int
main (void)
{
  test_Crc32KnownVector ();
  test_ValidHandoffIsAccepted ();
  test_EveryFieldIsFailClosed ();
  test_CallerSuppliedBoundsAreChecked ();
  test_AbiConstantsAreStable ();

  if (gFailures != 0) {
    printf ("\n%d assertion(s) FAILED\n", gFailures);
    return 1;
  }

  printf ("\nall assertions passed\n");
  return 0;
}

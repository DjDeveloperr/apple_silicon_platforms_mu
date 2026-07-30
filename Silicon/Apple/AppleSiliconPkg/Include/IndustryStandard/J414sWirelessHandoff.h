/** @file
  Versioned same-instance m1n1 -> Mu -> AppleDart wireless handoff ABI.

  SPDX-License-Identifier: MIT
**/

#ifndef NTASI_J414S_WIRELESS_HANDOFF_H_
#define NTASI_J414S_WIRELESS_HANDOFF_H_

#include <Base.h>

#define NTASI_WIRELESS_HANDOFF_V2_SIGNATURE         SIGNATURE_32 ('N', 'W', 'H', '2')
#define NTASI_WIRELESS_HANDOFF_V2_VERSION           2
#define NTASI_WIRELESS_HANDOFF_V2_FLAG_INSTALLED    BIT0
#define NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE  0x10000ULL
#define NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE         0x4000ULL
#define NTASI_WIRELESS_HANDOFF_V2_L1_OFFSET         0x0000ULL
#define NTASI_WIRELESS_HANDOFF_V2_MSI_L2_OFFSET     0x4000ULL
#define NTASI_WIRELESS_HANDOFF_V2_DESCRIPTOR_OFFSET 0xc000ULL
#define NTASI_WIRELESS_HANDOFF_V2_DART_BASE         0x594000000ULL
#define NTASI_WIRELESS_HANDOFF_V2_SID               1
#define NTASI_WIRELESS_HANDOFF_V2_PAGE_SHIFT        14

#pragma pack (push, 1)
typedef struct {
  UINT32    Signature;
  UINT16    Version;
  UINT16    StructureSize;
  UINT32    Flags;
  UINT16    Sid;
  UINT16    PageShift;
  UINT64    ReservationBase;
  UINT64    ReservationSize;
  UINT64    GuestMemoryTop;
  UINT64    PhysicalMemoryTop;
  UINT64    DartBase;
  UINT64    L1Physical;
  UINT64    MsiL2Physical;
  UINT64    DescriptorPhysical;
  UINT32    L1Crc32;
  UINT32    MsiL2Crc32;
  UINT32    DescriptorCrc32;
  UINT32    Reserved;
} NTASI_WIRELESS_HANDOFF_DESCRIPTOR_V2;
#pragma pack (pop)

STATIC_ASSERT (
  sizeof (NTASI_WIRELESS_HANDOFF_DESCRIPTOR_V2) == 96,
  "wireless handoff ABI v2 size"
  );

/**
  PEI -> DXE reservation handoff HOB.

  WHY A HOB AND NOT A PCD, 2026-07-30: the reservation used to travel from
  MemoryInitPeiLib to AcpiPlatformDxe through
  PcdAppleWirelessDartPageTableBase/Size, which are declared
  [PcdsPatchableInModule]. A PatchableInModule PCD is a PER-MODULE copy:
  PatchPcdSet64/32 writes the copy linked into PrePi, and AcpiPlatformDxe's
  PcdGet64/32 reads its own never-patched copy, which is always the DEC
  default of zero. So DRT0 was withheld on EVERY boot no matter what PEI
  derived and authenticated -- wireless could never work, and the log said
  "no reservation published by PEI this boot", which was actively
  misleading.

  The same trap was proven on hardware for PcdSystemMemoryBase/Size (see
  NtasiDeriveBootArgsWindows() in AcpiPlatform.c). A GUID HOB is the correct
  EDK2 mechanism for PEI -> DXE data and is already used in this tree by
  NTASI_APPENDED_RAMDISK_LOCATION_HOB_GUID.

  The PCDs are still patched, because BuildVirtualMemoryMap() reads them
  from inside PrePi where the patch IS visible. They are no longer the
  cross-phase channel.
**/
#define NTASI_WIRELESS_DART_RESERVATION_HOB_SIGNATURE  SIGNATURE_32 ('N', 'W', 'D', 'R')
#define NTASI_WIRELESS_DART_RESERVATION_HOB_VERSION    1U
#define NTASI_WIRELESS_DART_RESERVATION_HOB_GUID  \
  { 0x6f2d1c84, 0x9a3b, 0x4f57, { 0xb1, 0x0e, 0x2c, 0x74, 0x55, 0x8d, 0xe6, 0x21 } }

#pragma pack (push, 1)
typedef struct {
  UINT32    Signature;
  UINT16    Version;
  UINT16    StructureSize;
  UINT64    ReservationBase;
  UINT64    ReservationSize;
  /// The guest_top PEI authenticated the descriptor against, so DXE can
  /// repeat the exact same check rather than deriving a second opinion.
  UINT64    GuestMemoryTop;
} NTASI_WIRELESS_DART_RESERVATION_HOB;
#pragma pack (pop)

STATIC_ASSERT (
  sizeof (NTASI_WIRELESS_DART_RESERVATION_HOB) == 32,
  "wireless DART reservation HOB size"
  );

/*
 * Descriptor authentication, shared verbatim by PEI (MemoryInitPeiLib.c) and
 * DXE (AcpiPlatform.c).
 *
 * Deliberately dependency-free beyond <Base.h> -- no DebugLib, no PcdLib, no
 * allocation -- so exactly one copy of this logic exists and both phases run
 * the identical checks. PEI authenticates before reserving; DXE
 * re-authenticates before publishing DRT0, which additionally proves the
 * region survived the whole of PEI and DXE dispatch unmodified. Publishing a
 * DART page-table base to Windows is not something to do on the strength of
 * a HOB alone.
 */

/// CRC-32 (reflected, poly 0xedb88320), matching m1n1's wireless handoff.
STATIC
inline
UINT32
NtasiWirelessCrc32 (
  IN CONST VOID  *Data,
  IN UINT32      Length
  )
{
  CONST UINT8  *Bytes;
  UINT32       Crc;
  UINT32       Index;
  UINT32       Bit;

  Bytes = Data;
  Crc   = MAX_UINT32;
  for (Index = 0; Index < Length; Index++) {
    Crc ^= Bytes[Index];
    for (Bit = 0; Bit < 8; Bit++) {
      Crc = (Crc >> 1) ^ (0xedb88320U & (0U - (Crc & 1U)));
    }
  }

  return ~Crc;
}

/**
  TRUE only if a complete, self-consistent ABI v2 handoff is installed at
  Base/Size: every fixed field exactly as m1n1 writes it, the descriptor's
  own CRC32 correct, and both page tables' CRC32s correct.

  Fail-closed by construction -- there is no partial acceptance and no
  "looks close enough" path.
**/
STATIC
inline
BOOLEAN
NtasiValidateWirelessHandoffV2 (
  IN UINT64  Base,
  IN UINT32  Size,
  IN UINT64  GuestMemoryTop
  )
{
  CONST NTASI_WIRELESS_HANDOFF_DESCRIPTOR_V2  *Descriptor;
  NTASI_WIRELESS_HANDOFF_DESCRIPTOR_V2        Copy;
  UINT32                                      DescriptorCrc;

  if ((Size != NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE) ||
      ((Base & (NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE - 1)) != 0) ||
      (Base > MAX_UINT64 - Size))
  {
    return FALSE;
  }

  Descriptor = (CONST VOID *)(UINTN)(Base + NTASI_WIRELESS_HANDOFF_V2_DESCRIPTOR_OFFSET);
  if ((Descriptor->Signature != NTASI_WIRELESS_HANDOFF_V2_SIGNATURE) ||
      (Descriptor->Version != NTASI_WIRELESS_HANDOFF_V2_VERSION) ||
      (Descriptor->StructureSize != sizeof (*Descriptor)) ||
      (Descriptor->Flags != NTASI_WIRELESS_HANDOFF_V2_FLAG_INSTALLED) ||
      (Descriptor->Sid != NTASI_WIRELESS_HANDOFF_V2_SID) ||
      (Descriptor->PageShift != NTASI_WIRELESS_HANDOFF_V2_PAGE_SHIFT) ||
      (Descriptor->Reserved != 0) ||
      (Descriptor->ReservationBase != Base) ||
      (Descriptor->ReservationSize != Size) ||
      (Descriptor->GuestMemoryTop != GuestMemoryTop) ||
      (Descriptor->PhysicalMemoryTop < Base + Size) ||
      (Descriptor->DartBase != NTASI_WIRELESS_HANDOFF_V2_DART_BASE) ||
      (Descriptor->L1Physical != Base + NTASI_WIRELESS_HANDOFF_V2_L1_OFFSET) ||
      (Descriptor->MsiL2Physical != Base + NTASI_WIRELESS_HANDOFF_V2_MSI_L2_OFFSET) ||
      (Descriptor->DescriptorPhysical != Base + NTASI_WIRELESS_HANDOFF_V2_DESCRIPTOR_OFFSET))
  {
    return FALSE;
  }

  Copy                 = *Descriptor;
  DescriptorCrc        = Copy.DescriptorCrc32;
  Copy.DescriptorCrc32 = 0;
  return (BOOLEAN)((DescriptorCrc != 0) &&
                   (NtasiWirelessCrc32 (&Copy, sizeof (Copy)) == DescriptorCrc) &&
                   (NtasiWirelessCrc32 (
                      (CONST VOID *)(UINTN)(Base + NTASI_WIRELESS_HANDOFF_V2_L1_OFFSET),
                      NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE
                      ) == Descriptor->L1Crc32) &&
                   (NtasiWirelessCrc32 (
                      (CONST VOID *)(UINTN)(Base + NTASI_WIRELESS_HANDOFF_V2_MSI_L2_OFFSET),
                      NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE
                      ) == Descriptor->MsiL2Crc32));
}

#endif

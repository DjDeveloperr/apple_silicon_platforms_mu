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

#endif

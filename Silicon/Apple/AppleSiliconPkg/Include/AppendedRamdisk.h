/** @file
  Contract for a FAT ramdisk appended immediately after the loaded Mu FD.

  SPDX-License-Identifier: MIT
**/

#ifndef NTASI_APPENDED_RAMDISK_H_
#define NTASI_APPENDED_RAMDISK_H_

#include <Base.h>

#define NTASI_APPENDED_RAMDISK_SIGNATURE       SIGNATURE_64 ('N', 'T', 'A', 'S', 'I', 'R', 'D', 'K')
#define NTASI_APPENDED_RAMDISK_VERSION         1U
#define NTASI_APPENDED_RAMDISK_MAX_IMAGE_SIZE  0x40000000ULL
#define NTASI_APPENDED_RAMDISK_MAX_MAPPED_SPAN  0x40001000ULL
#define NTASI_APPENDED_RAMDISK_LOCATION_SIGNATURE  SIGNATURE_64 ('N', 'T', 'A', 'S', 'I', 'H', 'O', 'B')
#define NTASI_APPENDED_RAMDISK_LOCATION_VERSION    1U
#define NTASI_APPENDED_RAMDISK_LOCATION_HOB_GUID  \
  { 0x9d157fd4, 0x8f63, 0x4e6e, { 0xa4, 0x59, 0x64, 0x0e, 0x93, 0xf2, 0x38, 0x37 } }

typedef struct {
  UINT64    Signature;
  UINT32    Version;
  UINT32    HeaderSize;
  UINT64    ImageSize;
  UINT32    ImageCrc32;
  UINT32    HeaderCrc32;
} NTASI_APPENDED_RAMDISK_HEADER;

typedef struct {
  UINT64                  Signature;
  UINT32                  Version;
  UINT32                  StructureSize;
  EFI_PHYSICAL_ADDRESS    HeaderPhysicalAddress;
  UINT64                  ReservationSize;
} NTASI_APPENDED_RAMDISK_LOCATION;

STATIC_ASSERT (
  sizeof (NTASI_APPENDED_RAMDISK_HEADER) == 32,
  "The appended ramdisk header is part of the m1n1/Mu ABI"
  );

STATIC_ASSERT (
  sizeof (NTASI_APPENDED_RAMDISK_LOCATION) == 32,
  "The appended ramdisk PEI/DXE location HOB is a versioned ABI"
  );

STATIC
UINT32
NtasiAppendedRamdiskCrc32 (
  IN CONST VOID  *Buffer,
  IN UINTN       BufferSize
  )
{
  CONST UINT8  *Bytes;
  UINT32       Crc;
  UINT32       Table[256];
  UINTN        Index;
  UINTN        Bit;

  Bytes = (CONST UINT8 *)Buffer;
  for (Index = 0; Index < ARRAY_SIZE (Table); Index++) {
    Table[Index] = (UINT32)Index;
    for (Bit = 0; Bit < 8; Bit++) {
      Table[Index] = (Table[Index] >> 1) ^
                     ((0U - (Table[Index] & 1U)) & 0xEDB88320U);
    }
  }

  Crc   = MAX_UINT32;
  for (Index = 0; Index < BufferSize; Index++) {
    Crc = Table[(Crc ^ Bytes[Index]) & 0xFFU] ^ (Crc >> 8);
  }

  return ~Crc;
}

/**
  Validate the structural header and return its in-place image and reservation.

  MaximumBytes is the mapped system-memory span starting at Header.  PEI uses
  ValidatePayload=FALSE before reserving the span; DXE uses TRUE before handing
  it to RamDiskDxe.  A signature match followed by any validation failure is a
  corrupted explicit payload, never an invitation to use unrelated memory.
**/
STATIC
BOOLEAN
NtasiValidateAppendedRamdisk (
  IN  CONST NTASI_APPENDED_RAMDISK_HEADER  *Header,
  IN  UINT64                               MaximumBytes,
  IN  BOOLEAN                              ValidatePayload,
  OUT CONST VOID                           **Image OPTIONAL,
  OUT UINT64                               *ImageSize OPTIONAL,
  OUT UINT64                               *ReservationSize OPTIONAL
  )
{
  UINT64      TotalSize;
  UINT64      RoundedSize;
  CONST VOID  *Payload;

  if ((Header == NULL) || (MaximumBytes < sizeof (*Header)) ||
      (Header->Signature != NTASI_APPENDED_RAMDISK_SIGNATURE) ||
      (Header->Version != NTASI_APPENDED_RAMDISK_VERSION) ||
      (Header->HeaderSize != sizeof (*Header)) ||
      (Header->ImageSize < 512) ||
      (Header->ImageSize > NTASI_APPENDED_RAMDISK_MAX_IMAGE_SIZE) ||
      (Header->HeaderCrc32 != NtasiAppendedRamdiskCrc32 (
                                Header,
                                OFFSET_OF (NTASI_APPENDED_RAMDISK_HEADER, HeaderCrc32)
                                )))
  {
    return FALSE;
  }

  TotalSize = (UINT64)Header->HeaderSize + Header->ImageSize;
  if ((TotalSize < Header->ImageSize) || (TotalSize > MaximumBytes)) {
    return FALSE;
  }

  RoundedSize = ALIGN_VALUE (TotalSize, EFI_PAGE_SIZE);
  if ((RoundedSize < TotalSize) || (RoundedSize > MaximumBytes)) {
    return FALSE;
  }

  Payload = (CONST UINT8 *)Header + Header->HeaderSize;
  if (ValidatePayload &&
      (Header->ImageCrc32 != NtasiAppendedRamdiskCrc32 (Payload, (UINTN)Header->ImageSize)))
  {
    return FALSE;
  }

  if (Image != NULL) {
    *Image = Payload;
  }

  if (ImageSize != NULL) {
    *ImageSize = Header->ImageSize;
  }

  if (ReservationSize != NULL) {
    *ReservationSize = RoundedSize;
  }

  return TRUE;
}

#ifdef NTASI_APPENDED_RAMDISK_INCLUDE_FAT_VALIDATOR
STATIC
BOOLEAN
NtasiValidateFatBootSector (
  IN CONST VOID  *Image,
  IN UINT64      ImageSize
  )
{
  CONST UINT8  *BootSector;
  UINT32       BytesPerSector;
  UINT32       SectorsPerCluster;
  UINT32       ReservedSectors;
  UINT32       FatCount;
  UINT32       TotalSectors;

  if ((Image == NULL) || (ImageSize < 512)) {
    return FALSE;
  }

  BootSector       = (CONST UINT8 *)Image;
  BytesPerSector   = (UINT32)BootSector[11] | ((UINT32)BootSector[12] << 8);
  SectorsPerCluster = BootSector[13];
  ReservedSectors  = (UINT32)BootSector[14] | ((UINT32)BootSector[15] << 8);
  FatCount         = BootSector[16];
  TotalSectors     = (UINT32)BootSector[19] | ((UINT32)BootSector[20] << 8);
  if (TotalSectors == 0) {
    TotalSectors = (UINT32)BootSector[32] |
                   ((UINT32)BootSector[33] << 8) |
                   ((UINT32)BootSector[34] << 16) |
                   ((UINT32)BootSector[35] << 24);
  }

  return ((BootSector[0] == 0xE9) || (BootSector[0] == 0xEB)) &&
         (BootSector[510] == 0x55) && (BootSector[511] == 0xAA) &&
         (BytesPerSector >= 512) && (BytesPerSector <= 4096) &&
         ((BytesPerSector & (BytesPerSector - 1)) == 0) &&
         (SectorsPerCluster != 0) &&
         ((SectorsPerCluster & (SectorsPerCluster - 1)) == 0) &&
         (ReservedSectors != 0) && (FatCount != 0) && (FatCount <= 4) &&
         (TotalSectors != 0) &&
         (((UINT64)TotalSectors * BytesPerSector) == ImageSize);
}
#endif

#endif

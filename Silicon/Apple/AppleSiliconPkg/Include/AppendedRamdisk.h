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

typedef struct {
  UINT64    Signature;
  UINT32    Version;
  UINT32    HeaderSize;
  UINT64    ImageSize;
  UINT32    ImageCrc32;
  UINT32    HeaderCrc32;
} NTASI_APPENDED_RAMDISK_HEADER;

STATIC_ASSERT (
  sizeof (NTASI_APPENDED_RAMDISK_HEADER) == 32,
  "The appended ramdisk header is part of the m1n1/Mu ABI"
  );

STATIC
INLINE
UINT32
NtasiAppendedRamdiskCrc32 (
  IN CONST VOID  *Buffer,
  IN UINTN       BufferSize
  )
{
  CONST UINT8  *Bytes;
  UINT32       Crc;
  UINTN        Index;
  UINTN        Bit;

  Bytes = (CONST UINT8 *)Buffer;
  Crc   = MAX_UINT32;
  for (Index = 0; Index < BufferSize; Index++) {
    Crc ^= Bytes[Index];
    for (Bit = 0; Bit < 8; Bit++) {
      Crc = (Crc >> 1) ^ ((0U - (Crc & 1U)) & 0xEDB88320U);
    }
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
INLINE
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

STATIC
INLINE
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

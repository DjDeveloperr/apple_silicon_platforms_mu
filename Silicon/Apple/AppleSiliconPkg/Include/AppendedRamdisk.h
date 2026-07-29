/** @file
  Contract for a FAT ramdisk appended immediately after the loaded Mu FD.

  SPDX-License-Identifier: MIT
**/

#ifndef NTASI_APPENDED_RAMDISK_H_
#define NTASI_APPENDED_RAMDISK_H_

#include <Base.h>

#define NTASI_APPENDED_RAMDISK_SIGNATURE       SIGNATURE_64 ('N', 'T', 'A', 'S', 'I', 'R', 'D', 'K')
#define NTASI_APPENDED_RAMDISK_VERSION         2U
#define NTASI_APPENDED_RAMDISK_HEADER_SIZE     EFI_PAGE_SIZE
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
      (Header->HeaderSize != NTASI_APPENDED_RAMDISK_HEADER_SIZE) ||
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
UINT16
NtasiReadUint16 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT16)Bytes[0] | ((UINT16)Bytes[1] << 8);
}

STATIC
UINT32
NtasiReadUint32 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT32)Bytes[0] |
         ((UINT32)Bytes[1] << 8) |
         ((UINT32)Bytes[2] << 16) |
         ((UINT32)Bytes[3] << 24);
}

STATIC
UINT64
NtasiReadUint64 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT64)NtasiReadUint32 (Bytes) |
         ((UINT64)NtasiReadUint32 (Bytes + 4) << 32);
}

STATIC
BOOLEAN
NtasiValidateFatVolume (
  IN CONST UINT8  *BootSector,
  IN UINT64       VolumeSize
  )
{
  UINT32       BytesPerSector;
  UINT32       SectorsPerCluster;
  UINT32       ReservedSectors;
  UINT32       FatCount;
  UINT32       TotalSectors;

  BytesPerSector   = NtasiReadUint16 (BootSector + 11);
  SectorsPerCluster = BootSector[13];
  ReservedSectors  = NtasiReadUint16 (BootSector + 14);
  FatCount         = BootSector[16];
  TotalSectors     = NtasiReadUint16 (BootSector + 19);
  if (TotalSectors == 0) {
    TotalSectors = NtasiReadUint32 (BootSector + 32);
  }

  return ((BootSector[0] == 0xE9) || (BootSector[0] == 0xEB)) &&
         (BootSector[510] == 0x55) && (BootSector[511] == 0xAA) &&
         (BytesPerSector >= 512) && (BytesPerSector <= 4096) &&
         ((BytesPerSector & (BytesPerSector - 1)) == 0) &&
         (SectorsPerCluster != 0) &&
         ((SectorsPerCluster & (SectorsPerCluster - 1)) == 0) &&
         (ReservedSectors != 0) && (FatCount != 0) && (FatCount <= 4) &&
         (TotalSectors != 0) &&
         (((UINT64)TotalSectors * BytesPerSector) == VolumeSize);
}

STATIC
UINT32
NtasiCrc32WithZeroRange (
  IN CONST UINT8  *Bytes,
  IN UINTN        Size,
  IN UINTN        ZeroOffset,
  IN UINTN        ZeroSize
  )
{
  UINT32  Crc;
  UINT32  Table[256];
  UINT32  Value;
  UINTN   Index;
  UINTN   Bit;

  for (Index = 0; Index < ARRAY_SIZE (Table); Index++) {
    Table[Index] = (UINT32)Index;
    for (Bit = 0; Bit < 8; Bit++) {
      Table[Index] = (Table[Index] >> 1) ^
                     ((0U - (Table[Index] & 1U)) & 0xEDB88320U);
    }
  }

  Crc = MAX_UINT32;
  for (Index = 0; Index < Size; Index++) {
    Value = ((Index >= ZeroOffset) && (Index - ZeroOffset < ZeroSize)) ?
            0U : Bytes[Index];
    Crc = Table[(Crc ^ Value) & 0xFFU] ^ (Crc >> 8);
  }

  return ~Crc;
}

STATIC
BOOLEAN
NtasiValidateGptFatDisk (
  IN CONST UINT8  *Disk,
  IN UINT64       DiskSize
  )
{
  STATIC CONST UINT8  EspTypeGuid[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
  };
  CONST UINT8  *Header;
  CONST UINT8  *Entries;
  CONST UINT8  *Entry;
  UINT64       TotalLbas;
  UINT64       BackupLba;
  UINT64       FirstUsable;
  UINT64       LastUsable;
  UINT64       EntryLba;
  UINT64       EntryBytes;
  UINT64       FirstLba;
  UINT64       LastLba;
  UINT64       PartitionOffset;
  UINT64       PartitionSize;
  UINT32       HeaderSize;
  UINT32       EntryCount;
  UINT32       EntrySize;
  UINT32       Index;
  UINT32       ByteIndex;
  UINT32       ProtectiveCount;
  UINT32       EspCount;
  BOOLEAN      IsEsp;

  if ((DiskSize < (34ULL * 512ULL)) || ((DiskSize & 511ULL) != 0) ||
      (Disk[510] != 0x55) || (Disk[511] != 0xAA))
  {
    return FALSE;
  }

  ProtectiveCount = 0;
  for (Index = 0; Index < 4; Index++) {
    if (Disk[446 + (Index * 16) + 4] == 0xEE) {
      ProtectiveCount++;
    }
  }
  if (ProtectiveCount != 1) {
    return FALSE;
  }

  Header = Disk + 512;
  if ((Header[0] != 'E') || (Header[1] != 'F') || (Header[2] != 'I') ||
      (Header[3] != ' ') || (Header[4] != 'P') || (Header[5] != 'A') ||
      (Header[6] != 'R') || (Header[7] != 'T') ||
      (NtasiReadUint32 (Header + 8) != 0x00010000U))
  {
    return FALSE;
  }

  TotalLbas   = DiskSize / 512;
  HeaderSize  = NtasiReadUint32 (Header + 12);
  BackupLba   = NtasiReadUint64 (Header + 32);
  FirstUsable = NtasiReadUint64 (Header + 40);
  LastUsable  = NtasiReadUint64 (Header + 48);
  EntryLba    = NtasiReadUint64 (Header + 72);
  EntryCount  = NtasiReadUint32 (Header + 80);
  EntrySize   = NtasiReadUint32 (Header + 84);
  if ((HeaderSize < 92) || (HeaderSize > 512) ||
      (NtasiReadUint32 (Header + 20) != 0) ||
      (NtasiReadUint64 (Header + 24) != 1) ||
      (BackupLba != TotalLbas - 1) ||
      (FirstUsable < 2) || (FirstUsable > LastUsable) ||
      (LastUsable >= BackupLba) || (EntryLba < 2) ||
      (EntryLba >= FirstUsable) || (EntryCount == 0) ||
      (EntryCount > 4096) || (EntrySize < 128) ||
      (EntrySize > 1024) || ((EntrySize & 7U) != 0) ||
      (NtasiReadUint32 (Header + 16) != NtasiCrc32WithZeroRange (
                                               Header,
                                               HeaderSize,
                                               16,
                                               sizeof (UINT32)
                                               )))
  {
    return FALSE;
  }

  EntryBytes = (UINT64)EntryCount * EntrySize;
  if ((EntryBytes > MAX_UINTN) ||
      (EntryLba > MAX_UINT64 / 512) ||
      (EntryLba * 512 > DiskSize) ||
      (EntryBytes > DiskSize - (EntryLba * 512)) ||
      ((EntryLba * 512) + EntryBytes > FirstUsable * 512))
  {
    return FALSE;
  }
  Entries = Disk + (UINTN)(EntryLba * 512);
  if (NtasiReadUint32 (Header + 88) !=
      NtasiAppendedRamdiskCrc32 (Entries, (UINTN)EntryBytes))
  {
    return FALSE;
  }

  EspCount        = 0;
  PartitionOffset = 0;
  PartitionSize   = 0;
  for (Index = 0; Index < EntryCount; Index++) {
    Entry = Entries + ((UINTN)Index * EntrySize);
    IsEsp = TRUE;
    for (ByteIndex = 0; ByteIndex < ARRAY_SIZE (EspTypeGuid); ByteIndex++) {
      if (Entry[ByteIndex] != EspTypeGuid[ByteIndex]) {
        IsEsp = FALSE;
        break;
      }
    }
    if (!IsEsp) {
      continue;
    }

    FirstLba = NtasiReadUint64 (Entry + 32);
    LastLba  = NtasiReadUint64 (Entry + 40);
    if ((FirstLba < FirstUsable) || (FirstLba > LastLba) ||
        (LastLba > LastUsable) || (FirstLba > MAX_UINT64 / 512) ||
        ((LastLba - FirstLba + 1) > MAX_UINT64 / 512))
    {
      return FALSE;
    }
    PartitionOffset = FirstLba * 512;
    PartitionSize   = (LastLba - FirstLba + 1) * 512;
    if ((PartitionOffset > DiskSize) ||
        (PartitionSize > DiskSize - PartitionOffset))
    {
      return FALSE;
    }
    EspCount++;
  }

  return (EspCount == 1) &&
         NtasiValidateFatVolume (
           Disk + (UINTN)PartitionOffset,
           PartitionSize
           );
}

STATIC
BOOLEAN
NtasiValidateFatBootSector (
  IN CONST VOID  *Image,
  IN UINT64      ImageSize
  )
{
  CONST UINT8  *Disk;

  if ((Image == NULL) || (ImageSize < 512) || (ImageSize > MAX_UINTN)) {
    return FALSE;
  }
  Disk = (CONST UINT8 *)Image;
  return NtasiValidateFatVolume (Disk, ImageSize) ||
         NtasiValidateGptFatDisk (Disk, ImageSize);
}
#endif

#endif

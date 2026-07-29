/** @file
  Register an in-place appended FAT ramdisk, retaining the legacy FV fallback.

  SPDX-License-Identifier: MIT
**/

#include <PiDxe.h>
#include <Guid/GlobalVariable.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseMemoryLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "BootRamdiskHelperDxe.h"
#define NTASI_APPENDED_RAMDISK_INCLUDE_FAT_VALIDATOR  1
#include <AppendedRamdisk.h>

STATIC CONST EFI_GUID  mNtasiAppendedRamdiskLocationHobGuid =
  NTASI_APPENDED_RAMDISK_LOCATION_HOB_GUID;
STATIC CONST EFI_GUID  mNtasiEvidencePartitionGuid =
  { 0x4e544153, 0x492d, 0x4742, { 0x94, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 } };

STATIC
EFI_STATUS
FindRamdiskPartitionDevicePath (
  IN  EFI_DEVICE_PATH_PROTOCOL  *RamdiskDevicePath,
  OUT EFI_DEVICE_PATH_PROTOCOL  **PartitionDevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Candidate;
  EFI_DEVICE_PATH_PROTOCOL  *Remaining;
  EFI_HANDLE                RamdiskHandle;
  EFI_HANDLE                *Handles;
  HARDDRIVE_DEVICE_PATH     *HardDrive;
  EFI_STATUS                Status;
  UINTN                     HandleCount;
  UINTN                     Index;
  UINTN                     ParentPrefixSize;
  UINTN                     CandidateSize;

  if ((RamdiskDevicePath == NULL) || (PartitionDevicePath == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *PartitionDevicePath = NULL;

  Remaining = RamdiskDevicePath;
  Status = gBS->LocateDevicePath (
                  &gEfiBlockIoProtocolGuid,
                  &Remaining,
                  &RamdiskHandle
                  );
  if (EFI_ERROR (Status) || !IsDevicePathEnd (Remaining)) {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: cannot locate RAM-disk BlockIo handle: %r\n", Status));
    return EFI_NOT_FOUND;
  }
  Status = gBS->ConnectController (RamdiskHandle, NULL, NULL, TRUE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: cannot recursively connect RAM disk: %r\n", Status));
    return Status;
  }

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &HandleCount,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ParentPrefixSize = GetDevicePathSize (RamdiskDevicePath) - END_DEVICE_PATH_LENGTH;
  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&Candidate
                    );
    if (EFI_ERROR (Status) || (Candidate == NULL)) {
      continue;
    }
    CandidateSize = GetDevicePathSize (Candidate);
    if ((CandidateSize != ParentPrefixSize + sizeof (HARDDRIVE_DEVICE_PATH) +
                          END_DEVICE_PATH_LENGTH) ||
        (CompareMem (Candidate, RamdiskDevicePath, ParentPrefixSize) != 0))
    {
      continue;
    }

    HardDrive = (HARDDRIVE_DEVICE_PATH *)((UINT8 *)Candidate + ParentPrefixSize);
    if ((DevicePathType (&HardDrive->Header) != MEDIA_DEVICE_PATH) ||
        (DevicePathSubType (&HardDrive->Header) != MEDIA_HARDDRIVE_DP) ||
        (DevicePathNodeLength (&HardDrive->Header) != sizeof (*HardDrive)) ||
        (HardDrive->PartitionNumber != 1) ||
        (HardDrive->MBRType != MBR_TYPE_EFI_PARTITION_TABLE_HEADER) ||
        (HardDrive->SignatureType != SIGNATURE_TYPE_GUID) ||
        (CompareMem (HardDrive->Signature, &mNtasiEvidencePartitionGuid,
                     sizeof (mNtasiEvidencePartitionGuid)) != 0) ||
        !IsDevicePathEnd (NextDevicePathNode (&HardDrive->Header)))
    {
      continue;
    }
    if (*PartitionDevicePath != NULL) {
      FreePool (*PartitionDevicePath);
      *PartitionDevicePath = NULL;
      FreePool (Handles);
      DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: multiple matching GPT partition children\n"));
      return EFI_COMPROMISED_DATA;
    }
    *PartitionDevicePath = DuplicateDevicePath (Candidate);
    if (*PartitionDevicePath == NULL) {
      FreePool (Handles);
      return EFI_OUT_OF_RESOURCES;
    }
  }
  FreePool (Handles);
  if (*PartitionDevicePath == NULL) {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: exact GPT data-partition child was not produced\n"));
    return EFI_NOT_FOUND;
  }
  DEBUG ((DEBUG_INFO, "BootRamdiskHelperDxe: selected exact VirtualDisk/HD(1,GPT) data child\n"));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
PrioritizeRamdiskBoot (
  IN EFI_DEVICE_PATH_PROTOCOL  *RamdiskDevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL      *BootDevicePath;
  EFI_DEVICE_PATH_PROTOCOL      *FileDevicePathOnly;
  EFI_BOOT_MANAGER_LOAD_OPTION  LoadOption;
  EFI_STATUS                    Status;
  UINT16                        BootNext;

  FileDevicePathOnly = FileDevicePath (
                         NULL,
                         L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"
                         );
  if (FileDevicePathOnly == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  BootDevicePath = AppendDevicePath (RamdiskDevicePath, FileDevicePathOnly);
  FreePool (FileDevicePathOnly);
  if (BootDevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = EfiBootManagerInitializeLoadOption (
             &LoadOption,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE,
             L"NTASI Appended RAM WinPE",
             BootDevicePath,
             NULL,
             0
             );
  FreePool (BootDevicePath);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: cannot initialize RAM WinPE boot option: %r\n", Status));
    return Status;
  }

  Status = EfiBootManagerAddLoadOptionVariable (&LoadOption, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: cannot add RAM WinPE boot option: %r\n", Status));
    EfiBootManagerFreeLoadOption (&LoadOption);
    return Status;
  }
  if (LoadOption.OptionNumber > MAX_UINT16) {
    EfiBootManagerFreeLoadOption (&LoadOption);
    return EFI_COMPROMISED_DATA;
  }

  BootNext = (UINT16)LoadOption.OptionNumber;
  Status   = gRT->SetVariable (
                    L"BootNext",
                    &gEfiGlobalVariableGuid,
                    EFI_VARIABLE_NON_VOLATILE |
                    EFI_VARIABLE_BOOTSERVICE_ACCESS |
                    EFI_VARIABLE_RUNTIME_ACCESS,
                    sizeof (BootNext),
                    &BootNext
                    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: cannot set RAM WinPE BootNext: %r\n", Status));
  } else {
    DEBUG ((DEBUG_INFO, "BootRamdiskHelperDxe: BootNext=Boot%04x selects appended RAM WinPE\n", BootNext));
  }
  EfiBootManagerFreeLoadOption (&LoadOption);
  return Status;
}

STATIC
EFI_STATUS
RegisterRamdisk (
  IN UINTN   Address,
  IN UINT64  Size,
  IN BOOLEAN RequireGptPartition
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *PartitionDevicePath;
  EFI_RAM_DISK_PROTOCOL     *RamdiskProtocol;
  EFI_STATUS                Status;

  Status = gBS->LocateProtocol (
                  &gEfiRamDiskProtocolGuid,
                  NULL,
                  (VOID **)&RamdiskProtocol
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: RAM disk protocol unavailable: %r\n", Status));
    return Status;
  }

  Status = RamdiskProtocol->Register (
                              Address,
                              Size,
                              &gEfiVirtualDiskGuid,
                              NULL,
                              &DevicePath
                              );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: cannot register RAM disk: %r\n", Status));
    return Status;
  }

  if (!RequireGptPartition) {
    return PrioritizeRamdiskBoot (DevicePath);
  }
  Status = FindRamdiskPartitionDevicePath (DevicePath, &PartitionDevicePath);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = PrioritizeRamdiskBoot (PartitionDevicePath);
  FreePool (PartitionDevicePath);
  return Status;
}

STATIC
EFI_STATUS
RegisterAppendedRamdisk (
  VOID
  )
{
  VOID                                  *GuidHob;
  CONST NTASI_APPENDED_RAMDISK_LOCATION *Location;
  CONST NTASI_APPENDED_RAMDISK_HEADER   *Header;
  CONST VOID                            *Image;
  UINT64                                ImageSize;

  GuidHob = GetFirstGuidHob (&mNtasiAppendedRamdiskLocationHobGuid);
  if (GuidHob == NULL) {
    return EFI_NOT_FOUND;
  }
  if (GET_GUID_HOB_DATA_SIZE (GuidHob) != sizeof (*Location)) {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: malformed location HOB size\n"));
    return EFI_COMPROMISED_DATA;
  }

  Location = (CONST NTASI_APPENDED_RAMDISK_LOCATION *)GET_GUID_HOB_DATA (GuidHob);
  if ((Location->Signature != NTASI_APPENDED_RAMDISK_LOCATION_SIGNATURE) ||
      (Location->Version != NTASI_APPENDED_RAMDISK_LOCATION_VERSION) ||
      (Location->StructureSize != sizeof (*Location)) ||
      (Location->HeaderPhysicalAddress == 0) ||
      (Location->ReservationSize < sizeof (*Header)) ||
      (Location->ReservationSize > NTASI_APPENDED_RAMDISK_MAX_MAPPED_SPAN) ||
      (Location->HeaderPhysicalAddress + Location->ReservationSize <
       Location->HeaderPhysicalAddress))
  {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: invalid location HOB\n"));
    return EFI_COMPROMISED_DATA;
  }

  DEBUG ((
    DEBUG_INFO,
    "BootRamdiskHelperDxe: probing PEI-published header at 0x%lx (0x%lx-byte reservation)\n",
    Location->HeaderPhysicalAddress,
    Location->ReservationSize
    ));
  Header = (CONST NTASI_APPENDED_RAMDISK_HEADER *)(UINTN)Location->HeaderPhysicalAddress;
  if (Header->Signature != NTASI_APPENDED_RAMDISK_SIGNATURE) {
    return EFI_NOT_FOUND;
  }
  DEBUG ((DEBUG_INFO, "BootRamdiskHelperDxe: appended header signature found\n"));

  if (!NtasiValidateAppendedRamdisk (
         Header,
         Location->ReservationSize,
         TRUE,
         &Image,
         &ImageSize,
         NULL
         ) ||
      (ImageSize > MAX_UINTN) ||
      !NtasiValidateFatBootSector (Image, ImageSize))
  {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: appended ramdisk failed header or FAT validation\n"));
    return EFI_COMPROMISED_DATA;
  }

  DEBUG ((
    DEBUG_INFO,
    "BootRamdiskHelperDxe: header/FAT/payload CRC valid (0x%x)\n",
    Header->ImageCrc32
    ));

  DEBUG ((
    DEBUG_INFO,
    "BootRamdiskHelperDxe: registering appended FAT image at 0x%lx (0x%lx bytes)\n",
    (UINT64)(UINTN)Image,
    ImageSize
    ));
  return RegisterRamdisk ((UINTN)Image, ImageSize, TRUE);
}

EFI_STATUS
EFIAPI
BootRamdiskHelperDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  VOID        *DestinationRamdiskPtr;
  VOID        *OriginalRamDiskPtr;
  UINTN       RamDiskSize;
  EFI_STATUS  Status;

  DEBUG ((DEBUG_INFO, "BootRamdiskHelperDxe started\n"));

  Status = RegisterAppendedRamdisk ();
  if (Status != EFI_NOT_FOUND) {
    return Status;
  }

  if (!PcdGetBool (PcdInitializeRamdisk)) {
    DEBUG ((DEBUG_INFO, "BootRamdiskHelperDxe: no appended image and FV ramdisk is disabled\n"));
    return EFI_UNSUPPORTED;
  }

  Status = GetSectionFromAnyFv (
             &gAppleSiliconPkgEmbeddedRamdiskGuid,
             EFI_SECTION_RAW,
             0,
             &OriginalRamDiskPtr,
             &RamDiskSize
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: no FV embedded ramdisk\n"));
    return EFI_NOT_FOUND;
  }

  if ((OriginalRamDiskPtr == NULL) || (RamDiskSize == 0)) {
    return EFI_COMPROMISED_DATA;
  }

  DestinationRamdiskPtr = AllocateCopyPool (RamDiskSize, OriginalRamDiskPtr);
  if (DestinationRamdiskPtr == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  return RegisterRamdisk ((UINTN)DestinationRamdiskPtr, RamDiskSize, FALSE);
}

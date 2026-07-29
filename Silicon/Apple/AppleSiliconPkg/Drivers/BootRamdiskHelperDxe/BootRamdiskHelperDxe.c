/** @file
  Register an in-place appended FAT ramdisk, retaining the legacy FV fallback.

  SPDX-License-Identifier: MIT
**/

#include <PiDxe.h>

#include "BootRamdiskHelperDxe.h"
#include <AppendedRamdisk.h>

STATIC
EFI_STATUS
RegisterRamdisk (
  IN UINTN   Address,
  IN UINT64  Size
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
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
  }

  return Status;
}

STATIC
EFI_STATUS
RegisterAppendedRamdisk (
  VOID
  )
{
  EFI_PHYSICAL_ADDRESS                  FdTop;
  CONST NTASI_APPENDED_RAMDISK_HEADER   *Header;
  CONST VOID                            *Image;
  UINT64                                ImageSize;

  FdTop = PcdGet64 (PcdFdBaseAddress) + PcdGet32 (PcdFdSize);
  if (FdTop < PcdGet64 (PcdFdBaseAddress)) {
    return EFI_NOT_FOUND;
  }

  Header = (CONST NTASI_APPENDED_RAMDISK_HEADER *)(UINTN)FdTop;
  if (Header->Signature != NTASI_APPENDED_RAMDISK_SIGNATURE) {
    return EFI_NOT_FOUND;
  }

  if (!NtasiValidateAppendedRamdisk (
         Header,
         NTASI_APPENDED_RAMDISK_MAX_MAPPED_SPAN,
         TRUE,
         &Image,
         &ImageSize,
         NULL
         ) ||
      !NtasiValidateFatBootSector (Image, ImageSize))
  {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: appended ramdisk failed header, CRC, or FAT validation\n"));
    return EFI_COMPROMISED_DATA;
  }

  DEBUG ((
    DEBUG_INFO,
    "BootRamdiskHelperDxe: registering appended FAT image at 0x%lx (0x%lx bytes)\n",
    (UINT64)(UINTN)Image,
    ImageSize
    ));
  return RegisterRamdisk ((UINTN)Image, ImageSize);
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

  return RegisterRamdisk ((UINTN)DestinationRamdiskPtr, RamDiskSize);
}

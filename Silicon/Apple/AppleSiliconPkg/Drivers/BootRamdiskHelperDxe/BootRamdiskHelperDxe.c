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

//
// NTASI_DEPLOY_EVIDENCE_ECHO gates the WinPE deploy-verdict echo. It is OFF by
// default and is supplied by MacBookProEarly2023.dsc from the DSC define of the
// same name; see EchoEvidenceFile() for what it does and DEPLOY-EVIDENCE-ECHO
// in Docs/ for why it is opt-in rather than always-on.
//
// The gate is a preprocessor gate, not a runtime `if`, deliberately: with it off
// this driver must compile to the exact bytes it did before the diagnostic
// existed, so that a boot regression can never be attributed to a diagnostic
// that is not in the image. That property is checked by rebuilding with the gate
// off and comparing this module's FFS digest against the pre-diagnostic build.
//
#ifndef NTASI_DEPLOY_EVIDENCE_ECHO
#define NTASI_DEPLOY_EVIDENCE_ECHO  0
#endif

#if NTASI_DEPLOY_EVIDENCE_ECHO
  #include <Guid/FileInfo.h>
  #include <Library/UefiLib.h>

//
// Path the WinPE offline-deploy lane writes its verdict to, and the cap on how
// much of it we are willing to push through a 115200-baud console on the boot
// path. See EchoEvidenceFile() for the full rationale.
//
  #define NTASI_EVIDENCE_ECHO_PATH       L"\\NTASI\\last-deploy.txt"
  #define NTASI_EVIDENCE_ECHO_MAX_BYTES  (16u * 1024u)

//
// Upper bound on how many SimpleFileSystem volumes we are willing to open on the
// boot path. On this machine the boot disk is a USB 2.0 High-Speed device, so
// every OpenVolume is real bus traffic issued microseconds before Windows takes
// the controller over; an unbounded scan is not something a diagnostic gets to do.
//
  #define NTASI_EVIDENCE_ECHO_MAX_VOLUMES  16u
#endif

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

#if NTASI_DEPLOY_EVIDENCE_ECHO

/**
  Echo a WinPE evidence file from any attached filesystem to the SERIAL console.

  WHY THIS EXISTS: WinPE on this machine has no serial console at all. COM0 is an
  Apple UART with no inbox Windows driver, and m1n1 owns the only physical UART for
  its proxy protocol, so everything the offline-deploy lane prints reaches the
  PANEL and nowhere else -- the user has to photograph the screen to tell us what
  happened. That has repeatedly left the deploy lane un-debuggable: it silently
  failed to install a single driver across many runs while reporting partial
  success, and nobody could see why.

  Mu, unlike WinPE, has BOTH a FAT driver and the serial console. So the deploy
  lane writes its verdict to \NTASI\last-deploy.txt on a FAT volume (the ESP), and
  the very next Mu boot prints it over the wire, where it lands in the host-side
  log automatically.

  Deliberately best-effort: this is a diagnostic aid on the boot path, and it must
  never be able to stop the machine booting. Every failure is logged and ignored.

  The file is DELETED after a successful echo so each deploy's evidence is printed
  exactly once and a stale verdict can never be mistaken for a fresh one. If the
  delete fails the file is still echoed, but the failure is logged loudly -- a
  verdict that reprints every boot means the volume is not writable, which is
  itself worth knowing.

  BOUNDED ON PURPOSE. This runs microseconds before the OS loader is started, on a
  machine whose boot disk is a USB 2.0 High-Speed device, in a DEBUG build where
  any ASSERT anywhere deadloops the firmware. So it stops at the first volume that
  actually carries the file, never examines more than
  NTASI_EVIDENCE_ECHO_MAX_VOLUMES volumes, and only ever attempts a write on a
  handle it successfully opened for writing.
**/
STATIC
VOID
EchoEvidenceFile (
  VOID
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_INFO                    *Info;
  EFI_HANDLE                       *Handles;
  EFI_STATUS                       Status;
  UINTN                            HandleCount;
  UINTN                            Index;
  UINTN                            InfoSize;
  UINTN                            ReadSize;
  UINTN                            Cursor;
  UINTN                            LineStart;
  CHAR8                            *Buffer;
  UINT64                           FileSize;
  UINTN                            Echoed;
  BOOLEAN                          Writable;

  Handles     = NULL;
  HandleCount = 0;
  Echoed      = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    return;
  }

  if (HandleCount > NTASI_EVIDENCE_ECHO_MAX_VOLUMES) {
    DEBUG ((
      DEBUG_WARN,
      "NTASI evidence: %u filesystem volumes attached; only scanning the first %u\n",
      (UINT32)HandleCount,
      (UINT32)NTASI_EVIDENCE_ECHO_MAX_VOLUMES
      ));
    HandleCount = NTASI_EVIDENCE_ECHO_MAX_VOLUMES;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Fs
                    );
    if (EFI_ERROR (Status) || (Fs == NULL)) {
      continue;
    }

    Root = NULL;
    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status) || (Root == NULL)) {
      continue;
    }

    Writable = TRUE;
    File     = NULL;
    Status   = Root->Open (
                       Root,
                       &File,
                       NTASI_EVIDENCE_ECHO_PATH,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                       0
                       );
    if (EFI_ERROR (Status) || (File == NULL)) {
      //
      // Retry read-only: on a volume Mu cannot write we still want the text.
      //
      Writable = FALSE;
      File     = NULL;
      Status   = Root->Open (Root, &File, NTASI_EVIDENCE_ECHO_PATH, EFI_FILE_MODE_READ, 0);
      if (EFI_ERROR (Status) || (File == NULL)) {
        Root->Close (Root);
        continue;
      }
    }

    //
    // Size the file. Cap the echo: this runs on the boot path and the console is
    // a 115200-baud serial line, so an unbounded dump would add minutes to every
    // boot. The lane's verdict block is a few hundred bytes.
    //
    InfoSize = 0;
    Info     = NULL;
    Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      Info = AllocateZeroPool (InfoSize);
      if (Info != NULL) {
        Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
      } else {
        Status = EFI_OUT_OF_RESOURCES;
      }
    }
    if (EFI_ERROR (Status) || (Info == NULL)) {
      if (Info != NULL) {
        FreePool (Info);
      }
      File->Close (File);
      Root->Close (Root);
      continue;
    }

    FileSize = Info->FileSize;
    FreePool (Info);
    if (FileSize == 0) {
      File->Close (File);
      Root->Close (Root);
      continue;
    }
    if (FileSize > NTASI_EVIDENCE_ECHO_MAX_BYTES) {
      DEBUG ((
        DEBUG_WARN,
        "NTASI evidence: %s is %lu bytes; echoing only the first %u\n",
        NTASI_EVIDENCE_ECHO_PATH,
        FileSize,
        (UINT32)NTASI_EVIDENCE_ECHO_MAX_BYTES
        ));
      FileSize = NTASI_EVIDENCE_ECHO_MAX_BYTES;
    }

    Buffer = AllocateZeroPool ((UINTN)FileSize + 1);
    if (Buffer == NULL) {
      File->Close (File);
      Root->Close (Root);
      continue;
    }

    ReadSize = (UINTN)FileSize;
    Status   = File->Read (File, &ReadSize, Buffer);
    if (EFI_ERROR (Status) || (ReadSize == 0)) {
      FreePool (Buffer);
      File->Close (File);
      Root->Close (Root);
      continue;
    }
    Buffer[ReadSize] = '\0';

    DEBUG ((DEBUG_ERROR, "==== NTASI EVIDENCE BEGIN (%s, %u bytes) ====\n",
            NTASI_EVIDENCE_ECHO_PATH, (UINT32)ReadSize));
    //
    // Emit line by line. DEBUG() has a bounded internal buffer, so a single
    // print of the whole file would be truncated; and CR/LF from a Windows
    // batch script must not corrupt the framing of the host-side log.
    //
    LineStart = 0;
    for (Cursor = 0; Cursor <= ReadSize; Cursor++) {
      if ((Cursor == ReadSize) || (Buffer[Cursor] == '\n')) {
        if (Cursor > LineStart) {
          if (Buffer[Cursor - 1] == '\r') {
            Buffer[Cursor - 1] = '\0';
          }
        }
        if (Cursor < ReadSize) {
          Buffer[Cursor] = '\0';
        }
        DEBUG ((DEBUG_ERROR, "NTASI| %a\n", &Buffer[LineStart]));
        LineStart = Cursor + 1;
      }
    }
    DEBUG ((DEBUG_ERROR, "==== NTASI EVIDENCE END ====\n"));
    Echoed++;

    FreePool (Buffer);

    //
    // Print once, then remove. Delete() closes the handle whether or not the
    // removal itself succeeded, so the handle must not be closed again here.
    //
    // Only attempted on a handle we actually opened for writing: calling
    // Delete() on a read-only handle is a guaranteed EFI_WARN_DELETE_FAILURE
    // that still burns the handle, and it would make an unwritable volume look
    // like a failed delete rather than what it is.
    //
    // EFI_WARN_DELETE_FAILURE is a WARNING, so EFI_ERROR() is FALSE for it. The
    // check is against EFI_SUCCESS, which is the only outcome that means the
    // evidence is really gone.
    //
    if (Writable) {
      Status = File->Delete (File);
      if (Status != EFI_SUCCESS) {
        DEBUG ((
          DEBUG_ERROR,
          "NTASI evidence: could NOT delete %s (%r) -- it will reprint every boot; "
          "treat a repeated verdict as stale and the volume as unwritable\n",
          NTASI_EVIDENCE_ECHO_PATH,
          Status
          ));
      }
    } else {
      DEBUG ((
        DEBUG_ERROR,
        "NTASI evidence: %s was opened read-only and is NOT being deleted -- it "
        "will reprint every boot; treat a repeated verdict as stale\n",
        NTASI_EVIDENCE_ECHO_PATH
        ));
      File->Close (File);
    }

    Root->Close (Root);

    //
    // One verdict per boot. The deploy lane writes exactly one file, so once it
    // has been echoed there is nothing left to look for, and every further
    // OpenVolume is boot-path USB traffic spent for nothing.
    //
    break;
  }

  FreePool (Handles);

  if (Echoed == 0) {
    DEBUG ((DEBUG_INFO, "NTASI evidence: no %s on any attached volume\n",
            NTASI_EVIDENCE_ECHO_PATH));
  }
}

STATIC
VOID
EFIAPI
OnReadyToBootEchoEvidence (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  //
  // ReadyToBoot, not driver entry: at entry the ESP is not connected yet, so
  // OpenVolume would find nothing. By ReadyToBoot BDS has connected the boot
  // devices and every FAT volume is enumerable.
  //
  EchoEvidenceFile ();
}

#endif // NTASI_DEPLOY_EVIDENCE_ECHO

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
 #if NTASI_DEPLOY_EVIDENCE_ECHO
  EFI_EVENT  ReadyToBootEvent;
 #endif

  DEBUG ((DEBUG_INFO, "BootRamdiskHelperDxe started\n"));

 #if NTASI_DEPLOY_EVIDENCE_ECHO
  //
  // Registered before any early-return below: the evidence echo is independent
  // of whether this boot uses an appended ramdisk, an FV ramdisk, or neither.
  //
  // Announced unconditionally so that a boot log makes it obvious this build
  // carries an extra ReadyToBoot participant. A build produced without
  // NTASI_DEPLOY_EVIDENCE_ECHO prints nothing here and registers nothing.
  //
  ReadyToBootEvent = NULL;
  Status           = EfiCreateEventReadyToBootEx (
                       TPL_CALLBACK,
                       OnReadyToBootEchoEvidence,
                       NULL,
                       &ReadyToBootEvent
                       );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: evidence echo not armed: %r\n", Status));
  } else {
    DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: WinPE deploy-evidence echo ARMED at ReadyToBoot\n"));
  }

 #endif

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

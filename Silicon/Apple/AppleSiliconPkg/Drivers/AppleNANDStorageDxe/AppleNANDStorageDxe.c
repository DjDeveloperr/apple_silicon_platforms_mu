/** @file
  Apple ANS NVMe DXE driver.

  SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Library/AppleDTLib.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#include "Shared/AppleAscCore.h"
#include "Shared/AppleNvmeBlockCore.h"
#include "Shared/AppleNvmeControllerCore.h"
#include "Shared/AppleRtkitRuntimeCore.h"
#include "Shared/AppleSartRuntimeCore.h"

#define APPLE_ANS_MAILBOX_OFFSET  0x8000u
#define APPLE_ANS_NAMESPACE_ID    1u
#define APPLE_ANS_POLL_LIMIT      2000000u

typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} APPLE_ANS_DEVICE_PATH;

typedef struct {
  EFI_HANDLE                        Handle;
  EFI_EVENT                         ExitBootServicesEvent;
  UINTN                             CpuBase;
  UINTN                             MailboxBase;
  UINTN                             NvmeBase;
  UINTN                             SartBase;
  CONST struct ntasi_ans_hw         *NvmeHw;
  struct ntasi_asc_transport        Asc;
  struct ntasi_rtkit_runtime        Rtkit;
  struct ntasi_sart_runtime         Sart;
  struct ntasi_ans_controller       Controller;
  struct ntasi_ans_block_device     BlockDevice;
  struct ntasi_ans_queue_memory     AdminMemory;
  struct ntasi_ans_queue_memory     IoMemory;
  VOID                              *AdminCommands;
  VOID                              *AdminCompletions;
  VOID                              *AdminTcbs;
  VOID                              *IoCommands;
  VOID                              *IoCompletions;
  VOID                              *IoTcbs;
  VOID                              *Bounce;
  UINTN                             AdminCommandPages;
  UINTN                             AdminCompletionPages;
  UINTN                             AdminTcbPages;
  UINTN                             IoCommandPages;
  UINTN                             IoCompletionPages;
  UINTN                             IoTcbPages;
  EFI_BLOCK_IO_MEDIA                Media;
  EFI_BLOCK_IO_PROTOCOL             BlockIo;
  APPLE_ANS_DEVICE_PATH             DevicePath;
  BOOLEAN                           Fatal;
  BOOLEAN                           HandedOff;
} APPLE_ANS_DEVICE;

STATIC APPLE_ANS_DEVICE  *mAns;

STATIC CONST EFI_GUID  mAppleAnsDevicePathGuid = {
  0x171cfd4c, 0x628f, 0x4d87,
  { 0xa8, 0x55, 0x20, 0x57, 0x04, 0x15, 0x21, 0x10 }
};

STATIC BOOLEAN
PropertyContains (
  IN dt_node_t   *Node,
  IN CONST CHAR8 *Property,
  IN CONST CHAR8 *Needle
  )
{
  CONST CHAR8  *Value;
  UINTN        Length;
  UINTN        Offset;

  Value = dt_node_prop (Node, Property, &Length);
  if (Value == NULL) {
    return FALSE;
  }

  for (Offset = 0; Offset < Length; ) {
    UINTN  ItemLength;

    ItemLength = AsciiStrnLenS (Value + Offset, Length - Offset);
    if (AsciiStrStr (Value + Offset, Needle) != NULL) {
      return TRUE;
    }

    if (ItemLength == Length - Offset) {
      break;
    }

    Offset += ItemLength + 1;
  }

  return FALSE;
}

STATIC UINT32
AscCpuRead32 (
  IN VOID   *Opaque,
  IN UINT32 Offset
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;
  return MmioRead32 (Device->CpuBase + Offset);
}

STATIC VOID
AscCpuWrite32 (
  IN VOID   *Opaque,
  IN UINT32 Offset,
  IN UINT32 Value
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;
  MmioWrite32 (Device->CpuBase + Offset, Value);
}

STATIC UINT32
AscMailboxRead32 (
  IN VOID   *Opaque,
  IN UINT32 Offset
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;
  return MmioRead32 (Device->MailboxBase + Offset);
}

STATIC UINT64
AscMailboxRead64 (
  IN VOID   *Opaque,
  IN UINT32 Offset
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;
  return MmioRead64 (Device->MailboxBase + Offset);
}

STATIC VOID
AscMailboxWrite64 (
  IN VOID   *Opaque,
  IN UINT32 Offset,
  IN UINT64 Value
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;
  MmioWrite64 (Device->MailboxBase + Offset, Value);
}

STATIC VOID
DmaBarrier (
  IN VOID *Opaque
  )
{
  (VOID)Opaque;
  ArmDataMemoryBarrier ();
}

STATIC VOID
PollService (
  IN VOID *Opaque
  )
{
  (VOID)Opaque;
  CpuPause ();
}

STATIC UINT32
SartRead32 (
  IN VOID   *Opaque,
  IN UINT32 Offset
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;
  return MmioRead32 (Device->SartBase + Offset);
}

STATIC VOID
SartWrite32 (
  IN VOID   *Opaque,
  IN UINT32 Offset,
  IN UINT32 Value
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;
  MmioWrite32 (Device->SartBase + Offset, Value);
}

STATIC int
AllocateRtkitShared (
  IN VOID                              *Opaque,
  IN uint8_t                           Endpoint,
  IN size_t                            Size,
  OUT struct ntasi_rtkit_shared_buffer *Buffer
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;
  VOID              *Address;
  UINTN             Pages;
  UINTN             MappedSize;
  int               Result;

  (VOID)Endpoint;
  Pages      = EFI_SIZE_TO_PAGES (Size);
  MappedSize = EFI_PAGES_TO_SIZE (Pages);
  Address    = AllocateAlignedPages (Pages, NTASI_SART_PAGE_SIZE);
  if (Address == NULL) {
    return -1;
  }

  ZeroMem (Address, MappedSize);
  Result = ntasi_sart_runtime_add (
             &Device->Sart,
             (UINT64)(UINTN)Address,
             MappedSize,
             NULL
             );
  if (Result != 0) {
    FreeAlignedPages (Address, Pages);
    return Result;
  }

  Buffer->cpu_address   = Address;
  Buffer->device_address = (UINT64)(UINTN)Address;
  Buffer->size          = Size;
  return 0;
}

STATIC VOID
ReleaseRtkitShared (
  IN VOID                              *Opaque,
  IN uint8_t                           Endpoint,
  IN struct ntasi_rtkit_shared_buffer  *Buffer
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;
  UINTN             Pages;
  UINTN             MappedSize;

  (VOID)Endpoint;
  Pages      = EFI_SIZE_TO_PAGES (Buffer->size);
  MappedSize = EFI_PAGES_TO_SIZE (Pages);
  ntasi_sart_runtime_remove (
    &Device->Sart,
    Buffer->device_address,
    MappedSize
    );
  FreeAlignedPages (Buffer->cpu_address, Pages);
}

STATIC VOID
RtkitCrashed (
  IN VOID                                    *Opaque,
  IN CONST struct ntasi_rtkit_shared_buffer  *Crashlog
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;

  (VOID)Crashlog;
  Device->Fatal = TRUE;
  DEBUG ((DEBUG_ERROR, "AppleANS: RTKit firmware crashed\n"));
}

STATIC UINT32
NvmeRead32 (
  IN VOID   *Opaque,
  IN UINT32 Offset
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;
  return MmioRead32 (Device->NvmeBase + Offset);
}

STATIC VOID
NvmeWrite32 (
  IN VOID   *Opaque,
  IN UINT32 Offset,
  IN UINT32 Value
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;
  MmioWrite32 (Device->NvmeBase + Offset, Value);
}

STATIC VOID
NvmeService (
  IN VOID *Opaque
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;
  int               Result;

  Result = ntasi_rtkit_runtime_service (&Device->Rtkit, NULL);
  if ((Result < 0) && (Result != NTASI_RTKIT_RUNTIME_ERR_CRASHED)) {
    Device->Fatal = TRUE;
  }

  CpuPause ();
}

STATIC int
BlockExecute (
  IN VOID                         *Opaque,
  IN bool                         Admin,
  IN CONST struct ntasi_ans_sqe   *Command,
  IN enum ntasi_ans_dma_direction Direction,
  OUT uint64_t                    *Result
  )
{
  APPLE_ANS_DEVICE  *Device = Opaque;

  if (Device->Fatal || Device->HandedOff) {
    return NTASI_ANS_CONTROLLER_ERR_NOT_STARTED;
  }

  return ntasi_ans_controller_execute (
           &Device->Controller,
           Admin ? &Device->Controller.admin : &Device->Controller.io,
           Command,
           Direction,
           Result
           );
}

STATIC EFI_STATUS
MapBlockStatus (
  IN int Result
  )
{
  if (Result == 0) {
    return EFI_SUCCESS;
  }

  if (Result == NTASI_ANS_BLOCK_ERR_RANGE) {
    return EFI_INVALID_PARAMETER;
  }

  if (Result == NTASI_ANS_BLOCK_ERR_BUFFER) {
    return EFI_BAD_BUFFER_SIZE;
  }

  return EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS EFIAPI
AnsReset (
  IN EFI_BLOCK_IO_PROTOCOL *This,
  IN BOOLEAN               ExtendedVerification
  )
{
  (VOID)This;
  (VOID)ExtendedVerification;
  if ((mAns == NULL) || mAns->Fatal || mAns->HandedOff) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ValidateBlockRequest (
  IN EFI_BLOCK_IO_PROTOCOL *This,
  IN UINT32                MediaId,
  IN UINT64                Lba,
  IN UINTN                 BufferSize,
  IN CONST VOID            *Buffer
  )
{
  if ((mAns == NULL) || (This != &mAns->BlockIo) || mAns->HandedOff ||
      mAns->Fatal)
  {
    return EFI_DEVICE_ERROR;
  }

  if (MediaId != This->Media->MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize == 0) && (Buffer == NULL)) {
    return EFI_SUCCESS;
  }

  if ((Buffer == NULL) || ((BufferSize % This->Media->BlockSize) != 0)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > This->Media->LastBlock) ||
      ((BufferSize / This->Media->BlockSize) >
       This->Media->LastBlock - Lba + 1))
  {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
AnsReadBlocks (
  IN EFI_BLOCK_IO_PROTOCOL *This,
  IN UINT32                MediaId,
  IN EFI_LBA               Lba,
  IN UINTN                 BufferSize,
  OUT VOID                 *Buffer
  )
{
  EFI_STATUS  Status;
  UINTN       Blocks;

  Status = ValidateBlockRequest (This, MediaId, Lba, BufferSize, Buffer);
  if (EFI_ERROR (Status) || (BufferSize == 0)) {
    return Status;
  }

  Blocks = BufferSize / This->Media->BlockSize;
  return MapBlockStatus (
           ntasi_ans_block_read (&mAns->BlockDevice, Lba, Blocks, Buffer, BufferSize)
           );
}

STATIC EFI_STATUS EFIAPI
AnsWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL *This,
  IN UINT32                MediaId,
  IN EFI_LBA               Lba,
  IN UINTN                 BufferSize,
  IN VOID                  *Buffer
  )
{
  EFI_STATUS  Status;
  UINTN       Blocks;

  Status = ValidateBlockRequest (This, MediaId, Lba, BufferSize, Buffer);
  if (EFI_ERROR (Status) || (BufferSize == 0)) {
    return Status;
  }

  if (This->Media->ReadOnly) {
    return EFI_WRITE_PROTECTED;
  }

  Blocks = BufferSize / This->Media->BlockSize;
  return MapBlockStatus (
           ntasi_ans_block_write (&mAns->BlockDevice, Lba, Blocks, Buffer, BufferSize)
           );
}

STATIC EFI_STATUS EFIAPI
AnsFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL *This
  )
{
  if ((mAns == NULL) || (This != &mAns->BlockIo) || mAns->HandedOff ||
      mAns->Fatal)
  {
    return EFI_DEVICE_ERROR;
  }

  return MapBlockStatus (ntasi_ans_block_flush (&mAns->BlockDevice));
}

STATIC VOID *
AllocateQueueMemory (
  IN UINTN  Size,
  OUT UINTN *Pages
  )
{
  VOID  *Address;

  *Pages  = EFI_SIZE_TO_PAGES (Size);
  Address = AllocateAlignedPages (*Pages, NTASI_ANS_QUEUE_ALIGN);
  if (Address != NULL) {
    ZeroMem (Address, EFI_PAGES_TO_SIZE (*Pages));
  }

  return Address;
}

STATIC EFI_STATUS
AllocateControllerMemory (
  IN OUT APPLE_ANS_DEVICE *Device
  )
{
  BOOLEAN  Linear;
  UINT32   Slots;

  Slots  = Device->NvmeHw->max_queue_depth;
  Linear = Device->NvmeHw->submission_mode ==
           NTASI_ANS_SUBMISSION_LINEAR_NVMMU;

  Device->AdminCommands = AllocateQueueMemory (
                              ntasi_ans_command_bytes (
                                Device->NvmeHw,
                                TRUE,
                                Device->NvmeHw->admin_queue_depth
                                ),
                              &Device->AdminCommandPages
                              );
  Device->AdminCompletions = AllocateQueueMemory (
                                 ntasi_ans_cq_bytes (
                                   Device->NvmeHw->admin_queue_depth
                                   ),
                                 &Device->AdminCompletionPages
                                 );
  Device->IoCommands = AllocateQueueMemory (
                           ntasi_ans_command_bytes (Device->NvmeHw, FALSE, Slots),
                           &Device->IoCommandPages
                           );
  Device->IoCompletions = AllocateQueueMemory (
                              ntasi_ans_cq_bytes (Slots),
                              &Device->IoCompletionPages
                              );
  if (Linear) {
    Device->AdminTcbs = AllocateQueueMemory (
                            ntasi_ans_tcb_bytes (Slots),
                            &Device->AdminTcbPages
                            );
    Device->IoTcbs = AllocateQueueMemory (
                         ntasi_ans_tcb_bytes (Slots),
                         &Device->IoTcbPages
                         );
  }

  Device->Bounce = AllocateAlignedPages (1, NTASI_ANS_DATA_ALIGN);
  if ((Device->AdminCommands == NULL) ||
      (Device->AdminCompletions == NULL) ||
      (Device->IoCommands == NULL) ||
      (Device->IoCompletions == NULL) ||
      (Device->Bounce == NULL) ||
      (Linear && ((Device->AdminTcbs == NULL) || (Device->IoTcbs == NULL))))
  {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (Device->Bounce, NTASI_ANS_DATA_ALIGN);
  Device->AdminMemory = (struct ntasi_ans_queue_memory) {
    .commands        = Device->AdminCommands,
    .completions     = Device->AdminCompletions,
    .tcbs            = Device->AdminTcbs,
    .commands_dma    = (UINT64)(UINTN)Device->AdminCommands,
    .completions_dma = (UINT64)(UINTN)Device->AdminCompletions,
    .tcbs_dma        = (UINT64)(UINTN)Device->AdminTcbs,
  };
  Device->IoMemory = (struct ntasi_ans_queue_memory) {
    .commands        = Device->IoCommands,
    .completions     = Device->IoCompletions,
    .tcbs            = Device->IoTcbs,
    .commands_dma    = (UINT64)(UINTN)Device->IoCommands,
    .completions_dma = (UINT64)(UINTN)Device->IoCompletions,
    .tcbs_dma        = (UINT64)(UINTN)Device->IoTcbs,
  };
  return EFI_SUCCESS;
}

STATIC VOID
FreeControllerMemory (
  IN APPLE_ANS_DEVICE *Device
  )
{
  if (Device->AdminCommands != NULL) {
    FreeAlignedPages (Device->AdminCommands, Device->AdminCommandPages);
  }
  if (Device->AdminCompletions != NULL) {
    FreeAlignedPages (Device->AdminCompletions, Device->AdminCompletionPages);
  }
  if (Device->AdminTcbs != NULL) {
    FreeAlignedPages (Device->AdminTcbs, Device->AdminTcbPages);
  }
  if (Device->IoCommands != NULL) {
    FreeAlignedPages (Device->IoCommands, Device->IoCommandPages);
  }
  if (Device->IoCompletions != NULL) {
    FreeAlignedPages (Device->IoCompletions, Device->IoCompletionPages);
  }
  if (Device->IoTcbs != NULL) {
    FreeAlignedPages (Device->IoTcbs, Device->IoTcbPages);
  }
  if (Device->Bounce != NULL) {
    FreeAlignedPages (Device->Bounce, 1);
  }
}

STATIC VOID EFIAPI
AnsExitBootServices (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  APPLE_ANS_DEVICE  *Device = Context;
  int               Result;

  (VOID)Event;
  Device->HandedOff = TRUE;
  Result = ntasi_ans_controller_stop (&Device->Controller);
  if (Result != 0) {
    DEBUG ((DEBUG_ERROR, "AppleANS: controller handoff failed: %d\n", Result));
  }

  if (Device->Rtkit.booted) {
    Result = ntasi_rtkit_runtime_sleep (&Device->Rtkit);
    if (Result != 0) {
      DEBUG ((DEBUG_ERROR, "AppleANS: RTKit handoff failed: %d\n", Result));
    }
  }

  ntasi_rtkit_runtime_release_buffers (&Device->Rtkit);
  ntasi_sart_runtime_clear_owned (&Device->Sart);
  ArmDataSynchronizationBarrier ();
}

STATIC EFI_STATUS
DiscoverHardware (
  IN OUT APPLE_ANS_DEVICE              *Device,
  OUT CONST struct ntasi_asc_hw        **AscHw,
  OUT CONST struct ntasi_sart_params   **SartParams
  )
{
  dt_node_t  *AnsNode;
  dt_node_t  *SartNode;
  UINT64     Size;
  UINT64     CpuBase;
  UINT64     NvmeBase;
  UINT64     SartBase;
  UINT32     SartVersion;
  UINTN      PropertySize;
  UINT32     *VersionProperty;
  BOOLEAN    Legacy;

  AnsNode  = dt_get ("/arm-io/ans");
  SartNode = dt_get ("/arm-io/sart-ans");
  if ((AnsNode == NULL) || (SartNode == NULL)) {
    return EFI_NOT_FOUND;
  }

  if ((dt_node_reg (AnsNode, 0, &CpuBase, &Size) != 0) ||
      (dt_node_reg (AnsNode, 3, &NvmeBase, &Size) != 0) ||
      (dt_node_reg (SartNode, 0, &SartBase, &Size) != 0))
  {
    return EFI_NOT_FOUND;
  }

  Device->CpuBase     = (UINTN)CpuBase;
  Device->NvmeBase    = (UINTN)NvmeBase;
  Device->SartBase    = (UINTN)SartBase;
  Device->MailboxBase = Device->CpuBase + APPLE_ANS_MAILBOX_OFFSET;
  Legacy = PropertyContains (AnsNode, "compatible", "t8015");
  Device->NvmeHw = Legacy ? &ntasi_ans_hw_t8015 : &ntasi_ans_hw_t8103;
  *AscHw = Legacy ? &ntasi_asc_hw_t8015 : &ntasi_asc_hw_v4;

  VersionProperty = dt_node_prop (SartNode, "sart-version", &PropertySize);
  if ((VersionProperty != NULL) && (PropertySize >= sizeof (*VersionProperty))) {
    SartVersion = *VersionProperty;
  } else if (Legacy ||
             PropertyContains (SartNode, "compatible", "t8015"))
  {
    SartVersion = 0;
  } else {
    return EFI_UNSUPPORTED;
  }

  if (SartVersion == 0) {
    *SartParams = &ntasi_sart_params_v0;
  } else if (SartVersion == 2) {
    *SartParams = &ntasi_sart_params_v2;
  } else if (SartVersion == 3) {
    *SartParams = &ntasi_sart_params_v3;
  } else {
    return EFI_UNSUPPORTED;
  }

  DEBUG ((
    DEBUG_INFO,
    "AppleANS: cpu=%lx mailbox=%lx nvme=%lx sart=%lx legacy=%d sartv%d\n",
    Device->CpuBase,
    Device->MailboxBase,
    Device->NvmeBase,
    Device->SartBase,
    Legacy,
    SartVersion
    ));
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
AppleNANDStorageDxeInitialize (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  STATIC CONST struct ntasi_asc_ops AscOps = {
    .cpu_read32       = AscCpuRead32,
    .cpu_write32      = AscCpuWrite32,
    .mailbox_read32   = AscMailboxRead32,
    .mailbox_read64   = AscMailboxRead64,
    .mailbox_write64  = AscMailboxWrite64,
    .dma_read_barrier = DmaBarrier,
    .dma_write_barrier = DmaBarrier,
    .service          = PollService,
  };
  STATIC CONST struct ntasi_sart_runtime_ops SartOps = {
    .read32       = SartRead32,
    .write32      = SartWrite32,
    .write_barrier = DmaBarrier,
  };
  STATIC CONST struct ntasi_rtkit_runtime_ops RtkitOps = {
    .allocate_shared = AllocateRtkitShared,
    .release_shared  = ReleaseRtkitShared,
    .crashed         = RtkitCrashed,
  };
  STATIC CONST struct ntasi_ans_controller_ops ControllerOps = {
    .read32           = NvmeRead32,
    .write32          = NvmeWrite32,
    .dma_read_barrier = DmaBarrier,
    .dma_write_barrier = DmaBarrier,
    .service          = NvmeService,
  };
  APPLE_ANS_DEVICE                 *Device;
  CONST struct ntasi_asc_hw        *AscHw;
  CONST struct ntasi_sart_params   *SartParams;
  EFI_STATUS                       Status;
  int                              Result;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  Device = AllocateZeroPool (sizeof (*Device));
  if (Device == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  mAns = Device;
  Status = DiscoverHardware (Device, &AscHw, &SartParams);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Result = ntasi_sart_runtime_init (&Device->Sart, SartParams, &SartOps, Device);
  if (Result != 0) {
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  Result = ntasi_asc_init_variant (
             &Device->Asc,
             &AscOps,
             AscHw,
             Device,
             APPLE_ANS_POLL_LIMIT
             );
  if (Result != 0) {
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  if (ntasi_asc_cpu_running (&Device->Asc)) {
    DEBUG ((DEBUG_WARN, "AppleANS: coprocessor was left running; stopping before boot\n"));
    ntasi_asc_cpu_stop (&Device->Asc);
    MicroSecondDelay (1000);
  }

  Result = ntasi_rtkit_runtime_init (
             &Device->Rtkit,
             &Device->Asc,
             &RtkitOps,
             Device,
             APPLE_ANS_POLL_LIMIT
             );
  if (Result != 0) {
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  Status = AllocateControllerMemory (Device);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Result = ntasi_rtkit_runtime_boot (&Device->Rtkit);
  if (Result != 0) {
    DEBUG ((DEBUG_ERROR, "AppleANS: RTKit boot failed: %d\n", Result));
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  Result = ntasi_ans_controller_start_variant (
             &Device->Controller,
             &ControllerOps,
             Device->NvmeHw,
             Device,
             Device->NvmeHw->max_queue_depth,
             APPLE_ANS_POLL_LIMIT,
             &Device->AdminMemory,
             &Device->IoMemory
             );
  if (Result != 0) {
    DEBUG ((DEBUG_ERROR, "AppleANS: controller start failed: %d\n", Result));
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  Result = ntasi_ans_block_device_init (
             &Device->BlockDevice,
             BlockExecute,
             Device,
             Device->Bounce,
             (UINT64)(UINTN)Device->Bounce,
             NTASI_ANS_DATA_ALIGN
             );
  if (Result == 0) {
    Result = ntasi_ans_block_identify (
               &Device->BlockDevice,
               APPLE_ANS_NAMESPACE_ID
               );
  }
  if (Result != 0) {
    DEBUG ((DEBUG_ERROR, "AppleANS: namespace identify failed: %d\n", Result));
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  Device->Media = (EFI_BLOCK_IO_MEDIA) {
    .MediaId          = 1,
    .RemovableMedia   = FALSE,
    .MediaPresent     = TRUE,
    .LogicalPartition = FALSE,
    .ReadOnly         = FALSE,
    .WriteCaching     = TRUE,
    .BlockSize        = Device->BlockDevice.media.block_size,
    .IoAlign          = 1,
    .LastBlock        = Device->BlockDevice.media.block_count - 1,
    .LowestAlignedLba = 0,
    .LogicalBlocksPerPhysicalBlock = 1,
    .OptimalTransferLengthGranularity = 1,
  };
  Device->BlockIo = (EFI_BLOCK_IO_PROTOCOL) {
    .Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION3,
    .Media       = &Device->Media,
    .Reset       = AnsReset,
    .ReadBlocks  = AnsReadBlocks,
    .WriteBlocks = AnsWriteBlocks,
    .FlushBlocks = AnsFlushBlocks,
  };
  Device->DevicePath = (APPLE_ANS_DEVICE_PATH) {
    .Vendor = {
      .Header = {
        HARDWARE_DEVICE_PATH,
        HW_VENDOR_DP,
        {
          (UINT8)sizeof (VENDOR_DEVICE_PATH),
          (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8)
        }
      },
      .Guid = mAppleAnsDevicePathGuid,
    },
    .End = {
      END_DEVICE_PATH_TYPE,
      END_ENTIRE_DEVICE_PATH_SUBTYPE,
      { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
    },
  };

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  AnsExitBootServices,
                  Device,
                  &gEfiEventExitBootServicesGuid,
                  &Device->ExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Device->Handle,
                  &gEfiBlockIoProtocolGuid,
                  &Device->BlockIo,
                  &gEfiDevicePathProtocolGuid,
                  &Device->DevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  DEBUG ((
    DEBUG_INFO,
    "AppleANS: namespace 1 ready: %Lu blocks x %u bytes\n",
    Device->BlockDevice.media.block_count,
    Device->BlockDevice.media.block_size
    ));
  return EFI_SUCCESS;

Fail:
  if (Device->ExitBootServicesEvent != NULL) {
    gBS->CloseEvent (Device->ExitBootServicesEvent);
  }
  if (Device->Controller.enabled) {
    ntasi_ans_controller_stop (&Device->Controller);
  }
  if (Device->Rtkit.booted) {
    ntasi_rtkit_runtime_sleep (&Device->Rtkit);
  }
  ntasi_rtkit_runtime_release_buffers (&Device->Rtkit);
  ntasi_sart_runtime_clear_owned (&Device->Sart);
  FreeControllerMemory (Device);
  FreePool (Device);
  mAns = NULL;
  return Status;
}

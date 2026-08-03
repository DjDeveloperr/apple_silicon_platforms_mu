/** @file
  Apple ANS NVMe DXE driver.

  SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#if !defined (APPLE_ANS_QEMU_TEST)
#include <Library/AppleDTLib.h>
#endif
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#if !defined (APPLE_ANS_QEMU_TEST)
#include <Library/DebugLib.h>
#else
#include <Library/DxeServicesTableLib.h>
#endif
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Drivers/AppleAnsHardware.h>
#if !defined (APPLE_ANS_QEMU_TEST)
#include <Drivers/AppleAnsPmgrDomain.h>
#include <Drivers/NtasiMemoryMapDump.h>
#endif

#include "Shared/AppleAscCore.h"
#include "Shared/AppleNvmeBlockCore.h"
#include "Shared/AppleNvmeControllerCore.h"
#include "Shared/AppleRtkitRuntimeCore.h"
#include "Shared/AppleSartRuntimeCore.h"

#define APPLE_ANS_MAILBOX_OFFSET  0x8000u
#define APPLE_ANS_NAMESPACE_ID    1u
#define APPLE_ANS_POLL_LIMIT      2000000u
#define APPLE_ANS_MAX_GPT_PARTITIONS  32u
#define APPLE_ANS_MAX_TRACED_ERRORS   8u
#define APPLE_ANS_MAX_BCD_BYTES       0x100000u
#define APPLE_ANS_MAX_SART_RESERVATIONS  64u

#if defined (APPLE_ANS_QEMU_TEST)
#define APPLE_ANS_QEMU_ASC_SIZE   0x9000u
#define APPLE_ANS_QEMU_NVME_SIZE  0x30000u
#define APPLE_ANS_QEMU_SART_SIZE  0x1000u
#endif

#if defined (APPLE_ANS_QEMU_TEST)
#define ANS_DEBUG(Expression)  do { } while (FALSE)
#else
#define ANS_DEBUG(Expression)  DEBUG (Expression)
#endif

#if defined (APPLE_ANS_QEMU_TEST)
STATIC EFI_STATUS
MapQemuMmio (
  IN EFI_PHYSICAL_ADDRESS Base,
  IN UINT64               Length
  )
{
  EFI_STATUS  Status;

  Status = gDS->AddMemorySpace (
                  EfiGcdMemoryTypeMemoryMappedIo,
                  Base,
                  Length,
                  EFI_MEMORY_UC | EFI_MEMORY_XP
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gDS->SetMemorySpaceAttributes (
                Base,
                Length,
                EFI_MEMORY_UC | EFI_MEMORY_XP
                );
}

STATIC EFI_STATUS
MapQemuHardware (
  IN EFI_PHYSICAL_ADDRESS CpuBase,
  IN EFI_PHYSICAL_ADDRESS NvmeBase,
  IN EFI_PHYSICAL_ADDRESS SartBase
  )
{
  EFI_STATUS  Status;

  Status = MapQemuMmio (CpuBase, APPLE_ANS_QEMU_ASC_SIZE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = MapQemuMmio (NvmeBase, APPLE_ANS_QEMU_NVME_SIZE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return MapQemuMmio (SartBase, APPLE_ANS_QEMU_SART_SIZE);
}
#endif

typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} APPLE_ANS_DEVICE_PATH;

//
// One page allocation owned by this driver.
//
// RawBase/RawPages are EXACTLY what was handed back by gBS->AllocatePages and
// are the only pair ever passed to gBS->FreePages, so every free this driver
// performs is a whole-allocation free. Base/Size are the aligned, usable
// window inside it. See AnsAllocatePages() for why partial frees are banned
// here.
//
typedef struct {
  EFI_PHYSICAL_ADDRESS  RawBase;
  UINTN                 RawPages;
  VOID                  *Base;
  UINTN                 Size;
} APPLE_ANS_PAGE_ALLOCATION;

typedef struct {
  EFI_HANDLE  Handle;
  UINT32    PartitionNumber;
  EFI_LBA   StartingLba;
  EFI_LBA   EndingLba;
  EFI_GUID  PartitionTypeGuid;
  EFI_GUID  UniquePartitionGuid;
  CHAR16    PartitionName[37];
  BOOLEAN   SystemPartition;
} APPLE_ANS_GPT_PARTITION;

typedef struct {
  EFI_PHYSICAL_ADDRESS  Base;
  EFI_PHYSICAL_ADDRESS  End;
} APPLE_ANS_PHYSICAL_RANGE;

//
// 4 RTKit shared buffers (crashlog/syslog/ioreport/oslog) + 6 queue regions +
// 1 bounce buffer = 11. Sized with headroom; AnsAllocatePages() fails cleanly
// rather than overflowing it.
//
#define APPLE_ANS_MAX_ALLOCATIONS  16u

typedef struct {
  EFI_HANDLE                        Handle;
  EFI_EVENT                         ExitBootServicesEvent;
  EFI_EVENT                         ReadyToBootEvent;
  EFI_EVENT                         PartitionInfoEvent;
  EFI_EVENT                         SimpleFileSystemEvent;
  VOID                              *PartitionInfoRegistration;
  VOID                              *SimpleFileSystemRegistration;
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
  APPLE_ANS_PAGE_ALLOCATION         Allocations[APPLE_ANS_MAX_ALLOCATIONS];
  UINTN                             AllocationCount;
  EFI_BLOCK_IO_MEDIA                Media;
  EFI_BLOCK_IO_PROTOCOL             BlockIo;
  APPLE_ANS_DEVICE_PATH             DevicePath;
  APPLE_ANS_GPT_PARTITION           GptPartitions[APPLE_ANS_MAX_GPT_PARTITIONS];
  UINTN                             GptPartitionCount;
  UINT32                            PartitionReadSeen;
  UINT32                            TracedReadErrors;
  BOOLEAN                           ReadAttributionArmed;
  BOOLEAN                           ReadyToBootDiagnosticsComplete;
  BOOLEAN                           InheritedCoprocessor;
  BOOLEAN                           InheritedSartMemoryReserved;
  BOOLEAN                           Fatal;
  BOOLEAN                           HandedOff;
} APPLE_ANS_DEVICE;

/**
  Page allocator for everything this driver owns.

  WHY THIS EXISTS INSTEAD OF MemoryAllocationLib's AllocateAlignedPages() /
  AllocateAlignedReservedPages(), 2026-07-30 hardware failure:

    AppleANS: stage "rtkit-boot" (bounded at 2000000 polls per wait)
    ASSERT_EFI_ERROR (Status = Invalid Parameter)
    ASSERT [AppleNANDStorageDxe] MemoryAllocationLib.c(222): ...

  MemoryAllocationLib.c:222 is NOT the ASSERT after gBS->AllocatePages (that
  is line 200, and it returns NULL on error rather than asserting). It is the
  ASSERT_EFI_ERROR after the gBS->FreePages on line 221 -- the free of the
  TRAILING slack pages in InternalAllocateAlignedPages(). The allocation
  itself succeeded. The chain:

    1. AllocateAlignedReservedPages(Pages, 0x4000) over-allocates
       RealPages = Pages + 4 of EfiReservedMemoryType.
    2. CoreInternalAllocatePages (MdeModulePkg/Core/Dxe/Mem/Page.c) uses
       RUNTIME_PAGE_ALLOCATION_GRANULARITY for EfiReservedMemoryType, which is
       0x10000 on AARCH64 (MdePkg/Include/AArch64/ProcessorBind.h:169 --
       __DEPRECATED_AARCH64_4K_RUNTIME_GRANULARITY is NOT defined in this
       build). So it returns a 64 KiB-aligned address.
    3. The lib aligns up to 0x4000 -- already satisfied -- so it skips the
       LEADING free, then frees the trailing slack at
       AlignedMemory + EFI_PAGES_TO_SIZE(Pages). For a 16 KiB buffer that is
       base + 0x4000: 16 KiB-aligned, but NOT 64 KiB-aligned.
    4. CoreInternalFreePages sees Entry->Type == EfiReservedMemoryType, sets
       Alignment = 0x10000, and returns EFI_INVALID_PARAMETER at Page.c:1938
       because (Memory & 0xFFFF) != 0.
    5. ASSERT_EFI_ERROR at MemoryAllocationLib.c:222 kills the boot.

  AllocateAlignedReservedPages() is therefore structurally unusable on AARCH64
  for ANY alignment below 64 KiB: the trailing partial free can never satisfy
  the reserved-memory free granularity. It is not a bad size or a bad memory
  type -- it is the wrong API for this memory type on this architecture.

  This allocator makes that class of failure impossible rather than avoiding
  one instance of it:

    * It NEVER performs a partial free. RawBase/RawPages are recorded and the
      only free ever issued is the whole allocation, which is always legal at
      whatever granularity the core applied.
    * Alignment is raised to at least the memory type's own free granularity,
      so the base is guaranteed to be a legal free address.
    * Every status is checked. No ASSERT, no ASSERT_EFI_ERROR, no code path
      that can abort the boot. An allocation failure aborts ANS bring-up and
      nothing else -- this is an optional storage coprocessor on a machine
      that boots Windows from USB.
    * The fast path allocates exactly the pages needed and only falls back to
      an over-allocate-and-align if the core hands back a misaligned base,
      which for reserved memory it never will.

  @param[in]      Device      Owning device; the allocation is recorded in its
                              table so nothing can leak or be double-freed.
  @param[in]      Purpose     Short label for the log line.
  @param[in]      MemoryType  EFI memory type to allocate.
  @param[in]      Size        Requested size in bytes.
  @param[in]      Alignment   Required alignment, a power of two.
  @param[out]     Allocation  Receives the recorded allocation on success.

  @retval EFI_SUCCESS            Allocated; *Allocation is valid.
  @retval EFI_INVALID_PARAMETER  Bad size/alignment (logged).
  @retval EFI_OUT_OF_RESOURCES   Allocation failed or the table is full
                                 (logged).
**/
STATIC
EFI_STATUS
AnsAllocatePages (
  IN OUT APPLE_ANS_DEVICE           *Device,
  IN     CONST CHAR8                *Purpose,
  IN     EFI_MEMORY_TYPE            MemoryType,
  IN     UINTN                      Size,
  IN     UINTN                      Alignment,
  OUT    APPLE_ANS_PAGE_ALLOCATION  **Allocation
  )
{
  EFI_STATUS                 Status;
  EFI_PHYSICAL_ADDRESS       Raw;
  EFI_PHYSICAL_ADDRESS       Aligned;
  UINTN                      Granularity;
  UINTN                      Effective;
  UINTN                      UsableSize;
  UINTN                      UsablePages;
  UINTN                      RawPages;
  APPLE_ANS_PAGE_ALLOCATION  *Record;

  *Allocation = NULL;

  //
  // Free granularity for this memory type, mirroring CoreInternalFreePages()
  // exactly. Raising the requested alignment to at least this value is what
  // guarantees the recorded base is always a legal FreePages address.
  //
  if ((MemoryType == EfiReservedMemoryType) ||
      (MemoryType == EfiACPIMemoryNVS) ||
      (MemoryType == EfiRuntimeServicesCode) ||
      (MemoryType == EfiRuntimeServicesData))
  {
    Granularity = RUNTIME_PAGE_ALLOCATION_GRANULARITY;
  } else {
    Granularity = DEFAULT_PAGE_ALLOCATION_GRANULARITY;
  }

  Effective = (Alignment > Granularity) ? Alignment : Granularity;

  if ((Size == 0) || (Effective == 0) || ((Effective & (Effective - 1)) != 0)) {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: alloc \"%a\": refusing size=0x%Lx alignment=0x%Lx (must be nonzero, alignment a power of two)\n",
      Purpose,
      (UINT64)Size,
      (UINT64)Alignment
      ));
    return EFI_INVALID_PARAMETER;
  }

  if (Size > MAX_UINTN - (Effective - 1)) {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: alloc \"%a\": size 0x%Lx overflows when aligned up to 0x%Lx\n",
      Purpose,
      (UINT64)Size,
      (UINT64)Effective
      ));
    return EFI_INVALID_PARAMETER;
  }

  UsableSize  = ALIGN_VALUE (Size, Effective);
  UsablePages = EFI_SIZE_TO_PAGES (UsableSize);
  if (UsablePages == 0) {
    ANS_DEBUG ((DEBUG_ERROR, "AppleANS: alloc \"%a\": computed zero pages for size 0x%Lx\n", Purpose, (UINT64)Size));
    return EFI_INVALID_PARAMETER;
  }

  if (Device->AllocationCount >= APPLE_ANS_MAX_ALLOCATIONS) {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: alloc \"%a\": allocation table full (%Lu entries); ANS bring-up aborted\n",
      Purpose,
      (UINT64)Device->AllocationCount
      ));
    return EFI_OUT_OF_RESOURCES;
  }

  ANS_DEBUG ((
    DEBUG_INFO,
    "AppleANS: alloc \"%a\": requested=0x%Lx usable=0x%Lx pages=%Lu type=%Lu alignment=0x%Lx granularity=0x%Lx\n",
    Purpose,
    (UINT64)Size,
    (UINT64)UsableSize,
    (UINT64)UsablePages,
    (UINT64)MemoryType,
    (UINT64)Effective,
    (UINT64)Granularity
    ));

  //
  // Fast path: ask for exactly what is needed. For any type whose granularity
  // already meets or exceeds the requested alignment -- which is every
  // reserved allocation on AARCH64 -- the core's own alignment guarantee
  // satisfies us with zero slack.
  //
  Raw      = 0;
  RawPages = UsablePages;
  Status   = gBS->AllocatePages (AllocateAnyPages, MemoryType, RawPages, &Raw);
  if (!EFI_ERROR (Status) && ((Raw & (Effective - 1)) != 0)) {
    //
    // Core handed back a base that does not meet our alignment. Give it back
    // whole (always legal) and retry with one alignment unit of slack.
    //
    EFI_STATUS  FreeStatus;

    FreeStatus = gBS->FreePages (Raw, RawPages);
    if (EFI_ERROR (FreeStatus)) {
      ANS_DEBUG ((
        DEBUG_WARN,
        "AppleANS: alloc \"%a\": could not return misaligned block 0x%Lx/%Lu pages: %r (leaked, continuing)\n",
        Purpose,
        (UINT64)Raw,
        (UINT64)RawPages,
        FreeStatus
        ));
    }

    Raw      = 0;
    RawPages = UsablePages + EFI_SIZE_TO_PAGES (Effective);
    Status   = gBS->AllocatePages (AllocateAnyPages, MemoryType, RawPages, &Raw);
  }

  if (EFI_ERROR (Status)) {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: alloc \"%a\": gBS->AllocatePages(AllocateAnyPages, type=%Lu, pages=%Lu) failed: %r; ANS bring-up aborted, boot continues\n",
      Purpose,
      (UINT64)MemoryType,
      (UINT64)RawPages,
      Status
      ));
    return EFI_OUT_OF_RESOURCES;
  }

  Aligned = ALIGN_VALUE (Raw, (EFI_PHYSICAL_ADDRESS)Effective);
  if ((Aligned < Raw) ||
      ((Aligned - Raw) + UsableSize > EFI_PAGES_TO_SIZE (RawPages)))
  {
    //
    // Cannot happen with the slack computed above, but never trust arithmetic
    // that decides where a DMA-capable coprocessor may write.
    //
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: alloc \"%a\": 0x%Lx/%Lu pages cannot hold 0x%Lx aligned to 0x%Lx; releasing and aborting ANS bring-up\n",
      Purpose,
      (UINT64)Raw,
      (UINT64)RawPages,
      (UINT64)UsableSize,
      (UINT64)Effective
      ));
    gBS->FreePages (Raw, RawPages);
    return EFI_OUT_OF_RESOURCES;
  }

  Record = &Device->Allocations[Device->AllocationCount];
  Device->AllocationCount++;
  Record->RawBase  = Raw;
  Record->RawPages = RawPages;
  Record->Base     = (VOID *)(UINTN)Aligned;
  Record->Size     = UsableSize;
  ZeroMem (Record->Base, Record->Size);

  ANS_DEBUG ((
    DEBUG_INFO,
    "AppleANS: alloc \"%a\": base=0x%Lx size=0x%Lx (raw 0x%Lx/%Lu pages)\n",
    Purpose,
    (UINT64)Aligned,
    (UINT64)UsableSize,
    (UINT64)Raw,
    (UINT64)RawPages
    ));

  *Allocation = Record;
  return EFI_SUCCESS;
}

/**
  Release one allocation previously made by AnsAllocatePages(), identified by
  its aligned base. Whole-allocation free only; status checked, never
  asserted. A failure is logged and the allocation is dropped from the table
  (leaked) rather than retried -- at this point the alternative is an assert.
**/
STATIC
VOID
AnsFreePagesByBase (
  IN OUT APPLE_ANS_DEVICE  *Device,
  IN     CONST CHAR8       *Purpose,
  IN     VOID              *Base
  )
{
  UINTN       Index;
  EFI_STATUS  Status;

  if (Base == NULL) {
    return;
  }

  for (Index = 0; Index < Device->AllocationCount; Index++) {
    if (Device->Allocations[Index].Base != Base) {
      continue;
    }

    Status = gBS->FreePages (
                    Device->Allocations[Index].RawBase,
                    Device->Allocations[Index].RawPages
                    );
    if (EFI_ERROR (Status)) {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: free \"%a\": gBS->FreePages(0x%Lx, %Lu) failed: %r (leaked, boot continues)\n",
        Purpose,
        (UINT64)Device->Allocations[Index].RawBase,
        (UINT64)Device->Allocations[Index].RawPages,
        Status
        ));
    }

    Device->AllocationCount--;
    Device->Allocations[Index] = Device->Allocations[Device->AllocationCount];
    ZeroMem (&Device->Allocations[Device->AllocationCount], sizeof (Device->Allocations[0]));
    return;
  }

  ANS_DEBUG ((DEBUG_WARN, "AppleANS: free \"%a\": 0x%lx is not a tracked allocation\n", Purpose, (UINTN)Base));
}

/**
  Release every allocation still recorded. Whole-allocation frees only,
  status checked, never asserted.
**/
STATIC
VOID
AnsFreeAllPages (
  IN OUT APPLE_ANS_DEVICE  *Device
  )
{
  EFI_STATUS  Status;

  while (Device->AllocationCount > 0) {
    Device->AllocationCount--;
    Status = gBS->FreePages (
                    Device->Allocations[Device->AllocationCount].RawBase,
                    Device->Allocations[Device->AllocationCount].RawPages
                    );
    if (EFI_ERROR (Status)) {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: free-all: gBS->FreePages(0x%Lx, %Lu) failed: %r (leaked, boot continues)\n",
        (UINT64)Device->Allocations[Device->AllocationCount].RawBase,
        (UINT64)Device->Allocations[Device->AllocationCount].RawPages,
        Status
        ));
    }

    ZeroMem (&Device->Allocations[Device->AllocationCount], sizeof (Device->Allocations[0]));
  }
}

STATIC APPLE_ANS_DEVICE  *mAns;

#if !defined (APPLE_ANS_QEMU_TEST)
STATIC VOID
DumpSartState (
  IN APPLE_ANS_DEVICE  *Device,
  IN CONST CHAR8       *When
  );

STATIC VOID
ReportAnsPmgrDomains (
  VOID
  );
#endif

STATIC CONST EFI_GUID  mAppleAnsDevicePathGuid = {
  0x171cfd4c, 0x628f, 0x4d87,
  { 0xa8, 0x55, 0x20, 0x57, 0x04, 0x15, 0x21, 0x10 }
};

#if !defined (APPLE_ANS_QEMU_TEST)
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
#endif

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
  MicroSecondDelay (1);
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

#if !defined (APPLE_ANS_QEMU_TEST)
STATIC BOOLEAN
AnsMemoryTypePersistsIntoOs (
  IN EFI_MEMORY_TYPE  Type
  )
{
  return (Type == EfiReservedMemoryType) ||
         (Type == EfiUnusableMemory) ||
         (Type == EfiRuntimeServicesCode) ||
         (Type == EfiRuntimeServicesData) ||
         (Type == EfiACPIMemoryNVS) ||
         (Type == EfiPalCode);
}

STATIC EFI_STATUS
AnsGetMemoryMapSnapshot (
  OUT EFI_MEMORY_DESCRIPTOR  **MemoryMap,
  OUT UINTN                  *MemoryMapSize,
  OUT UINTN                  *DescriptorSize
  )
{
  EFI_STATUS  Status;
  UINTN       MapKey;
  UINT32      DescriptorVersion;
  UINTN       Attempt;

  *MemoryMap      = NULL;
  *MemoryMapSize  = 0;
  *DescriptorSize = 0;

  Status = gBS->GetMemoryMap (
                  MemoryMapSize,
                  NULL,
                  &MapKey,
                  DescriptorSize,
                  &DescriptorVersion
                  );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return Status;
  }

  for (Attempt = 0; Attempt < 4; Attempt++) {
    EFI_MEMORY_DESCRIPTOR  *Map;
    UINTN                  Capacity;

    if ((*DescriptorSize == 0) ||
        (*MemoryMapSize > MAX_UINTN - (2 * *DescriptorSize)))
    {
      return EFI_OUT_OF_RESOURCES;
    }

    Capacity = *MemoryMapSize + (2 * *DescriptorSize);
    Map      = AllocatePool (Capacity);
    if (Map == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    *MemoryMapSize = Capacity;
    Status         = gBS->GetMemoryMap (
                            MemoryMapSize,
                            Map,
                            &MapKey,
                            DescriptorSize,
                            &DescriptorVersion
                            );
    if (!EFI_ERROR (Status)) {
      *MemoryMap = Map;
      return EFI_SUCCESS;
    }

    FreePool (Map);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      return Status;
    }
  }

  return EFI_OUT_OF_RESOURCES;
}

/**
  Reserve every inherited SART grant from the OS memory map before attaching
  to a live ANS coprocessor.

  A live handoff cannot merely preserve the SART register entries.  The five
  entries measured from iBoot on J414s point at ordinary DRAM, and without a
  matching UEFI reservation Windows is free to reuse those pages while the
  still-running coprocessor retains DMA permission to them.  Mu-owned RTKit
  and queue buffers already use EfiReservedMemoryType; this closes the same
  ownership gap for buffers allocated before Mu began executing.

  Ranges are rounded to the AArch64 reserved-memory granularity and merged.
  Conventional-RAM intersections are converted to EfiReservedMemoryType;
  persistent memory and addresses absent from the UEFI memory map are already
  unavailable to Windows and need no conversion.  Any intersection with a
  reclaimable non-conventional allocation fails closed: the EBS callback will
  take the cold stop-and-revoke path instead of preserving a live DMA master.
**/
STATIC EFI_STATUS
AnsReserveInheritedSartMemory (
  IN OUT APPLE_ANS_DEVICE  *Device
  )
{
  APPLE_ANS_PHYSICAL_RANGE  Ranges[NTASI_SART_MAX_ENTRIES];
  APPLE_ANS_PHYSICAL_RANGE  Reservations[APPLE_ANS_MAX_SART_RESERVATIONS];
  EFI_MEMORY_DESCRIPTOR     *MemoryMap;
  UINTN                     MemoryMapSize;
  UINTN                     DescriptorSize;
  UINTN                     RangeCount;
  UINTN                     Index;
  UINTN                     Move;
  UINTN                     ReservationCount;
  EFI_STATUS                Status;

  RangeCount = 0;
  for (Index = 0; Index < NTASI_SART_MAX_ENTRIES; Index++) {
    EFI_PHYSICAL_ADDRESS  Base;
    EFI_PHYSICAL_ADDRESS  End;
    UINT64                Paddr;
    UINT64                Size;
    UINT64                Granularity;
    uint8_t               Flags;
    int                   Result;

    if ((Device->Sart.protected_entries & (1u << Index)) == 0) {
      continue;
    }

    Flags  = 0;
    Paddr  = 0;
    Size   = 0;
    Result = ntasi_sart_runtime_read (
               &Device->Sart,
               (unsigned int)Index,
               &Flags,
               &Paddr,
               &Size
               );
    Granularity = RUNTIME_PAGE_ALLOCATION_GRANULARITY;
    if ((Result != NTASI_SART_RUNTIME_OK) || (Flags == 0) || (Size == 0) ||
        (Granularity == 0) || ((Granularity & (Granularity - 1)) != 0) ||
        (Paddr > MAX_UINT64 - Size))
    {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: inherited SART[%Lu] cannot be reserved safely: result=%d flags=0x%x paddr=0x%Lx size=0x%Lx\n",
        (UINT64)Index,
        Result,
        (UINT32)Flags,
        Paddr,
        Size
        ));
      return EFI_DEVICE_ERROR;
    }

    Base = Paddr & ~(Granularity - 1);
    End  = Paddr + Size;
    if (End > MAX_UINT64 - (Granularity - 1)) {
      return EFI_DEVICE_ERROR;
    }

    End = ALIGN_VALUE (End, Granularity);
    Ranges[RangeCount].Base = Base;
    Ranges[RangeCount].End  = End;
    RangeCount++;
  }

  for (Index = 1; Index < RangeCount; Index++) {
    APPLE_ANS_PHYSICAL_RANGE  Range;
    UINTN                     Position;

    Range    = Ranges[Index];
    Position = Index;
    while ((Position > 0) && (Ranges[Position - 1].Base > Range.Base)) {
      Ranges[Position] = Ranges[Position - 1];
      Position--;
    }

    Ranges[Position] = Range;
  }

  for (Index = 1; Index < RangeCount; ) {
    if (Ranges[Index].Base <= Ranges[Index - 1].End) {
      if (Ranges[Index].End > Ranges[Index - 1].End) {
        Ranges[Index - 1].End = Ranges[Index].End;
      }

      for (Move = Index; Move + 1 < RangeCount; Move++) {
        Ranges[Move] = Ranges[Move + 1];
      }
      RangeCount--;
      continue;
    }

    Index++;
  }

  MemoryMap      = NULL;
  MemoryMapSize  = 0;
  DescriptorSize = 0;
  Status = AnsGetMemoryMapSnapshot (
             &MemoryMap,
             &MemoryMapSize,
             &DescriptorSize
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ReservationCount = 0;
  Status           = EFI_SUCCESS;
  for (Index = 0; Index < RangeCount; Index++) {
    UINT64  CoveredBytes;
    UINTN   Offset;

    CoveredBytes = 0;
    for (Offset = 0; Offset < MemoryMapSize; Offset += DescriptorSize) {
      CONST EFI_MEMORY_DESCRIPTOR  *Descriptor;
      EFI_PHYSICAL_ADDRESS         DescriptorEnd;
      EFI_PHYSICAL_ADDRESS         IntersectionBase;
      EFI_PHYSICAL_ADDRESS         IntersectionEnd;

      Descriptor = (CONST EFI_MEMORY_DESCRIPTOR *)((CONST UINT8 *)MemoryMap + Offset);
      if (Descriptor->NumberOfPages >
          RShiftU64 (MAX_UINT64 - Descriptor->PhysicalStart, EFI_PAGE_SHIFT))
      {
        continue;
      }

      DescriptorEnd = Descriptor->PhysicalStart + EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages);
      IntersectionBase = (Descriptor->PhysicalStart > Ranges[Index].Base) ?
                         Descriptor->PhysicalStart : Ranges[Index].Base;
      IntersectionEnd = (DescriptorEnd < Ranges[Index].End) ?
                        DescriptorEnd : Ranges[Index].End;
      if (IntersectionBase >= IntersectionEnd) {
        continue;
      }

      CoveredBytes += IntersectionEnd - IntersectionBase;
      if (AnsMemoryTypePersistsIntoOs ((EFI_MEMORY_TYPE)Descriptor->Type)) {
        continue;
      }

      if (Descriptor->Type != EfiConventionalMemory) {
        ANS_DEBUG ((
          DEBUG_ERROR,
          "AppleANS: inherited SART range 0x%Lx..0x%Lx intersects reclaimable memory type %u at 0x%Lx..0x%Lx\n",
          (UINT64)Ranges[Index].Base,
          (UINT64)Ranges[Index].End,
          (UINT32)Descriptor->Type,
          (UINT64)IntersectionBase,
          (UINT64)IntersectionEnd
          ));
        Status = EFI_ACCESS_DENIED;
        break;
      }

      if (ReservationCount >= ARRAY_SIZE (Reservations)) {
        Status = EFI_OUT_OF_RESOURCES;
        break;
      }

      Reservations[ReservationCount].Base = IntersectionBase;
      Reservations[ReservationCount].End  = IntersectionEnd;
      ReservationCount++;
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    if (CoveredBytes < (Ranges[Index].End - Ranges[Index].Base)) {
      ANS_DEBUG ((
        DEBUG_INFO,
        "AppleANS: inherited SART range 0x%Lx..0x%Lx includes 0x%Lx byte(s) outside the UEFI RAM map; those addresses are already unavailable to Windows\n",
        (UINT64)Ranges[Index].Base,
        (UINT64)Ranges[Index].End,
        (UINT64)((Ranges[Index].End - Ranges[Index].Base) - CoveredBytes)
        ));
    }
  }

  FreePool (MemoryMap);
  if (EFI_ERROR (Status)) {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: inherited SART memory reservation failed at range %Lu/%Lu (%r); live RTKit handoff is DISABLED and EBS will stop ANS and revoke all grants\n",
      (UINT64)Index,
      (UINT64)RangeCount,
      Status
      ));
    return Status;
  }

  for (Index = 1; Index < ReservationCount; Index++) {
    APPLE_ANS_PHYSICAL_RANGE  Range;
    UINTN                     Position;

    Range    = Reservations[Index];
    Position = Index;
    while ((Position > 0) && (Reservations[Position - 1].Base > Range.Base)) {
      Reservations[Position] = Reservations[Position - 1];
      Position--;
    }

    Reservations[Position] = Range;
  }

  for (Index = 1; Index < ReservationCount; ) {
    if (Reservations[Index].Base <= Reservations[Index - 1].End) {
      if (Reservations[Index].End > Reservations[Index - 1].End) {
        Reservations[Index - 1].End = Reservations[Index].End;
      }

      for (Move = Index; Move + 1 < ReservationCount; Move++) {
        Reservations[Move] = Reservations[Move + 1];
      }
      ReservationCount--;
      continue;
    }

    Index++;
  }

  for (Index = 0; Index < ReservationCount; Index++) {
    EFI_PHYSICAL_ADDRESS  Base;
    UINTN                 Pages;

    Base  = Reservations[Index].Base;
    Pages = EFI_SIZE_TO_PAGES (Reservations[Index].End - Reservations[Index].Base);
    Status = gBS->AllocatePages (
                    AllocateAddress,
                    EfiReservedMemoryType,
                    Pages,
                    &Base
                    );
    if (EFI_ERROR (Status) || (Base != Reservations[Index].Base)) {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: inherited SART RAM reservation failed at segment %Lu/%Lu 0x%Lx..0x%Lx (%r)\n",
        (UINT64)Index,
        (UINT64)ReservationCount,
        (UINT64)Reservations[Index].Base,
        (UINT64)Reservations[Index].End,
        Status
        ));
      return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    }

    ANS_DEBUG ((
      DEBUG_INFO,
      "AppleANS: inherited SART RAM RESERVED for live OS handoff: 0x%Lx..0x%Lx (%Lu pages)\n",
      (UINT64)Reservations[Index].Base,
      (UINT64)Reservations[Index].End,
      (UINT64)Pages
      ));
  }

  ANS_DEBUG ((
    DEBUG_INFO,
    "AppleANS: inherited SART ownership sealed: %Lu merged grant range(s), %Lu RAM reservation(s), all non-reclaimable across ExitBootServices\n",
    (UINT64)RangeCount,
    (UINT64)ReservationCount
    ));
  return EFI_SUCCESS;
}
#endif

//
// RTKit shared-buffer allocator.
//
// Two deliberate departures from the original implementation, both taken
// straight from m1n1:
//
//   * 16 KiB granularity, not 4 KiB. m1n1's rtkit_alloc_buffer()/rtkit_map()
//     do memalign(SZ_16K, ...) and ALIGN_UP(sz, 16384) before calling
//     sart_add_allowed_region(). SART accepts 4 KiB granularity, but 16 KiB
//     is the CPU page size here, so a 4 KiB grant hands the coprocessor DMA
//     rights over part of a CPU page the AP also owns.
//
//   * EfiReservedMemoryType, not EfiBootServicesData. m1n1's own
//     rtkit_set_buffer_pool() comment states the requirement plainly: "the
//     pool region must be reserved out of that OS's memory map ... whenever
//     the IOP keeps running into the next OS". Boot-services memory is
//     handed straight back to Windows at ExitBootServices, so an ANS that is
//     still alive -- or that quiesces less than perfectly -- would be
//     writing into memory Windows has already reallocated. Reserved memory
//     survives the handoff, which is why the ExitBootServices path below can
//     safely leave these buffers in place instead of freeing them.
//
// Buffer->size is set to the MAPPED size, not the requested size. It used to
// carry the requested size while the SART grant covered the rounded-up size,
// so ReleaseRtkitShared()'s ntasi_sart_runtime_remove() could look for a
// (paddr, size) pair that was never programmed -- an exact-match lookup that
// silently failed and stranded a SART entry. There are only 16 entries on
// this silicon (minus whatever iBoot left armed), so leaking them is not
// harmless.
//
STATIC int
AllocateRtkitShared (
  IN VOID                              *Opaque,
  IN uint8_t                           Endpoint,
  IN size_t                            Size,
  OUT struct ntasi_rtkit_shared_buffer *Buffer
  )
{
  APPLE_ANS_DEVICE           *Device = Opaque;
  APPLE_ANS_PAGE_ALLOCATION  *Allocation;
  CHAR8                      Purpose[32];
  EFI_STATUS                 Status;
  int                        Result;

  AsciiSPrint (Purpose, sizeof (Purpose), "rtkit-ep-0x%x", (UINT32)Endpoint);

  //
  // EfiReservedMemoryType, not EfiBootServicesData: m1n1's own
  // rtkit_set_buffer_pool() states the requirement outright -- "the pool
  // region must be reserved out of that OS's memory map ... whenever the IOP
  // keeps running into the next OS". Boot-services memory goes straight back
  // to Windows at ExitBootServices.
  //
  // The alignment asked for is NTASI_RTKIT_SHARED_ALIGN (16 KiB, matching
  // m1n1's memalign(SZ_16K)); AnsAllocatePages() raises it to the reserved
  // free granularity (64 KiB on AARCH64) so the base is always a legal
  // FreePages address, and never performs the partial free that made
  // AllocateAlignedReservedPages() assert here on 2026-07-30.
  //
  Status = AnsAllocatePages (
             Device,
             Purpose,
             EfiReservedMemoryType,
             Size,
             NTASI_RTKIT_SHARED_ALIGN,
             &Allocation
             );
  if (EFI_ERROR (Status)) {
    // AnsAllocatePages already logged the specifics.
    return -1;
  }

  //
  // Grant exactly what was allocated, so the matching
  // ntasi_sart_runtime_remove() looks for the same (paddr, size) pair that
  // was programmed. Granting less than we own would leave the tail
  // unreachable; granting more would expose memory we do not own.
  //
  Result = ntasi_sart_runtime_add (
             &Device->Sart,
             (UINT64)(UINTN)Allocation->Base,
             Allocation->Size,
             NULL
             );
  if (Result != 0) {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: RTKit endpoint 0x%x: SART grant for 0x%lx/+0x%Lx failed: %d "
      "(only 16 SART entries exist; iBoot may hold some); ANS bring-up aborted, boot continues\n",
      (UINT32)Endpoint,
      (UINTN)Allocation->Base,
      (UINT64)Allocation->Size,
      Result
      ));
    AnsFreePagesByBase (Device, Purpose, Allocation->Base);
    return Result;
  }

  Buffer->cpu_address    = Allocation->Base;
  Buffer->device_address = (UINT64)(UINTN)Allocation->Base;
  Buffer->size           = Allocation->Size;
  Buffer->iop_owned      = false;
  ANS_DEBUG ((
    DEBUG_INFO,
    "AppleANS: RTKit endpoint 0x%x: granted 0x%lx/+0x%Lx (reserved, SART)\n",
    (UINT32)Endpoint,
    (UINTN)Allocation->Base,
    (UINT64)Allocation->Size
    ));
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
  CHAR8             Purpose[32];

  if (Buffer->iop_owned || (Buffer->cpu_address == NULL)) {
    return;
  }

  AsciiSPrint (Purpose, sizeof (Purpose), "rtkit-ep-0x%x", (UINT32)Endpoint);
  ntasi_sart_runtime_remove (
    &Device->Sart,
    Buffer->device_address,
    Buffer->size
    );
  AnsFreePagesByBase (Device, Purpose, Buffer->cpu_address);
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
  ANS_DEBUG ((DEBUG_ERROR, "AppleANS: RTKit firmware crashed\n"));
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

  // Keep every controller poll budget in microseconds.  ANS firmware can
  // legitimately take hundreds of milliseconds to change state on a cold
  // boot, and a tight CpuPause loop made the nominal two-second budget depend
  // on the host CPU generation.
  MicroSecondDelay (1);
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
  UINTN       PartitionIndex;

  Status = ValidateBlockRequest (This, MediaId, Lba, BufferSize, Buffer);
  if (EFI_ERROR (Status) || (BufferSize == 0)) {
    return Status;
  }

  Blocks = BufferSize / This->Media->BlockSize;
  Status = MapBlockStatus (
             ntasi_ans_block_read (&mAns->BlockDevice, Lba, Blocks, Buffer, BufferSize)
             );
  if (!mAns->ReadAttributionArmed) {
    return Status;
  }

  if (EFI_ERROR (Status)) {
    if (mAns->TracedReadErrors < APPLE_ANS_MAX_TRACED_ERRORS) {
      mAns->TracedReadErrors++;
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS ReadyToBoot I/O: read failed LBA=0x%Lx blocks=0x%Lx status=%r (bounded error %u/%u)\n",
        (UINT64)Lba,
        (UINT64)Blocks,
        Status,
        mAns->TracedReadErrors,
        APPLE_ANS_MAX_TRACED_ERRORS
        ));
    }

    return Status;
  }

  for (PartitionIndex = 0;
       PartitionIndex < mAns->GptPartitionCount;
       PartitionIndex++)
  {
    APPLE_ANS_GPT_PARTITION  *Partition;
    UINT32                   PartitionBit;

    Partition    = &mAns->GptPartitions[PartitionIndex];
    PartitionBit = (UINT32)(1u << PartitionIndex);
    if ((Lba < Partition->StartingLba) ||
        (Lba > Partition->EndingLba) ||
        ((mAns->PartitionReadSeen & PartitionBit) != 0))
    {
      continue;
    }

    mAns->PartitionReadSeen |= PartitionBit;
    ANS_DEBUG ((
      DEBUG_INFO,
      "AppleANS ReadyToBoot I/O: first successful HD(%u) read LBA=0x%Lx blocks=0x%Lx unique=%g name=\"%s\"\n",
      Partition->PartitionNumber,
      (UINT64)Lba,
      (UINT64)Blocks,
      &Partition->UniquePartitionGuid,
      Partition->PartitionName
      ));
    break;
  }

  return Status;
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

  Status = ValidateBlockRequest (This, MediaId, Lba, BufferSize, Buffer);
  if (EFI_ERROR (Status) || (BufferSize == 0)) {
    return Status;
  }

  return EFI_WRITE_PROTECTED;
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

  return EFI_SUCCESS;
}

//
// Queue/TCB/bounce memory. Routed through AnsAllocatePages() for the same
// reason the RTKit buffers are: MemoryAllocationLib's AllocateAlignedPages()
// ASSERT_EFI_ERRORs on any FreePages failure during its own internal
// slack-trimming, and FreeAlignedPages() ASSERTs on both Pages == 0 and any
// FreePages failure. Those asserts have not fired for these EfiBootServicesData
// allocations (their free granularity is only 4 KiB on AARCH64, so the partial
// frees are legal), but an optional storage coprocessor must not retain ANY
// code path that can abort the boot -- so no allocation here uses that library
// at all any more.
//
STATIC VOID *
AllocateQueueMemory (
  IN OUT APPLE_ANS_DEVICE  *Device,
  IN     CONST CHAR8       *Purpose,
  IN     UINTN             Size
  )
{
  APPLE_ANS_PAGE_ALLOCATION  *Allocation;
  EFI_MEMORY_TYPE            MemoryType;
  EFI_STATUS                 Status;

  //
  // A preserve-for-OS build may reach ExitBootServices with the ANS front
  // end still enabled if its bounded NVMe shutdown fails.  Windows is then
  // free to reclaim EfiBootServicesData immediately, so no address the DMA
  // engine has ever been given may use that type.  Reserved pages turn the
  // failure case into a bounded leak instead of DMA into arbitrary OS memory.
  // The ordinary quiesce/reset profile retains BootServicesData semantics.
  //
  MemoryType = FixedPcdGetBool (PcdAppleAnsPreserveForOs) ?
                 EfiReservedMemoryType : EfiBootServicesData;
  Status = AnsAllocatePages (
             Device,
             Purpose,
             MemoryType,
             Size,
             NTASI_ANS_QUEUE_ALIGN,
             &Allocation
             );
  if (EFI_ERROR (Status)) {
    // AnsAllocatePages already logged the specifics.
    return NULL;
  }

  return Allocation->Base;
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
                              Device,
                              "admin-sq",
                              ntasi_ans_command_bytes (
                                Device->NvmeHw,
                                TRUE,
                                Device->NvmeHw->admin_queue_depth
                                )
                              );
  Device->AdminCompletions = AllocateQueueMemory (
                                 Device,
                                 "admin-cq",
                                 ntasi_ans_cq_bytes (
                                   Device->NvmeHw->admin_queue_depth
                                   )
                                 );
  Device->IoCommands = AllocateQueueMemory (
                           Device,
                           "io-sq",
                           ntasi_ans_command_bytes (Device->NvmeHw, FALSE, Slots)
                           );
  Device->IoCompletions = AllocateQueueMemory (
                              Device,
                              "io-cq",
                              ntasi_ans_cq_bytes (Slots)
                              );
  if (Linear) {
    Device->AdminTcbs = AllocateQueueMemory (
                            Device,
                            "admin-tcb",
                            ntasi_ans_tcb_bytes (Slots)
                            );
    Device->IoTcbs = AllocateQueueMemory (
                         Device,
                         "io-tcb",
                         ntasi_ans_tcb_bytes (Slots)
                         );
  }

  Device->Bounce = AllocateQueueMemory (Device, "bounce", NTASI_ANS_DATA_ALIGN);
  if ((Device->AdminCommands == NULL) ||
      (Device->AdminCompletions == NULL) ||
      (Device->IoCommands == NULL) ||
      (Device->IoCompletions == NULL) ||
      (Device->Bounce == NULL) ||
      (Linear && ((Device->AdminTcbs == NULL) || (Device->IoTcbs == NULL))))
  {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: controller memory allocation failed; ANS bring-up aborted, boot continues\n"
      ));
    return EFI_OUT_OF_RESOURCES;
  }

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

//
// Releases every page allocation this driver still owns -- queue memory and
// any RTKit shared buffer that was not already released. Whole-allocation
// frees with checked status; nothing here can assert.
//
STATIC VOID
FreeControllerMemory (
  IN OUT APPLE_ANS_DEVICE *Device
  )
{
  AnsFreeAllPages (Device);
  Device->AdminCommands    = NULL;
  Device->AdminCompletions = NULL;
  Device->AdminTcbs        = NULL;
  Device->IoCommands       = NULL;
  Device->IoCompletions    = NULL;
  Device->IoTcbs           = NULL;
  Device->Bounce           = NULL;
}

STATIC BOOLEAN
AnsIsChildDevicePath (
  IN APPLE_ANS_DEVICE          *Device,
  IN EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  UINTN  ParentPrefixSize;
  UINTN  PathSize;

  if (Path == NULL) {
    return FALSE;
  }

  ParentPrefixSize = GetDevicePathSize ((EFI_DEVICE_PATH_PROTOCOL *)&Device->DevicePath) -
                     sizeof (EFI_DEVICE_PATH_PROTOCOL);
  PathSize = GetDevicePathSize (Path);
  return (PathSize > ParentPrefixSize) &&
         (CompareMem (Path, &Device->DevicePath, ParentPrefixSize) == 0);
}

STATIC UINT32
AnsPartitionNumberFromDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  for (Node = Path; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (Node) == MEDIA_HARDDRIVE_DP))
    {
      return ((HARDDRIVE_DEVICE_PATH *)Node)->PartitionNumber;
    }
  }

  return 0;
}

STATIC CONST CHAR8 *
AnsFileSystemSignature (
  IN CONST UINT8  *Block,
  IN UINTN        BlockSize
  )
{
  if ((BlockSize >= 11) && (CompareMem (&Block[3], "NTFS    ", 8) == 0)) {
    return "NTFS";
  }

  if ((BlockSize >= 90) &&
      ((CompareMem (&Block[54], "FAT12   ", 8) == 0) ||
       (CompareMem (&Block[54], "FAT16   ", 8) == 0) ||
       (CompareMem (&Block[82], "FAT32   ", 8) == 0)))
  {
    return "FAT";
  }

  if ((BlockSize >= 36) && (CompareMem (&Block[32], "NXSB", 4) == 0)) {
    return "APFS";
  }

  return "unknown";
}

STATIC BOOLEAN
AnsBufferContains (
  IN CONST UINT8  *Buffer,
  IN UINTN        BufferSize,
  IN CONST VOID   *Pattern,
  IN UINTN        PatternSize
  )
{
  UINTN  Offset;

  if ((Buffer == NULL) || (Pattern == NULL) || (PatternSize == 0) ||
      (PatternSize > BufferSize))
  {
    return FALSE;
  }

  for (Offset = 0; Offset <= BufferSize - PatternSize; Offset++) {
    if (CompareMem (&Buffer[Offset], Pattern, PatternSize) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC UINT8
AnsAsciiLower (
  IN UINT8  Character
  )
{
  return ((Character >= 'A') && (Character <= 'Z')) ?
           (UINT8)(Character - 'A' + 'a') : Character;
}

STATIC BOOLEAN
AnsBufferContainsUtf16Ascii (
  IN CONST UINT8  *Buffer,
  IN UINTN        BufferSize,
  IN CONST CHAR8  *Ascii
  )
{
  UINTN  CharacterCount;
  UINTN  PatternBytes;
  UINTN  Offset;
  UINTN  Index;

  if ((Buffer == NULL) || (Ascii == NULL)) {
    return FALSE;
  }

  CharacterCount = AsciiStrLen (Ascii);
  if ((CharacterCount == 0) || (CharacterCount > (MAX_UINTN / sizeof (CHAR16)))) {
    return FALSE;
  }

  PatternBytes = CharacterCount * sizeof (CHAR16);
  if (PatternBytes > BufferSize) {
    return FALSE;
  }

  for (Offset = 0; Offset <= BufferSize - PatternBytes; Offset++) {
    for (Index = 0; Index < CharacterCount; Index++) {
      UINT16  Value;

      Value = ReadUnaligned16 ((CONST UINT16 *)&Buffer[Offset + Index * 2]);
      if ((Value > MAX_UINT8) ||
          (AnsAsciiLower ((UINT8)Value) != AnsAsciiLower ((UINT8)Ascii[Index])))
      {
        break;
      }
    }

    if (Index == CharacterCount) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC VOID
AnsFindGuidReferences (
  IN CONST UINT8     *Buffer,
  IN UINTN           BufferSize,
  IN CONST EFI_GUID  *Guid,
  OUT BOOLEAN        *BinaryReference,
  OUT BOOLEAN        *TextReference
  )
{
  CHAR8  GuidText[37];

  *BinaryReference = AnsBufferContains (
                       Buffer,
                       BufferSize,
                       Guid,
                       sizeof (*Guid)
                       );
  AsciiSPrint (GuidText, sizeof (GuidText), "%g", Guid);
  *TextReference = AnsBufferContainsUtf16Ascii (
                     Buffer,
                     BufferSize,
                     GuidText
                     );
}

STATIC EFI_STATUS
AnsReadGptDiskGuid (
  IN APPLE_ANS_DEVICE  *Device,
  OUT EFI_GUID         *DiskGuid
  )
{
  EFI_PARTITION_TABLE_HEADER  *Header;
  UINT8                       *Block;
  EFI_STATUS                  Status;

  ZeroMem (DiskGuid, sizeof (*DiskGuid));
  Block = AllocatePool (Device->Media.BlockSize);
  if (Block == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = Device->BlockIo.ReadBlocks (
                             &Device->BlockIo,
                             Device->Media.MediaId,
                             PRIMARY_PART_HEADER_LBA,
                             Device->Media.BlockSize,
                             Block
                             );
  if (!EFI_ERROR (Status)) {
    Header = (EFI_PARTITION_TABLE_HEADER *)Block;
    if ((Header->Header.Signature != EFI_PTAB_HEADER_ID) ||
        (Header->Header.HeaderSize < sizeof (EFI_PARTITION_TABLE_HEADER)) ||
        (Header->MyLBA != PRIMARY_PART_HEADER_LBA))
    {
      Status = EFI_COMPROMISED_DATA;
    } else {
      *DiskGuid = Header->DiskGUID;
    }
  }

  FreePool (Block);
  return Status;
}

STATIC EFI_STATUS
AnsAuditBcdReferences (
  IN APPLE_ANS_DEVICE  *Device,
  IN EFI_HANDLE        EspHandle,
  IN UINT32            EspPartitionNumber,
  IN CONST EFI_GUID    *DiskGuid,
  IN BOOLEAN           DiskGuidValid
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  UINT8                            *Bcd;
  UINTN                            ReadSize;
  UINTN                            ScanSize;
  UINTN                            Index;
  EFI_STATUS                       Status;
  BOOLEAN                          Truncated;
  BOOLEAN                          HiveValid;
  BOOLEAN                          WinloadPathPresent;
  BOOLEAN                          DiskGuidBinary;
  BOOLEAN                          DiskGuidText;

  FileSystem = NULL;
  Root       = NULL;
  File       = NULL;
  Bcd        = NULL;
  Status     = gBS->HandleProtocol (
                      EspHandle,
                      &gEfiSimpleFileSystemProtocolGuid,
                      (VOID **)&FileSystem
                      );
  if (!EFI_ERROR (Status)) {
    Status = FileSystem->OpenVolume (FileSystem, &Root);
  }

  if (!EFI_ERROR (Status)) {
    Status = Root->Open (
                     Root,
                     &File,
                     L"\\EFI\\Microsoft\\Boot\\BCD",
                     EFI_FILE_MODE_READ,
                     0
                     );
  }

  if (!EFI_ERROR (Status)) {
    Bcd = AllocatePool (APPLE_ANS_MAX_BCD_BYTES + 1u);
    if (Bcd == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
    }
  }

  ReadSize = APPLE_ANS_MAX_BCD_BYTES + 1u;
  if (!EFI_ERROR (Status)) {
    Status = File->Read (File, &ReadSize, Bcd);
  }

  if (EFI_ERROR (Status)) {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS ReadyToBoot BCD audit: HD(%u) read-only open/read failed: %r\n",
      EspPartitionNumber,
      Status
      ));
    goto Done;
  }

  Truncated = ReadSize > APPLE_ANS_MAX_BCD_BYTES;
  ScanSize  = Truncated ? APPLE_ANS_MAX_BCD_BYTES : ReadSize;
  HiveValid = (ScanSize >= 4) && (CompareMem (Bcd, "regf", 4) == 0);
  WinloadPathPresent = AnsBufferContainsUtf16Ascii (
                         Bcd,
                         ScanSize,
                         "\\WINDOWS\\system32\\winload.efi"
                         );
  DiskGuidBinary = FALSE;
  DiskGuidText   = FALSE;
  if (DiskGuidValid) {
    AnsFindGuidReferences (
      Bcd,
      ScanSize,
      DiskGuid,
      &DiskGuidBinary,
      &DiskGuidText
      );
  }
  ANS_DEBUG ((
    DEBUG_INFO,
    "AppleANS ReadyToBoot BCD audit: HD(%u) read 0x%Lx byte(s)%a; hive=%a winload-path=%a disk-guid-binary=%a disk-guid-text=%a\n",
    EspPartitionNumber,
    (UINT64)ScanSize,
    Truncated ? " (TRUNCATED at hard 1 MiB bound)" : "",
    HiveValid ? "regf" : "unexpected",
    WinloadPathPresent ? "present" : "absent",
    DiskGuidBinary ? "REFERENCED" :
      (DiskGuidValid ? "absent" : "unavailable"),
    DiskGuidText ? "REFERENCED" :
      (DiskGuidValid ? "absent" : "unavailable")
    ));
  // QEMU's ANS_DEBUG compiles away completely; retain the read-only scan in
  // that build so the same code is type-checked without unused warnings.
  (VOID)HiveValid;
  (VOID)WinloadPathPresent;
  (VOID)DiskGuidBinary;
  (VOID)DiskGuidText;

  for (Index = 0; Index < Device->GptPartitionCount; Index++) {
    APPLE_ANS_GPT_PARTITION  *Partition;
    BOOLEAN                  BinaryGuid;
    BOOLEAN                  TextGuid;

    Partition = &Device->GptPartitions[Index];
    AnsFindGuidReferences (
      Bcd,
      ScanSize,
      &Partition->UniquePartitionGuid,
      &BinaryGuid,
      &TextGuid
      );
    ANS_DEBUG ((
      (BinaryGuid || TextGuid) ? DEBUG_INFO : DEBUG_WARN,
      "AppleANS ReadyToBoot BCD reference: HD(%u) unique=%g name=\"%s\" system=%a binary-guid=%a text-guid=%a\n",
      Partition->PartitionNumber,
      &Partition->UniquePartitionGuid,
      Partition->PartitionName,
      Partition->SystemPartition ? "yes" : "no",
      BinaryGuid ? "REFERENCED" : "absent",
      TextGuid ? "REFERENCED" : "absent"
      ));
    (VOID)BinaryGuid;
    (VOID)TextGuid;
  }

Done:
  if (Bcd != NULL) {
    FreePool (Bcd);
  }
  if (File != NULL) {
    File->Close (File);
  }
  if (Root != NULL) {
    Root->Close (Root);
  }
  return Status;
}

//
// PartitionDxe intentionally remains generic.  At the last boot-services
// boundary before the selected Windows boot application starts, audit only
// the child handles rooted at this AppleANS vendor device path.  This proves
// both the exact GPT identity bootmgfw must resolve and that the corresponding
// child Block I/O can read its first sector.  After the audit, root Block I/O
// attribution is armed so bootmgfw's first read of every partition is logged.
//
STATIC VOID EFIAPI
AnsReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  APPLE_ANS_DEVICE             *Device;
  EFI_HANDLE                   *Handles;
  UINTN                        HandleCount;
  UINTN                        HandleIndex;
  EFI_STATUS                   Status;
  EFI_GUID                     DiskGuid;
  BOOLEAN                      DiskGuidValid;
  BOOLEAN                      BcdAudited;
  BOOLEAN                      WindowsPartitionPresent;
  VOID                         *Interface;

  Device = Context;
  if (Device->ReadyToBootDiagnosticsComplete || Device->HandedOff) {
    return;
  }

  // Advance protocol-notify registrations before doing the global child
  // scan. ReadyToBoot fires before BDS connects this device on J414s, so the
  // PartitionInfo and SimpleFS notifications are what make the audit run at
  // the useful boundary: after PartitionDxe has published the GPT children.
  if ((Event == Device->PartitionInfoEvent) &&
      (Device->PartitionInfoRegistration != NULL))
  {
    while (!EFI_ERROR (gBS->LocateProtocol (
                              &gEfiPartitionInfoProtocolGuid,
                              Device->PartitionInfoRegistration,
                              &Interface
                              )))
    {
    }
  } else if ((Event == Device->SimpleFileSystemEvent) &&
             (Device->SimpleFileSystemRegistration != NULL))
  {
    while (!EFI_ERROR (gBS->LocateProtocol (
                              &gEfiSimpleFileSystemProtocolGuid,
                              Device->SimpleFileSystemRegistration,
                              &Interface
                              )))
    {
    }
  }

  Device->ReadAttributionArmed            = FALSE;
  Device->GptPartitionCount               = 0;
  Device->PartitionReadSeen               = 0;
  Device->TracedReadErrors                = 0;
  Handles                                 = NULL;
  HandleCount                             = 0;
  DiskGuidValid                           = FALSE;
  BcdAudited                              = FALSE;
  WindowsPartitionPresent                 = FALSE;

  Status = AnsReadGptDiskGuid (Device, &DiskGuid);
  if (EFI_ERROR (Status)) {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS ReadyToBoot GPT audit: primary disk GUID unavailable (%r)\n",
      Status
      ));
  } else {
    DiskGuidValid = TRUE;
    ANS_DEBUG ((
      DEBUG_INFO,
      "AppleANS ReadyToBoot GPT: disk unique GUID=%g\n",
      &DiskGuid
      ));
  }

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPartitionInfoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS ReadyToBoot GPT audit: partition handles are not published yet (%r); deferring until PartitionInfo/SimpleFS notification\n",
      Status
      ));
    return;
  }

  for (HandleIndex = 0;
       (HandleIndex < HandleCount) &&
       (Device->GptPartitionCount < APPLE_ANS_MAX_GPT_PARTITIONS);
       HandleIndex++)
  {
    EFI_DEVICE_PATH_PROTOCOL       *Path;
    EFI_PARTITION_INFO_PROTOCOL   *PartitionInfo;
    EFI_BLOCK_IO_PROTOCOL         *BlockIo;
    EFI_PARTITION_ENTRY           *Gpt;
    APPLE_ANS_GPT_PARTITION       *Record;
    UINT8                         *BootSector;
    EFI_STATUS                    ReadStatus;
    CONST CHAR8                   *FileSystem;

    Path = DevicePathFromHandle (Handles[HandleIndex]);
    if (!AnsIsChildDevicePath (Device, Path)) {
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[HandleIndex],
                    &gEfiPartitionInfoProtocolGuid,
                    (VOID **)&PartitionInfo
                    );
    if (EFI_ERROR (Status) || (PartitionInfo->Type != PARTITION_TYPE_GPT)) {
      continue;
    }

    Gpt    = &PartitionInfo->Info.Gpt;
    Record = &Device->GptPartitions[Device->GptPartitionCount];
    ZeroMem (Record, sizeof (*Record));
    Record->Handle              = Handles[HandleIndex];
    Record->PartitionNumber     = AnsPartitionNumberFromDevicePath (Path);
    Record->StartingLba         = Gpt->StartingLBA;
    Record->EndingLba           = Gpt->EndingLBA;
    Record->PartitionTypeGuid   = Gpt->PartitionTypeGUID;
    Record->UniquePartitionGuid = Gpt->UniquePartitionGUID;
    Record->SystemPartition     = PartitionInfo->System ? TRUE : FALSE;
    CopyMem (Record->PartitionName, Gpt->PartitionName, sizeof (Gpt->PartitionName));
    Record->PartitionName[ARRAY_SIZE (Record->PartitionName) - 1] = L'\0';
    Device->GptPartitionCount++;

    BlockIo    = NULL;
    BootSector = NULL;
    FileSystem = "unread";
    ReadStatus = gBS->HandleProtocol (
                        Handles[HandleIndex],
                        &gEfiBlockIoProtocolGuid,
                        (VOID **)&BlockIo
                        );
    if (!EFI_ERROR (ReadStatus) && (BlockIo->Media != NULL) &&
        (BlockIo->Media->BlockSize != 0))
    {
      BootSector = AllocatePool (BlockIo->Media->BlockSize);
      if (BootSector == NULL) {
        ReadStatus = EFI_OUT_OF_RESOURCES;
      } else {
        ReadStatus = BlockIo->ReadBlocks (
                                BlockIo,
                                BlockIo->Media->MediaId,
                                0,
                                BlockIo->Media->BlockSize,
                                BootSector
                                );
        if (!EFI_ERROR (ReadStatus)) {
          FileSystem = AnsFileSystemSignature (
                         BootSector,
                         BlockIo->Media->BlockSize
                         );
          if (AsciiStrCmp (FileSystem, "NTFS") == 0) {
            WindowsPartitionPresent = TRUE;
          }
        }
      }
    }

    ANS_DEBUG ((
      EFI_ERROR (ReadStatus) ? DEBUG_ERROR : DEBUG_INFO,
      "AppleANS ReadyToBoot GPT: HD(%u) type=%g unique=%g start=0x%Lx end=0x%Lx attrs=0x%Lx name=\"%s\" boot-sector=%r signature=%a\n",
      Record->PartitionNumber,
      &Record->PartitionTypeGuid,
      &Record->UniquePartitionGuid,
      (UINT64)Record->StartingLba,
      (UINT64)Record->EndingLba,
      Gpt->Attributes,
      Record->PartitionName,
      ReadStatus,
      FileSystem
      ));
    if (BootSector != NULL) {
      FreePool (BootSector);
    }
  }

  for (HandleIndex = 0;
       HandleIndex < Device->GptPartitionCount;
       HandleIndex++)
  {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
    APPLE_ANS_GPT_PARTITION          *Partition;

    Partition = &Device->GptPartitions[HandleIndex];
    Status = gBS->HandleProtocol (
                    Partition->Handle,
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&FileSystem
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = AnsAuditBcdReferences (
               Device,
               Partition->Handle,
               Partition->PartitionNumber,
               &DiskGuid,
               DiskGuidValid
               );
    if (!EFI_ERROR (Status)) {
      BcdAudited = TRUE;
      break;
    }
  }

  if (!BcdAudited) {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS ReadyToBoot BCD audit: no AppleANS SimpleFS partition contained a readable Microsoft BCD\n"
      ));
  }

  FreePool (Handles);
  ArmDataSynchronizationBarrier ();
  Device->ReadAttributionArmed = TRUE;
  Device->ReadyToBootDiagnosticsComplete =
    BcdAudited && WindowsPartitionPresent;
  ANS_DEBUG ((
    Device->ReadyToBootDiagnosticsComplete ? DEBUG_INFO : DEBUG_WARN,
    "AppleANS ReadyToBoot GPT audit %a: %Lu child partitions; BCD=%a NTFS=%a; bounded bootmgfw I/O attribution armed\n",
    Device->ReadyToBootDiagnosticsComplete ? "complete" : "partial (waiting for protocol publication)",
    (UINT64)Device->GptPartitionCount,
    BcdAudited ? "read" : "unavailable",
    WindowsPartitionPresent ? "present" : "absent"
    ));
}

//
// Hand the ANS coprocessor to Windows.
//
// Order mirrors m1n1's nvme_shutdown() (src/nvme.c): delete the I/O queues,
// CC.SHN=NORMAL until CSTS.SHST=DONE, CC.EN=0 until CSTS.RDY=0
// (ntasi_ans_controller_stop does all of that), then the RTKit quiesce
// (AP->QUIESCED, IOP->SLEEP) and finally clear the ASC run bit. m1n1 then
// does pmgr_reset(ANS/ANS2); this driver deliberately does not -- the Windows
// AppleNvme miniport owns that reset, and writing a PMGR word from here is
// the failure mode that once pointed at DCS_09/DCS_10 (DRAM controllers).
//
// TWO CORRECTIONS, both from the 2026-07-30 hardware capture where this
// callback logged "RTKit handoff failed: -25" immediately before Windows
// started:
//
//  1. The failure is now reported with what it actually means, and the
//     coprocessor's run bit is driven low regardless (see
//     ntasi_rtkit_runtime_handoff()). Previously a failed quiesce returned
//     early WITHOUT stopping the coprocessor.
//
//  2. The shared buffers are no longer freed, and SART grants are revoked
//     only once the coprocessor is confirmed halted. Freeing reserved pages
//     here would un-reserve them microseconds before Windows takes over,
//     and revoking a SART grant a live coprocessor is still DMAing through
//     turns a benign handoff hiccup into a DMA fault of unknown blast
//     radius. Leaving EfiReservedMemoryType buffers in place costs a few
//     16 KiB pages and is what m1n1 does for pool/IOP-owned buffers.
//
STATIC VOID EFIAPI
AnsExitBootServices (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  APPLE_ANS_DEVICE  *Device = Context;
  int               Result;
  BOOLEAN           Stopped;
  bool              CoprocessorStopped;
 #if !defined (APPLE_ANS_QEMU_TEST)
  UINT32            AscControl;
 #endif

  (VOID)Event;
  Device->HandedOff = TRUE;

  /*
   * A Mu-booted OS inherits a LIVE RTKit/ASC, not Mu's NVMe queues.  Quiesce
   * the NVMe front end (delete I/O queues, normal shutdown, CC.EN=0), but do
   * not sleep RTKit, clear CPU_CONTROL.RUN, revoke SART, or free any buffer.
   * AppleNvme observes the running coprocessor, takes its WAKE path, and then
   * programs wholly new admin/I/O queues.
   *
   * This is deliberately not "stop ANS before Windows": the coprocessor and
   * its RTKit shared-buffer grants remain live.  If the bounded controller
   * stop fails, every queue/TCB/bounce allocation is EfiReservedMemoryType in
   * this profile, so the still-live DMA master cannot target reclaimed OS
   * pages.  The Windows driver will disable the inherited controller before
   * programming its own queues, exactly as its start core already does.
   */
  if (FixedPcdGetBool (PcdAppleAnsPreserveForOs) &&
      Device->InheritedSartMemoryReserved && Device->Rtkit.booted &&
      ntasi_asc_cpu_running (&Device->Asc))
  {
    Result = 0;
    if (Device->Controller.enabled) {
      Result = ntasi_ans_controller_stop (&Device->Controller);
    }
    ArmDataSynchronizationBarrier ();
    if (Result == 0) {
      ANS_DEBUG ((
        DEBUG_INFO,
        "AppleANS: OS handoff is NVMe-quiesced/RTKit-live: controller disabled; preserving ASC run state, reserved buffers and SART grants for AppleNvme WAKE adoption\n"
        ));
    } else {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: bounded NVMe quiesce failed (%d); preserving live RTKit/SART and reserved queue memory so Windows can disable the inherited controller safely\n",
        Result
        ));
    }
    return;
  }

  if (FixedPcdGetBool (PcdAppleAnsPreserveForOs) &&
      Device->Rtkit.booted && ntasi_asc_cpu_running (&Device->Asc) &&
      !Device->InheritedSartMemoryReserved)
  {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: refusing live RTKit handoff: at least one inherited SART DMA range is reclaimable by Windows; taking cold stop-and-revoke path\n"
      ));
  }

  Result = ntasi_ans_controller_stop (&Device->Controller);
  if (Result != 0) {
    ANS_DEBUG ((DEBUG_ERROR, "AppleANS: controller handoff failed: %d\n", Result));
  }

  Stopped            = FALSE;
  CoprocessorStopped = false;
  if (Device->Rtkit.booted) {
    Result = ntasi_rtkit_runtime_handoff (&Device->Rtkit, &CoprocessorStopped);
    Stopped = CoprocessorStopped ? TRUE : FALSE;
    if (Result != 0) {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: RTKit quiesce failed: %d (%a); coprocessor run bit now %a\n",
        Result,
        (Result == NTASI_RTKIT_RUNTIME_ERR_BUFFER)      ? "shared-buffer grant" :
        (Result == NTASI_RTKIT_RUNTIME_ERR_TIMEOUT)     ? "no power-state ack" :
        (Result == NTASI_RTKIT_RUNTIME_ERR_TRANSPORT)   ? "mailbox transport" :
        (Result == NTASI_RTKIT_RUNTIME_ERR_PROTOCOL)    ? "unexpected message" :
        (Result == NTASI_RTKIT_RUNTIME_ERR_CRASHED)     ? "firmware crashed" :
        "argument",
        Stopped ? "clear" : "STILL SET"
        ));
    }
  } else if (Device->Asc.hw != NULL) {
    // Never booted (or already torn down): still make sure the run bit is
    // low before Windows inherits the controller. Guarded on Asc.hw because
    // an uninitialized transport has NULL ops.
    ntasi_asc_cpu_stop (&Device->Asc);
    Stopped = ntasi_asc_cpu_running (&Device->Asc) ? FALSE : TRUE;
  } else {
    // Transport was never initialized, so nothing was ever started.
    Stopped = TRUE;
  }

  if (Stopped) {
    //
    // CLOSE THE WHOLE SART WINDOW, not just the entries this driver added.
    //
    // SART is an ALLOW list: an armed entry PERMITS ANS DMA into that physical
    // range. clear_owned() closes only Mu's own grants and leaves iBoot's
    // entries armed -- and iBoot's entries cover iBoot's ANS buffers, which
    // Windows reclaims as conventional RAM seconds later. A standing DMA grant
    // over memory the OS is handing to arbitrary drivers is a loaded gun, and
    // it is loaded in every profile that carries ANS, whether or not this
    // driver ever booted the IOP.
    //
    // Safe here and nowhere earlier: the coprocessor's run bit is confirmed
    // clear on this path, so no grant is being revoked out from under a live
    // DMA. close_all() reads every entry back, so "SART is shut" is a
    // measurement rather than an assumption.
    //
    unsigned int  StillArmed;
    int           CloseResult;

    StillArmed  = 0;
    CloseResult = ntasi_sart_runtime_close_all (&Device->Sart, &StillArmed);
    if (CloseResult != NTASI_SART_RUNTIME_OK) {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: SART close FAILED: %Lu of %Lu entries still read back armed "
        "(%d).  Windows is inheriting a DMA grant this firmware could not "
        "revoke\n",
        (UINT64)StillArmed,
        (UINT64)NTASI_SART_MAX_ENTRIES,
        CloseResult
        ));
    } else {
      ANS_DEBUG ((
        DEBUG_INFO,
        "AppleANS: handoff complete; coprocessor halted, ALL %Lu SART entries "
        "cleared and verified closed (iBoot's included), shared buffers left "
        "reserved for the OS\n",
        (UINT64)NTASI_SART_MAX_ENTRIES
        ));
    }
  } else {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: coprocessor did not halt; leaving SART grants and reserved "
      "shared buffers intact so it cannot DMA into revoked or reallocated "
      "memory\n"
      ));
  }

#if !defined (APPLE_ANS_QEMU_TEST)
  //
  // The step Mu was missing, and m1n1 never omits. nvme_shutdown() (m1n1
  // src/nvme.c) ends with:
  //
  //     rtkit_sleep(nvme_rtkit);
  //     pmgr_reset(nvme_die, "ANS");
  //     pmgr_reset(nvme_die, "ANS2");
  //
  // Clearing the ASC run bit (all rtkit_sleep does) halts the coprocessor's
  // CPU. It does NOT quiesce the block's AXI/fabric interface. pmgr_reset is
  // what does that: DEV_DISABLE detaches the device from the fabric and RESET
  // returns its master port to a known state, so nothing that was in flight
  // when the CPU stopped can remain dangling.
  //
  // WHY THIS IS STILL HERE EVEN THOUGH THE ANS CORRELATION IS DEAD.
  //
  // This reset was originally added because a BUGCODE_USB3_DRIVER 0x144
  // correlated 4-for-4 with ANS-carrying profiles and 0-for-2 without. That
  // correlation was FALSIFIED on 2026-07-30 when the `gpu-wireless` profile --
  // which contains no ANS FFS, boots no IOP, and never reaches this code --
  // produced a 0x144 of its own. ANS is not a necessary condition for the
  // bugcheck, and nothing below should be read as claiming otherwise.
  //
  // It stays because it is correct on its own terms, independent of the 0x144:
  // m1n1's nvme_shutdown() performs it, this firmware previously omitted it,
  // and "halted but never reset" leaves a block's AXI master port in a state
  // no downstream owner has any reason to expect. Leaving a known-incorrect
  // teardown in place because its motivating hypothesis lost is how a second
  // bug gets built on the first.
  //
  // Only reached when bring-up actually ran, and only after the coprocessor is
  // confirmed halted -- resetting a running block would be worse than not
  // resetting at all.
  //
  // The exact SART state Windows inherits, read BEFORE the PMGR reset.
  //
  // Ordering matters and used to be wrong. This dump used to run after
  // AppleAnsPmgrResetDomain(), i.e. after DEV_DISABLE had detached the block
  // from the fabric -- reading a register block whose domain has just been
  // cycled is how an ExitBootServices callback turns into a hang with no
  // console left to say so. Read-only or not, it goes first.
  DumpSartState (Device, "handed-to-os");

  //
  // The run bit, read into a real variable BEFORE the PMGR reset.
  //
  // Two corrections in one. It used to be read twice inside a DEBUG() argument
  // list, which means it was not read at all in a RELEASE build -- the only
  // build that ships -- so the log line that "proved" the coprocessor was
  // handed over halted never existed on the hardware it was written for. And
  // it used to run AFTER the PMGR reset, i.e. after DEV_DISABLE had detached
  // the block from the fabric.
  //
  AscControl = MmioRead32 (Device->CpuBase + NTASI_ASC_CPU_CONTROL);
  ANS_DEBUG ((
    DEBUG_INFO,
    "AppleANS: ASC CPU_CONTROL before PMGR reset = 0x%08x (run bit %a)\n",
    AscControl,
    ((AscControl & NTASI_ASC_CPU_CONTROL_START) != 0) ? "SET" : "clear"
    ));

  if (Stopped) {
    EFI_STATUS  ResetStatus;
    UINT32      ResetFinalValue;

    ResetFinalValue = 0;
    ResetStatus     = AppleAnsPmgrResetDomain (
                        "AppleANS",
                        "ANS2",
                        FixedPcdGet64 (PcdAppleAnsPmgrResetBase),
                        &ResetFinalValue
                        );
    if (EFI_ERROR (ResetStatus)) {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: ANS2 PMGR reset did not complete (%r, last power-state word 0x%08x); "
        "the block is halted but its fabric interface was NOT quiesced\n",
        ResetStatus,
        ResetFinalValue
        ));
    } else {
      ANS_DEBUG ((
        DEBUG_INFO,
        "AppleANS: ANS2 PMGR reset converged; power-state word 0x%08x\n",
        ResetFinalValue
        ));
      //
      // Only re-read the ASC once the domain has provably converged back to
      // ACTIVE with DEV_DISABLE clear. Reading a block whose fabric attachment
      // is in an unknown state is how this callback would hang with the
      // console already gone.
      //
      AscControl = MmioRead32 (Device->CpuBase + NTASI_ASC_CPU_CONTROL);
      ANS_DEBUG ((
        DEBUG_INFO,
        "AppleANS: ASC CPU_CONTROL handed to OS = 0x%08x (run bit %a)\n",
        AscControl,
        ((AscControl & NTASI_ASC_CPU_CONTROL_START) != 0) ? "SET" : "clear"
        ));
    }

    ReportAnsPmgrDomains ();
  }
#endif

  ArmDataSynchronizationBarrier ();
}

#if !defined (APPLE_ANS_QEMU_TEST)
//
// Read-only PMGR power-domain report, run BEFORE the first ANS MMIO access.
//
// m1n1 performs no power enable for ANS at all -- it inherits iBoot's state,
// and on T602X it could not do otherwise because /arm-io/ans's "clock-gates"
// property is zero-length (see Include/Drivers/AppleAnsPmgrDomain.h for the
// full reasoning and the hardware measurements). This firmware therefore
// does not enable, reset, or write anything either. What it does do is say,
// on every ANS boot, what state the four domains were actually in -- so that
// if a future boot does find ANS gated, the log names the domain instead of
// leaving a bare MMIO stall with no explanation.
//
// Deliberately non-fatal in every direction:
//   * A domain that cannot be resolved from the ADT is reported and ignored;
//     that is exactly the situation the driver has always run in.
//   * A domain positively decoded as NOT ACTIVE is reported at DEBUG_ERROR
//     and bring-up continues anyway. Refusing here would be a regression
//     risk with no upside: ANS bring-up is known to work on this hardware
//     with all four domains ACTIVE, and this firmware has no sanctioned way
//     to fix a gated domain (m1n1 has none either).
//
// The cross-check against the DSC PCDs is the reason a "NOT ACTIVE" verdict
// can be trusted at all: AppleAnsPmgrReportDomain() refuses to read any
// address that does not equal the hardware-confirmed expectation, so this
// can never report on a DCS_xx DRAM-controller word by accident.
//
STATIC VOID
ReportAnsPmgrDomains (
  VOID
  )
{
  STATIC CONST CHAR8  Tag[] = "AppleANS";
  CONST struct {
    CONST CHAR8  *Name;
    UINT64       Expected;
  } Domains[] = {
    { "ANS2",          FixedPcdGet64 (PcdAppleAnsPmgrResetBase)        },
    { "APCIE_ST",      FixedPcdGet64 (PcdAppleAnsPmgrApcieStBase)      },
    { "APCIE_ST_SYS",  FixedPcdGet64 (PcdAppleAnsPmgrApcieStSysBase)   },
    { "APCIE_ST1_SYS", FixedPcdGet64 (PcdAppleAnsPmgrApcieSt1SysBase)  },
  };
  UINTN    Index;
  UINTN    ResolvedCount;
  UINTN    GatedCount;
  BOOLEAN  Resolved;
  BOOLEAN  Active;

  ResolvedCount = 0;
  GatedCount    = 0;
  for (Index = 0; Index < ARRAY_SIZE (Domains); Index++) {
    AppleAnsPmgrReportDomain (
      Tag,
      Domains[Index].Name,
      Domains[Index].Expected,
      &Resolved,
      &Active
      );
    if (Resolved) {
      ResolvedCount++;
      if (!Active) {
        GatedCount++;
      }
    }
  }

  if (GatedCount != 0) {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: %Lu of %Lu resolvable PMGR domains are NOT ACTIVE; continuing "
      "anyway (no firmware here enables PMGR domains -- neither does m1n1), but "
      "any MMIO stall or mailbox timeout below is most likely this\n",
      (UINT64)GatedCount,
      (UINT64)ResolvedCount
      ));
  } else if (ResolvedCount == ARRAY_SIZE (Domains)) {
    ANS_DEBUG ((
      DEBUG_INFO,
      "AppleANS: all four ANS PMGR domains resolved from the live ADT and are ACTIVE\n"
      ));
  } else {
    ANS_DEBUG ((
      DEBUG_WARN,
      "AppleANS: only %Lu of 4 ANS PMGR domains could be resolved and corroborated; "
      "power state unverified, continuing\n",
      (UINT64)ResolvedCount
      ));
  }
}
#endif // !APPLE_ANS_QEMU_TEST

#if !defined (APPLE_ANS_QEMU_TEST)
//
// Dump the SART DMA filter's true hardware state, all 16 entries decoded.
//
// PURELY OBSERVATIONAL -- reads three registers per entry and writes nothing.
//
// Added 2026-07-30 because a BUGCODE_USB3_DRIVER 0x144 correlated with
// ANS-carrying firmware profiles, with XHC1 halted on USBSTS.HSE (Host System
// Error = the host bus rejected the controller's DMA). SART is the only
// DMA-address-filtering hardware this firmware programs, so "what did Mu
// actually leave in the filter?" had to become a question answerable from one
// boot log rather than from this driver's own used_entries bitmap -- which is
// exactly the bookkeeping that would be wrong if there were a bug.
//
// SCOPE NOTE, so this dump is not over-read: SART is not a global fabric
// filter. m1n1 instantiates exactly one, sart_init("/arm-io/sart-ans")
// (src/nvme.c:334 and :445), and its only consumer is the ANS/NVMe RTKit
// instance (src/rtkit.c rtkit_map/rtkit_unmap). It sits in front of the ANS
// coprocessor's DMA path and gates no other bus master. XHC1 does not go
// through it.
//
// SART is also an ALLOW list, not a deny list: an entry left armed PERMITS
// DMA to that range, it cannot cause a transaction to be rejected. A stale
// entry is a confidentiality/integrity concern for the range it names, never
// a route to another master's bus error.
//
STATIC VOID
DumpSartState (
  IN APPLE_ANS_DEVICE  *Device,
  IN CONST CHAR8       *When
  )
{
  UINTN    Index;
  UINTN    Armed;
  uint8_t  Flags;
  uint64_t Paddr;
  uint64_t Size;
  int      Result;

  Armed = 0;
  ANS_DEBUG ((
    DEBUG_INFO,
    "AppleANS: SART %a: base=0x%lx protected=0x%04x owned=0x%04x\n",
    When,
    Device->SartBase,
    (UINT32)Device->Sart.protected_entries,
    (UINT32)Device->Sart.used_entries
    ));

  for (Index = 0; Index < NTASI_SART_MAX_ENTRIES; Index++) {
    Result = ntasi_sart_runtime_read (&Device->Sart, (unsigned int)Index, &Flags, &Paddr, &Size);
    if (Result != NTASI_SART_RUNTIME_OK) {
      ANS_DEBUG ((DEBUG_ERROR, "AppleANS: SART %a:   [%02Lu] unreadable (%d)\n", When, (UINT64)Index, Result));
      continue;
    }

    if (Flags == 0) {
      continue;
    }

    Armed++;
    ANS_DEBUG ((
      DEBUG_INFO,
      "AppleANS: SART %a:   [%02Lu] flags=0x%02x paddr=0x%Lx size=0x%Lx %a%a\n",
      When,
      (UINT64)Index,
      (UINT32)Flags,
      (UINT64)Paddr,
      (UINT64)Size,
      ((Device->Sart.protected_entries & (1u << Index)) != 0) ? "iBoot-owned" : "",
      ((Device->Sart.used_entries & (1u << Index)) != 0) ? "Mu-owned" : ""
      ));
  }

  ANS_DEBUG ((
    DEBUG_INFO,
    "AppleANS: SART %a: %Lu of %Lu entries armed (SART is an ALLOW list; an armed entry "
    "permits ANS DMA to that range and can never reject another master's transaction)\n",
    When,
    (UINT64)Armed,
    (UINT64)NTASI_SART_MAX_ENTRIES
    ));
}
#endif // !APPLE_ANS_QEMU_TEST

STATIC EFI_STATUS
DiscoverHardware (
  IN OUT APPLE_ANS_DEVICE              *Device,
  OUT CONST struct ntasi_asc_hw        **AscHw,
  OUT CONST struct ntasi_sart_params   **SartParams
  )
{
#if !defined (APPLE_ANS_QEMU_TEST)
  dt_node_t  *AnsNode;
  dt_node_t  *SartNode;
  UINT64     CpuBase;
  UINT64     CpuSize;
  UINT64     NvmeBase;
  UINT64     NvmeSize;
  UINT64     SartBase;
  UINT64     SartSize;
  UINT64     NvmeMinimumSize;
  UINT64     SartMinimumSize;
  UINT32     SartVersion;
  UINTN      PropertySize;
  UINT32     *VersionProperty;
  BOOLEAN    Legacy;
#endif

#if defined (APPLE_ANS_QEMU_TEST)
  // Generic QEMU/EDK2 does not initialize the Apple ADT library.  Do not
  // inspect its process-global tree pointer in the option-ROM build: it can
  // retain an arbitrary non-NULL value and make dt_get() walk unrelated
  // firmware memory.
  Device->CpuBase     = 0x250000000ULL;
  Device->MailboxBase = Device->CpuBase + APPLE_ANS_MAILBOX_OFFSET;
  Device->NvmeBase    = 0x250010000ULL;
  Device->SartBase    = 0x250040000ULL;
  Device->NvmeHw      = &ntasi_ans_hw_t8103;
  *AscHw              = &ntasi_asc_hw_v4;
  *SartParams         = &ntasi_sart_params_v2;
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: using QEMU fixed-resource profile\n"));
  return EFI_SUCCESS;
#else
  AnsNode  = dt_get ("/arm-io/ans");
  SartNode = dt_get ("/arm-io/sart-ans");
  if ((AnsNode == NULL) || (SartNode == NULL)) {
    return EFI_NOT_FOUND;
  }

  if ((dt_node_reg (AnsNode, 0, &CpuBase, &CpuSize) != 0) ||
      (dt_node_reg (AnsNode, 3, &NvmeBase, &NvmeSize) != 0) ||
      (dt_node_reg (SartNode, 0, &SartBase, &SartSize) != 0))
  {
    return EFI_NOT_FOUND;
  }

  Legacy = PropertyContains (AnsNode, "compatible", "t8015");
  NvmeMinimumSize = Legacy ? APPLE_ANS_NVME_T8015_MIN_SIZE : APPLE_ANS_NVME_MIN_SIZE;

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
    SartMinimumSize = APPLE_ANS_SART_V0_MIN_SIZE;
  } else if (SartVersion == 2) {
    *SartParams = &ntasi_sart_params_v2;
    SartMinimumSize = APPLE_ANS_SART_V2_MIN_SIZE;
  } else if (SartVersion == 3) {
    *SartParams = &ntasi_sart_params_v3;
    SartMinimumSize = APPLE_ANS_SART_V3_MIN_SIZE;
  } else {
    return EFI_UNSUPPORTED;
  }

  if (!AppleAnsMmioRangeValid (CpuBase, CpuSize, APPLE_ANS_CPU_MIN_SIZE) ||
      !AppleAnsMmioRangeValid (NvmeBase, NvmeSize, NvmeMinimumSize) ||
      !AppleAnsMmioRangeValid (SartBase, SartSize, SartMinimumSize))
  {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: refusing MMIO cpu=%Lx/%Lx nvme=%Lx/%Lx sart=%Lx/%Lx minimum=%Lx/%Lx/%Lx\n",
      CpuBase,
      CpuSize,
      NvmeBase,
      NvmeSize,
      SartBase,
      SartSize,
      (UINT64)APPLE_ANS_CPU_MIN_SIZE,
      NvmeMinimumSize,
      SartMinimumSize
      ));
    return EFI_DEVICE_ERROR;
  }

  Device->CpuBase     = (UINTN)CpuBase;
  Device->NvmeBase    = (UINTN)NvmeBase;
  Device->SartBase    = (UINTN)SartBase;
  Device->MailboxBase = Device->CpuBase + APPLE_ANS_MAILBOX_OFFSET;
  Device->NvmeHw      = Legacy ? &ntasi_ans_hw_t8015 : &ntasi_ans_hw_t8103;
  *AscHw              = Legacy ? &ntasi_asc_hw_t8015 : &ntasi_asc_hw_v4;

  ANS_DEBUG ((
    DEBUG_INFO,
    "AppleANS: cpu=%Lx/%Lx mailbox=%lx nvme=%Lx/%Lx sart=%Lx/%Lx legacy=%d sartv%d\n",
    CpuBase,
    CpuSize,
    Device->MailboxBase,
    NvmeBase,
    NvmeSize,
    SartBase,
    SartSize,
    Legacy,
    SartVersion
    ));
  return EFI_SUCCESS;
#endif
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
  CONST CHAR8                      *Stage;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  //
  // Named-stage breadcrumbs, logged *before* each risky call as well as on
  // failure: on 2026-07-30 this driver's DXE-time bring-up produced zero
  // console output on a hang, indistinguishable from a driver that never
  // even started. If it hangs again, whichever DEBUG_INFO line below is the
  // last one that reached the log names the exact stage; if a call instead
  // faults, that same line pins down the last place execution was known to
  // be before the exception. Every wait this stage sequence drives is
  // already bounded by APPLE_ANS_POLL_LIMIT (see Shared/*.c) -- this only
  // adds visibility, it does not change what is bounded.
  //
  Stage = "start";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: bring-up starting\n"));

  Device = AllocateZeroPool (sizeof (*Device));
  if (Device == NULL) {
    ANS_DEBUG ((DEBUG_ERROR, "AppleANS: bring-up failed at stage \"allocate-device\": out of resources\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  mAns = Device;

  Stage = "discover-hardware";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\"\n", Stage));
  Status = DiscoverHardware (Device, &AscHw, &SartParams);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

#if defined (APPLE_ANS_QEMU_TEST)
  Stage = "map-qemu-hardware";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\"\n", Stage));
  Status = MapQemuHardware (
             Device->CpuBase,
             Device->NvmeBase,
             Device->SartBase
             );
  if (EFI_ERROR (Status)) {
    goto Fail;
  }
#endif

#if !defined (APPLE_ANS_QEMU_TEST)
  Stage = "pmgr-domain-report";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\" (read-only; never writes a PMGR word)\n", Stage));
  ReportAnsPmgrDomains ();

#endif

  Stage = "sart-init";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\"\n", Stage));
  Result = ntasi_sart_runtime_init (&Device->Sart, SartParams, &SartOps, Device);
  if (Result != 0) {
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

#if !defined (APPLE_ANS_QEMU_TEST)
  // Snapshot the filter as iBoot left it, before this driver adds anything.
  DumpSartState (Device, "as-inherited-from-iBoot");

  if (FixedPcdGetBool (PcdAppleAnsPreserveForOs)) {
    Stage  = "reserve-inherited-sart-memory";
    Status = AnsReserveInheritedSartMemory (Device);
    Device->InheritedSartMemoryReserved = EFI_ERROR (Status) ? FALSE : TRUE;
    if (EFI_ERROR (Status)) {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: live OS ownership handoff is unavailable because inherited SART memory could not be made non-reclaimable; Mu may still use ANS, but ExitBootServices will take the bounded cold stop/revoke path\n"
        ));
    }
  }
#endif

#if !defined (APPLE_ANS_QEMU_TEST)
  //
  // MINIMAL PERTURBATION GATE -- default: leave the hardware exactly as iBoot
  // left it.
  //
  // 2026-07-30 hardware result: the `ans` profile bugchecked
  // BUGCODE_USB3_DRIVER 0x144 with the Windows ANS driver DISABLED in the
  // registry (Start=4, verified from an offline hive), while `gpu` on the same
  // commit, same cable and same everything else booted to the desktop. Tally
  // on that cable: non-ANS profiles 6 boots / 0 failures, ANS profiles
  // 0 boots / 3 failures. With no Windows ANS driver loading, the only
  // remaining variable is hardware state Mu's bring-up leaves behind.
  //
  // Mu's bring-up previously did two things m1n1 never does:
  //   1. It hard-stopped a LIVE, un-quiesced coprocessor. That is now replaced
  //      by the ownership-aware WAKE path below; iBoot's run bit is untouched.
  //   2. It used to end without pmgr_reset(ANS2). That is now fixed (see
  //      AnsExitBootServices), but a reset only helps if bring-up ran at all.
  //
  // Nothing needs Mu to bring ANS up. PcdAppleAnsPublishBlockIo is FALSE, so
  // no boot option is produced, and Windows' own driver performs a full
  // bring-up from a cold ADT state exactly as Linux's apple-nvme does. The
  // DXE bring-up only ever validated the seam -- and it has: a previous boot
  // identified namespace 1 and read real blocks.
  //
  // So the default is now to touch nothing: discovery, the read-only PMGR
  // report, and the diagnostics above all still run, then this driver returns.
  // Set PcdAppleAnsPerformDxeBringUp TRUE (DSC define NTASI_ANS_DXE_BRINGUP)
  // to restore the full bring-up, which is now m1n1-complete including the
  // teardown reset.
  //
  if (!FixedPcdGetBool (PcdAppleAnsPerformDxeBringUp)) {
    ANS_DEBUG ((
      DEBUG_WARN,
      "AppleANS: DXE bring-up withheld by PcdAppleAnsPerformDxeBringUp; NVMe registers and RTKit "
      "untouched. NTAS2003 is still published for the OS driver, which performs its own bring-up. "
      "The coprocessor and SART are still QUIESCED at ExitBootServices -- see below.\n"
      ));

    //
    // MEASURED ON HARDWARE 2026-07-31, and the reason this branch no longer
    // simply walks away.
    //
    // A boot of the ans-gpu-wireless profile bugchecked BUGCODE_USB3_DRIVER
    // 0x144 (Arg1=2) after stalling >100 s at storport tag-list init, where a
    // healthy boot is past that point in ~25 s. Read from EL2 at the stop:
    //
    //   AIC line 1832 (ANS, published GSIV 38): asserted=TRUE but masked=TRUE
    //   ASC CPU_CONTROL = 0x00000010          -> START set, coprocessor RUNNING
    //   SART: 5 of 16 entries still ARMED     -> 0x2A2414000+0x3000,
    //         0x2A2434000+0x33000, 0x29E2CC000+0x3000, 0x103FF4EC000+0x33000,
    //         0x10000004000+0x1D7000
    //
    // The masked line rules OUT an interrupt storm -- a masked AIC line cannot
    // reach a CPU, so it cannot starve anything. What is left is far worse:
    // Windows inherits a LIVE DMA MASTER with an open allow list. SART permits;
    // it does not deny. iBoot's ANS firmware is still servicing the internal
    // SSD, and every range above stays writable by it while the OS reclaims
    // that memory. Corrupting arbitrary reclaimed pages is a complete
    // explanation for a USB3 boot device that goes quiet and then times out.
    //
    // Withholding bring-up was correct -- do not undo it. But "leave the
    // hardware as iBoot handed it over" also meant returning BEFORE the
    // ExitBootServices event was ever registered further down, so NOTHING
    // quiesced ANS at handoff. That was the real defect: not what bring-up
    // did, but what withholding it skipped.
    //
    // AnsExitBootServices already handles precisely this case. With
    // Rtkit.booted false it takes the "never booted" branch and calls
    // ntasi_asc_cpu_stop(), then ntasi_sart_runtime_close_all(), which clears
    // and READS BACK every entry including iBoot's -- clear_owned() would only
    // revoke grants this driver made and would leave all five armed. The PMGR
    // reset after it is already gated on the run bit being confirmed low.
    //
    // Registering the event is therefore the entire fix. The ASC transport is
    // initialised here only so the callback has a handle to stop the CPU
    // through; it binds ops and MMIO and starts nothing. If it fails we still
    // return success -- publishing NTAS2003 for the OS driver must not depend
    // on our ability to quiesce, and a failure is loud rather than silent.
    //
    Result = ntasi_asc_init_variant (
               &Device->Asc,
               &AscOps,
               AscHw,
               Device,
               APPLE_ANS_POLL_LIMIT
               );
    if (Result != 0) {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: ASC transport init failed (%d) in the withheld path; the coprocessor CANNOT be "
        "stopped at ExitBootServices and Windows will inherit a live DMA master.\n",
        Result
        ));
    } else {
      Status = gBS->CreateEventEx (
                      EVT_NOTIFY_SIGNAL,
                      TPL_NOTIFY,
                      AnsExitBootServices,
                      Device,
                      &gEfiEventExitBootServicesGuid,
                      &Device->ExitBootServicesEvent
                      );
      if (EFI_ERROR (Status)) {
        ANS_DEBUG ((
          DEBUG_ERROR,
          "AppleANS: could not register the ExitBootServices quiesce (%r); Windows will inherit a "
          "running coprocessor and an open SART.\n",
          Status
          ));
      } else {
        ANS_DEBUG ((
          DEBUG_WARN,
          "AppleANS: ExitBootServices quiesce ARMED -- the coprocessor run bit will be cleared and "
          "every SART entry closed and verified before Windows starts.\n"
          ));
        NtasiDumpReservedMemoryMap ("AppleANS");
        mAns = Device;
        return EFI_SUCCESS;
      }
    }

    NtasiDumpReservedMemoryMap ("AppleANS");
    FreePool (Device);
    mAns = NULL;
    return EFI_SUCCESS;
  }

  ANS_DEBUG ((
    DEBUG_WARN,
    "AppleANS: DXE bring-up ENABLED by PcdAppleAnsPerformDxeBringUp -- selecting WAKE or COLD from inherited ownership\n"
    ));
#endif

  Stage = "asc-init";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\"\n", Stage));
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

  Stage = "asc-ownership-select";
  Device->InheritedCoprocessor = ntasi_asc_cpu_running (&Device->Asc) ? TRUE : FALSE;
  ANS_DEBUG ((
    DEBUG_INFO,
    "AppleANS: stage \"%a\": inherited coprocessor is %a; selecting %a RTKit ownership path\n",
    Stage,
    Device->InheritedCoprocessor ? "running" : "stopped",
    Device->InheritedCoprocessor ? "WAKE" : "COLD"
    ));

  Stage = "rtkit-init";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\"\n", Stage));
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
  Device->Rtkit.boot_mode = Device->InheritedCoprocessor
                                ? NTASI_RTKIT_BOOT_MODE_WAKE
                                : NTASI_RTKIT_BOOT_MODE_COLD;

#if !defined (APPLE_ANS_QEMU_TEST)
  if (!Device->InheritedCoprocessor) {
    UINT32  ResetFinalValue;

    Stage = "cold-pmgr-reset";
    ResetFinalValue = 0;
    Status = AppleAnsPmgrResetDomain (
               "AppleANS",
               "ANS2",
               FixedPcdGet64 (PcdAppleAnsPmgrResetBase),
               &ResetFinalValue
               );
    if (EFI_ERROR (Status)) {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: cold ownership requires a completed ANS2 reset; failed with %r (last 0x%08x)\n",
        Status,
        ResetFinalValue
        ));
      goto Fail;
    }
  }
#endif

  Stage = "allocate-controller-memory";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\"\n", Stage));
  Status = AllocateControllerMemory (Device);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Stage = "rtkit-boot";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\" (bounded at %u polls per wait)\n", Stage, (UINT32)APPLE_ANS_POLL_LIMIT));
  Result = ntasi_rtkit_runtime_boot (&Device->Rtkit);
  if (Result != 0) {
    ANS_DEBUG ((DEBUG_ERROR, "AppleANS: stage \"%a\" failed: %d\n", Stage, Result));
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  Stage = "controller-start";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\" (bounded at %u polls per wait)\n", Stage, (UINT32)APPLE_ANS_POLL_LIMIT));
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
    ANS_DEBUG ((DEBUG_ERROR, "AppleANS: stage \"%a\" failed: %d\n", Stage, Result));
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  Stage = "block-device-init-and-identify";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\" (read-only: no write/format/TRIM path exists in this driver)\n", Stage));
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
    ANS_DEBUG ((DEBUG_ERROR, "AppleANS: stage \"%a\" failed: %d\n", Stage, Result));
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  Device->Media = (EFI_BLOCK_IO_MEDIA) {
    .MediaId          = 1,
    .RemovableMedia   = FALSE,
    .MediaPresent     = TRUE,
    .LogicalPartition = FALSE,
    .ReadOnly         = TRUE,
    .WriteCaching     = FALSE,
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

  Stage = "register-exit-boot-services-event";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\"\n", Stage));
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

  //
  // Publishing Block I/O hands BDS a bootable device.  On J414s the internal
  // SSD still carries its original OS loader, so the moment this appeared the
  // boot manager chose it over the Windows loader on USB and booted GRUB --
  // with no way to intervene, because Mu drives no keyboard on this machine.
  // Windows never needs this protocol: it finds the controller through the
  // NTAS200x runtime SSDT, which reads the live ADT.  Everything above still
  // runs, including stopping the coprocessor before boot, so the controller is
  // left in the state the Windows driver expects. This is also the only
  // Block I/O this driver ever installs -- WriteBlocks always returns
  // EFI_WRITE_PROTECTED (see AnsWriteBlocks above) regardless of this PCD,
  // so there is no write/format/TRIM path here to gate at all.
  //
  Stage = "publish-block-io";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\"\n", Stage));
  if (FixedPcdGetBool (PcdAppleAnsPublishBlockIo)) {
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

    Status = gBS->CreateEventEx (
                    EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    AnsReadyToBoot,
                    Device,
                    &gEfiEventReadyToBootGuid,
                    &Device->ReadyToBootEvent
                    );
    if (EFI_ERROR (Status)) {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: ReadyToBoot GPT/I/O diagnostic could not be registered (%r); continuing without it\n",
        Status
        ));
      Device->ReadyToBootEvent = NULL;
    }

    Status = gBS->CreateEvent (
                    EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    AnsReadyToBoot,
                    Device,
                    &Device->PartitionInfoEvent
                    );
    if (!EFI_ERROR (Status)) {
      Status = gBS->RegisterProtocolNotify (
                      &gEfiPartitionInfoProtocolGuid,
                      Device->PartitionInfoEvent,
                      &Device->PartitionInfoRegistration
                      );
    }
    if (EFI_ERROR (Status)) {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: PartitionInfo diagnostic notification could not be registered (%r); ReadyToBoot fallback remains active\n",
        Status
        ));
      if (Device->PartitionInfoEvent != NULL) {
        gBS->CloseEvent (Device->PartitionInfoEvent);
        Device->PartitionInfoEvent = NULL;
      }
      Device->PartitionInfoRegistration = NULL;
    }

    Status = gBS->CreateEvent (
                    EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    AnsReadyToBoot,
                    Device,
                    &Device->SimpleFileSystemEvent
                    );
    if (!EFI_ERROR (Status)) {
      Status = gBS->RegisterProtocolNotify (
                      &gEfiSimpleFileSystemProtocolGuid,
                      Device->SimpleFileSystemEvent,
                      &Device->SimpleFileSystemRegistration
                      );
    }
    if (EFI_ERROR (Status)) {
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: SimpleFileSystem diagnostic notification could not be registered (%r); ReadyToBoot fallback remains active\n",
        Status
        ));
      if (Device->SimpleFileSystemEvent != NULL) {
        gBS->CloseEvent (Device->SimpleFileSystemEvent);
        Device->SimpleFileSystemEvent = NULL;
      }
      Device->SimpleFileSystemRegistration = NULL;
    }
  } else {
    ANS_DEBUG ((
      DEBUG_INFO,
      "AppleANS: Block I/O withheld from BDS by PcdAppleAnsPublishBlockIo\n"
      ));
  }

#if !defined (APPLE_ANS_QEMU_TEST)
  // Everything this driver will ever grant is granted by now.
  DumpSartState (Device, "after-bring-up");
  //
  // The reserved regions this driver added are now in the map. Dumping it
  // here rather than from the ExitBootServices callback is deliberate:
  // allocation is forbidden there, and GetMemoryMap would perturb the very
  // map being handed over. Nothing between here and the handoff changes the
  // reserved regions -- AnsExitBootServices only revokes SART grants and
  // halts the coprocessor; it allocates and frees nothing.
  //
  NtasiDumpReservedMemoryMap ("AppleANS");
#endif

  ANS_DEBUG ((
    DEBUG_INFO,
    "AppleANS: bring-up completed all stages; namespace 1 ready: %Lu blocks x %u bytes\n",
    Device->BlockDevice.media.block_count,
    Device->BlockDevice.media.block_size
    ));
  return EFI_SUCCESS;

Fail:
  ANS_DEBUG ((DEBUG_ERROR, "AppleANS: bring-up failed at stage \"%a\": %r\n", Stage, Status));
  if (Device->ExitBootServicesEvent != NULL) {
    gBS->CloseEvent (Device->ExitBootServicesEvent);
  }
  if (Device->ReadyToBootEvent != NULL) {
    gBS->CloseEvent (Device->ReadyToBootEvent);
  }
  if (Device->PartitionInfoEvent != NULL) {
    gBS->CloseEvent (Device->PartitionInfoEvent);
  }
  if (Device->SimpleFileSystemEvent != NULL) {
    gBS->CloseEvent (Device->SimpleFileSystemEvent);
  }
  if (Device->Controller.enabled) {
    ntasi_ans_controller_stop (&Device->Controller);
  }

  if (Device->InheritedCoprocessor && !Device->Rtkit.booted) {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: warm attach failed before ownership transferred; preserving inherited coprocessor and SART state\n"
      ));
    Device->HandedOff = TRUE;
    return Status;
  }

  //
  // Unwind in the same order as the ExitBootServices handoff, and for the same
  // reason: nothing that the coprocessor might still be DMAing through may be
  // revoked or freed until its run bit is confirmed low. Unlike the handoff
  // path this one DOES free the shared buffers -- DXE continues after this and
  // the reserved pages would otherwise leak for the rest of the boot -- but
  // only once the coprocessor is halted.
  //
  {
    bool  CoprocessorStopped = false;

    if (Device->Rtkit.booted) {
      ntasi_rtkit_runtime_handoff (&Device->Rtkit, &CoprocessorStopped);
    } else if (Device->Asc.hw != NULL) {
      ntasi_asc_cpu_stop (&Device->Asc);
      CoprocessorStopped = !ntasi_asc_cpu_running (&Device->Asc);
    } else {
      // ASC transport was never initialized, so nothing was ever started.
      CoprocessorStopped = true;
    }

    if (CoprocessorStopped) {
      ntasi_rtkit_runtime_release_buffers (&Device->Rtkit);
      ntasi_sart_runtime_clear_owned (&Device->Sart);
      FreeControllerMemory (Device);
    } else {
      //
      // CORRECTED 2026-07-30. This branch used to log "leaking on purpose" and
      // then fall straight into FreeControllerMemory(), which returned every
      // tracked page -- including the RTKit shared buffers the coprocessor is
      // still SART-granted to write -- to the allocator. Those pages would then
      // be handed to the next DXE consumer and ultimately to Windows, while a
      // live coprocessor retained DMA permission to them. That is memory
      // corruption with an arbitrary victim, and it is exactly the class of bug
      // the USB3 0x144 investigation was asking about.
      //
      // Leak deliberately and completely instead: no free, no SART revoke. A
      // few hundred KiB stranded for one boot is strictly better than DMA into
      // memory somebody else owns. The allocation table is dropped without
      // freeing so nothing can free it later either.
      //
      ANS_DEBUG ((
        DEBUG_ERROR,
        "AppleANS: coprocessor did not halt during unwind; deliberately leaking %Lu "
        "tracked allocations AND their SART grants rather than returning memory a "
        "live coprocessor can still DMA into\n",
        (UINT64)Device->AllocationCount
        ));
      Device->AllocationCount = 0;
      Device->AdminCommands   = NULL;
      Device->AdminCompletions = NULL;
      Device->AdminTcbs       = NULL;
      Device->IoCommands      = NULL;
      Device->IoCompletions   = NULL;
      Device->IoTcbs          = NULL;
      Device->Bounce          = NULL;
    }

#if !defined (APPLE_ANS_QEMU_TEST)
    DumpSartState (Device, "after-failed-bring-up");
#endif
  }

  //
  // gBS->FreePool directly rather than MemoryAllocationLib's FreePool(), which
  // ASSERT_EFI_ERRORs on failure. Nothing on this unwind path may abort the
  // boot -- ANS is optional.
  //
  {
    EFI_STATUS  PoolStatus;

    PoolStatus = gBS->FreePool (Device);
    if (EFI_ERROR (PoolStatus)) {
      ANS_DEBUG ((DEBUG_ERROR, "AppleANS: FreePool failed: %r (leaked, boot continues)\n", PoolStatus));
    }
  }
  mAns = NULL;
  return Status;
}

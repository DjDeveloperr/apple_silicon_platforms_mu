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

//
// 4 RTKit shared buffers (crashlog/syslog/ioreport/oslog) + 6 queue regions +
// 1 bounce buffer = 11. Sized with headroom; AnsAllocatePages() fails cleanly
// rather than overflowing it.
//
#define APPLE_ANS_MAX_ALLOCATIONS  16u

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
  APPLE_ANS_PAGE_ALLOCATION         Allocations[APPLE_ANS_MAX_ALLOCATIONS];
  UINTN                             AllocationCount;
  EFI_BLOCK_IO_MEDIA                Media;
  EFI_BLOCK_IO_PROTOCOL             BlockIo;
  APPLE_ANS_DEVICE_PATH             DevicePath;
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
  EFI_STATUS                 Status;

  Status = AnsAllocatePages (
             Device,
             Purpose,
             EfiBootServicesData,
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

  (VOID)Event;
  Device->HandedOff = TRUE;
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
    ntasi_sart_runtime_clear_owned (&Device->Sart);
    ANS_DEBUG ((
      DEBUG_INFO,
      "AppleANS: handoff complete; coprocessor halted, SART grants released, "
      "shared buffers left reserved for the OS\n"
      ));
  } else {
    ANS_DEBUG ((
      DEBUG_ERROR,
      "AppleANS: coprocessor did not halt; leaving SART grants and reserved "
      "shared buffers intact so it cannot DMA into revoked or reallocated "
      "memory\n"
      ));
  }

#if !defined (APPLE_ANS_QEMU_TEST)
  // The exact SART state Windows inherits. Read-only, so it is safe here even
  // though allocation is not.
  DumpSartState (Device, "handed-to-os");
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

  Stage = "asc-cold-stop-check";
  ANS_DEBUG ((DEBUG_INFO, "AppleANS: stage \"%a\" (bounded at %u polls)\n", Stage, (UINT32)APPLE_ANS_POLL_LIMIT));
  if (ntasi_asc_cpu_running (&Device->Asc)) {
    ANS_DEBUG ((DEBUG_WARN, "AppleANS: coprocessor was left running; stopping before boot\n"));
    ntasi_asc_cpu_stop (&Device->Asc);
    MicroSecondDelay (1000);
  }

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
  if (Device->Controller.enabled) {
    ntasi_ans_controller_stop (&Device->Controller);
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

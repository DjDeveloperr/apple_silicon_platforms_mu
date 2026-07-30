/**
 * @file MemoryInitPeiLib.c
 * 
 * @author amarioguy (Arminder Singh)
 * 
 * This file implements page table setup, memory HOB setup, and MMU initialization.
 * Adapted from SurfaceDuoPkg/MemoryInitPeiLib.c
 * 
 * @version 1.0
 * @date 2022-07-31
 * 
 * @copyright Copyright (c) amarioguy (Arminder Singh) 2022.
 * 
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 * 
 **/

#include <PiPei.h>

#include <Library/ArmMmuLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/AppleDTLib.h>

//Device memory map configuration file for UEFI (this is to help with pagetable initialization)
#include <Library/T602XFamilyVirtualMemoryMapDefines.h>
#include <AppendedRamdisk.h>
#include <IndustryStandard/J414sWirelessHandoff.h>

#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS 44
#define DDR_ATTRIBUTES_CACHED           ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
#define DDR_ATTRIBUTES_UNCACHED         ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED

STATIC BOOLEAN  mAppendedRamdiskCorrupt;
STATIC UINT64   mAppendedRamdiskReservationSize;

#if NTASI_ENABLE_WIRELESS_DART_HANDOFF
STATIC
UINT32
NtasiWirelessCrc32 (
  IN CONST VOID  *Data,
  IN UINT32      Length
  )
{
  CONST UINT8  *Bytes;
  UINT32       Crc;
  UINT32       Index;
  UINT32       Bit;

  Bytes = Data;
  Crc   = MAX_UINT32;
  for (Index = 0; Index < Length; Index++) {
    Crc ^= Bytes[Index];
    for (Bit = 0; Bit < 8; Bit++) {
      Crc = (Crc >> 1) ^ (0xedb88320U & (0U - (Crc & 1U)));
    }
  }

  return ~Crc;
}

STATIC
BOOLEAN
NtasiValidateWirelessHandoffV2 (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT32                Size,
  IN EFI_PHYSICAL_ADDRESS  GuestMemoryTop
  )
{
  CONST NTASI_WIRELESS_HANDOFF_DESCRIPTOR_V2  *Descriptor;
  NTASI_WIRELESS_HANDOFF_DESCRIPTOR_V2        Copy;
  UINT32                                       DescriptorCrc;

  if ((Size != NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE) ||
      ((Base & (NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE - 1)) != 0) ||
      (Base > MAX_UINT64 - Size))
  {
    return FALSE;
  }

  Descriptor = (CONST VOID *)(UINTN)(Base + NTASI_WIRELESS_HANDOFF_V2_DESCRIPTOR_OFFSET);
  if ((Descriptor->Signature != NTASI_WIRELESS_HANDOFF_V2_SIGNATURE) ||
      (Descriptor->Version != NTASI_WIRELESS_HANDOFF_V2_VERSION) ||
      (Descriptor->StructureSize != sizeof (*Descriptor)) ||
      (Descriptor->Flags != NTASI_WIRELESS_HANDOFF_V2_FLAG_INSTALLED) ||
      (Descriptor->Sid != NTASI_WIRELESS_HANDOFF_V2_SID) ||
      (Descriptor->PageShift != NTASI_WIRELESS_HANDOFF_V2_PAGE_SHIFT) ||
      (Descriptor->Reserved != 0) ||
      (Descriptor->ReservationBase != Base) ||
      (Descriptor->ReservationSize != Size) ||
      (Descriptor->GuestMemoryTop != GuestMemoryTop) ||
      (Descriptor->PhysicalMemoryTop < Base + Size) ||
      (Descriptor->DartBase != NTASI_WIRELESS_HANDOFF_V2_DART_BASE) ||
      (Descriptor->L1Physical != Base + NTASI_WIRELESS_HANDOFF_V2_L1_OFFSET) ||
      (Descriptor->MsiL2Physical != Base + NTASI_WIRELESS_HANDOFF_V2_MSI_L2_OFFSET) ||
      (Descriptor->DescriptorPhysical != Base + NTASI_WIRELESS_HANDOFF_V2_DESCRIPTOR_OFFSET))
  {
    return FALSE;
  }

  Copy          = *Descriptor;
  DescriptorCrc = Copy.DescriptorCrc32;
  Copy.DescriptorCrc32 = 0;
  return (DescriptorCrc != 0) &&
         (NtasiWirelessCrc32 (&Copy, sizeof (Copy)) == DescriptorCrc) &&
         (NtasiWirelessCrc32 (
            (CONST VOID *)(UINTN)(Base + NTASI_WIRELESS_HANDOFF_V2_L1_OFFSET),
            NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE
            ) == Descriptor->L1Crc32) &&
         (NtasiWirelessCrc32 (
            (CONST VOID *)(UINTN)(Base + NTASI_WIRELESS_HANDOFF_V2_MSI_L2_OFFSET),
            NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE
            ) == Descriptor->MsiL2Crc32);
}
#endif // NTASI_ENABLE_WIRELESS_DART_HANDOFF

STATIC CONST EFI_GUID  mNtasiAppendedRamdiskLocationHobGuid =
  NTASI_APPENDED_RAMDISK_LOCATION_HOB_GUID;

VOID BuildMemoryTypeInformationHob(VOID);

VOID BuildVirtualMemoryMap(OUT ARM_MEMORY_REGION_DESCRIPTOR **VirtualMemoryMap);

STATIC VOID InitMmu(IN ARM_MEMORY_REGION_DESCRIPTOR *MemoryTable)
{
    VOID *MemoryTranslationTableBase;
    UINTN MemoryTranslationTableSize;
    RETURN_STATUS StatusCode;

    DEBUG(
        (DEBUG_INFO,
        "MemoryInitPeiLib: Enabling MMU, Page Table Base: 0x%p, Page Table Size: 0x%p\n",
        &MemoryTranslationTableBase, &MemoryTranslationTableSize)
        );
    StatusCode = ArmConfigureMmu(MemoryTable, &MemoryTranslationTableBase, &MemoryTranslationTableSize);
    DEBUG((DEBUG_INFO, "MMU enable successful\n"));

    if(EFI_ERROR(StatusCode))
    {
        DEBUG((DEBUG_ERROR | DEBUG_INFO, "MemoryInitPeiLib: MMU enable failed!! Status: %llx\n", StatusCode));
    }
}


//borrowed from edk2-platforms:Armada7k8kMemoryInitPeiLib
STATIC VOID ReserveMemoryRegion ( IN EFI_PHYSICAL_ADDRESS ReservedRegionBase, IN UINT32 ReservedRegionSize)
{
  EFI_RESOURCE_ATTRIBUTE_TYPE  ResourceAttributes;
  EFI_PHYSICAL_ADDRESS         ReservedRegionTop;
  EFI_PHYSICAL_ADDRESS         ResourceTop;
  EFI_PEI_HOB_POINTERS         NextHob;
  UINT64                       ResourceLength;

  ReservedRegionTop = ReservedRegionBase + ReservedRegionSize;

  //
  // Search for System Memory Hob that covers the reserved region,
  // and punch a hole in it
  //
  for (NextHob.Raw = GetHobList ();
       NextHob.Raw != NULL;
       NextHob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR,
                                 NextHob.Raw)) {

    if ((NextHob.ResourceDescriptor->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) &&
        (ReservedRegionBase >= NextHob.ResourceDescriptor->PhysicalStart) &&
        (ReservedRegionTop <= NextHob.ResourceDescriptor->PhysicalStart +
                      NextHob.ResourceDescriptor->ResourceLength))
    {
      ResourceAttributes = NextHob.ResourceDescriptor->ResourceAttribute;
      ResourceLength = NextHob.ResourceDescriptor->ResourceLength;
      ResourceTop = NextHob.ResourceDescriptor->PhysicalStart + ResourceLength;

      if (ReservedRegionBase == NextHob.ResourceDescriptor->PhysicalStart) {
        //
        // This region starts right at the start of the reserved region, so we
        // can simply move its start pointer and reduce its length by the same
        // value
        //
        NextHob.ResourceDescriptor->PhysicalStart += ReservedRegionSize;
        NextHob.ResourceDescriptor->ResourceLength -= ReservedRegionSize;

      } else if ((NextHob.ResourceDescriptor->PhysicalStart +
                  NextHob.ResourceDescriptor->ResourceLength) ==
                  ReservedRegionTop) {

        //
        // This region ends right at the end of the reserved region, so we
        // can simply reduce its length by the size of the region.
        //
        NextHob.ResourceDescriptor->ResourceLength -= ReservedRegionSize;

      } else {
        //
        // This region covers the reserved region. So split it into two regions,
        // each one touching the reserved region at either end, but not covering
        // it.
        //
        NextHob.ResourceDescriptor->ResourceLength =
                 ReservedRegionBase - NextHob.ResourceDescriptor->PhysicalStart;

        // Create the System Memory HOB for the remaining region (top of the FD)
        BuildResourceDescriptorHob (EFI_RESOURCE_SYSTEM_MEMORY,
                                    ResourceAttributes,
                                    ReservedRegionTop,
                                    ResourceTop - ReservedRegionTop);
      }

      //
      // Reserve the memory space.
      //
      BuildResourceDescriptorHob (EFI_RESOURCE_MEMORY_RESERVED,
        0,
        ReservedRegionBase,
        ReservedRegionSize);

      break;
    }
    NextHob.Raw = GET_NEXT_HOB (NextHob);
  }
}

STATIC BOOLEAN
ReserveAllocatedSystemMemoryRegion (
  IN EFI_PHYSICAL_ADDRESS        Base,
  IN UINT32                      Size,
  IN EFI_RESOURCE_ATTRIBUTE_TYPE Attributes
  )
{
  EFI_PEI_HOB_POINTERS  NextHob;

  ReserveMemoryRegion (Base, Size);
  for (NextHob.Raw = GetHobList ();
       NextHob.Raw != NULL;
       NextHob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, NextHob.Raw))
  {
    if ((NextHob.ResourceDescriptor->ResourceType == EFI_RESOURCE_MEMORY_RESERVED) &&
        (NextHob.ResourceDescriptor->PhysicalStart == Base) &&
        (NextHob.ResourceDescriptor->ResourceLength == Size))
    {
      // Preserve GCD CPU access while preventing DXE/OS allocation.
      NextHob.ResourceDescriptor->ResourceType      = EFI_RESOURCE_SYSTEM_MEMORY;
      NextHob.ResourceDescriptor->ResourceAttribute = Attributes;
      BuildMemoryAllocationHob (Base, Size, EfiReservedMemoryType);
      return TRUE;
    }
    NextHob.Raw = GET_NEXT_HOB (NextHob);
  }

  return FALSE;
}

#if NTASI_J414S_GPU_RESOURCE_PROFILE
//
// GPU firmware init-data payload sizes for the currently supported GPU
// firmware compat version (13.5 -- see GPU.asl's
// "ntasp,gpu-firmware-compat-major"/"-minor" and "ntasp,*-payload-size"
// _DSD values, which must move together with these if the compat version
// ever changes). These are fixed ABI struct sizes, not a per-boot memory
// layout, so unlike the addresses below they are safe to keep as compile
// time constants.
//
#define NTASI_GPU_HWCAL_A_PAYLOAD_SIZE  0x6C34ULL
#define NTASI_GPU_HWCAL_B_PAYLOAD_SIZE  0x1884ULL
#define NTASI_GPU_GLOBALS_PAYLOAD_SIZE  0x1715CULL

//
// Read a raw 64-bit Apple ADT scalar property (the "-base"/"-size" style
// properties are stored as a bare native UINT64, not an OpenFirmware
// #address-cells/#size-cells encoded "reg" pair -- see m1n1's
// ADT_GETPROP(adt, node, "gfx-handoff-base", &u64_var) in src/adt.h,
// which copies sizeof(UINT64) bytes verbatim).
//
STATIC
BOOLEAN
NtasiDtNodeU64 (
  IN  dt_node_t    *Node,
  IN  CONST CHAR8  *PropName,
  OUT UINT64       *Value
  )
{
  VOID    *Raw;
  size_t  Length;

  if (Node == NULL) {
    return FALSE;
  }

  Raw = dt_node_prop (Node, PropName, &Length);
  if ((Raw == NULL) || (Length < sizeof (UINT64))) {
    return FALSE;
  }

  *Value = *(UINT64 *)Raw;
  return TRUE;
}

//
// uat_ttbs / uat_pagetables / uat_handoff: fixed silicon carveouts read
// live from the "/arm-io/sgx" ADT node, using exactly the property names
// m1n1's dt_set_region() (src/kboot_gpu.c) reads for the same three
// regions ("gpu-region", "gfx-shared-region", "gfx-handoff" + "-base"/
// "-size"). These are carved out of physical DRAM by iBoot *above* the
// boot_args memory window Mu is handed -- that is why they measure as
// "outside system memory" tonight; it is how the hardware is wired, not
// drift. Nothing in Mu's own memory map ever claims that address range on
// its own, so this can never need ReserveAllocatedSystemMemoryRegion()'s
// "must already be covered by a System Memory HOB" contract; it always
// succeeds, and only documents the reservation in the UEFI memory map.
//
STATIC
VOID
NtasiReserveGpuAdtCarveout (
  IN dt_node_t                    *SgxNode,
  IN CONST CHAR8                  *AdtPropertyPrefix,
  IN CONST CHAR8                  *Label,
  IN EFI_PHYSICAL_ADDRESS         SystemMemoryBase,
  IN EFI_PHYSICAL_ADDRESS         SystemMemoryTop,
  IN EFI_RESOURCE_ATTRIBUTE_TYPE  ResourceAttributes
  )
{
  CHAR8   PropName[40];
  UINT64  Base;
  UINT64  Size;

  AsciiSPrint (PropName, sizeof (PropName), "%a-base", AdtPropertyPrefix);
  if (!NtasiDtNodeU64 (SgxNode, PropName, &Base)) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: %a: missing ADT property \"%a\" on sgx; GPU degraded\n", Label, PropName));
    return;
  }

  AsciiSPrint (PropName, sizeof (PropName), "%a-size", AdtPropertyPrefix);
  if (!NtasiDtNodeU64 (SgxNode, PropName, &Size)) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: %a: missing ADT property \"%a\" on sgx; GPU degraded\n", Label, PropName));
    return;
  }

  if ((Size == 0) || (Base > MAX_UINT64 - (Size - 1))) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: %a: implausible ADT region 0x%lx/+0x%lx; GPU degraded\n", Label, Base, Size));
    return;
  }

  if ((Base >= SystemMemoryBase) && (Base <= SystemMemoryTop) &&
      ((SystemMemoryTop - Base) >= Size))
  {
    //
    // Unusual but not impossible: this unit's carveout happens to fall
    // inside the boot_args window. Fold it into System Memory like the
    // "inside" reservations below so DXE/Windows can never allocate over
    // it.
    //
    if ((Size > MAX_UINT32) ||
        !ReserveAllocatedSystemMemoryRegion (Base, (UINT32)Size, ResourceAttributes))
    {
      DEBUG ((DEBUG_ERROR, "AppleAgxGpu: %a: could not reserve 0x%lx/+0x%lx inside system memory; GPU degraded\n", Label, Base, Size));
    } else {
      DEBUG ((DEBUG_INFO, "AppleAgxGpu: %a: reserved 0x%lx/+0x%lx (in-window)\n", Label, Base, Size));
    }

    return;
  }

  //
  // Expected case: iBoot's own carveout, outside the boot_args memory
  // window. Record it explicitly so it is visible in the UEFI memory map
  // even though nothing would otherwise claim it.
  //
  BuildResourceDescriptorHob (
    EFI_RESOURCE_MEMORY_RESERVED,
    0,
    Base,
    Size
    );
  DEBUG ((DEBUG_INFO, "AppleAgxGpu: %a: reserved 0x%lx/+0x%lx (out-of-window carveout)\n", Label, Base, Size));
}

//
// hw_data_a / hw_data_b / globals: the GPU firmware init-data blobs m1n1
// carves from the *top* of the same usable-memory window Mu is handed,
// via top_of_memory_alloc() called for data_a then data_b then globals in
// that order (kboot_gpu.c's dt_set_gpu()) -- a bump allocator that hands
// out the highest address first. So instead of hardcoding a snapshot of
// one boot's addresses, stack the three (firmware-ABI-fixed-size) blobs
// downward from Mu's own live SystemMemoryTop in the same order m1n1
// uses. If SystemMemoryTop is ever too small to hold them, log loudly and
// leave the GPU degraded instead of hanging PEI.
//
STATIC
VOID
NtasiReserveGpuHandoffData (
  IN EFI_PHYSICAL_ADDRESS         SystemMemoryBase,
  IN EFI_PHYSICAL_ADDRESS         SystemMemoryTop,
  IN EFI_RESOURCE_ATTRIBUTE_TYPE  ResourceAttributes
  )
{
  UINT64  HwDataASize;
  UINT64  HwDataBSize;
  UINT64  GlobalsSize;
  UINT64  HwDataABase;
  UINT64  HwDataBBase;
  UINT64  GlobalsBase;

  HwDataASize = ALIGN_VALUE (NTASI_GPU_HWCAL_A_PAYLOAD_SIZE, SIZE_16KB);
  HwDataBSize = ALIGN_VALUE (NTASI_GPU_HWCAL_B_PAYLOAD_SIZE, SIZE_16KB);
  GlobalsSize = ALIGN_VALUE (NTASI_GPU_GLOBALS_PAYLOAD_SIZE, SIZE_16KB);

  if ((SystemMemoryTop <= SystemMemoryBase) ||
      ((SystemMemoryTop - SystemMemoryBase) < (HwDataASize + HwDataBSize + GlobalsSize)))
  {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: hw_data_a/hw_data_b/globals (0x%lx total) do not fit under SystemMemoryTop 0x%lx; GPU degraded\n",
      HwDataASize + HwDataBSize + GlobalsSize,
      SystemMemoryTop
      ));
    return;
  }

  HwDataABase = SystemMemoryTop - HwDataASize;
  HwDataBBase = HwDataABase - HwDataBSize;
  GlobalsBase = HwDataBBase - GlobalsSize;

  if ((HwDataASize > MAX_UINT32) ||
      !ReserveAllocatedSystemMemoryRegion (HwDataABase, (UINT32)HwDataASize, ResourceAttributes))
  {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: hw_data_a: could not reserve 0x%lx/+0x%lx; GPU degraded\n", HwDataABase, HwDataASize));
  } else {
    DEBUG ((DEBUG_INFO, "AppleAgxGpu: hw_data_a: reserved 0x%lx/+0x%lx (SystemMemoryTop - 0x%lx)\n", HwDataABase, HwDataASize, HwDataASize));
  }

  if ((HwDataBSize > MAX_UINT32) ||
      !ReserveAllocatedSystemMemoryRegion (HwDataBBase, (UINT32)HwDataBSize, ResourceAttributes))
  {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: hw_data_b: could not reserve 0x%lx/+0x%lx; GPU degraded\n", HwDataBBase, HwDataBSize));
  } else {
    DEBUG ((DEBUG_INFO, "AppleAgxGpu: hw_data_b: reserved 0x%lx/+0x%lx\n", HwDataBBase, HwDataBSize));
  }

  if ((GlobalsSize > MAX_UINT32) ||
      !ReserveAllocatedSystemMemoryRegion (GlobalsBase, (UINT32)GlobalsSize, ResourceAttributes))
  {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: globals: could not reserve 0x%lx/+0x%lx; GPU degraded\n", GlobalsBase, GlobalsSize));
  } else {
    DEBUG ((DEBUG_INFO, "AppleAgxGpu: globals: reserved 0x%lx/+0x%lx\n", GlobalsBase, GlobalsSize));
  }
}
#endif // NTASI_J414S_GPU_RESOURCE_PROFILE

//Borrowed from ArmPlatformPkg
EFI_STATUS EFIAPI MemoryPeim(IN EFI_PHYSICAL_ADDRESS UefiMemoryBase, IN UINT64 UefiMemorySize)
{
  ARM_MEMORY_REGION_DESCRIPTOR  *MemoryTable;
  EFI_RESOURCE_ATTRIBUTE_TYPE   ResourceAttributes;
  UINT64                        ResourceLength;
  EFI_PEI_HOB_POINTERS          NextHob;
  EFI_PHYSICAL_ADDRESS          FdTop;
  EFI_PHYSICAL_ADDRESS          SystemMemoryTop;
  EFI_PHYSICAL_ADDRESS          ResourceTop;
  BOOLEAN                       Found;

  DEBUG((DEBUG_INFO, "%a: Building VirtualMemoryMap\n", __FUNCTION__));
  // build up virtual memory map
  BuildVirtualMemoryMap(&MemoryTable);

  // Ensure PcdSystemMemorySize has been set
  ASSERT (PcdGet64 (PcdSystemMemorySize) != 0);

  //
  // Now, the permanent memory has been installed, we can call AllocatePages()
  //

  DEBUG((DEBUG_INFO, "%a: Building VirtualMemoryMap\n", __FUNCTION__));
  ResourceAttributes = (
                        EFI_RESOURCE_ATTRIBUTE_PRESENT |
                        EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                        EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
                        EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
                        EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |
                        EFI_RESOURCE_ATTRIBUTE_TESTED
                        );

  DEBUG((DEBUG_INFO, "%a: Resource Attributes: 0x%lx\n", __FUNCTION__, ResourceAttributes));
  //
  // Check if the resource for the main system memory has been declared
  //
  Found       = FALSE;
  NextHob.Raw = GetHobList ();
  while ((NextHob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, NextHob.Raw)) != NULL) {
    if ((NextHob.ResourceDescriptor->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) &&
        (PcdGet64 (PcdSystemMemoryBase) >= NextHob.ResourceDescriptor->PhysicalStart) &&
        (NextHob.ResourceDescriptor->PhysicalStart + NextHob.ResourceDescriptor->ResourceLength <= PcdGet64 (PcdSystemMemoryBase) + PcdGet64 (PcdSystemMemorySize)))
    {
      Found = TRUE;
      break;
    }

    NextHob.Raw = GET_NEXT_HOB (NextHob);
  }

  if (!Found) {
    // Reserved the memory space occupied by the firmware volume
    BuildResourceDescriptorHob (
      EFI_RESOURCE_SYSTEM_MEMORY,
      ResourceAttributes,
      PcdGet64 (PcdSystemMemoryBase),
      PcdGet64 (PcdSystemMemorySize)
      );
  }

  //
  // Reserved the memory space occupied by the firmware volume
  //

  SystemMemoryTop = (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdSystemMemoryBase) + (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdSystemMemorySize);
  FdTop           = (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdFdBaseAddress) + (EFI_PHYSICAL_ADDRESS)PcdGet32 (PcdFdSize);

  // EDK2 does not have the concept of boot firmware copied into DRAM. To avoid the DXE
  // core to overwrite this area we must create a memory allocation HOB for the region,
  // but this only works if we split off the underlying resource descriptor as well.
  if ((PcdGet64 (PcdFdBaseAddress) >= PcdGet64 (PcdSystemMemoryBase)) && (FdTop <= SystemMemoryTop)) {
    Found = FALSE;

    // Search for System Memory Hob that contains the firmware
    NextHob.Raw = GetHobList ();
    while ((NextHob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, NextHob.Raw)) != NULL) {
      if ((NextHob.ResourceDescriptor->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) &&
          (PcdGet64 (PcdFdBaseAddress) >= NextHob.ResourceDescriptor->PhysicalStart) &&
          (FdTop <= NextHob.ResourceDescriptor->PhysicalStart + NextHob.ResourceDescriptor->ResourceLength))
      {
        ResourceAttributes = NextHob.ResourceDescriptor->ResourceAttribute;
        ResourceLength     = NextHob.ResourceDescriptor->ResourceLength;
        ResourceTop        = NextHob.ResourceDescriptor->PhysicalStart + ResourceLength;

        if (PcdGet64 (PcdFdBaseAddress) == NextHob.ResourceDescriptor->PhysicalStart) {
          if (SystemMemoryTop != FdTop) {
            // Create the System Memory HOB for the firmware
            BuildResourceDescriptorHob (
              EFI_RESOURCE_SYSTEM_MEMORY,
              ResourceAttributes,
              PcdGet64 (PcdFdBaseAddress),
              PcdGet32 (PcdFdSize)
              );

            // Top of the FD is system memory available for UEFI
            NextHob.ResourceDescriptor->PhysicalStart  += PcdGet32 (PcdFdSize);
            NextHob.ResourceDescriptor->ResourceLength -= PcdGet32 (PcdFdSize);
          }
        } else {
          // Create the System Memory HOB for the firmware
          BuildResourceDescriptorHob (
            EFI_RESOURCE_SYSTEM_MEMORY,
            ResourceAttributes,
            PcdGet64 (PcdFdBaseAddress),
            PcdGet32 (PcdFdSize)
            );

          // Update the HOB
          NextHob.ResourceDescriptor->ResourceLength = PcdGet64 (PcdFdBaseAddress) - NextHob.ResourceDescriptor->PhysicalStart;

          // If there is some memory available on the top of the FD then create a HOB
          if (FdTop < NextHob.ResourceDescriptor->PhysicalStart + ResourceLength) {
            // Create the System Memory HOB for the remaining region (top of the FD)
            BuildResourceDescriptorHob (
              EFI_RESOURCE_SYSTEM_MEMORY,
              ResourceAttributes,
              FdTop,
              ResourceTop - FdTop
              );
          }
        }

        // Mark the memory covering the Firmware Device as runtime services data
        BuildMemoryAllocationHob (
          PcdGet64 (PcdFdBaseAddress),
          PcdGet32 (PcdFdSize),
          EfiRuntimeServicesData
          );

        Found = TRUE;
        break;
      }

      NextHob.Raw = GET_NEXT_HOB (NextHob);
    }

    ASSERT (Found);
  }

  // MTP multitouch firmware staging carveout: published to Windows as the
  // fourth NTAS0050 _CRS memory resource (bus 0x1800000 via MTP DART stream 1)
  // and pre-mapped by the m1n1 preboot handoff.  The second megabyte is
  // the preboot RTKit buffer pool the MTP IOP keeps DMA-writing after
  // boot.  Reserve both so neither UEFI nor Windows ever allocates them.
  ReserveMemoryRegion (0x10020000000ULL, 0x200000);

  // Reserve only the intersection with advertised system RAM.  Bytes
  // in m1n1 proxy scratch are not allocatable HOB memory, but remain
  // reachable through the explicit cached identity mapping above.
  // Keep an in-RAM image as cacheable SystemMemory plus a reserved
  // allocation HOB so DXE can read it without allocating over it.
  if (mAppendedRamdiskCorrupt) {
    DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: invalid appended ramdisk header\n"));
    return EFI_COMPROMISED_DATA;
  }
  if (mAppendedRamdiskReservationSize != 0) {
    EFI_PHYSICAL_ADDRESS  AppendedTop;
    EFI_PHYSICAL_ADDRESS  ReserveBase;
    EFI_PHYSICAL_ADDRESS  ReserveTop;
    NTASI_APPENDED_RAMDISK_LOCATION  Location;

    AppendedTop = FdTop + mAppendedRamdiskReservationSize;
    ReserveBase = MAX (FdTop, PcdGet64 (PcdSystemMemoryBase));
    ReserveTop  = MIN (AppendedTop, SystemMemoryTop);
    if (ReserveTop > ReserveBase) {
      if (!ReserveAllocatedSystemMemoryRegion (
             ReserveBase,
             (UINT32)(ReserveTop - ReserveBase),
             ResourceAttributes
             ))
      {
        DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: cannot reserve appended ramdisk allocation\n"));
        return EFI_OUT_OF_RESOURCES;
      }
    }
    Location.Signature             = NTASI_APPENDED_RAMDISK_LOCATION_SIGNATURE;
    Location.Version               = NTASI_APPENDED_RAMDISK_LOCATION_VERSION;
    Location.StructureSize         = sizeof (Location);
    Location.HeaderPhysicalAddress = FdTop;
    Location.ReservationSize       = mAppendedRamdiskReservationSize;
    if (BuildGuidDataHob (
          &mNtasiAppendedRamdiskLocationHobGuid,
          &Location,
          sizeof (Location)
          ) == NULL)
    {
      DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: cannot publish appended ramdisk location HOB\n"));
      return EFI_OUT_OF_RESOURCES;
    }
    DEBUG ((DEBUG_INFO, "MemoryInitPeiLib: mapped appended ramdisk and published location HOB at 0x%lx (0x%lx bytes)\n", FdTop, mAppendedRamdiskReservationSize));
  }

  // Preserve the SID-1 DART tables installed by m1n1's J414s wireless
  // handoff.  Keep the range cacheable and CPU-readable so AppleDart can
  // validate it, while the allocation HOB prevents DXE/OS reuse.
#if NTASI_ENABLE_WIRELESS_DART_HANDOFF
  {
    EFI_PHYSICAL_ADDRESS  WirelessDartBase;
    UINT32                WirelessDartSize;

    WirelessDartBase = PcdGet64 (PcdAppleWirelessDartPageTableBase);
    WirelessDartSize = PcdGet32 (PcdAppleWirelessDartPageTableSize);
    if ((WirelessDartBase == 0) != (WirelessDartSize == 0)) {
      DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: incomplete wireless DART reservation 0x%lx/+0x%x\n", WirelessDartBase, WirelessDartSize));
      return EFI_INVALID_PARAMETER;
    }
    if (WirelessDartBase != 0) {
      if (!NtasiValidateWirelessHandoffV2 (
             WirelessDartBase,
             WirelessDartSize,
             SystemMemoryTop
             ))
      {
        DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: invalid wireless DART ABI v2 descriptor 0x%lx/+0x%x\n", WirelessDartBase, WirelessDartSize));
        return EFI_COMPROMISED_DATA;
      }

      // top_of_memory_alloc() removes this reservation from boot_args before
      // Mu. Publish it as cacheable RAM, then reserve its allocation so DXE
      // and Windows can validate it but can never reuse it.
      BuildResourceDescriptorHob (
        EFI_RESOURCE_SYSTEM_MEMORY,
        ResourceAttributes,
        WirelessDartBase,
        WirelessDartSize
        );
      BuildMemoryAllocationHob (
        WirelessDartBase,
        WirelessDartSize,
        EfiReservedMemoryType
        );
      DEBUG ((DEBUG_INFO, "MemoryInitPeiLib: authenticated and reserved wireless DART ABI v2 at 0x%lx (0x%x bytes)\n", WirelessDartBase, WirelessDartSize));
    }
  }
#endif // NTASI_ENABLE_WIRELESS_DART_HANDOFF

#if NTASI_J414S_GPU_RESOURCE_PROFILE
  //
  // AppleAgxGpu preboot reservations, derived live from the ADT and from
  // Mu's own observed memory geometry -- the way Asahi/m1n1 derive them --
  // instead of a hardcoded snapshot of one boot's addresses that goes
  // stale the moment the memory layout shifts. See NtasiReserveGpuAdtCarveout()
  // and NtasiReserveGpuHandoffData() above for the derivation and its
  // rationale. Every failure path there logs loudly and simply leaves that
  // one region unreserved; nothing here can return EFI_DEVICE_ERROR, because
  // PEI cannot recover or debug that (no console exists yet at this point
  // in boot) and a degraded GPU is infinitely better than a machine that
  // will not boot.
  //
  {
    dt_node_t  *SgxNode;

    SgxNode = dt_get ("sgx");
    if (SgxNode == NULL) {
      DEBUG ((DEBUG_ERROR, "AppleAgxGpu: \"sgx\" ADT node not found; all GPU preboot reservations skipped, GPU degraded\n"));
    } else {
      NtasiReserveGpuAdtCarveout (SgxNode, "gpu-region", "uat_ttbs", PcdGet64 (PcdSystemMemoryBase), SystemMemoryTop, ResourceAttributes);
      NtasiReserveGpuAdtCarveout (SgxNode, "gfx-shared-region", "uat_pagetables", PcdGet64 (PcdSystemMemoryBase), SystemMemoryTop, ResourceAttributes);
      NtasiReserveGpuAdtCarveout (SgxNode, "gfx-handoff", "uat_handoff", PcdGet64 (PcdSystemMemoryBase), SystemMemoryTop, ResourceAttributes);
    }

    NtasiReserveGpuHandoffData (PcdGet64 (PcdSystemMemoryBase), SystemMemoryTop, ResourceAttributes);
  }
#endif // NTASI_J414S_GPU_RESOURCE_PROFILE
  //reserve secondary stacks carveouts passed into cpm-impl-reg 
  for(int i = 0; i < PcdGet32(PcdCoreCount); i++){
    CHAR8 CpuNodeName[14];
    UINTN CarveoutLength = 0;

    AsciiSPrint(CpuNodeName, ARRAY_SIZE(CpuNodeName), "/cpus/cpu%d", i);
    dt_node_t *CpuNode = dt_get(CpuNodeName);
    if (CpuNode == NULL) {
      DEBUG((DEBUG_INFO, "Skipping absent CPU node %a\n", CpuNodeName));
      continue;
    }

    UINT32 *Carveout = (UINT32 *)dt_node_prop(CpuNode, "cpm-impl-reg", &CarveoutLength);
    if ((Carveout == NULL) || (CarveoutLength < (4 * sizeof (UINT32)))) {
      DEBUG((DEBUG_WARN, "CPU node %a has no valid cpm-impl-reg\n", CpuNodeName));
      continue;
    }

    ReserveMemoryRegion (
      ((UINT64)Carveout[1] << 32) | Carveout[0],
      ((UINT64)Carveout[3] << 32) | Carveout[2]
    );
  }

  // Build Memory Allocation Hob
  InitMmu (MemoryTable);

  if (FeaturePcdGet (PcdPrePiProduceMemoryTypeInformationHob)) {
    // Optional feature that helps prevent EFI memory map fragmentation.
    BuildMemoryTypeInformationHob ();
  }

  return EFI_SUCCESS;
}

/**
 * BuildVirtualMemoryMap
 * 
 * This will build up the memory map of the platform used to initialize the MMU and page tables.
 * 
 * @param VirtualMemoryMap - A pointer to a pointer to be used for page table setup.
 * 
 * does not return anything as the value will be stored in a pointer accessible by MemoryPeim.
 * 
 */
VOID BuildVirtualMemoryMap(OUT ARM_MEMORY_REGION_DESCRIPTOR **VirtualMemoryMap)
{
  ARM_MEMORY_REGION_ATTRIBUTES CacheAttributes;
  UINTN Index = 0;
  ARM_MEMORY_REGION_DESCRIPTOR *VirtualMemoryTable;

  //ensure we actually have a valid memory map pointer
  ASSERT(VirtualMemoryMap != NULL);

  DEBUG((DEBUG_INFO, "Allocating virtual memory table pages\n"));
  VirtualMemoryTable = (ARM_MEMORY_REGION_DESCRIPTOR *)AllocatePages (EFI_SIZE_TO_PAGES (sizeof (ARM_MEMORY_REGION_DESCRIPTOR) * MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS));
  if (VirtualMemoryTable == NULL) {
    DEBUG((DEBUG_INFO, "Unexpected failure to allocate VirtualMemoryTable\n"));
    return;
  }

  CacheAttributes = DDR_ATTRIBUTES_CACHED;

  /**
   * NOTE - On Apple silicon platforms, non PCIe MMIO regions *must* use nGnRnE mappings, 
   * while all PCIe regions *must* use nGnRE mappings.
   * by default EDK2 sets up the MMIO as nGnRnE, good for core system devices
   * though we will need to add an attribute for nGnRE mappings at some point.
   * 
   * TODO: add ARM_MEMORY_REGION_ATTRIBUTE_DEVICE_POSTED_WRITE
   **/

  //MMIO - PMGR/AIC/Core System Peripherals and PCIe
  VirtualMemoryTable[Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_1_BASE;
  VirtualMemoryTable[Index].VirtualBase  = APPLE_CORE_SYSTEM_MMIO_RANGE_1_BASE;
  VirtualMemoryTable[Index].Length       = APPLE_CORE_SYSTEM_MMIO_RANGE_1_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_2_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_2_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_2_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_3_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_3_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_3_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_1_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_1_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_1_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_2_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_2_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_2_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_4_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_4_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_4_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_3_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_3_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_3_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_4_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_4_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_4_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_5_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_5_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_5_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_5_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_5_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_5_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_6_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_6_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_6_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_6_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_6_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_6_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_7_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_7_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_7_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_8_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_8_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_8_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_7_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_7_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_7_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_9_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_9_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_9_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_10_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_10_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_10_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  //
  // HACK: shoving in the two ranges that aren't on M1 Pro/Max/Ultra - this code has to get mega refactored later...
  //

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_22_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_22_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_22_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_21_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_21_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_21_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_8_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_8_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_8_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_9_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_9_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_9_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_10_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_10_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_10_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_11_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_11_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_11_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_12_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_12_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_12_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_11_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_11_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_11_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_13_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_13_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_13_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_14_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_14_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_14_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_12_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_12_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_12_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_15_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_15_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_15_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_16_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_16_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_16_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_13_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_13_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_13_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_17_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_17_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_17_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_18_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_18_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_18_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_14_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_14_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_14_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_19_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_19_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_19_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_20_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_20_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_20_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_15_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_15_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_15_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_16_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_16_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_16_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  // Inspect the header while the MMU is still off.  HV.load_raw normally
  // puts the FD and append inside BootArgs/Pcd RAM, already covered by
  // the ordinary DRAM descriptor.  Add a cached identity map only for
  // an exceptional placement outside that span.
  {
    EFI_PHYSICAL_ADDRESS                 AppendedFdTop;
    EFI_PHYSICAL_ADDRESS                 AppendedTop;
    CONST NTASI_APPENDED_RAMDISK_HEADER  *AppendedHeader;
    EFI_PHYSICAL_ADDRESS                 MapSystemTop;

    mAppendedRamdiskCorrupt         = FALSE;
    mAppendedRamdiskReservationSize = 0;
    AppendedFdTop = PcdGet64 (PcdFdBaseAddress) + PcdGet32 (PcdFdSize);
    if (AppendedFdTop >= PcdGet64 (PcdFdBaseAddress)) {
      AppendedHeader = (CONST NTASI_APPENDED_RAMDISK_HEADER *)(UINTN)AppendedFdTop;
      if (AppendedHeader->Signature == NTASI_APPENDED_RAMDISK_SIGNATURE) {
        if (!NtasiValidateAppendedRamdisk (
               AppendedHeader,
               NTASI_APPENDED_RAMDISK_MAX_MAPPED_SPAN,
               FALSE,
               NULL,
               NULL,
               &mAppendedRamdiskReservationSize
               ))
        {
          mAppendedRamdiskCorrupt = TRUE;
        } else {
          AppendedTop = AppendedFdTop + mAppendedRamdiskReservationSize;
          MapSystemTop = PcdGet64 (PcdSystemMemoryBase) +
                         PcdGet64 (PcdSystemMemorySize);
          if ((AppendedTop < AppendedFdTop) ||
              (MapSystemTop < PcdGet64 (PcdSystemMemoryBase)))
          {
            mAppendedRamdiskCorrupt = TRUE;
          } else if ((AppendedFdTop < PcdGet64 (PcdSystemMemoryBase)) ||
                     (AppendedTop > MapSystemTop))
          {
            ASSERT ((Index + 4) <= MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS);
            VirtualMemoryTable[++Index].PhysicalBase = AppendedFdTop;
            VirtualMemoryTable[Index].VirtualBase    = AppendedFdTop;
            VirtualMemoryTable[Index].Length         = mAppendedRamdiskReservationSize;
            VirtualMemoryTable[Index].Attributes     = CacheAttributes;
          }
        }
      }
    }
  }

  //System DRAM
  if (NTASI_ENABLE_WIRELESS_DART_HANDOFF) {
    EFI_PHYSICAL_ADDRESS  WirelessDartBase;
    UINT32                WirelessDartSize;
    EFI_PHYSICAL_ADDRESS  MapSystemTop;

    WirelessDartBase = PcdGet64 (PcdAppleWirelessDartPageTableBase);
    WirelessDartSize = PcdGet32 (PcdAppleWirelessDartPageTableSize);
    MapSystemTop = PcdGet64 (PcdSystemMemoryBase) + PcdGet64 (PcdSystemMemorySize);
    if ((WirelessDartBase != 0) && (WirelessDartSize != 0) &&
        ((WirelessDartBase < PcdGet64 (PcdSystemMemoryBase)) ||
         (WirelessDartBase + WirelessDartSize > MapSystemTop)))
    {
      ASSERT ((Index + 3) <= MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS);
      VirtualMemoryTable[++Index].PhysicalBase = WirelessDartBase;
      VirtualMemoryTable[Index].VirtualBase    = WirelessDartBase;
      VirtualMemoryTable[Index].Length         = WirelessDartSize;
      VirtualMemoryTable[Index].Attributes     = CacheAttributes;
    }
  }

  VirtualMemoryTable[++Index].PhysicalBase = PcdGet64(PcdSystemMemoryBase);
  VirtualMemoryTable[Index].VirtualBase    = PcdGet64(PcdSystemMemoryBase);
  VirtualMemoryTable[Index].Length         = PcdGet64(PcdSystemMemorySize);
  VirtualMemoryTable[Index].Attributes     = CacheAttributes;


  DEBUG ((
    DEBUG_ERROR,
    "%a: Dumping System DRAM Memory Map:\n"
    "\tPhysicalBase: 0x%lX\n"
    "\tVirtualBase: 0x%lX\n"
    "\tLength: 0x%lX\n"
    "\tTop of system RAM: 0x%lX\n",
    __FUNCTION__,
    VirtualMemoryTable[Index].PhysicalBase,
    VirtualMemoryTable[Index].VirtualBase,
    VirtualMemoryTable[Index].Length,
    VirtualMemoryTable[Index].PhysicalBase + VirtualMemoryTable[Index].Length
    ));

  //Framebuffer
  VirtualMemoryTable[++Index].PhysicalBase = PcdGet64(PcdFrameBufferAddress);
  VirtualMemoryTable[Index].VirtualBase    = PcdGet64(PcdFrameBufferAddress);
  VirtualMemoryTable[Index].Length         = PcdGet64(PcdFrameBufferSize);
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;

  DEBUG ((
    DEBUG_ERROR,
    "%a: Dumping Framebuffer Memory Map:\n"
    "\tPhysicalBase: 0x%lX\n"
    "\tVirtualBase: 0x%lX\n"
    "\tLength: 0x%lX\n"
    "\tTop of framebuffer RAM: 0x%lX\n",
    __FUNCTION__,
    VirtualMemoryTable[Index].PhysicalBase,
    VirtualMemoryTable[Index].VirtualBase,
    VirtualMemoryTable[Index].Length,
    VirtualMemoryTable[Index].PhysicalBase + VirtualMemoryTable[Index].Length
    ));

  //TODO: add other NC regions here?

  // End of Table
  VirtualMemoryTable[++Index].PhysicalBase  = 0;
  VirtualMemoryTable[Index].VirtualBase     = 0;
  VirtualMemoryTable[Index].Length          = 0;
  VirtualMemoryTable[Index].Attributes      = (ARM_MEMORY_REGION_ATTRIBUTES)0;

  ASSERT((Index + 1) <= MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS);

  *VirtualMemoryMap = VirtualMemoryTable;
}

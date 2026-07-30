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

// Bumped from 44 on 2026-07-30 to make room for
// APPLE_CORE_SYSTEM_MMIO_RANGE_17 (the /arm-io/ans MMIO-gap fix) without
// crowding the existing headroom for the two conditional identity-map
// entries (appended ramdisk, wireless DART) below.
#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS 48
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

//
// Derive the wireless SID-1 reservation live at boot, the way SystemMemoryTop
// itself is already derived -- never from a hand-picked constant. Baking a
// chosen carveout address into a profile is exactly the mistake that
// produced the GPU PEI crash earlier tonight (a hardcoded reservation that
// went stale and landed on Mu's own live stack).
//
// m1n1's wlan_validate_reservation() (src/wireless_handoff.c, not editable
// from here) requires: size exactly NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE,
// base 0x4000-aligned, base >= guest_top + 16KiB where guest_top =
// boot_args.phys_base + boot_args.mem_size (== PcdSystemMemoryBase +
// PcdSystemMemorySize == SystemMemoryTop, already computed by the caller),
// and base + size <= ALIGN_DOWN(boot_args.phys_base, 4GiB) + mem_size_actual.
// mem_size_actual is not exposed via PcdSystemMemorySize (that is boot_args'
// deliberately-smaller mem_size) but the full boot_args struct is already
// copied to PcdBootArgsPointer during SEC/PrePi's EarlySetup(), so it is
// read directly here -- the same revision-dispatch SmbiosInfoDxe.c's
// GetInstalledMemoryBytes() already uses successfully in DXE, reused
// verbatim rather than reinvented.
//
// The reservation is placed at the top of the mem_size_actual-derived
// window (PhysTop - size, 0x4000-aligned) rather than counted up from
// guest_top+16KiB: mem_size_actual describes the same "headroom above
// boot_args mem_size" pool m1n1's own top_of_memory_alloc() already carves
// iBoot/GPU/wireless carveouts from, so anchoring on it -- instead of on
// raw physical DRAM capacity, which runs into iBoot's own even-higher
// GUAT/CARV firmware carveouts -- is what keeps this derivation inside the
// pool m1n1 actually set aside for this purpose.
//
// UNVERIFIED CAVEAT, stated plainly: Mu has no ADT- or boot_args-visible
// way to learn the exact bounds of the TZ0/TZ1 TrustZone carveouts that
// also live inside [guest_top, PhysTop) on this hardware (m1n1 reads them
// from privileged MCC hardware registers -- see m1n1/src/mcc.c
// mcc_unmap_carveouts() -- which are not exposed to a guest at any
// privilege level Mu runs at). This derivation can only satisfy m1n1's
// own numeric validator; it cannot independently prove the chosen address
// avoids a TZ carveout the way the ANS/GPU fixes earlier tonight could
// prove their addresses were correct against a live ADT read. Both m1n1
// and Mu computing the *same* formula from the *same* boot_args inputs is
// what makes this safe in practice, not anything Mu can verify alone.
//
STATIC
BOOLEAN
NtasiDeriveWirelessReservation (
  IN  EFI_PHYSICAL_ADDRESS  SystemMemoryBase,
  IN  EFI_PHYSICAL_ADDRESS  SystemMemoryTop,
  OUT EFI_PHYSICAL_ADDRESS  *ReservationBase,
  OUT UINT32                *ReservationSize
  )
{
  CONST struct boot_args  *BootArgs;
  UINT64                   MemSizeActual;
  UINT64                   PhysTop;
  UINT64                   GuestTopWithMargin;
  UINT64                   CandidateBase;

  *ReservationBase = 0;
  *ReservationSize = 0;

  BootArgs = (CONST struct boot_args *)(UINTN)FixedPcdGet64 (PcdBootArgsPointer);
  if (BootArgs == NULL) {
    DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: wireless: no boot_args at PcdBootArgsPointer; wireless withheld\n"));
    return FALSE;
  }

  // Matches SmbiosInfoDxe.c's GetInstalledMemoryBytes() union-variant
  // dispatch exactly.
  MemSizeActual = 0;
  switch (BootArgs->revision) {
    case 1:
      MemSizeActual = BootArgs->rv1.mem_size_actual;
      break;
    case 2:
      MemSizeActual = BootArgs->rv2.mem_size_actual;
      break;
    case 3:
      MemSizeActual = BootArgs->rv3.mem_size_actual;
      break;
    default:
      DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: wireless: unknown boot_args revision %u; wireless withheld\n", BootArgs->revision));
      return FALSE;
  }

  if ((MemSizeActual == 0) || (MemSizeActual > (1ULL << 40))) {
    DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: wireless: boot_args mem_size_actual unusable (0x%lx); wireless withheld\n", MemSizeActual));
    return FALSE;
  }

  PhysTop = (SystemMemoryBase & ~(SIZE_4GB - 1)) + MemSizeActual;

  if (PhysTop <= NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE) {
    DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: wireless: computed PhysTop 0x%lx too small; wireless withheld\n", PhysTop));
    return FALSE;
  }

  CandidateBase = (PhysTop - NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE) &
                  ~(NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE - 1);

  GuestTopWithMargin = SystemMemoryTop + SIZE_16KB;

  if (CandidateBase < GuestTopWithMargin) {
    DEBUG ((
      DEBUG_ERROR,
      "MemoryInitPeiLib: wireless: derived base 0x%lx is below guest_top+16KiB 0x%lx; wireless withheld\n",
      CandidateBase,
      GuestTopWithMargin
      ));
    return FALSE;
  }

  if ((CandidateBase & (NTASI_WIRELESS_HANDOFF_V2_PAGE_SIZE - 1)) != 0) {
    DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: wireless: derived base 0x%lx is not 16KiB-aligned; wireless withheld\n", CandidateBase));
    return FALSE;
  }

  if (CandidateBase + NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE > PhysTop) {
    DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: wireless: derived reservation 0x%lx/+0x%lx exceeds PhysTop 0x%lx; wireless withheld\n", CandidateBase, NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE, PhysTop));
    return FALSE;
  }

  *ReservationBase = CandidateBase;
  *ReservationSize = (UINT32)NTASI_WIRELESS_HANDOFF_V2_RESERVATION_SIZE;

  DEBUG ((
    DEBUG_INFO,
    "MemoryInitPeiLib: wireless: derived reservation 0x%lx/+0x%x (guest_top=0x%lx, mem_size_actual=0x%lx, PhysTop=0x%lx)\n",
    *ReservationBase,
    *ReservationSize,
    SystemMemoryTop,
    MemSizeActual,
    PhysTop
    ));
  return TRUE;
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

#if NTASI_ENABLE_WIRELESS_DART_HANDOFF
  //
  // Derive (never hardcode -- see NtasiDeriveWirelessReservation()'s own
  // comment) and publish the wireless reservation PCDs *before*
  // BuildVirtualMemoryMap() runs: that function's own out-of-window
  // identity-map logic for this same reservation reads these PCDs, and it
  // is called (below) before the rest of this function would otherwise
  // compute SystemMemoryTop. PatchPcdSet64/32 leave the PCDs at 0 (their
  // default) on any failure, which both BuildVirtualMemoryMap() and the
  // validate-and-reserve stage further down already treat as "wireless
  // withheld this boot" -- never a fatal PEI status.
  //
  {
    EFI_PHYSICAL_ADDRESS  EarlySystemMemoryTop;
    EFI_PHYSICAL_ADDRESS  EarlyWirelessDartBase;
    UINT32                EarlyWirelessDartSize;

    EarlySystemMemoryTop = (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdSystemMemoryBase) +
                           (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdSystemMemorySize);
    if (NtasiDeriveWirelessReservation (
          PcdGet64 (PcdSystemMemoryBase),
          EarlySystemMemoryTop,
          &EarlyWirelessDartBase,
          &EarlyWirelessDartSize
          ))
    {
      PatchPcdSet64 (PcdAppleWirelessDartPageTableBase, EarlyWirelessDartBase);
      PatchPcdSet32 (PcdAppleWirelessDartPageTableSize, EarlyWirelessDartSize);
    }
  }
#endif // NTASI_ENABLE_WIRELESS_DART_HANDOFF

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
  //
  // Every failure path below logs loudly and leaves wireless withheld
  // (PcdAppleWirelessDartPageTableBase/Size stay 0, which
  // AcpiPlatformDxe's NtasiInstallWirelessDartTable() reads as "do not
  // publish DRT0") instead of returning a fatal PEI status: a validation
  // mismatch here used to return EFI_COMPROMISED_DATA/EFI_INVALID_PARAMETER,
  // which PrePi.c's caller treats as fatal (CpuDeadLoop()) -- exactly the
  // "no console, no recovery" hang class every other fix tonight has been
  // eliminating. A wireless adapter that never enumerates is infinitely
  // better than a machine that will not boot.
#if NTASI_ENABLE_WIRELESS_DART_HANDOFF
  {
    EFI_PHYSICAL_ADDRESS  WirelessDartBase;
    UINT32                WirelessDartSize;

    // Derivation already ran above, before BuildVirtualMemoryMap() (that
    // function's own out-of-window identity-map logic needs the PCDs set
    // before it runs). Read back what it published rather than deriving a
    // second time; zero means derivation declined this boot (already
    // logged) and there is nothing further to do here.
    WirelessDartBase = PcdGet64 (PcdAppleWirelessDartPageTableBase);
    WirelessDartSize = PcdGet32 (PcdAppleWirelessDartPageTableSize);
    if (WirelessDartBase != 0) {
      DEBUG ((DEBUG_INFO, "MemoryInitPeiLib: wireless: stage \"validate-descriptor\"\n"));
      if (!NtasiValidateWirelessHandoffV2 (
             WirelessDartBase,
             WirelessDartSize,
             SystemMemoryTop
             ))
      {
        DEBUG ((
          DEBUG_ERROR,
          "MemoryInitPeiLib: wireless: invalid ABI v2 descriptor at derived 0x%lx/+0x%x; "
          "either m1n1 did not honor this exact address or the handoff was not installed "
          "this boot; wireless withheld\n",
          WirelessDartBase,
          WirelessDartSize
          ));
        // Do not leave a stale, unauthenticated base/size published for
        // DXE to trust.
        PatchPcdSet64 (PcdAppleWirelessDartPageTableBase, 0);
        PatchPcdSet32 (PcdAppleWirelessDartPageTableSize, 0);
      } else {
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
        DEBUG ((DEBUG_INFO, "MemoryInitPeiLib: wireless: authenticated and reserved ABI v2 at 0x%lx (0x%x bytes)\n", WirelessDartBase, WirelessDartSize));
      }
    }
  }
#endif // NTASI_ENABLE_WIRELESS_DART_HANDOFF

  //
  // AppleAgxGpu preboot reservations used to be computed here (PEI has no
  // console and, as of this boot, no installed exception vector table --
  // VBAR_EL1 is zero -- so a fault here is silent and unrecoverable by
  // construction). Moved to DXE on 2026-07-30 after two rounds of PEI-only
  // fixes here still produced an unreported early-PEI crash at an
  // unchanged stack pointer: NtasiResolveAndReserveGpuCarveouts() in
  // AcpiPlatform.c now does this same ADT-derived, safety-guarded
  // reservation late in DXE dispatch (after console, AIC2, and CpuDxe's
  // exception vectors are all up), where a bug produces a diagnosable
  // fault or a logged failure instead of 0 bytes of UART output.
  //
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

  // Plugs the [0x2C0000000, 0x380000000) gap that left /arm-io/ans's ASC/
  // SART/NVMe apertures unmapped -- see the comment on
  // APPLE_CORE_SYSTEM_MMIO_RANGE_17_BASE in
  // T602XFamilyVirtualMemoryMapDefines.h for the 2026-07-30 hardware
  // incident this fixes.
  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_17_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_17_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_17_SIZE;
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

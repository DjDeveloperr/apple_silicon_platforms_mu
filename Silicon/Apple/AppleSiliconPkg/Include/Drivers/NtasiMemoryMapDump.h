/** @file
  Read-only dump of every non-conventional descriptor in the UEFI memory map.

  WHY THIS EXISTS (2026-07-30). A BUGCODE_USB3_DRIVER 0x144 with
  Arg1 = 2 (USB3_BUGCODE_BOOT_DEVICE_FAILED, boot device failed
  re-enumeration) was captured on J414s with XHC1 halted, USBSTS reporting
  HSE (Host System Error) and DWC3 GSTS reporting buserr_valid = 1. HSE is
  raised when the host bus rejects the controller's DMA, so the boot disk did
  not "go away" -- its controller took a DMA/fabric fault.

  The failure correlated with firmware profiles, and firmware's most
  plausible route to a DMA fault is the memory map it hands the OS: a region
  the OS believes is usable but the fabric (or the m1n1 hypervisor's stage-2
  mapping) treats otherwise produces exactly that signature. Reasoning about
  that from source is not good enough -- the map has to be observed.

  This prints every descriptor that is NOT EfiConventionalMemory, which is
  the complete set of regions firmware asked the OS to treat specially, plus
  a total of conventional memory. Purely observational: it allocates a
  scratch buffer for GetMemoryMap and frees it, and changes nothing else.

  Do NOT call this from an ExitBootServices callback -- allocation is
  forbidden there and GetMemoryMap would perturb the very map being handed
  over. Call it late in DXE instead; nothing between that point and the
  handoff changes the reserved regions this is looking for.

  SPDX-License-Identifier: MIT
**/

#ifndef NTASI_MEMORY_MAP_DUMP_H_
#define NTASI_MEMORY_MAP_DUMP_H_

#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC
inline
CONST CHAR8 *
NtasiMemoryTypeName (
  IN UINT32  Type
  )
{
  switch (Type) {
    case EfiReservedMemoryType:      return "Reserved";
    case EfiLoaderCode:              return "LoaderCode";
    case EfiLoaderData:              return "LoaderData";
    case EfiBootServicesCode:        return "BootSvcCode";
    case EfiBootServicesData:        return "BootSvcData";
    case EfiRuntimeServicesCode:     return "RtSvcCode";
    case EfiRuntimeServicesData:     return "RtSvcData";
    case EfiConventionalMemory:      return "Conventional";
    case EfiUnusableMemory:          return "Unusable";
    case EfiACPIReclaimMemory:       return "ACPIReclaim";
    case EfiACPIMemoryNVS:           return "ACPINvs";
    case EfiMemoryMappedIO:          return "MMIO";
    case EfiMemoryMappedIOPortSpace: return "MMIOPort";
    case EfiPalCode:                 return "PalCode";
    case EfiPersistentMemory:        return "Persistent";
    default:                         return "Unknown";
  }
}

/**
  Log every non-conventional memory descriptor, plus totals.

  @param[in] Tag  Short subsystem tag for the log lines.
**/
STATIC
inline
VOID
NtasiDumpReservedMemoryMap (
  IN CONST CHAR8  *Tag
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *Entry;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;
  UINTN                  Offset;
  UINTN                  Printed;
  UINT64                 ConventionalPages;
  UINT64                 HighestEnd;
  UINT64                 End;

  Map               = NULL;
  MapSize           = 0;
  MapKey            = 0;
  DescriptorSize    = 0;
  DescriptorVersion = 0;

  Status = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_ERROR, "%a: memory map: sizing GetMemoryMap failed: %r\n", Tag, Status));
    return;
  }

  //
  // Allocating grows the map; ask for generous slack so the second call does
  // not fail, and never loop (this is diagnostics, not a critical path).
  //
  MapSize += 16 * DescriptorSize;
  Map      = AllocatePool (MapSize);
  if (Map == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: memory map: cannot allocate %Lu bytes of scratch\n", Tag, (UINT64)MapSize));
    return;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: memory map: GetMemoryMap failed: %r\n", Tag, Status));
    FreePool (Map);
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: memory map: %Lu descriptors, non-conventional regions follow "
    "(anything NOT listed here is Conventional, i.e. free for the OS)\n",
    Tag,
    (UINT64)(MapSize / DescriptorSize)
    ));

  Printed           = 0;
  ConventionalPages = 0;
  HighestEnd        = 0;
  for (Offset = 0; Offset + DescriptorSize <= MapSize; Offset += DescriptorSize) {
    Entry = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);

    //
    // Track the top across EVERY descriptor, conventional included. With the
    // USB DARTs in full bypass (AppleDartIoMmuDxe writes TCR = BYPASS_DART |
    // BYPASS_DAPF to all 16 SIDs and installs no IOMMU protocol), XHC DMA is
    // raw physical -- so "what is the highest physical address firmware told
    // the OS exists?" is the single number most likely to differ between a
    // profile that boots and one that dies on USBSTS.HSE. It would be
    // invisible if only non-conventional regions were printed.
    //
    End = Entry->PhysicalStart + LShiftU64 (Entry->NumberOfPages, EFI_PAGE_SHIFT);
    if (End > HighestEnd) {
      HighestEnd = End;
    }

    if (Entry->Type == EfiConventionalMemory) {
      ConventionalPages += Entry->NumberOfPages;
      continue;
    }

    DEBUG ((
      DEBUG_INFO,
      "%a: memory map:   [0x%016Lx, 0x%016Lx) %Lu pages %-12a attr=0x%Lx\n",
      Tag,
      Entry->PhysicalStart,
      Entry->PhysicalStart + LShiftU64 (Entry->NumberOfPages, EFI_PAGE_SHIFT),
      (UINT64)Entry->NumberOfPages,
      NtasiMemoryTypeName (Entry->Type),
      Entry->Attribute
      ));
    Printed++;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: memory map: %Lu non-conventional regions; %Lu conventional pages (0x%Lx bytes) free for the OS; "
    "highest described physical address 0x%Lx\n",
    Tag,
    (UINT64)Printed,
    ConventionalPages,
    LShiftU64 (ConventionalPages, EFI_PAGE_SHIFT),
    HighestEnd
    ));

  FreePool (Map);
}

#endif // NTASI_MEMORY_MAP_DUMP_H_

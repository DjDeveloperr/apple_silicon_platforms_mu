/** @file
 * Copyright (c) 2023, amarioguy (AppleWOA authors).
 * 
 * Module Name:
 *  AcpiPlatformDxe.c
 * 
 * Abstract:
 *  ACPI platform driver. Installs ACPI tables for the platform (device specific and SoC general)
 *  Based off the sample driver in MdeModulePkg.
 * 
 * Environment:
 *  UEFI Driver Execution Environment (DXE)/UEFI boot services
 * 
 * License:
 *  SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
 * 
**/

#include <PiDxe.h>

#include <Protocol/AcpiTable.h>
#include <Protocol/FirmwareVolume2.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/AppleDTLib.h>
#include <Library/AmlLib/AmlLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>

#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/J414sWirelessHandoff.h>
#include <Drivers/AppleAnsHardware.h>
//
// Moved out of this directory on 2026-07-30 so AppleNANDStorageDxe can share
// the one copy instead of a second module growing its own PMGR resolution.
// AppleAnsPmgrDomain.h is the EDK2-side wrapper; it includes the
// dependency-free arithmetic header the host test compiles verbatim.
//
#include <Drivers/AppleAnsPmgrDomain.h>
#include <Drivers/NtasiMemoryMapDump.h>

#define APPLE_ANS_ACPI_OEM_ID        "NTASP "
#define APPLE_ANS_ACPI_OEM_TABLE_ID  "APPLEANS"
#define APPLE_ANS_PMGR_RESET_SIZE     sizeof (UINT32)

STATIC CONST CHAR8  mAppleAnsAcpiTag[] = "AppleANS ACPI";

//
// ACPI 6.4 s6.2.5 device-properties UUID, daffd814-6eba-4d8c-8a91-bc9bbf4aa301.
//
STATIC CONST EFI_GUID  gAppleAnsDsdPropertiesGuid = {
  0xdaffd814, 0x6eba, 0x4d8c,
  { 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01 }
};

#if NTASI_ENABLE_WIRELESS_DART_HANDOFF
STATIC CONST EFI_GUID  mNtasiWirelessDartReservationHobGuid =
  NTASI_WIRELESS_DART_RESERVATION_HOB_GUID;
#endif

STATIC
BOOLEAN
AppleAnsBoundedContains (
  IN CONST CHAR8 *Haystack,
  IN UINTN       HaystackLength,
  IN CONST CHAR8 *Needle
  )
{
  UINTN  NeedleLength;
  UINTN  Offset;

  NeedleLength = AsciiStrLen (Needle);
  if ((NeedleLength == 0) || (NeedleLength > HaystackLength)) {
    return FALSE;
  }

  for (Offset = 0; Offset <= HaystackLength - NeedleLength; Offset++) {
    if (AsciiStrnCmp (Haystack + Offset, Needle, NeedleLength) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
BOOLEAN
AppleAnsPropertyContains (
  IN dt_node_t   *Node,
  IN CONST CHAR8 *Property,
  IN CONST CHAR8 *Needle
  )
{
  CHAR8  *Value;
  UINTN  Size;
  UINTN  Offset;

  Value = dt_node_prop (Node, Property, &Size);
  if (Value == NULL) {
    return FALSE;
  }

  for (Offset = 0; Offset < Size;) {
    UINTN Length = AsciiStrnLenS (Value + Offset, Size - Offset);

    if (AppleAnsBoundedContains (Value + Offset, Length, Needle)) {
      return TRUE;
    }

    Offset += Length + 1;
  }

  return FALSE;
}

STATIC
EFI_STATUS
AppleAnsAddMemoryResource (
  IN AML_OBJECT_NODE_HANDLE  CrsNode,
  IN UINT64                  Base,
  IN UINT64                  Length
  )
{
  if ((Length == 0) || (Base > MAX_UINT64 - (Length - 1))) {
    return EFI_INVALID_PARAMETER;
  }

  return AmlCodeGenRdQWordMemory (
           TRUE,                       // ResourceConsumer
           TRUE,                       // PosDecode
           TRUE,                       // MinFixed
           TRUE,                       // MaxFixed
           AmlMemoryNonCacheable,
           TRUE,                       // ReadWrite
           0,
           Base,
           Base + Length - 1,
           0,
           Length,
           0,
           NULL,
           AmlAddressRangeMemory,
           TRUE,
           CrsNode,
           NULL
           );
}


#if NTASI_J414S_GPU_RESOURCE_PROFILE
#include "NtasiGpuReservationGuard.h"

//
// AppleAgxGpu's _CRS is eight memory resources in a FIXED order, matched
// POSITIONALLY by the driver (ntasi_agx_t6020_resources_validate() in
// drivers/AppleAgxGpu/cores/agx-resource-core/agx_resource.c). Nothing may be
// inserted, removed or reordered here without changing that function.
//
//   0 ASC   1 SGX   2 uat_ttbs   3 uat_pagetables
//   4 uat_handoff   5 hw_data_a  6 hw_data_b      7 globals
//
#define NTASI_GPU_RES_ASC          0
#define NTASI_GPU_RES_SGX          1
#define NTASI_GPU_RES_TTBS         2
#define NTASI_GPU_RES_PAGETABLES   3
#define NTASI_GPU_RES_HANDOFF      4
#define NTASI_GPU_RES_HWDATA_A     5
#define NTASI_GPU_RES_HWDATA_B     6
#define NTASI_GPU_RES_GLOBALS      7
#define NTASI_GPU_RES_COUNT        8

//
// Resources 0 and 1 are MMIO, and the driver validates them against these
// EXACT values -- start AND length -- returning ERR_FIXED for anything else.
// They are therefore a driver ABI constant, not a free derivation: publishing
// the live ADT's own window lengths (gfx-asc reg[0] is 0x6C000, sgx reg[0] is
// 0x100000) would be more "truthful" and would be REFUSED by the driver.
//
// So they are hardcoded here, and then PROVEN against the live ADT before
// anything is published -- NtasiGpuMmioWindowsAgreeWithAdt() below. That is
// the difference between this and the GPU.asl these replaced: a constant that
// is checked against the machine, rather than a constant that is trusted.
//
// SGX deliberately spans 16 MiB rather than sgx's own two reg windows: the
// driver reaches the GPU PMGR page at SgxBase + 0xE80000 (= 0x404E80000,
// /arm-io/pmgr reg[44]) through it, which is outside /arm-io/sgx's reg but
// inside this span. That over-claim is why the check below is "contains the
// real windows", not "equals" them.
//
#define NTASI_GPU_ASC_BASE   0x406400000ULL
#define NTASI_GPU_ASC_SIZE   0x40000ULL
#define NTASI_GPU_SGX_BASE   0x404000000ULL
#define NTASI_GPU_SGX_SIZE   0x1000000ULL

//
// Sizes the driver pins for resources 5-7 (ERR_SIZE otherwise), and the 16 KiB
// alignment it requires of resources 2-7 (ERR_ALIGN otherwise).
//
#define NTASI_GPU_HWDATA_A_SIZE  0x8000ULL
#define NTASI_GPU_HWDATA_B_SIZE  0x4000ULL
#define NTASI_GPU_GLOBALS_SIZE   0x18000ULL
#define NTASI_GPU_PAGE_SIZE      0x4000ULL

//
// The granularity the DXE core forces on EfiReservedMemoryType -- 64 KiB on
// AArch64 (MdePkg/Include/AArch64/ProcessorBind.h). Named from the MdePkg
// macro rather than written as 0x10000 so a __DEPRECATED_AARCH64_4K_RUNTIME_
// GRANULARITY build, or another architecture, tracks automatically.
//
#define NTASI_GPU_RESERVED_GRANULARITY  ((UINT64)RUNTIME_PAGE_ALLOCATION_GRANULARITY)

//
// BUILD-TIME GUARD, added 2026-07-31 with the fix for the deadloop this file
// caused on its first boot. It states the two facts that made
// AllocateAlignedReservedPages() fatal here, so a future edit that changes a
// blob size or the requested alignment fails the BUILD instead of wedging a
// machine in DXE:
//
//   1. The alignment the driver needs must be no coarser than the granularity
//      the core already gives reserved memory for free. If it ever became
//      coarser, an alignment wrapper would be needed again -- and the only one
//      MdePkg offers is the one that deadloops on reserved memory, so that
//      must be a conscious decision, not a silent inheritance.
//   2. The three published sizes are individually 16 KiB aligned, which is
//      what makes carving one block into three driver-legal regions valid.
//
// It deliberately does NOT require the total to be a whole number of 64 KiB
// granules -- it is not (0x24000), and that is fine now that the allocation is
// explicitly rounded up rather than handed to a wrapper that trims the
// remainder back with a free the core rejects.
//
STATIC_ASSERT (
  NTASI_GPU_PAGE_SIZE <= NTASI_GPU_RESERVED_GRANULARITY,
  "the GPU _CRS alignment is coarser than the reserved-memory granularity; a plain "
  "AllocateReservedPages() no longer satisfies it, and AllocateAlignedReservedPages() "
  "ASSERT_EFI_ERRORs on the free-back (MemoryAllocationLib.c:222 via Page.c:1938)"
  );
STATIC_ASSERT (
  (NTASI_GPU_HWDATA_A_SIZE % NTASI_GPU_PAGE_SIZE) == 0 &&
  (NTASI_GPU_HWDATA_B_SIZE % NTASI_GPU_PAGE_SIZE) == 0 &&
  (NTASI_GPU_GLOBALS_SIZE % NTASI_GPU_PAGE_SIZE) == 0,
  "hw_data_a/hw_data_b/globals must each be a whole number of 16 KiB pages or carving "
  "one block into three leaves a misaligned region the driver refuses (ERR_ALIGN)"
  );

//
// Published GSIV for the AGX ASC mailbox doorbell. The physical line is AIC
// 1146 (/arm-io/gfx-asc interrupts[2]), which is above the GIC carrier's 1019
// limit and is illegal as a GSIV, so the CSRT ALI2 tail translates 46 -> 1146.
// CSRT.aslc carries that entry under this same NTASI_J414S_GPU_RESOURCE_PROFILE
// flag, so the two cannot get out of step.
//
// 46, NOT 40: 40 is the media profile's admac-sio. See the CSRT.aslc header.
//
#define NTASI_GPU_PUBLISHED_GSIV  46
#define NTASI_GPU_PHYSICAL_AIC    1146

typedef struct {
  UINT64     Base;
  UINT64     Size;
  BOOLEAN    Resolved;
} NTASI_GPU_REGION;

typedef struct {
  //
  // Indexed by NTASI_GPU_RES_*, so the publication loop cannot get the order
  // wrong by construction. Entries 0 and 1 are filled from the constants
  // above; 2-4 from the live ADT; 5-7 from the live ADT if it ever carries
  // them, and otherwise from a firmware-owned allocation.
  //
  NTASI_GPU_REGION    Resources[NTASI_GPU_RES_COUNT];
  UINTN               AdtResolvedCount;
  BOOLEAN             PrebootHandoffPresent;
  BOOLEAN             PlaceholdersAllocated;
} NTASI_GPU_HANDOFF;

//
// GPU preboot carveout reservation. Runs from DXE, not PEI -- see
// NtasiGpuReservationGuard.h for the full incident history of why this
// moved here on 2026-07-30. Every stage below logs a breadcrumb before it
// runs, the same pattern that turned ANS's unreported hang into a
// one-line diagnosis: DXE has a console and (via CpuDxe, apriori-
// dispatched before this driver ever runs) an installed exception vector
// table, so a bug here produces a diagnosable fault or a logged failure,
// never 0 bytes of UART output.
//

STATIC
UINT64
NtasiCurrentStackPointer (
  VOID
  )
{
  UINT64  Sp;

  Sp = 0;
  __asm__ __volatile__ ("mov %0, sp" : "=r" (Sp));
  return Sp;
}

//
// Both memory windows the GPU carveout guard needs, derived live from this
// boot's own boot_args.
//
// WHY NOT PcdSystemMemoryBase/PcdSystemMemorySize -- a hardware-confirmed
// trap, 2026-07-30: those two are declared in [PcdsPatchableInModule]
// (T602XFamilyPkg.dsc.inc), so PatchPcdSet64() in PrePi writes PrePi's OWN
// copy. A DXE driver that calls PcdGet64() on them reads its own,
// never-patched copy, i.e. the DSC defaults 0x10000000000/0x400000000 -- the
// whole 16 GiB physical span, not the ~15.4 GiB window m1n1 actually handed
// this firmware. The captured hardware log proves it: every GPU boot printed
//
//   AppleAgxGpu: uat_ttbs: 0x103FFFB8000/+0x4000 unexpectedly overlaps Mu's
//   system-memory window [0x10000000000, 0x10400000000); refusing to reserve
//
// while the real window ends at 0x103DB29C000 (phys_base 0x10001E40000 +
// mem_size 0x3D945C000, printed by PEI as "Top of system RAM"). All three
// carveouts sit safely ABOVE the real top and were rejected purely because
// the comparison window was the unpatched default. The guard was not too
// aggressive -- it was being fed the wrong numbers.
//
// boot_args is the one source that is correct in DXE: PrePi's EarlySetup()
// (AdtParser.c) copies the whole struct to the FIXED address
// PcdBootArgsPointer, and computes SystemMemoryBase/Size from exactly the
// two fields read below. SmbiosInfoDxe.c already reads it this way from DXE.
//
//   Mu window   = [phys_base, phys_base + mem_size)          (what PEI meant)
//   DRAM window = [ALIGN_DOWN(phys_base, 4GiB),
//                  ALIGN_DOWN(phys_base, 4GiB) + mem_size_actual)
//
// The DRAM formula is byte for byte m1n1's own top_of_memory_alloc(), and the
// same one NtasiDeriveWirelessReservation() in MemoryInitPeiLib.c uses. Only
// mem_size_actual exposes the real installed capacity; mem_size is
// deliberately smaller because it excludes m1n1's reservations and Apple's
// preboot carveouts -- which is precisely where the GPU's UAT regions live.
//
// Returns FALSE if boot_args is absent or its fields are unusable. Callers
// treat that as "cannot prove any candidate is backed by DRAM, and cannot
// prove it avoids Mu's own memory" and reserve nothing -- degraded GPU, never
// a boot risk.
//
STATIC
BOOLEAN
NtasiDeriveBootArgsWindows (
  OUT UINT64  *MuWindowBase,
  OUT UINT64  *MuWindowTop,
  OUT UINT64  *DramWindowBase,
  OUT UINT64  *DramWindowTop
  )
{
  CONST struct boot_args  *BootArgs;
  UINT64                  MemSizeActual;
  UINT64                  PhysBase;
  UINT64                  MemSize;

  *MuWindowBase   = 0;
  *MuWindowTop    = 0;
  *DramWindowBase = 0;
  *DramWindowTop  = 0;

  BootArgs = (CONST struct boot_args *)(UINTN)FixedPcdGet64 (PcdBootArgsPointer);
  if (BootArgs == NULL) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: no boot_args at PcdBootArgsPointer; cannot bound carveouts, GPU degraded\n"));
    return FALSE;
  }

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
      DEBUG ((DEBUG_ERROR, "AppleAgxGpu: unknown boot_args revision %u; cannot bound carveouts, GPU degraded\n", BootArgs->revision));
      return FALSE;
  }

  PhysBase = BootArgs->phys_base;
  MemSize  = BootArgs->mem_size;

  if ((PhysBase == 0) || (MemSize == 0) || (MemSize > (1ULL << 40)) ||
      (PhysBase > MAX_UINT64 - MemSize))
  {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: boot_args phys_base/mem_size unusable (0x%lx/0x%lx); cannot bound carveouts, GPU degraded\n",
      PhysBase,
      MemSize
      ));
    return FALSE;
  }

  if ((MemSizeActual == 0) || (MemSizeActual > (1ULL << 40)) ||
      (MemSizeActual < MemSize))
  {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: boot_args mem_size_actual unusable (0x%lx vs mem_size 0x%lx); cannot bound carveouts, GPU degraded\n",
      MemSizeActual,
      MemSize
      ));
    return FALSE;
  }

  *MuWindowBase   = PhysBase;
  *MuWindowTop    = PhysBase + MemSize;
  *DramWindowBase = PhysBase & ~(SIZE_4GB - 1);
  if (*DramWindowBase > MAX_UINT64 - MemSizeActual) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: DRAM window 0x%lx + 0x%lx overflows; GPU degraded\n", *DramWindowBase, MemSizeActual));
    *MuWindowBase   = 0;
    *MuWindowTop    = 0;
    *DramWindowBase = 0;
    return FALSE;
  }

  *DramWindowTop = *DramWindowBase + MemSizeActual;
  if (*DramWindowTop < *MuWindowTop) {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: derived DRAM top 0x%lx is below Mu's own top 0x%lx; refusing to trust either, GPU degraded\n",
      *DramWindowTop,
      *MuWindowTop
      ));
    *MuWindowBase   = 0;
    *MuWindowTop    = 0;
    *DramWindowBase = 0;
    *DramWindowTop  = 0;
    return FALSE;
  }

  return TRUE;
}

//
// Refuse Base/Size if it does not lie entirely inside the machine's real
// installed-DRAM window, if it contains the live stack pointer, or if it
// unexpectedly overlaps Mu's own [SystemMemoryBase, SystemMemoryTop)
// window -- every carveout this function reserves is asserted to live
// entirely outside that window (iBoot's own reservation, above the
// boot_args memory ceiling) but still inside real DRAM, so an overlap or an
// out-of-DRAM address both mean the address is wrong, not that the carveout
// is unusually placed.
//
STATIC
BOOLEAN
NtasiGpuCarveoutIsSafe (
  IN CONST CHAR8           *Label,
  IN UINT64                Base,
  IN UINT64                Size,
  IN EFI_PHYSICAL_ADDRESS  SystemMemoryBase,
  IN EFI_PHYSICAL_ADDRESS  SystemMemoryTop,
  IN UINT64                DramWindowBase,
  IN UINT64                DramWindowTop,
  IN UINT64                CurrentStackPointer
  )
{
  if (!NtasiRangeWithinWindow (Base, Size, DramWindowBase, DramWindowTop)) {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: %a: 0x%lx/+0x%lx is not inside this machine's real DRAM window [0x%lx, 0x%lx) "
      "derived from boot_args mem_size_actual; refusing to reserve, GPU degraded\n",
      Label,
      Base,
      Size,
      DramWindowBase,
      DramWindowTop
      ));
    return FALSE;
  }

  if (NtasiRangeContainsPoint (Base, Size, CurrentStackPointer)) {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: %a: 0x%lx/+0x%lx contains the live stack pointer (0x%lx); refusing to reserve, GPU degraded\n",
      Label,
      Base,
      Size,
      CurrentStackPointer
      ));
    return FALSE;
  }

  if (NtasiRangesOverlap (Base, Size, SystemMemoryBase, SystemMemoryTop - SystemMemoryBase)) {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: %a: 0x%lx/+0x%lx unexpectedly overlaps Mu's system-memory window [0x%lx, 0x%lx); refusing to reserve, GPU degraded\n",
      Label,
      Base,
      Size,
      SystemMemoryBase,
      SystemMemoryTop
      ));
    return FALSE;
  }

  return TRUE;
}

//
// Read a raw 64-bit Apple ADT scalar property (the "-base"/"-size" style
// properties are stored as a bare native UINT64, not an OpenFirmware
// #address-cells/#size-cells encoded "reg" pair -- see m1n1's
// ADT_GETPROP(adt, node, "gfx-handoff-base", &u64_var) in src/adt.h,
// which copies sizeof(UINT64) bytes verbatim).
//
STATIC
BOOLEAN
NtasiGpuDtNodeU64 (
  IN  dt_node_t    *Node,
  IN  CONST CHAR8  *PropName,
  OUT UINT64       *Value
  )
{
  VOID    *Raw;
  UINTN   Length;

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
// "-size"). Confirmed against a live m1n1 boot log on 2026-07-30: two of
// the three are byte-exact matches for "MMU: Adding Normal-NC mapping"
// lines printed at 0x103fffb8000 and 0x103fff70000. Every candidate is
// still run through NtasiGpuCarveoutIsSafe() before being reserved.
//
// Uses gDS->AddMemorySpace(..., EfiGcdMemoryTypeReserved, ...) rather than
// a PEI resource HOB: nothing in Mu's own memory map ever claims this
// address range on its own (it sits above SystemMemoryTop), so this is
// purely documentation in the GCD memory space map, not a requirement for
// correctness, and a failure here is logged and never fatal.
//
STATIC
BOOLEAN
NtasiReserveGpuAdtCarveout (
  IN dt_node_t             *SgxNode,
  IN CONST CHAR8           *AdtPropertyPrefix,
  IN CONST CHAR8           *Label,
  IN EFI_PHYSICAL_ADDRESS  SystemMemoryBase,
  IN EFI_PHYSICAL_ADDRESS  SystemMemoryTop,
  IN UINT64                DramWindowBase,
  IN UINT64                DramWindowTop,
  IN UINT64                CurrentStackPointer,
  OUT NTASI_GPU_REGION     *Region OPTIONAL
  )
{
  CHAR8       PropName[40];
  UINT64      Base;
  UINT64      Size;
  EFI_STATUS  Status;

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: stage \"resolve-%a\"\n", Label));

  AsciiSPrint (PropName, sizeof (PropName), "%a-base", AdtPropertyPrefix);
  if (!NtasiGpuDtNodeU64 (SgxNode, PropName, &Base)) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: %a: missing ADT property \"%a\" on /arm-io/sgx; GPU degraded\n", Label, PropName));
    return FALSE;
  }

  AsciiSPrint (PropName, sizeof (PropName), "%a-size", AdtPropertyPrefix);
  if (!NtasiGpuDtNodeU64 (SgxNode, PropName, &Size)) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: %a: missing ADT property \"%a\" on /arm-io/sgx; GPU degraded\n", Label, PropName));
    return FALSE;
  }

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: %a: ADT reports 0x%lx/+0x%lx\n", Label, Base, Size));

  if ((Size == 0) || (Base > MAX_UINT64 - (Size - 1))) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: %a: implausible ADT region 0x%lx/+0x%lx; GPU degraded\n", Label, Base, Size));
    return FALSE;
  }

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: stage \"safety-check-%a\"\n", Label));
  if (!NtasiGpuCarveoutIsSafe (
         Label,
         Base,
         Size,
         SystemMemoryBase,
         SystemMemoryTop,
         DramWindowBase,
         DramWindowTop,
         CurrentStackPointer
         ))
  {
    return FALSE;
  }

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: stage \"gcd-reserve-%a\"\n", Label));
  Status = gDS->AddMemorySpace (EfiGcdMemoryTypeReserved, Base, Size, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: %a: gDS->AddMemorySpace(0x%lx, +0x%lx) failed: %r (documentation-only reservation; not fatal)\n",
      Label,
      Base,
      Size,
      Status
      ));
    return FALSE;
  }

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: %a: reserved 0x%lx/+0x%lx (out-of-window carveout, GCD)\n", Label, Base, Size));

  //
  // Only recorded for publication AFTER every safety check above passed and
  // the GCD reservation succeeded. A region that failed any of them leaves
  // Resolved FALSE and can never reach a _CRS.
  //
  if (Region != NULL) {
    Region->Base     = Base;
    Region->Size     = Size;
    Region->Resolved = TRUE;
  }

  return TRUE;
}

//
// THE NTAS0023 PUBLICATION DECISION, MADE EXPLICIT (2026-07-30).
//
// AppleAgxGpu's _CRS contract is eight resources in a fixed order:
//   0 ASC, 1 SGX, 2 uat_ttbs, 3 uat_pagetables, 4 uat_handoff,
//   5 hw_data_a, 6 hw_data_b, 7 globals.
//
// Resources 2-4 are real silicon carveouts and ARE derivable: they come from
// "/arm-io/sgx"'s gpu-region / gfx-shared-region / gfx-handoff "-base"/"-size"
// properties, exactly the ones m1n1's kboot_gpu.c reads for Linux. This
// function resolves them live, bounds them against the machine's real DRAM
// window, and reserves them in the GCD.
//
// Resources 5-7 have NO live source on this boot path:
//   * /arm-io/sgx carries no matching property (a live probe found only
//     gpu-region, gfx-shared-region, gfx-handoff, ttbat-phys-addr-base and
//     rtkit-private-vm-region-base/size).
//   * m1n1's dt_set_gpu() -- the only code that computes anything comparable
//     -- is never called on this project's chainload/HV path.
//   * A prior attempt to compute them from Mu's own memory window crashed the
//     machine twice (see NtasiGpuReservationGuard.h).
//
// Until 2026-07-30 the tree "handled" this by shipping a static GPU.asl whose
// _CRS hardcoded hw_data_a at [0x103db294000, 0x103db29c000) -- inside OS RAM,
// ending exactly at SystemMemoryTop, and containing the exact SP_EL1
// (0x103db29ba10) that crashed PEI. That table was ALSO never installed: its
// FFS GUID was not one of the four Pcd*AcpiTableStorageFile GUIDs
// AcpiPlatformDxe reads, so it was compiled into every gpu-profile FV and
// silently ignored. Both facts were accidents.
//
// They are now decisions. GPU.asl and GpuAcpiTables.inf are deleted, so no
// build can ship those addresses again.
//
// HOW THIS IS RESOLVED, 2026-07-31. NTAS0023 is now published behind
// NTASI_ENABLE_GPU_ACPI_PUBLICATION, and the three unsourceable resources are
// no longer guessed OR omitted -- they are BACKED. See
// NtasiGpuAllocatePlaceholderHandoff(): when the live ADT does not carry them,
// firmware allocates one 16 KiB-aligned EfiReservedMemoryType block and carves
// hw_data_a/hw_data_b/globals out of it, zero-filled.
//
// This corrects a factual error in the previous version of this comment, which
// claimed firmware-allocated regions would be refused by "the driver's carveout
// gate" and so "publication buys nothing". That is not what the gate does.
// AgxkmdVerifyCarveoutsNotOsOwned() walks MmGetPhysicalMemoryRanges() and
// refuses ranges Windows OWNS; EfiReservedMemoryType pages are excluded from
// that list, so they are exactly the RESERVED verdict it accepts. The refusal
// happens later and elsewhere -- in the calibration blob validator, which
// detects an all-zero blob deliberately, because the DT reserves zero
// placeholders before m1n1 fills them. So publication buys the machine getting
// all the way to the ONE gate that names the missing thing, instead of the
// device never existing. And the harm the old comment feared -- addresses
// inside memory Windows owns -- is eliminated by construction, not mitigated.
//
// Nothing is claimed falsely: _DSD carries ntasp,preboot-handoff-present = 0
// whenever the blobs are placeholders, and the payload sizes and CRC32s the
// deleted GPU.asl asserted are NOT republished, because they described data
// captured on a different boot.
//
// WHAT STILL UNBLOCKS THE REAL THING. This function probes all six regions
// using one naming convention. The moment "/arm-io/sgx" carries
// hw-data-a-base/-size, hw-data-b-base/-size and gpu-globals-base/-size -- i.e.
// m1n1 publishes the preboot handoff its own _DSD contract already promises --
// all six resolve, PrebootHandoffPresent becomes TRUE, the placeholder path is
// skipped, and _DSD flips to 1 with no other change.
//
// WORTH RAISING WITH THE AGX WORKSTREAM: in Asahi these three are not preboot
// carveouts at all. HwDataA/HwDataB/Globals are AGX *initdata* structures the
// GPU driver builds itself at runtime from the ADT's power/perf tables; m1n1
// only forwards those tables (as DT properties), it never allocates a region
// for them. If AppleAgxGpu built them the same way, resources 5-7 would not
// need to exist at all and the placeholder path could be deleted outright.
// That is a driver-side ABI question, not something firmware can decide
// unilaterally, which is why the 8-resource contract is honoured here rather
// than unilaterally shortened -- a 5-resource _CRS is refused by the driver's
// own ERR_COUNT check and would publish a device that can never start.
//
STATIC
VOID
NtasiReportGpuPublicationDecision (
  IN UINTN  ResolvedRegions,
  IN UINTN  TotalRegions
  )
{
  DEBUG ((
    (ResolvedRegions == TotalRegions) ? DEBUG_INFO : DEBUG_WARN,
    "AppleAgxGpu: %Lu of %Lu preboot regions resolved from the live ADT\n",
    (UINT64)ResolvedRegions,
    (UINT64)TotalRegions
    ));

  if (ResolvedRegions == TotalRegions) {
    DEBUG ((
      DEBUG_WARN,
      "AppleAgxGpu: the live ADT now carries hw-data-a/hw-data-b/gpu-globals -- the real "
      "m1n1 preboot handoff is present and is what gets published. The firmware-owned "
      "placeholder path is not taken this boot.\n"
      ));
  } else {
    DEBUG ((
      DEBUG_WARN,
      "AppleAgxGpu: hw_data_a/hw_data_b/globals are absent from the live ADT, as expected on "
      "this chainload/HV path (m1n1's dt_set_gpu() never runs, and /arm-io/sgx carries only "
      "gpu-region, gfx-shared-region, gfx-handoff, ttbat-phys-addr-base and "
      "rtkit-private-vm-region-*).\n"
      ));
  }

#if !NTASI_ENABLE_GPU_ACPI_PUBLICATION
  DEBUG ((
    DEBUG_ERROR,
    "AppleAgxGpu: NTAS0023 NOT PUBLISHED -- this build has NTASI_ENABLE_GPU_ACPI_PUBLICATION "
    "off, so the GPU profile reserves carveouts only. GPU unavailable; boot unaffected.\n"
    ));
#endif
}

/**
  Resolve and reserve the GPU's out-of-window ADT carveouts. Called once
  from AcpiPlatformEntryPoint, late in DXE dispatch.
**/
STATIC
VOID
NtasiResolveAndReserveGpuCarveouts (
  OUT NTASI_GPU_HANDOFF  *Handoff
  )
{
  dt_node_t  *SgxNode;
  UINT64     CurrentSp;
  UINT64     SystemMemoryBase;
  UINT64     SystemMemoryTop;
  UINT64     DramWindowBase;
  UINT64     DramWindowTop;
  UINTN      Resolved;
  UINTN      Total;

  Resolved = 0;
  Total    = 6;

  ZeroMem (Handoff, sizeof (*Handoff));

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: bring-up starting\n"));

  CurrentSp = NtasiCurrentStackPointer ();

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: stage \"derive-memory-windows\"\n"));
  if (!NtasiDeriveBootArgsWindows (
         &SystemMemoryBase,
         &SystemMemoryTop,
         &DramWindowBase,
         &DramWindowTop
         ))
  {
    // Already logged in detail. Without provable windows there is no way to
    // tell a real carveout from a stale constant, so reserve nothing.
    NtasiReportGpuPublicationDecision (0, Total);
    DEBUG ((DEBUG_INFO, "AppleAgxGpu: bring-up finished\n"));
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "AppleAgxGpu: real DRAM window [0x%lx, 0x%lx); Mu window [0x%lx, 0x%lx); sp=0x%lx\n",
    DramWindowBase,
    DramWindowTop,
    SystemMemoryBase,
    SystemMemoryTop,
    CurrentSp
    ));

  //
  // Make the PatchableInModule trap visible instead of silently misleading.
  // If these ever agree, PrePi's patch has become visible to DXE and the
  // boot_args derivation above can be revisited; until then a disagreement
  // is expected and is exactly why this code does not use them.
  //
  if ((PcdGet64 (PcdSystemMemoryBase) != SystemMemoryBase) ||
      ((PcdGet64 (PcdSystemMemoryBase) + PcdGet64 (PcdSystemMemorySize)) != SystemMemoryTop))
  {
    DEBUG ((
      DEBUG_WARN,
      "AppleAgxGpu: PcdSystemMemoryBase/Size read [0x%lx, 0x%lx) in this module -- "
      "PatchableInModule copies are per-module and PrePi's patch is not visible here; "
      "using the boot_args-derived window above instead\n",
      PcdGet64 (PcdSystemMemoryBase),
      PcdGet64 (PcdSystemMemoryBase) + PcdGet64 (PcdSystemMemorySize)
      ));
  }

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: stage \"find-sgx-node\"\n"));
  SgxNode = dt_get ("/arm-io/sgx");
  if (SgxNode == NULL) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: \"/arm-io/sgx\" ADT node not found; all GPU preboot reservations skipped, GPU degraded\n"));
  } else {
    //
    // All six AppleAgxGpu preboot regions, probed with ONE naming convention.
    // The first three exist today. The last three are what m1n1 must publish
    // before NTAS0023 can be built truthfully -- probing for them now means
    // the day they appear, this log line says so and nothing here has to
    // change to notice.
    //
    STATIC CONST struct {
      CONST CHAR8    *Prefix;
      CONST CHAR8    *Label;
      UINTN          ResourceIndex;
    } Regions[] = {
      { "gpu-region",        "uat_ttbs",       NTASI_GPU_RES_TTBS       },
      { "gfx-shared-region", "uat_pagetables", NTASI_GPU_RES_PAGETABLES },
      { "gfx-handoff",       "uat_handoff",    NTASI_GPU_RES_HANDOFF    },
      { "hw-data-a",         "hw_data_a",      NTASI_GPU_RES_HWDATA_A   },
      { "hw-data-b",         "hw_data_b",      NTASI_GPU_RES_HWDATA_B   },
      { "gpu-globals",       "globals",        NTASI_GPU_RES_GLOBALS    },
    };
    UINTN  Index;

    for (Index = 0; Index < ARRAY_SIZE (Regions); Index++) {
      if (NtasiReserveGpuAdtCarveout (
            SgxNode,
            Regions[Index].Prefix,
            Regions[Index].Label,
            SystemMemoryBase,
            SystemMemoryTop,
            DramWindowBase,
            DramWindowTop,
            CurrentSp,
            &Handoff->Resources[Regions[Index].ResourceIndex]
            ))
      {
        Resolved++;
      }
    }

    Total = ARRAY_SIZE (Regions);
  }

  Handoff->AdtResolvedCount = Resolved;

  //
  // "The preboot handoff is present" means exactly one thing: all three of
  // hw_data_a/hw_data_b/globals came from the live ADT and survived every
  // safety check. It is NOT inferred from the count, because the count would
  // also be satisfied by a different mix.
  //
  Handoff->PrebootHandoffPresent =
    (BOOLEAN)(Handoff->Resources[NTASI_GPU_RES_HWDATA_A].Resolved &&
              Handoff->Resources[NTASI_GPU_RES_HWDATA_B].Resolved &&
              Handoff->Resources[NTASI_GPU_RES_GLOBALS].Resolved);

  NtasiReportGpuPublicationDecision (Resolved, Total);
  DEBUG ((DEBUG_INFO, "AppleAgxGpu: bring-up finished\n"));
}

#if NTASI_ENABLE_GPU_ACPI_PUBLICATION

/**
  Prove the two hardcoded MMIO windows against the live ADT before publishing.

  Resources 0 and 1 cannot be derived, because AppleAgxGpu validates them
  against exact constants and refuses anything else (ERR_FIXED). They can still
  be CHECKED, and that is the whole difference between this and the GPU.asl it
  replaced. Both checks are containment, not equality:

    * ASC must lie entirely INSIDE /arm-io/gfx-asc reg[0]. The published window
      is a 0x40000 prefix of a real 0x6C000 register block, so a subset is
      correct and an equality check would wrongly fail.
    * SGX must CONTAIN /arm-io/sgx reg[0] and reg[1], because the published
      16 MiB span deliberately over-claims in order to reach the GPU PMGR page
      the driver derives at SgxBase + 0xE80000.

  If this machine's ADT ever disagrees, nothing is published at all.
**/
STATIC
BOOLEAN
NtasiGpuMmioWindowsAgreeWithAdt (
  VOID
  )
{
  dt_node_t  *AscNode;
  dt_node_t  *SgxNode;
  UINT64     AscBase;
  UINT64     AscSize;
  UINT64     SgxBase0;
  UINT64     SgxSize0;
  UINT64     SgxBase1;
  UINT64     SgxSize1;

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: stage \"verify-mmio-against-adt\"\n"));

  AscNode = dt_get ("/arm-io/gfx-asc");
  SgxNode = dt_get ("/arm-io/sgx");
  if ((AscNode == NULL) || (SgxNode == NULL)) {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: \"/arm-io/gfx-asc\" or \"/arm-io/sgx\" missing from the live ADT; "
      "NTAS0023 withheld\n"
      ));
    return FALSE;
  }

  if ((dt_node_reg (AscNode, 0, &AscBase, &AscSize) != 0) ||
      (dt_node_reg (SgxNode, 0, &SgxBase0, &SgxSize0) != 0) ||
      (dt_node_reg (SgxNode, 1, &SgxBase1, &SgxSize1) != 0))
  {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: could not read GPU \"reg\" windows from the live ADT; NTAS0023 withheld\n"));
    return FALSE;
  }

  DEBUG ((
    DEBUG_INFO,
    "AppleAgxGpu: ADT gfx-asc reg[0]=0x%lx/+0x%lx sgx reg[0]=0x%lx/+0x%lx reg[1]=0x%lx/+0x%lx\n",
    AscBase,
    AscSize,
    SgxBase0,
    SgxSize0,
    SgxBase1,
    SgxSize1
    ));

  if (!NtasiRangeWithinWindow (
         NTASI_GPU_ASC_BASE,
         NTASI_GPU_ASC_SIZE,
         AscBase,
         AscBase + AscSize
         ))
  {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: published ASC window 0x%lx/+0x%lx is not inside the live ADT's gfx-asc "
      "reg[0] [0x%lx, 0x%lx); NTAS0023 withheld\n",
      NTASI_GPU_ASC_BASE,
      NTASI_GPU_ASC_SIZE,
      AscBase,
      AscBase + AscSize
      ));
    return FALSE;
  }

  if (!NtasiRangeWithinWindow (
         SgxBase0,
         SgxSize0,
         NTASI_GPU_SGX_BASE,
         NTASI_GPU_SGX_BASE + NTASI_GPU_SGX_SIZE
         ) ||
      !NtasiRangeWithinWindow (
         SgxBase1,
         SgxSize1,
         NTASI_GPU_SGX_BASE,
         NTASI_GPU_SGX_BASE + NTASI_GPU_SGX_SIZE
         ))
  {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: published SGX window [0x%lx, 0x%lx) does not contain both live sgx reg "
      "windows; NTAS0023 withheld\n",
      NTASI_GPU_SGX_BASE,
      NTASI_GPU_SGX_BASE + NTASI_GPU_SGX_SIZE
      ));
    return FALSE;
  }

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: MMIO windows agree with the live ADT\n"));
  return TRUE;
}

/**
  Back hw_data_a/hw_data_b/globals with memory FIRMWARE owns.

  THIS IS THE FIX FOR THE ORIGINAL BUG. GPU.asl published these three at
  [0x103db278000, 0x103db29c000) -- addresses m1n1's dt_set_gpu() had allocated
  with top_of_memory_alloc() during a DIFFERENT session, on the Linux boot path,
  and which nothing shrinks on this project's chainload/HV path. Mu's own
  boot_args therefore still covered them: they were inside OS RAM, ended exactly
  at SystemMemoryTop, and hw_data_a contained the live PEI SP_EL1 0x103db29ba10.

  There is no live ADT source for them (a full probe of /arm-io/sgx finds only
  gpu-region, gfx-shared-region, gfx-handoff, ttbat-phys-addr-base and
  rtkit-private-vm-region-*), and deriving them from Mu's own window is what
  crashed the machine. So when the ADT does not carry them, firmware ALLOCATES
  them instead of guessing: one EfiReservedMemoryType block, zero-filled,
  carved into the three sizes the driver pins.

  WHY THIS ALLOCATES PLAIN PAGES AND NOT AllocateAlignedReservedPages().
  CORRECTED 2026-07-31 after the first boot of this code wedged Mu.

  The first version asked for EFI_SIZE_TO_PAGES(0x24000) = 36 pages at 16 KiB
  alignment via AllocateAlignedReservedPages(). That function is unusable for
  EfiReservedMemoryType on AArch64, and it does not fail gracefully -- it
  DEADLOOPS:

    * MdePkg/Library/UefiMemoryAllocationLib/MemoryAllocationLib.c:189-223
      implements alignment by OVER-allocating (Pages + EFI_SIZE_TO_PAGES
      (Alignment) = 40 pages) and then gBS->FreePages()-ing the unaligned head
      and tail back.
    * MdeModulePkg/Core/Dxe/Mem/Page.c:1649-1657 forces EfiReservedMemoryType
      allocations to RUNTIME_PAGE_ALLOCATION_GRANULARITY, which is 0x10000 on
      AArch64 (MdePkg/Include/AArch64/ProcessorBind.h:169). The base therefore
      comes back 64 KiB aligned and the head free is skipped -- but the TAIL
      free is at Base + EFI_PAGES_TO_SIZE (36) = Base + 0x24000, which is only
      16 KiB aligned.
    * Page.c:1928-1941 rejects a free of a reserved range whose address is not
      64 KiB aligned with EFI_INVALID_PARAMETER, and MemoryAllocationLib.c:222
      turns that into ASSERT_EFI_ERROR() -> CpuDeadLoop() in a DEBUG build.

  Measured, not deduced. The 3450262 boot's last line on the secondary UART is:

      AppleAgxGpu: stage "allocate-placeholder-handoff"
      ASSERT_EFI_ERROR (Status = Invalid Parameter)
      ASSERT [AcpiPlatform] MemoryAllocationLib.c(222): !(((RETURN_STATUS)(Status)) >= 0x8000000000000000ULL)

  and nothing after it. Every DXE driver ordered behind AcpiPlatformDxe never
  ran, Mu never reached ExitBootServices, and Windows never started.

  The alignment wrapper was never needed in the first place: the DXE core
  already rounds a reserved allocation up to 64 KiB and returns a 64 KiB
  aligned base, and 64 KiB alignment satisfies the driver's 16 KiB requirement
  a fortiori. So this asks for a page count that is already a whole number of
  64 KiB granules and takes the base the core hands back. That request is
  exactly what the core would have rounded to anyway, so no memory is wasted
  relative to the broken version, and there is no free-back step to fail.

  That makes the published addresses true by construction rather than by
  assumption. EfiReservedMemoryType is excluded from the OS's usable RAM, so
  these pages never appear in Windows' MmGetPhysicalMemoryRanges() and cannot
  collide with anything Windows owns -- which is exactly what AppleAgxGpu's
  carveout gate checks, and it will pass.

  The blobs are ZERO, and that is stated rather than hidden: _DSD carries
  ntasp,preboot-handoff-present = 0. The driver's own calibration validator
  rejects an all-zero blob by design (it exists precisely because the DT
  reserves zero placeholders before m1n1 fills them), so bring-up stops at a
  named, non-destructive gate instead of the device never appearing at all.

  @retval TRUE   All three are backed and recorded.
  @retval FALSE  Nothing was allocated; the caller must withhold NTAS0023.
**/
STATIC
BOOLEAN
NtasiGpuAllocatePlaceholderHandoff (
  IN OUT NTASI_GPU_HANDOFF  *Handoff,
  IN     UINT64             CurrentStackPointer
  )
{
  UINT64  Total;
  UINT64  Allocation;
  VOID    *Block;
  UINT64  Base;

  Total = NTASI_GPU_HWDATA_A_SIZE + NTASI_GPU_HWDATA_B_SIZE + NTASI_GPU_GLOBALS_SIZE;

  //
  // What is actually taken from the core: Total rounded up to the reserved-
  // memory granularity the core enforces anyway. Computed rather than assumed
  // so the tail padding is owned, zeroed and logged instead of being an
  // invisible rounding artefact.
  //
  Allocation = ALIGN_VALUE (Total, (UINT64)NTASI_GPU_RESERVED_GRANULARITY);

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: stage \"allocate-placeholder-handoff\"\n"));

  //
  // One contiguous block. NOT AllocateAlignedReservedPages() -- see the
  // function header for the deadloop that call caused on 2026-07-31. The core
  // returns EfiReservedMemoryType at RUNTIME_PAGE_ALLOCATION_GRANULARITY
  // alignment (Page.c:1649-1657), which is a multiple of the 16 KiB the driver
  // requires, so no alignment wrapper is involved and nothing is freed back.
  //
  Block = AllocateReservedPages (EFI_SIZE_TO_PAGES ((UINTN)Allocation));
  if (Block == NULL) {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: could not allocate 0x%lx bytes of reserved memory for the preboot "
      "handoff placeholders; NTAS0023 withheld\n",
      Allocation
      ));
    return FALSE;
  }

  ZeroMem (Block, (UINTN)Allocation);
  Base = (UINT64)(UINTN)Block;

  //
  // Verified, not assumed, and fail-closed rather than ASSERT-ed: the driver
  // rejects any of resources 2-7 that is not 16 KiB aligned (ERR_ALIGN), and
  // this is the one property of the returned base that the caller depends on.
  // The core's own contract makes it 64 KiB aligned; if a future core, a heap
  // guard or a 4 KiB-granularity build ever weakens that, NTAS0023 is withheld
  // instead of a misaligned _CRS being published.
  //
  // It must never ASSERT here. AcpiPlatformDxe promises the boot is unaffected
  // by a GPU failure, and an ASSERT in a DEBUG build is a CpuDeadLoop() that
  // breaks exactly that promise -- which is what actually happened.
  //
  // NOTHING IS FREED ON THE FAILURE PATHS BELOW, DELIBERATELY.
  // MdePkg's FreePages() is ASSERT_EFI_ERROR (gBS->FreePages (...)) with no
  // way to inspect the status -- the same shape as the wrapper that deadlooped
  // here -- and the misalignment branch would be handing it exactly the
  // unaligned reserved address that Page.c:1938 refuses. Stranding one
  // 192 KiB EfiReservedMemoryType block on a should-never-happen path costs
  // Windows 192 KiB; asserting costs the boot.
  //
  if ((Base & (NTASI_GPU_PAGE_SIZE - 1)) != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: reserved placeholder block 0x%lx is not %u-byte aligned; NTAS0023 withheld "
      "(block left reserved rather than freed; see the comment above)\n",
      Base,
      (UINT32)NTASI_GPU_PAGE_SIZE
      ));
    return FALSE;
  }

  //
  // Paranoia, not ceremony: the 2026-07-30 incident was a GPU region landing on
  // Mu's live stack. The allocator cannot return the stack, but this is the one
  // invariant whose violation is unrecoverable, so it is checked rather than
  // assumed -- the same reason NtasiGpuCarveoutIsSafe() checks it for the ADT
  // path.
  //
  // Checked over the WHOLE allocation, not just the published prefix: the
  // rounding tail is firmware-owned memory too.
  //
  if (NtasiRangeContainsPoint (Base, Allocation, CurrentStackPointer)) {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: placeholder block 0x%lx/+0x%lx contains the live stack pointer 0x%lx; "
      "NTAS0023 withheld\n",
      Base,
      Allocation,
      CurrentStackPointer
      ));
    return FALSE;
  }

  Handoff->Resources[NTASI_GPU_RES_HWDATA_A].Base     = Base;
  Handoff->Resources[NTASI_GPU_RES_HWDATA_A].Size     = NTASI_GPU_HWDATA_A_SIZE;
  Handoff->Resources[NTASI_GPU_RES_HWDATA_A].Resolved = TRUE;

  Handoff->Resources[NTASI_GPU_RES_HWDATA_B].Base     = Base + NTASI_GPU_HWDATA_A_SIZE;
  Handoff->Resources[NTASI_GPU_RES_HWDATA_B].Size     = NTASI_GPU_HWDATA_B_SIZE;
  Handoff->Resources[NTASI_GPU_RES_HWDATA_B].Resolved = TRUE;

  Handoff->Resources[NTASI_GPU_RES_GLOBALS].Base =
    Base + NTASI_GPU_HWDATA_A_SIZE + NTASI_GPU_HWDATA_B_SIZE;
  Handoff->Resources[NTASI_GPU_RES_GLOBALS].Size     = NTASI_GPU_GLOBALS_SIZE;
  Handoff->Resources[NTASI_GPU_RES_GLOBALS].Resolved = TRUE;

  Handoff->PlaceholdersAllocated = TRUE;

  DEBUG ((
    DEBUG_WARN,
    "AppleAgxGpu: preboot handoff is ABSENT from the live ADT; published hw_data_a/hw_data_b/"
    "globals are firmware-owned EfiReservedMemoryType placeholders at 0x%lx/+0x%lx (0x%lx bytes "
    "reserved, rounded to the 64 KiB reserved-memory granularity), zero-filled. "
    "They are NOT calibration data: AppleAgxGpu's blob validator will reject them and stop, "
    "which is the intended, non-destructive outcome. _DSD says so via "
    "ntasp,preboot-handoff-present = 0.\n",
    Base,
    Total,
    Allocation
    ));

  return TRUE;
}

/**
  Publish NTAS0023 (AppleAgxGpu) as a runtime-generated SSDT.

  Generated with AmlLib at DXE rather than compiled from a static .asl, for the
  same reason ANS0 and DRT0 are: a static .aml lands in the firmware volume of
  EVERY profile, so a default-off feature could not be default-off on disk. It
  is also what lets the addresses come from this boot's own ADT instead of from
  a constant captured on some other boot -- which is the bug that deleted
  GPU.asl.

  Fails closed everywhere: any missing resource, any failed check, or any AmlLib
  error abandons the whole device. A short or reordered _CRS is worse than no
  device at all, because the driver matches resources positionally.
**/
STATIC
EFI_STATUS
NtasiInstallGpuTable (
  IN EFI_ACPI_TABLE_PROTOCOL  *AcpiTable,
  IN NTASI_GPU_HANDOFF        *Handoff
  )
{
  EFI_STATUS                   Status;
  EFI_STATUS                   DeleteStatus;
  AML_ROOT_NODE_HANDLE         RootNode;
  AML_OBJECT_NODE_HANDLE       ScopeNode;
  AML_OBJECT_NODE_HANDLE       DeviceNode;
  AML_OBJECT_NODE_HANDLE       CrsNode;
  AML_OBJECT_NODE_HANDLE       DsdNode;
  AML_OBJECT_NODE_HANDLE       DsdPackageNode;
  EFI_ACPI_DESCRIPTION_HEADER  *Table;
  UINTN                        TableHandle;
  UINTN                        Index;
  UINT32                       Irq;

  RootNode = NULL;
  Table    = NULL;

  //
  // Resources 0 and 1 are the checked constants; 2-7 are already in Handoff.
  //
  Handoff->Resources[NTASI_GPU_RES_ASC].Base     = NTASI_GPU_ASC_BASE;
  Handoff->Resources[NTASI_GPU_RES_ASC].Size     = NTASI_GPU_ASC_SIZE;
  Handoff->Resources[NTASI_GPU_RES_ASC].Resolved = TRUE;
  Handoff->Resources[NTASI_GPU_RES_SGX].Base     = NTASI_GPU_SGX_BASE;
  Handoff->Resources[NTASI_GPU_RES_SGX].Size     = NTASI_GPU_SGX_SIZE;
  Handoff->Resources[NTASI_GPU_RES_SGX].Resolved = TRUE;

  //
  // Every one of the eight, or none. The driver's contract is a fixed count in
  // a fixed order; a gap cannot be expressed and must not be improvised.
  //
  for (Index = 0; Index < NTASI_GPU_RES_COUNT; Index++) {
    if (!Handoff->Resources[Index].Resolved) {
      DEBUG ((
        DEBUG_ERROR,
        "AppleAgxGpu: _CRS resource %u is unresolved; NTAS0023 withheld (the contract is "
        "exactly %u resources in a fixed order and a short list cannot be published)\n",
        (UINT32)Index,
        (UINT32)NTASI_GPU_RES_COUNT
        ));
      return EFI_NOT_FOUND;
    }
  }

  //
  // Last line of defence, and deliberately independent of how each resource got
  // here: no two published resources may overlap. The driver checks this too
  // (ERR_OVERLAP), but a firmware that publishes an overlapping _CRS has
  // already handed the arbiter a conflict, so it is caught here first.
  //
  for (Index = 0; Index < NTASI_GPU_RES_COUNT; Index++) {
    UINTN  Other;

    for (Other = Index + 1; Other < NTASI_GPU_RES_COUNT; Other++) {
      if (NtasiRangesOverlap (
            Handoff->Resources[Index].Base,
            Handoff->Resources[Index].Size,
            Handoff->Resources[Other].Base,
            Handoff->Resources[Other].Size
            ))
      {
        DEBUG ((
          DEBUG_ERROR,
          "AppleAgxGpu: _CRS resources %u (0x%lx/+0x%lx) and %u (0x%lx/+0x%lx) overlap; "
          "NTAS0023 withheld\n",
          (UINT32)Index,
          Handoff->Resources[Index].Base,
          Handoff->Resources[Index].Size,
          (UINT32)Other,
          Handoff->Resources[Other].Base,
          Handoff->Resources[Other].Size
          ));
        return EFI_INVALID_PARAMETER;
      }
    }
  }

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: stage \"build-ssdt\"\n"));

  Status = AmlCodeGenDefinitionBlock ("SSDT", "Apple", "J414GPU", 1, &RootNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenScope ("\\_SB_", RootNode, &ScopeNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenDevice ("GPU0", ScopeNode, &DeviceNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameString ("_HID", "NTAS0023", DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameInteger ("_UID", 0, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  //
  // _CCA = 1: AGX DMA is coherent with the CPU caches, which is what lets the
  // driver map its ring and BO memory cached.
  //
  Status = AmlCodeGenNameInteger ("_CCA", 1, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameInteger ("_STA", 0x0F, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameResourceTemplate ("_CRS", DeviceNode, &CrsNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  //
  // The eight windows in index order, then the interrupt LAST -- the same shape
  // the media generator emits and the same shape the deleted GPU.asl had.
  //
  for (Index = 0; Index < NTASI_GPU_RES_COUNT; Index++) {
    Status = AppleAnsAddMemoryResource (
               CrsNode,
               Handoff->Resources[Index].Base,
               Handoff->Resources[Index].Size
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "AppleAgxGpu: _CRS resource %u (0x%lx/+0x%lx) refused: %r; NTAS0023 withheld\n",
        (UINT32)Index,
        Handoff->Resources[Index].Base,
        Handoff->Resources[Index].Size,
        Status
        ));
      goto Exit;
    }
  }

  //
  // One level-triggered, active-high, exclusive vector: the AGX ASC mailbox
  // "recv not empty" doorbell. Published as GSIV 46; the CSRT ALI2 tail
  // translates it to physical AIC line 1146. AIC lines are level/active-high,
  // and the ASC v4 mailbox has no ack register, so the driver drains it.
  //
  Irq    = NTASI_GPU_PUBLISHED_GSIV;
  Status = AmlCodeGenRdInterrupt (
             TRUE,                              // ResourceConsumer
             FALSE,                             // EdgeTriggered -> Level
             FALSE,                             // ActiveLow -> ActiveHigh
             FALSE,                             // Shared -> Exclusive
             &Irq,
             1,
             CrsNode,
             NULL
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: interrupt descriptor refused: %r; NTAS0023 withheld\n", Status));
    goto Exit;
  }

  //
  // _DSD carries data, never a resource claim, so nothing below is visible to
  // the OS resource arbiter. This is the same lesson as NTAS2003-vs-KBL0: a
  // base address the driver merely needs to READ belongs here, not in _CRS.
  //
  Status = AmlCodeGenNamePackage ("_DSD", DeviceNode, &DsdNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlAddDeviceDataDescriptorPackage (
             &gAppleAnsDsdPropertiesGuid,
             DsdNode,
             &DsdPackageNode
             );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  {
    //
    // Integer-valued only: this AmlLib has AmlAddNameIntegerPackage() but no
    // string equivalent, so the deleted GPU.asl's string properties
    // ("ntasp,gpu-variant" = "G14X", "ntasp,preboot-owner" = "m1n1") are
    // omitted. The driver reads none of them.
    //
    // The payload sizes and CRC32s that GPU.asl carried are omitted for a
    // different and deliberate reason: they described calibration blobs
    // captured on another boot. Publishing them next to placeholder memory
    // would assert a checksum for data that does not exist.
    //
    STATIC CONST struct {
      CONST CHAR8    *Name;
      UINT64         Value;
    } Properties[] = {
      { "ntasp,gpu-chip-id",              0x6020  },
      { "ntasp,gpu-generation",           14      },
      { "ntasp,gpu-firmware-compat-major", 13     },
      { "ntasp,gpu-firmware-compat-minor", 5      },
      { "ntasp,gpu-max-frequency-khz",    1398000 },
      { "ntasp,gpu-mailbox-aic-line",     NTASI_GPU_PHYSICAL_AIC   },
      { "ntasp,gpu-mailbox-gsiv",         NTASI_GPU_PUBLISHED_GSIV },
      { "ntasp,gpu-pmgr-gpx-offset",      0x0     },
      { "ntasp,gpu-pmgr-afr-offset",      0x100   },
      { "ntasp,gpu-pmgr-gfx-offset",      0x108   },
      { "ntasp,gpu-pmgr-afr-min-state",   4       },
      { "ntasp,gpu-pmgr-gpx-always-on",   1       },
    };

    for (Index = 0; Index < ARRAY_SIZE (Properties); Index++) {
      Status = AmlAddNameIntegerPackage (
                 Properties[Index].Name,
                 Properties[Index].Value,
                 DsdPackageNode
                 );
      if (EFI_ERROR (Status)) {
        goto Exit;
      }
    }
  }

  //
  // THE property that makes this publication honest. 1 means hw_data_a/
  // hw_data_b/globals are real m1n1 calibration data resolved from the live
  // ADT; 0 means they are firmware-owned zeroed placeholders and the driver
  // must not treat their contents as calibration.
  //
  Status = AmlAddNameIntegerPackage (
             "ntasp,preboot-handoff-present",
             Handoff->PrebootHandoffPresent ? 1 : 0,
             DsdPackageNode
             );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlAddNameIntegerPackage (
             "ntasp,preboot-handoff-required",
             1,
             DsdPackageNode
             );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlSerializeDefinitionBlock (RootNode, &Table);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  TableHandle = 0;
  Status      = AcpiTable->InstallAcpiTable (AcpiTable, Table, Table->Length, &TableHandle);
  if (!EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "AppleAgxGpu: NTAS0023 PUBLISHED, 8 memory resources + 1 interrupt (GSIV %u -> AIC %u). "
      "uat_ttbs=0x%lx/+0x%lx uat_pagetables=0x%lx/+0x%lx uat_handoff=0x%lx/+0x%lx (live ADT); "
      "hw_data_a=0x%lx hw_data_b=0x%lx globals=0x%lx (%a). preboot-handoff-present=%u\n",
      (UINT32)NTASI_GPU_PUBLISHED_GSIV,
      (UINT32)NTASI_GPU_PHYSICAL_AIC,
      Handoff->Resources[NTASI_GPU_RES_TTBS].Base,
      Handoff->Resources[NTASI_GPU_RES_TTBS].Size,
      Handoff->Resources[NTASI_GPU_RES_PAGETABLES].Base,
      Handoff->Resources[NTASI_GPU_RES_PAGETABLES].Size,
      Handoff->Resources[NTASI_GPU_RES_HANDOFF].Base,
      Handoff->Resources[NTASI_GPU_RES_HANDOFF].Size,
      Handoff->Resources[NTASI_GPU_RES_HWDATA_A].Base,
      Handoff->Resources[NTASI_GPU_RES_HWDATA_B].Base,
      Handoff->Resources[NTASI_GPU_RES_GLOBALS].Base,
      Handoff->PlaceholdersAllocated ? "firmware-owned placeholders" : "live ADT",
      (UINT32)(Handoff->PrebootHandoffPresent ? 1 : 0)
      ));
  }

Exit:
  if (Table != NULL) {
    FreePool (Table);
  }

  if (RootNode != NULL) {
    DeleteStatus = AmlDeleteTree (RootNode);
    if (!EFI_ERROR (Status) && EFI_ERROR (DeleteStatus)) {
      Status = DeleteStatus;
    }
  }

  return Status;
}

/**
  Resolve, reserve, back and publish the GPU. Non-fatal throughout.
**/
STATIC
VOID
NtasiPublishGpu (
  IN EFI_ACPI_TABLE_PROTOCOL  *AcpiTable,
  IN NTASI_GPU_HANDOFF        *Handoff
  )
{
  EFI_STATUS  Status;

  if (!NtasiGpuMmioWindowsAgreeWithAdt ()) {
    return;
  }

  //
  // The three UAT carveouts are the ones that MUST come from the live ADT.
  // They are real silicon addresses that legitimately live above boot_args'
  // mem_size ceiling, and there is no honest way to invent them.
  //
  if (!Handoff->Resources[NTASI_GPU_RES_TTBS].Resolved ||
      !Handoff->Resources[NTASI_GPU_RES_PAGETABLES].Resolved ||
      !Handoff->Resources[NTASI_GPU_RES_HANDOFF].Resolved)
  {
    DEBUG ((
      DEBUG_ERROR,
      "AppleAgxGpu: one or more UAT carveouts did not resolve from the live ADT or failed a "
      "safety check; NTAS0023 withheld. GPU unavailable; boot unaffected.\n"
      ));
    return;
  }

  if (!Handoff->PrebootHandoffPresent) {
    if (!NtasiGpuAllocatePlaceholderHandoff (Handoff, NtasiCurrentStackPointer ())) {
      return;
    }
  }

  Status = NtasiInstallGpuTable (AcpiTable, Handoff);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: NTAS0023 SSDT installation failed: %r\n", Status));
  }
}

#endif // NTASI_ENABLE_GPU_ACPI_PUBLICATION
#endif // NTASI_J414S_GPU_RESOURCE_PROFILE

#if NTASI_ENABLE_WIRELESS_DART_HANDOFF
#define NTASI_WIRELESS_DART_APERTURE_BASE  0x594000000ULL
#define NTASI_WIRELESS_DART_APERTURE_SIZE  0x4000ULL

/**
  Publish DRT0 (the wireless SID-1 DART page-table handoff) to Windows.

  Generated dynamically instead of the static WDRT.asl this table used to
  be: the reservation base/size are derived by MemoryInitPeiLib.c's
  NtasiDeriveWirelessReservation() at boot from live boot_args, not known
  at build time, so a compiled-in QWordMemory resource baking a build-time
  constant is no longer possible (nor desirable -- see that function's own
  comment on why a hardcoded reservation address is exactly what produced
  the GPU PEI crash earlier tonight).

  PcdAppleWirelessDartPageTableBase/Size read back whatever PEI derived
  and authenticated against m1n1's own ABI v2 descriptor. Zero means
  wireless was withheld this boot (PEI already logged why -- derivation
  failed, or the descriptor at the derived address did not validate) and
  DRT0 is not published at all: AppleDart/AppleBcmWifi then simply find no
  SID-1 handoff to adopt, the same as a build with
  NTASI_ENABLE_WIRELESS_DART_HANDOFF off. This mirrors
  AcpiPlatformInstallAppleAnsTable()'s own "no ADT node -> EFI_NOT_FOUND,
  not fatal" contract.

  Stage-tagged breadcrumbs match the same pattern the ANS/GPU fixes
  established tonight; DXE has a console and (via CpuDxe, apriori-
  dispatched before this driver runs) working exception vectors, so a bug
  here is diagnosable rather than a silent hang.
**/
STATIC
EFI_STATUS
NtasiInstallWirelessDartTable (
  IN EFI_ACPI_TABLE_PROTOCOL  *AcpiTable
  )
{
  EFI_STATUS                   Status;
  EFI_STATUS                   DeleteStatus;
  AML_ROOT_NODE_HANDLE         RootNode;
  AML_OBJECT_NODE_HANDLE       ScopeNode;
  AML_OBJECT_NODE_HANDLE       DeviceNode;
  // No CrsNode: DRT0 deliberately publishes no _CRS at all. See the comment
  // at the DRTB/DRTL/RSVB/RSVS methods below.
  EFI_ACPI_DESCRIPTION_HEADER  *Table;
  UINTN                        TableHandle;
  UINT64                       ReservationBase;
  UINT32                       ReservationSize;

  RootNode = NULL;
  Table    = NULL;

  //
  // FIXED 2026-07-30. This used to read
  // PcdAppleWirelessDartPageTableBase/Size, which are
  // [PcdsPatchableInModule] -- a PER-MODULE copy. MemoryInitPeiLib's
  // PatchPcdSet64/32 writes the copy linked into PrePi; this driver's
  // PcdGet64/32 read their own never-patched copies, which are always the DEC
  // default of zero. DRT0 was therefore withheld on EVERY boot regardless of
  // what PEI derived and authenticated, and the log line said "no reservation
  // published by PEI this boot" -- the exact opposite of the truth. Wireless
  // could not work, and the evidence pointed at the wrong phase.
  //
  // PEI now hands the authenticated reservation over in a GUID HOB, the same
  // mechanism it already uses for the appended ramdisk. DXE then
  // re-authenticates the descriptor itself, using the one shared copy of the
  // validator in <IndustryStandard/J414sWirelessHandoff.h>, against the exact
  // guest_top PEI used. That is deliberately not a formality: it proves the
  // reservation survived all of PEI and DXE dispatch byte-intact, and
  // publishing a DART page-table base to Windows on the strength of a HOB
  // alone would mean trusting a structure nothing re-checked.
  //
  // Fail-closed throughout: no HOB, a malformed HOB, or a descriptor that
  // fails re-authentication all withhold DRT0 loudly. A zero is never
  // published.
  //
  DEBUG ((DEBUG_INFO, "WirelessDART ACPI: stage \"read-reservation-hob\"\n"));
  {
    CONST NTASI_WIRELESS_DART_RESERVATION_HOB  *Reservation;
    VOID                                       *GuidHob;

    GuidHob = GetFirstGuidHob (&mNtasiWirelessDartReservationHobGuid);
    if (GuidHob == NULL) {
      DEBUG ((
        DEBUG_ERROR,
        "WirelessDART ACPI: PEI published no reservation HOB this boot; DRT0 withheld. "
        "PEI logs the reason (derivation declined, or the ABI v2 descriptor at the derived "
        "address failed authentication) -- look for \"MemoryInitPeiLib: wireless:\".\n"
        ));
      return EFI_NOT_FOUND;
    }

    Reservation = GET_GUID_HOB_DATA (GuidHob);
    if ((GET_GUID_HOB_DATA_SIZE (GuidHob) < sizeof (*Reservation)) ||
        (Reservation->Signature != NTASI_WIRELESS_DART_RESERVATION_HOB_SIGNATURE) ||
        (Reservation->Version != NTASI_WIRELESS_DART_RESERVATION_HOB_VERSION) ||
        (Reservation->StructureSize != sizeof (*Reservation)))
    {
      DEBUG ((
        DEBUG_ERROR,
        "WirelessDART ACPI: reservation HOB is malformed (size=%u sig=0x%x ver=%u struct=%u); DRT0 withheld\n",
        (UINT32)GET_GUID_HOB_DATA_SIZE (GuidHob),
        Reservation->Signature,
        (UINT32)Reservation->Version,
        (UINT32)Reservation->StructureSize
        ));
      return EFI_NOT_FOUND;
    }

    if ((Reservation->ReservationBase == 0) ||
        (Reservation->ReservationSize == 0) ||
        (Reservation->ReservationSize > MAX_UINT32))
    {
      DEBUG ((
        DEBUG_ERROR,
        "WirelessDART ACPI: reservation HOB carries an unusable 0x%lx/+0x%lx; DRT0 withheld\n",
        Reservation->ReservationBase,
        Reservation->ReservationSize
        ));
      return EFI_NOT_FOUND;
    }

    DEBUG ((DEBUG_INFO, "WirelessDART ACPI: stage \"reauthenticate-descriptor\"\n"));
    if (!NtasiValidateWirelessHandoffV2 (
           Reservation->ReservationBase,
           (UINT32)Reservation->ReservationSize,
           Reservation->GuestMemoryTop
           ))
    {
      DEBUG ((
        DEBUG_ERROR,
        "WirelessDART ACPI: ABI v2 descriptor at 0x%lx/+0x%lx no longer authenticates in DXE "
        "(PEI accepted it, so something modified the reservation after PEI reserved it); DRT0 withheld\n",
        Reservation->ReservationBase,
        Reservation->ReservationSize
        ));
      return EFI_NOT_FOUND;
    }

    ReservationBase = Reservation->ReservationBase;
    ReservationSize = (UINT32)Reservation->ReservationSize;
    DEBUG ((
      DEBUG_INFO,
      "WirelessDART ACPI: reservation 0x%lx/+0x%x re-authenticated in DXE (guest_top 0x%lx)\n",
      ReservationBase,
      ReservationSize,
      Reservation->GuestMemoryTop
      ));
  }

  DEBUG ((DEBUG_INFO, "WirelessDART ACPI: stage \"build-ssdt\" reservation=0x%lx/+0x%x\n", ReservationBase, ReservationSize));

  Status = AmlCodeGenDefinitionBlock ("SSDT", "NTASP ", "J414WDRT", 1, &RootNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenScope ("\\_SB_", RootNode, &ScopeNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenDevice ("DRT0", ScopeNode, &DeviceNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameString ("_HID", "NTAS0011", DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameInteger ("_UID", 0, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameInteger ("_CCA", 1, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  //
  // NO _CRS -- deliberate.
  //
  // DRT0 used to publish two QWordMemory consumer resources: the DART
  // aperture at 0x594000000 and the derived top-of-DRAM reservation (e.g.
  // 0x103FFFF0000 on the measured boot).  The measured result was
  // CM_PROB_NORMAL_CONFLICT (Code 12) on ACPI\NTAS0011 with no resources
  // assigned, which also starves the three PCIe endpoints of their DART
  // provider.
  //
  // THE RESERVATION IS THE ONE THAT CANNOT BE ARBITRATED, AND IT IS NOT A
  // MATTER OF BEING "HIGH".  PEI publishes that exact range as an
  // EFI_RESOURCE_SYSTEM_MEMORY resource HOB *and* an EfiReservedMemoryType
  // allocation HOB (MemoryInitPeiLib; the measured UEFI memory map shows
  // "[0x103FFFF0000, 0x10400000000) 16 pages Reserved").  Windows' root
  // memory arbiter builds its assignable range list by subtracting the
  // firmware-reported system-memory ranges, so a device asking for that range
  // is asking to own loader-owned reserved DRAM and is refused.  A refused
  // descriptor fails the whole devnode -- there is no partial grant, and no
  // fallback -- so this can only ever be data, never a resource.
  //
  // CORRECTION, 2026-07-30.  An earlier version of this comment also claimed
  // that "the only high consumer resource ever tested on this machine (native
  // XHC1 at 0xB02280000) was refused outright".  That claim is FALSE and is
  // the same falsified premise commit 848b3b5 used for the PCIe producer
  // windows, reverted in 1ea4a93: the only CM_PROB_NORMAL_CONFLICT ever
  // measured on ACPI\PNP0D15\1 was cleared by removing its *Interrupt*
  // descriptor (GSIV 1274, inside the 1024..4095 range the ARM64 interrupt
  // arbiter refuses), with the memory descriptor left in place.  No memory
  // descriptor has ever been refused on this machine, at any address.
  //
  // So the DART register aperture at 0x594000000 IS arbitrable in principle:
  // it is genuine MMIO outside DRAM, it does not overlap PCI0's producer
  // windows (0x5A0000000..0x5BFFFFFFF prefetchable and
  // 0x5C0000000..0x5FFFFFFFF non-prefetchable), and nothing else in these
  // tables claims it.  It is nevertheless left out of _CRS for now, for one
  // reason only: AppleDart.sys must implement the method channel regardless
  // (for the reservation), so publishing the aperture as a resource adds an
  // arbiter dependency with no fallback and buys nothing.  Restoring an
  // aperture-only _CRS later is a firmware-only change -- the driver already
  // accepts one and cross-checks it against DRTB/DRTL rather than silently
  // preferring either channel.
  //
  // AppleDart.sys therefore discovers both ranges by evaluating the integer
  // methods below (IOCTL_ACPI_EVAL_METHOD to its own PDO) and maps them with
  // MmMapIoSpaceEx.  The addresses stay TRUE physical addresses, so every
  // physical-equality check of the wireless-handoff ABI v2 descriptor
  // (reservation_base, l1_physical, dart_base, TTBR adoption) is unchanged.
  // The descriptor's signature/CRC validation -- not ACPI -- remains the
  // proof of authenticity, exactly as docs/
  // apple-bcm-wireless-dart-handoff-abi-spec.md specifies; ACPI is only the
  // coarse "where to look" channel, and a method serves that role as well as
  // a _CRS entry without exposing the range to resource arbitration.
  //
  //   DRTB / DRTH  the fixed T8110 DART aperture base (silicon constant), and
  //                its bits 63:32.
  //   DRTL         the aperture length.
  //   RSVB / RSVH  this boot's derived, re-authenticated reservation base,
  //                and its bits 63:32.
  //   RSVS         the reservation size.
  //
  // WHY DRTH/RSVH EXIST.  Both bases are wider than 32 bits (0x5_94000000 and
  // 0x103_FFFF0000), and acpi.sys' reply format is not obviously 64-bit
  // clean: ACPI_METHOD_ARGUMENT's integer union member is `ULONG Argument` in
  // both the V1 and the V2 layouts, and the only ULONG64 fields in
  // acpiioct.h are *input* arguments of the _EX variants.  The reply does
  // carry an explicit DataLength, so an 8-byte integer is representable, but
  // whether this ACPI build ever emits one cannot be established without
  // booting -- and guessing wrong costs a boot and produces a driver that
  // maps a wrong physical address.  Each high half always fits in 32 bits and
  // therefore survives any truncation; AppleDart.sys recombines the pair and
  // logs whether the primary method was already intact or had to be repaired.
  // This is belt-and-braces on purpose: if the ACPI build turns out to be
  // 64-bit clean, DRTH/RSVH are simply redundant cross-checks.
  //
  Status = AmlCodeGenMethodRetInteger ("DRTB", NTASI_WIRELESS_DART_APERTURE_BASE, 0, FALSE, 0, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenMethodRetInteger ("DRTH", NTASI_WIRELESS_DART_APERTURE_BASE >> 32, 0, FALSE, 0, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenMethodRetInteger ("DRTL", NTASI_WIRELESS_DART_APERTURE_SIZE, 0, FALSE, 0, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenMethodRetInteger ("RSVB", ReservationBase, 0, FALSE, 0, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenMethodRetInteger ("RSVH", ReservationBase >> 32, 0, FALSE, 0, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenMethodRetInteger ("RSVS", ReservationSize, 0, FALSE, 0, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameInteger ("_STA", 0x0F, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  DEBUG ((DEBUG_INFO, "WirelessDART ACPI: stage \"serialize-and-install\"\n"));
  Status = AmlSerializeDefinitionBlock (RootNode, &Table);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  TableHandle = 0;
  Status      = AcpiTable->InstallAcpiTable (
                              AcpiTable,
                              Table,
                              Table->Length,
                              &TableHandle
                              );
  if (!EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "WirelessDART ACPI: DRT0 published, dart-aperture=0x%lx/+0x%lx reservation=0x%lx/+0x%x "
      "(methods DRTB/DRTH=0x%x DRTL RSVB/RSVH=0x%x RSVS, no _CRS)\n",
      NTASI_WIRELESS_DART_APERTURE_BASE,
      NTASI_WIRELESS_DART_APERTURE_SIZE,
      ReservationBase,
      ReservationSize,
      (UINT32)(NTASI_WIRELESS_DART_APERTURE_BASE >> 32),
      (UINT32)(ReservationBase >> 32)
      ));
  }

Exit:
  if (Table != NULL) {
    FreePool (Table);
  }

  if (RootNode != NULL) {
    DeleteStatus = AmlDeleteTree (RootNode);
    if (!EFI_ERROR (Status) && EFI_ERROR (DeleteStatus)) {
      Status = DeleteStatus;
    }
  }

  return Status;
}
#endif // NTASI_ENABLE_WIRELESS_DART_HANDOFF

#if NTASI_ENABLE_MEDIA_PUBLICATION
//
// ===========================================================================
// J414s MEDIA PROFILE -- MCA0 (NTAS0080), AOPA (NTAS0081), ISP0 (NTAS0090)
// ===========================================================================
//
// Publishes the three ACPI devices the J414s media drivers bind to: the MCA
// I2S/TDM audio complex (speakers and headset jack), the AOP internal PDM
// microphone array, and the FaceTime camera ISP coprocessor.
//
// GATING.  The whole block is preprocessor-excluded unless
// NTASI_ENABLE_MEDIA_PUBLICATION is 1, exactly like the GPU carveout and
// wireless DART blocks above.  A profile without the flag therefore compiles
// byte-identical firmware -- not merely "behaviourally identical" -- and this
// is why the tables are generated at runtime with AmlLib instead of being
// static ASL sources: a static .aml would land in the firmware volume of EVERY
// profile, including the baseline that boots today.  ANS0 and DRT0 use the
// same technique for the same reason.
//
// INTERRUPTS.  Seven descriptors' worth of vectors across the three devices,
// each list appended AFTER that device's memory windows:
//
//   MCA0  40, 41, 42, 43, 45   PUBLISHED GSIVs; the real lines (1218, 1211,
//                              1213, 1221, 1231) are above the GIC carrier's
//                              1019 limit and are illegal as GSIVs, so the
//                              CSRT ALI2 tail translates them.
//   AOPA  631                  real AIC line, below 1019, identity-mapped.
//   ISP0  569                  real AIC line, below 1019, identity-mapped.
//
// Because MCA0 needs translations, the media profile MUST build the
// "m2-pro-media" CSRT (8 aliases, 296 bytes,
// sha256 a082eb6c95a12a29...) instead of the ordinary "m2-pro" (3 aliases, 256
// bytes, sha256 cdee0da81d9c54c1...).  CSRT.aslc selects it on this same
// NTASI_ENABLE_MEDIA_PUBLICATION flag, so the two cannot get out of step, and
// it #errors at compile time if the GPU profile is selected alongside --
// published GSIV 40 means the AGX mailbox there and admac-sio here.
//
// Every non-media profile's CSRT is byte-for-byte unchanged; the media table
// is a strict SUPERSET, so the boot USB controller's 37 -> 1274 alias is
// bit-identical in both.
//
// NOT 44 anywhere: AIC 44 is claimed by /arm-io/i2c0/hpmBusManager.
//
// MEASURED, and the reason these are an investment rather than a fix: of the
// three drivers only AppleIsp consumes an interrupt resource at all.
// AppleMcaAudio and AppleAopAudio contain no CmResourceTypeInterrupt handling
// and no WdfInterrupt object, so six of the seven vectors are inert for the
// shipped binaries, whose bring-up is wholly polled.  They are published so
// the streaming path's resources are already arbitrated and so a grant now is
// evidence of a grant later -- at the cost that every one of them must be
// satisfiable for its devnode to start.  See
// apple_silicon_nt_drivers/docs/j414s-media-gsiv-allocation.md.
//
// _CRS ORDER IS A CONTRACT.  All three drivers match memory descriptors
// POSITIONALLY by index and fail closed only on a SHORT list -- a REORDERED
// list is not detected.  For AppleIsp a reorder would put a DART TTBR write
// into a coprocessor control register.  The window tables below are therefore
// the whole specification; nothing may be inserted, removed or reordered
// without changing the matching NTASI_*_RES_* indices in the drivers.
//   MCA0: 9 windows.  AppleMcaMapResources() accepts exactly 8, 9 or 12 and
//         refuses anything between, because a partial capture set would
//         silently shift every later index.  Nine is the "no capture
//         resources" shape; the three capture windows (i2c2, pinctrl_nub,
//         sio_dart) are added all-three-or-none, and are withheld for first
//         light because they arm code that mutates hardware -- a CS42L84 GPIO
//         reset and a sio_dart stream programming.
//   ISP0: 8 windows.  Window 4's length is 0x4034 EXACTLY, not page-rounded:
//         that is what isp0's own `reg` index 1 publishes in the live ADT and
//         it is one byte past ps_isp_clr at offset 0x4030.
//   AOPA: 4 windows.  The AOP mailbox is deliberately NOT a fifth window -- it
//         is ASC + 0x8000, already inside window 0, and the driver derives it.
//
// NOTHING HERE CAN MAKE A SOUND.  ntasp,mca-allow-render is intentionally
// absent.  The MCA render path is gated by an ACPI opt-in AND a registry
// opt-in AND a compile-time authorisation constant that is 0; all three stay
// closed, and adding that property here would open the first of them.
//
// KNOWN RESOURCE OVERLAP, STATED RATHER THAN HIDDEN.  MCA0 window 4 is
// [0x290280000, 0x290280FFF] and ISP0 window 4 is [0x290280000, 0x290284033].
// KBL0 (NTAS0051) already claims that page exclusively (KBL.asl).  This is the
// same defect class as the NTAS2003-vs-KBL0 pmgr_east collision fixed on
// 2026-07-30 -- see "THE FOUR PMGR WORDS ARE NO LONGER _CRS RESOURCES" below
// -- and the same fix applies: publish the base as _DSD data, not as a
// resource claim.  It is NOT applied here because it is not firmware's to
// make: both drivers index _CRS positionally, so removing window 4 shifts
// every later window and breaks the contract above.  Expect
// CM_PROB_NORMAL_CONFLICT (Code 12) on one of KBL0 / MCA0 / ISP0 while the
// media profile is selected.  No other profile is affected: with the flag off,
// none of these three devices exists.
//
// The specification these tables implement is
// Platform/MacBookProEarly2023Pkg/AcpiTables/Media/{MCA,AOPA,ISP}.asl, which
// the build does not compile.  Tests/test_j414s_media_acpi_contract.py pins
// the two against each other so they cannot drift.
//
// Only the STRING-valued _DSD properties of those files are omitted: this
// AmlLib has AmlAddNameIntegerPackage() but no string equivalent.  No driver
// reads _DSD at all, so the omission is documentary rather than functional.
//

typedef struct {
  UINT64    Base;
  UINT64    Length;
} NTASI_MEDIA_WINDOW;

typedef struct {
  CONST CHAR8    *Name;
  UINT64         Value;
} NTASI_MEDIA_PROPERTY;

//
// The largest published interrupt list of any media device (MCA0's five).
// A fixed bound lets the emitter copy into a stack buffer, which is what keeps
// the tables below CONST: AmlCodeGenRdInterrupt() takes a non-const UINT32 *.
//
#define NTASI_MEDIA_MAX_INTERRUPTS  5u

typedef struct {
  CONST CHAR8                   *DeviceName;
  CONST CHAR8                   *HardwareId;
  CONST CHAR8                   *OemTableId;
  CONST NTASI_MEDIA_WINDOW      *Windows;
  UINTN                         WindowCount;
  CONST UINT32                  *Interrupts;
  UINTN                         InterruptCount;
  CONST NTASI_MEDIA_PROPERTY    *Properties;
  UINTN                         PropertyCount;
} NTASI_MEDIA_DEVICE;

//
// MCA0 -- NTAS0080, speakers and headset jack.  Order per MCA.asl.
//
STATIC CONST NTASI_MEDIA_WINDOW  mNtasiMcaWindows[] = {
  { 0x39B600000ULL, 0x10000ULL },  // 0: MCA cluster registers (4 x 0x4000)
  { 0x39B500000ULL, 0x20000ULL },  // 1: MCA switch / DMA glue
  { 0x39B400000ULL, 0x34000ULL },  // 2: ADMAC (audio DMA)
  { 0x28E03C000ULL, 0x14000ULL },  // 3: NCO clock generator (5 x 0x4000)
  { 0x290280000ULL, 0x1000ULL  },  // 4: pmgr_east PS page (overlaps KBL0)
  { 0x39B044000ULL, 0x4000ULL  },  // 5: i2c1 -- left amps
  { 0x39B04C000ULL, 0x4000ULL  },  // 6: i2c3 -- right amps
  { 0x39B028000ULL, 0x4000ULL  },  // 7: pinctrl_ap -- speaker SDZ is pin 57
  { 0x28E03807CULL, 0x18ULL    },  // 8: mca-switch clock mux, sub-page by design
};

//
// MCA0's PUBLISHED GSIVs -- not its physical AIC lines.  Every line in this
// subsystem (1211-1231) is above the GIC carrier's 1019 limit and is illegal
// as a GSIV, so the CSRT's ALI2 tail translates them:
//
//   40 -> 1218 admac-sio   41 -> 1211 mca0   42 -> 1213 mca2
//   43 -> 1221 i2c2        45 -> 1231 dart-sio
//
// This is exactly why the media profile must build the "m2-pro-media" CSRT
// (8 aliases, 296 bytes) rather than the ordinary "m2-pro" (3 aliases, 256
// bytes); CSRT.aslc selects it on the same NTASI_ENABLE_MEDIA_PUBLICATION flag
// and #errors if the GPU profile -- which gives 40 a different meaning -- is
// selected alongside.
//
// NOT 44: AIC 44 belongs to /arm-io/i2c0/hpmBusManager in the live ADT.
//
STATIC CONST UINT32  mNtasiMcaInterrupts[] = { 40, 41, 42, 43, 45 };

STATIC CONST NTASI_MEDIA_PROPERTY  mNtasiMcaProperties[] = {
  { "ntasp,mca-cluster-count",              4          },
  { "ntasp,mca-speaker-cluster-left",       0          },
  { "ntasp,mca-speaker-cluster-right",      1          },
  { "ntasp,mca-jack-cluster",               2          },
  { "ntasp,nco-ref-hz",                     1068000000 },
  { "ntasp,mca-slot-width",                 32         },
  { "ntasp,mca-bclk-ratio-speakers",        256        },
  { "ntasp,mca-bclk-ratio-jack",            64         },
  { "ntasp,admac-irq-output-index",         1          },
  { "ntasp,speaker-amp-count",              6          },
  { "ntasp,speaker-sdz-gpio",               57         },
  { "ntasp,speaker-irq-gpio",               58         },
  { "ntasp,jack-irq-gpio",                  59         },
  { "ntasp,speaker-safe-dvc-floor",         40         },
  { "ntasp,speaker-resting-dvc",            200        },
  { "ntasp,speaker-amp-gain-ceiling",       15         },
  { "ntasp,preboot-handoff-required",       0          },
  { "ntasp,clk-mux-window-published",       1          },
  { "ntasp,clk-mux-register-count",         6          },
  { "ntasp,mca-interrupts-published",       5          },
  { "ntasp,mca-csrt-ali2-required",         1          },
  { "ntasp,mca-capture-windows-published",  0          },
  { "ntasp,mca-clusters-instantiated",      3          },
  { "ntasp,adt-speaker-cluster",            0          },
  { "ntasp,adt-loopback-cluster",           1          },
  { "ntasp,admac-channel-jack-capture",     11         },
  { "ntasp,admac-channel-speaker-play",     0          },
};

//
// AOPA -- NTAS0081, internal PDM microphone array.  Order per AOPA.asl.
//
STATIC CONST NTASI_MEDIA_WINDOW  mNtasiAopWindows[] = {
  { 0x2A6400000ULL, 0x6C000ULL  },  // 0: aop ASC control (mailbox at +0x8000)
  { 0x2A6C00000ULL, 0x250000ULL },  // 1: aop SRAM / mmio window
  { 0x2A6808000ULL, 0x4000ULL   },  // 2: aop_dart (T8110)
  { 0x2A6980000ULL, 0x34000ULL  },  // 3: aop_admac
};

//
// AIC 631 (admac-aop-audio).  Below 1019, so identity-mapped with no ALI2
// entry.  613/614/615/616 (mailbox) and 628 (dart-aop) are equally legal and
// deliberately not published: each is another descriptor the arbiter must
// satisfy for a devnode that reads none of them.
//
STATIC CONST UINT32  mNtasiAopInterrupts[] = { 631 };

STATIC CONST NTASI_MEDIA_PROPERTY  mNtasiAopProperties[] = {
  { "ntasp,aop-mic-rate-hz",              48000     },
  { "ntasp,aop-mic-channels",             3         },
  { "ntasp,aop-mic-sample-bits",          32        },
  { "ntasp,aop-mic-period-bytes-min",     256       },
  { "ntasp,aop-mic-period-bytes-max",     16384     },
  { "ntasp,aop-admac-channel",            1         },
  { "ntasp,aop-admac-irq-output-index",   2         },
  { "ntasp,aop-dart-stream-aop",          0         },
  { "ntasp,aop-dart-stream-admac",        10        },
  { "ntasp,aop-dart-page-size",           16384     },
  { "ntasp,aop-aic-mailbox-0",            613       },
  { "ntasp,aop-aic-mailbox-1",            614       },
  { "ntasp,aop-aic-mailbox-2",            615       },
  { "ntasp,aop-aic-mailbox-3",            616       },
  { "ntasp,aop-aic-dart",                 628       },
  { "ntasp,aop-aic-admac",                631       },
  { "ntasp,aop-interrupts-published",     1         },
  { "ntasp,aop-csrt-ali2-required",       0         },
  { "ntasp,aop-mailbox-offset",           0x8000    },
  { "ntasp,aop-cpu-control-offset",       0x44      },
  { "ntasp,aop-cpu-run-bit",              0x10      },
  { "ntasp,aop-bootargs-ptr-offset",      0x22C     },
  { "ntasp,aop-bootargs-size-offset",     0x230     },
  { "ntasp,aop-firmware-preloaded",       1         },
  { "ntasp,aop-pdm-frequency-hz",         2400000   },
  { "ntasp,aop-pdmc-frequency-hz",        24000000  },
  { "ntasp,aop-pdm-bytes-per-sample",     2         },
  { "ntasp,aop-pdm-filter-lengths",       0x00542C47},
  { "ntasp,aop-pdm-ratio1",               15        },
  { "ntasp,aop-pdm-ratio2",               5         },
  { "ntasp,aop-pdm-ratio3",               2         },
  { "ntasp,aop-decimator-latency",        15        },
  { "ntasp,aop-mic-turn-on-time-ms",      20        },
  { "ntasp,aop-mic-settle-time-ms",       50        },
  { "ntasp,aop-pdm-coefficient-taps",     100       },
  { "ntasp,aop-pdm-coefficient-slots",    120       },
};

//
// ISP0 -- NTAS0090, FaceTime camera.  Order per ISP.asl.
//
STATIC CONST NTASI_MEDIA_WINDOW  mNtasiIspWindows[] = {
  { 0x384000000ULL, 0x2000000ULL },  // 0: ISP coprocessor
  { 0x386104000ULL, 0x100ULL     },  // 1: ISP mailbox
  { 0x386104170ULL, 0x100ULL     },  // 2: ISP scratch words ("gpio", not GPIO)
  { 0x3861043F0ULL, 0x100ULL     },  // 3: ISP mailbox 2
  { 0x290280000ULL, 0x4034ULL    },  // 4: pmgr_east, length verbatim, overlaps KBL0
  { 0x3860E8000ULL, 0x4000ULL    },  // 5: dart-isp0 DARTLLT
  { 0x3860F4000ULL, 0x4000ULL    },  // 6: dart-isp0 DARTBULK
  { 0x3860FC000ULL, 0x4000ULL    },  // 7: dart-isp0 DARTRT
};

//
// AIC 569.  Below 1019, identity-mapped, no ALI2 entry.  The ADT also lists
// 570/571/572; Linux wires only 569.  Do not add them without changing the
// driver: AppleIsp ASSIGNS Device->Gsiv per descriptor rather than
// accumulating, so it keeps the last one it sees.
//
STATIC CONST UINT32  mNtasiIspInterrupts[] = { 569 };

STATIC CONST NTASI_MEDIA_PROPERTY  mNtasiIspProperties[] = {
  { "ntasp,isp-camera-config-index",        0             },
  { "ntasp,isp-camera-config-index-pinned", 1             },
  { "ntasp,isp-platform-id",                7             },
  { "ntasp,isp-sensor-native-dim",          1920          },
  { "ntasp,isp-mode-count",                 10            },
  { "ntasp,isp-published-mode-count",       5             },
  { "ntasp,isp-stride-alignment",           64            },
  { "ntasp,isp-frame-rate-max",             30            },
  { "ntasp,isp-frame-rate-min",             15            },
  { "ntasp,isp-firmware-preloaded",         1             },
  { "ntasp,isp-firmware-carveout-base",     0x100009FC000 },
  { "ntasp,isp-firmware-carveout-size",     0x1284000     },
  { "ntasp,isp-firmware-text-iova",         0x0           },
  { "ntasp,isp-firmware-data-iova",         0x934000      },
  { "ntasp,isp-firmware-iova-span",         0xC48000      },
  { "ntasp,isp-dart-node-count",            1             },
  { "ntasp,isp-dart-window-count",          6             },
  { "ntasp,isp-dart-translation-count",     3             },
  { "ntasp,isp-dart-windows-published",     3             },
  { "ntasp,isp-dart-sid",                   0             },
  { "ntasp,isp-dart-page-shift",            14            },
  { "ntasp,isp-dart-pa-width",              42            },
  { "ntasp,isp-dart-vm-size",               0xA0000000    },
  { "ntasp,isp-dart-adopt-inherited-table", 1             },
  { "ntasp,isp-gsiv",                       569           },
  { "ntasp,isp-gsiv-needs-ali2",            0             },
  { "ntasp,isp-interrupts-published",       1             },
  { "ntasp,isp-adt-irq-count",              4             },
  { "ntasp,isp-bringup-is-polled",          1             },
  { "ntasp,isp-delivers-frames",            0             },
  { "ntasp,isp-power-domain-count",         7             },
};

//
// One SSDT per device, matching the three DefinitionBlocks in the ASL specs.
// OEM ID and OEM table ID are byte-identical to what iasl emits for those
// files (both fields are NUL-padded by iasl, and CopyMem() copies the same
// 6 and 8 bytes from these literals).
//
STATIC CONST NTASI_MEDIA_DEVICE  mNtasiMediaDevices[] = {
  {
    "MCA0", "NTAS0080", "J414MCA",
    mNtasiMcaWindows, ARRAY_SIZE (mNtasiMcaWindows),
    mNtasiMcaInterrupts, ARRAY_SIZE (mNtasiMcaInterrupts),
    mNtasiMcaProperties, ARRAY_SIZE (mNtasiMcaProperties)
  },
  {
    "AOPA", "NTAS0081", "J414AOPA",
    mNtasiAopWindows, ARRAY_SIZE (mNtasiAopWindows),
    mNtasiAopInterrupts, ARRAY_SIZE (mNtasiAopInterrupts),
    mNtasiAopProperties, ARRAY_SIZE (mNtasiAopProperties)
  },
  {
    "ISP0", "NTAS0090", "J414ISP",
    mNtasiIspWindows, ARRAY_SIZE (mNtasiIspWindows),
    mNtasiIspInterrupts, ARRAY_SIZE (mNtasiIspInterrupts),
    mNtasiIspProperties, ARRAY_SIZE (mNtasiIspProperties)
  },
};

/**
  Build and install one media device's SSDT.

  Fails closed: any AmlLib error abandons the whole device rather than
  installing a table with a short or partial _CRS, because a short list is what
  the drivers detect and a partial one is what they cannot.

  @param[in] AcpiTable  The ACPI table protocol.
  @param[in] Device     The device description to publish.

  @retval EFI_SUCCESS   The SSDT was built and installed.
  @retval other         Nothing was installed.
**/
STATIC
EFI_STATUS
NtasiInstallMediaDevice (
  IN EFI_ACPI_TABLE_PROTOCOL      *AcpiTable,
  IN CONST NTASI_MEDIA_DEVICE     *Device
  )
{
  EFI_STATUS                   Status;
  EFI_STATUS                   DeleteStatus;
  AML_ROOT_NODE_HANDLE         RootNode;
  AML_OBJECT_NODE_HANDLE       ScopeNode;
  AML_OBJECT_NODE_HANDLE       DeviceNode;
  AML_OBJECT_NODE_HANDLE       CrsNode;
  AML_OBJECT_NODE_HANDLE       DsdNode;
  AML_OBJECT_NODE_HANDLE       DsdPackageNode;
  EFI_ACPI_DESCRIPTION_HEADER  *Table;
  UINTN                        TableHandle;
  UINTN                        Index;

  RootNode = NULL;
  Table    = NULL;

  DEBUG ((
    DEBUG_INFO,
    "AppleMedia ACPI: stage \"build-ssdt\" device=%a hid=%a windows=%u properties=%u\n",
    Device->DeviceName,
    Device->HardwareId,
    (UINT32)Device->WindowCount,
    (UINT32)Device->PropertyCount
    ));

  Status = AmlCodeGenDefinitionBlock ("SSDT", "Apple", Device->OemTableId, 1, &RootNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenScope ("\\_SB_", RootNode, &ScopeNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenDevice (Device->DeviceName, ScopeNode, &DeviceNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameString ("_HID", Device->HardwareId, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameInteger ("_UID", 0, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  //
  // _CCA = 1 for all three.  Their DMA is coherent with the CPU caches, which
  // is what lets the drivers map their IPC and frame surfaces MmCached and
  // skip explicit flushes.  Changing this to Zero and changing the drivers'
  // surface allocators to MmNonCached are one decision, not two.
  //
  Status = AmlCodeGenNameInteger ("_CCA", 1, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameInteger ("_STA", 0x0F, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameResourceTemplate ("_CRS", DeviceNode, &CrsNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  //
  // The windows go in strictly in table order, and the interrupt descriptor
  // (if any) is appended only AFTER all of them -- see the loop below for why
  // that ordering is load-bearing.  AppleAnsAddMemoryResource is reused
  // verbatim: it is a plain QWordMemory ResourceConsumer/PosDecode/MinFixed/
  // MaxFixed/NonCacheable/ReadWrite emitter with nothing ANS-specific in it,
  // and duplicating it would create a second copy to keep in step.
  //
  for (Index = 0; Index < Device->WindowCount; Index++) {
    Status = AppleAnsAddMemoryResource (
               CrsNode,
               Device->Windows[Index].Base,
               Device->Windows[Index].Length
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "AppleMedia ACPI: %a window %u (0x%lx/+0x%lx) refused: %r; device withheld\n",
        Device->DeviceName,
        (UINT32)Index,
        Device->Windows[Index].Base,
        Device->Windows[Index].Length,
        Status
        ));
      goto Exit;
    }
  }

  //
  // Interrupts LAST, after every memory window, as one descriptor carrying the
  // whole vector list -- byte-for-byte the shape the ASL specs compile to.
  //
  // The ordering is not cosmetic.  All three drivers count memory descriptors
  // in their own index space while handling interrupts in a separate branch,
  // so an interrupt appended at the end cannot shift a window; an interrupt
  // placed FIRST would still not shift a window, but it would put this
  // generator out of step with the ASL that the contract test diffs it
  // against, and there is no reason to invite that.
  //
  // Level-triggered, active-high, exclusive: AIC lines are level/active-high,
  // and the translated descriptor -- not this one -- is what carries the
  // synchronisation IRQL the drivers must use.
  //
  // MCA0's five are PUBLISHED GSIVs that only mean anything because the media
  // CSRT translates them; AOPA's 631 and ISP0's 569 are real AIC lines below
  // the carrier limit, published identity-mapped.
  //
  if (Device->InterruptCount != 0) {
    UINT32  Irqs[NTASI_MEDIA_MAX_INTERRUPTS];

    if (Device->InterruptCount > ARRAY_SIZE (Irqs)) {
      //
      // Fail closed rather than truncate.  A silently shortened interrupt list
      // is exactly the class of bug the _CRS contract exists to prevent.
      //
      DEBUG ((
        DEBUG_ERROR,
        "AppleMedia ACPI: %a declares %u interrupts, max is %u; device withheld\n",
        Device->DeviceName,
        (UINT32)Device->InterruptCount,
        (UINT32)ARRAY_SIZE (Irqs)
        ));
      Status = EFI_INVALID_PARAMETER;
      goto Exit;
    }

    //
    // Copied to a local because AmlCodeGenRdInterrupt() takes a non-const
    // UINT32 *, and the tables above are deliberately CONST.
    //
    for (Index = 0; Index < Device->InterruptCount; Index++) {
      Irqs[Index] = Device->Interrupts[Index];
    }

    Status = AmlCodeGenRdInterrupt (
               TRUE,                                  // ResourceConsumer
               FALSE,                                 // EdgeTriggered -> Level
               FALSE,                                 // ActiveLow -> ActiveHigh
               FALSE,                                 // Shared -> Exclusive
               Irqs,
               (UINT8)Device->InterruptCount,
               CrsNode,
               NULL
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "AppleMedia ACPI: %a interrupt list refused: %r; device withheld\n",
        Device->DeviceName,
        Status
        ));
      goto Exit;
    }
  }

  //
  // _DSD carries data, never a resource claim, so nothing below is visible to
  // the OS resource arbiter.  ntasp,mca-allow-render is deliberately absent
  // from every table: it is the ACPI half of the MCA render gate and the
  // speakers have no thermal protection on Windows.
  //
  if (Device->PropertyCount != 0) {
    DsdNode        = NULL;
    DsdPackageNode = NULL;

    Status = AmlCodeGenNamePackage ("_DSD", DeviceNode, &DsdNode);
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    Status = AmlAddDeviceDataDescriptorPackage (
               &gAppleAnsDsdPropertiesGuid,
               DsdNode,
               &DsdPackageNode
               );
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    for (Index = 0; Index < Device->PropertyCount; Index++) {
      Status = AmlAddNameIntegerPackage (
                 Device->Properties[Index].Name,
                 Device->Properties[Index].Value,
                 DsdPackageNode
                 );
      if (EFI_ERROR (Status)) {
        goto Exit;
      }
    }
  }

  Status = AmlSerializeDefinitionBlock (RootNode, &Table);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  TableHandle = 0;
  Status      = AcpiTable->InstallAcpiTable (
                             AcpiTable,
                             Table,
                             Table->Length,
                             &TableHandle
                             );
  if (!EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "AppleMedia ACPI: %a (%a) published, %u memory windows, 0 interrupts, "
      "%u _DSD properties, no render opt-in\n",
      Device->DeviceName,
      Device->HardwareId,
      (UINT32)Device->WindowCount,
      (UINT32)Device->PropertyCount
      ));
  }

Exit:
  if (Table != NULL) {
    FreePool (Table);
  }

  if (RootNode != NULL) {
    DeleteStatus = AmlDeleteTree (RootNode);
    if (!EFI_ERROR (Status) && EFI_ERROR (DeleteStatus)) {
      Status = DeleteStatus;
    }
  }

  return Status;
}

/**
  Publish every media device.  Each is independent: one failing does not
  withhold the others, and none is fatal to the boot.  A machine that reaches
  Windows with two of three media devices is strictly better than one that does
  not reach Windows at all, and this whole feature is an experiment behind a
  default-off profile flag.

  @param[in] AcpiTable  The ACPI table protocol.
**/
STATIC
VOID
NtasiInstallMediaTables (
  IN EFI_ACPI_TABLE_PROTOCOL  *AcpiTable
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  for (Index = 0; Index < ARRAY_SIZE (mNtasiMediaDevices); Index++) {
    Status = NtasiInstallMediaDevice (AcpiTable, &mNtasiMediaDevices[Index]);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "AppleMedia ACPI: %a SSDT installation failed: %r\n",
        mNtasiMediaDevices[Index].DeviceName,
        Status
        ));
    }
  }
}
#endif // NTASI_ENABLE_MEDIA_PUBLICATION

/**
  Publish the native Apple ANS controller to Windows.  Addresses and the
  hardware profile are derived from the live Apple Device Tree so one firmware
  binary does not bake in a board-specific MMIO map.

  The three memory resources have a stable ABI with the Windows miniport:
    0: ASC CPU/mailbox aperture (mailbox registers are at +0x8000)
    1: ANS NVMe aperture (ADT reg[3])
    2: SART aperture

  CHANGED 2026-07-30: NTAS2003 used to append four more resources -- the exact
  4-byte ps_ans2 / ps_apcie_st / ps_apcie_st_sys / ps_apcie_st1_sys PMGR words
  -- as _CRS indices 3-6. Every one of them lands inside the 4 KiB page KBL0
  already claims (KBL.asl:75-87), so two _STA=0x0F devices were claiming the
  same memory exclusively. They are now published as _DSD integer properties
  instead; see the comment at the removal site for the full rationale. A
  future ANS driver must read them from _DSD, not from _CRS.
**/
STATIC
EFI_STATUS
AcpiPlatformInstallAppleAnsTable (
  IN EFI_ACPI_TABLE_PROTOCOL  *AcpiTable
  )
{
  EFI_STATUS                   Status;
  EFI_STATUS                   DeleteStatus;
  dt_node_t                    *AnsNode;
  dt_node_t                    *SartNode;
  AML_ROOT_NODE_HANDLE         RootNode;
  AML_OBJECT_NODE_HANDLE       ScopeNode;
  AML_OBJECT_NODE_HANDLE       DeviceNode;
  AML_OBJECT_NODE_HANDLE       CrsNode;
  EFI_ACPI_DESCRIPTION_HEADER  *Table;
  UINTN                        TableHandle;
  UINT64                       CpuBase;
  UINT64                       CpuSize;
  UINT64                       NvmeBase;
  UINT64                       NvmeSize;
  UINT64                       SartBase;
  UINT64                       SartSize;
  UINT64                       PmgrResetBase;
  UINT64                       PmgrApcieStBase;
  UINT64                       PmgrApcieStSysBase;
  UINT64                       PmgrApcieSt1SysBase;
  UINT64                       NvmeMinimumSize;
  UINT64                       SartMinimumSize;
  UINT32                       AcpiInterrupt;
  UINT32                       ExpectedPhysicalInterrupt;
  UINT32                       PhysicalInterrupt;
  UINT32                       SartVersion;
  UINT32                       *VersionProperty;
  UINT32                       NvmeInterruptIndex;
  UINT32                       *InterruptIndexProperty;
  UINT32                       *InterruptsProperty;
  UINTN                        PropertySize;
  UINTN                        InterruptsSize;
  BOOLEAN                      Legacy;
  CONST CHAR8                  *HardwareId;
  CONST CHAR8                  *InterruptContract;

  RootNode = NULL;
  Table    = NULL;
  PmgrResetBase = 0;
  PmgrApcieStBase = 0;
  PmgrApcieStSysBase = 0;
  PmgrApcieSt1SysBase = 0;

  //
  // Do not publish the ANS controller in a build whose FV has no
  // AppleNANDStorageDxe: an NTAS200x device would enumerate with no driver
  // behind it.  This was an unconditional return while ANS was quarantined out
  // of the input profile, which silently survived re-enabling the DXE and left
  // the ANS build carrying a driver that nothing in ACPI ever pointed at.
  //
  if (!FixedPcdGetBool (PcdAppleAnsPublishAcpiDevice)) {
    return EFI_NOT_FOUND;
  }

  AnsNode  = dt_get ("/arm-io/ans");
  SartNode = dt_get ("/arm-io/sart-ans");
  if ((AnsNode == NULL) || (SartNode == NULL)) {
    DEBUG ((DEBUG_WARN, "AppleANS ACPI: ANS or SART node is absent\n"));
    return EFI_NOT_FOUND;
  }

  if ((dt_node_reg (AnsNode, 0, &CpuBase, &CpuSize) != 0) ||
      (dt_node_reg (AnsNode, 3, &NvmeBase, &NvmeSize) != 0) ||
      (dt_node_reg (SartNode, 0, &SartBase, &SartSize) != 0))
  {
    return EFI_DEVICE_ERROR;
  }

  //
  // Apple ADT keeps all ASC mailbox and NVMe interrupts in one UINT32 array.
  // nvme-interrupt-idx identifies the dedicated controller interrupt within
  // that array (currently index 4).  Derive it from the live ADT so the ACPI
  // GSIV remains correct across SoCs and dies.
  //
  InterruptIndexProperty = dt_node_prop (
                             AnsNode,
                             "nvme-interrupt-idx",
                             &PropertySize
                             );
  InterruptsProperty = dt_node_prop (
                         AnsNode,
                         "interrupts",
                         &InterruptsSize
                         );
  if ((InterruptIndexProperty == NULL) ||
      (PropertySize < sizeof (*InterruptIndexProperty)) ||
      (InterruptsProperty == NULL))
  {
    DEBUG ((DEBUG_ERROR, "AppleANS ACPI: interrupt metadata is absent\n"));
    return EFI_NOT_FOUND;
  }

  NvmeInterruptIndex = *InterruptIndexProperty;
  if ((NvmeInterruptIndex >= InterruptsSize / sizeof (*InterruptsProperty)) ||
      (NvmeInterruptIndex > MAX_UINT8))
  {
    DEBUG ((
      DEBUG_ERROR,
      "AppleANS ACPI: invalid NVMe interrupt index %u for %u bytes\n",
      NvmeInterruptIndex,
      (UINT32)InterruptsSize
      ));
    return EFI_DEVICE_ERROR;
  }

  PhysicalInterrupt = InterruptsProperty[NvmeInterruptIndex];

  //
  // Windows' architectural GIC interrupt arbiter refuses T6020's physical
  // AIC line 1832 because it falls in GIC's reserved 1024..4095 INTID gap.
  // A platform may therefore publish an arbiter-legal GSIV and describe the
  // one-to-one mapping in the AIC2 CSRT ALI2 tail.  Require both PCDs as a
  // pair and verify the physical line against the live ADT before publishing
  // the alias.  A zero/zero pair retains the legacy direct publication for
  // platforms that do not use this contract.
  //
  AcpiInterrupt            = FixedPcdGet32 (PcdAppleAnsPublishedInterrupt);
  ExpectedPhysicalInterrupt =
    FixedPcdGet32 (PcdAppleAnsExpectedPhysicalInterrupt);
  if ((AcpiInterrupt == 0) != (ExpectedPhysicalInterrupt == 0)) {
    DEBUG ((
      DEBUG_ERROR,
      "AppleANS ACPI: refusing alias published=%u expected-physical=%u live-physical=%u\n",
      AcpiInterrupt,
      ExpectedPhysicalInterrupt,
      PhysicalInterrupt
      ));
    return EFI_DEVICE_ERROR;
  }

  if (ExpectedPhysicalInterrupt != 0) {
    if (PhysicalInterrupt != ExpectedPhysicalInterrupt) {
      DEBUG ((
        DEBUG_ERROR,
        "AppleANS ACPI: refusing alias published=%u expected-physical=%u live-physical=%u\n",
        AcpiInterrupt,
        ExpectedPhysicalInterrupt,
        PhysicalInterrupt
        ));
      return EFI_DEVICE_ERROR;
    }

    InterruptContract = "published-gsiv-to-physical-aic";
  } else {
    AcpiInterrupt     = PhysicalInterrupt;
    InterruptContract = "physical-aic-line";
  }

  Legacy = AppleAnsPropertyContains (AnsNode, "compatible", "t8015");
  NvmeMinimumSize = Legacy ? APPLE_ANS_NVME_T8015_MIN_SIZE : APPLE_ANS_NVME_MIN_SIZE;
  VersionProperty = dt_node_prop (SartNode, "sart-version", &PropertySize);
  if ((VersionProperty != NULL) && (PropertySize >= sizeof (*VersionProperty))) {
    SartVersion = *VersionProperty;
  } else if (Legacy ||
             AppleAnsPropertyContains (SartNode, "compatible", "t8015"))
  {
    SartVersion = 0;
  } else {
    return EFI_UNSUPPORTED;
  }

  if (Legacy && (SartVersion == 0)) {
    HardwareId = "NTAS1000";
    SartMinimumSize = APPLE_ANS_SART_V0_MIN_SIZE;
  } else if (!Legacy && (SartVersion == 2)) {
    HardwareId = "NTAS2002";
    SartMinimumSize = APPLE_ANS_SART_V2_MIN_SIZE;
  } else if (!Legacy && (SartVersion == 3)) {
    HardwareId = "NTAS2003";

    //
    // Resolve all four PMGR domains live from the ADT, by exact uppercase
    // name -- never from a hardcoded constant. See
    // AppleAnsPmgrResolveDomain() (Include/Drivers/AppleAnsPmgrDomain.h)
    // for why: a hardcoded constant is
    // exactly what pointed these four words at DCS_09/DCS_10 (DRAM
    // controller power domains) on 2026-07-30. Any single domain failing
    // to resolve uniquely withholds NTAS2003 entirely (EFI_NOT_FOUND,
    // handled by the caller as "no ANS device today") rather than
    // publishing three good addresses and one wrong or missing one.
    //
    if (EFI_ERROR (AppleAnsPmgrResolveDomain (mAppleAnsAcpiTag, "ANS2", &PmgrResetBase)) ||
        EFI_ERROR (AppleAnsPmgrResolveDomain (mAppleAnsAcpiTag, "APCIE_ST", &PmgrApcieStBase)) ||
        EFI_ERROR (AppleAnsPmgrResolveDomain (mAppleAnsAcpiTag, "APCIE_ST_SYS", &PmgrApcieStSysBase)) ||
        EFI_ERROR (AppleAnsPmgrResolveDomain (mAppleAnsAcpiTag, "APCIE_ST1_SYS", &PmgrApcieSt1SysBase)))
    {
      DEBUG ((
        DEBUG_ERROR,
        "AppleANS ACPI: could not resolve all four PMGR domains from the live ADT; NTAS2003 withheld\n"
        ));
      return EFI_NOT_FOUND;
    }

    //
    // Cross-check against the compiled-in PCDs. These are kept only as a
    // documented expectation and a build-time record of the last
    // hardware-confirmed values -- never as a fallback address -- so any
    // disagreement here means either the DSC constants or this
    // resolution logic has drifted from the live hardware and must be
    // investigated before trusting either one.
    //
    {
      UINT64  PcdResetBase       = FixedPcdGet64 (PcdAppleAnsPmgrResetBase);
      UINT64  PcdApcieStBase     = FixedPcdGet64 (PcdAppleAnsPmgrApcieStBase);
      UINT64  PcdApcieStSysBase  = FixedPcdGet64 (PcdAppleAnsPmgrApcieStSysBase);
      UINT64  PcdApcieSt1SysBase = FixedPcdGet64 (PcdAppleAnsPmgrApcieSt1SysBase);

      if (PcdResetBase != PmgrResetBase) {
        DEBUG ((
          DEBUG_WARN,
          "AppleANS ACPI: PcdAppleAnsPmgrResetBase 0x%lx disagrees with ADT-resolved ANS2 0x%lx\n",
          PcdResetBase,
          PmgrResetBase
          ));
      }

      if (PcdApcieStBase != PmgrApcieStBase) {
        DEBUG ((
          DEBUG_WARN,
          "AppleANS ACPI: PcdAppleAnsPmgrApcieStBase 0x%lx disagrees with ADT-resolved APCIE_ST 0x%lx\n",
          PcdApcieStBase,
          PmgrApcieStBase
          ));
      }

      if (PcdApcieStSysBase != PmgrApcieStSysBase) {
        DEBUG ((
          DEBUG_WARN,
          "AppleANS ACPI: PcdAppleAnsPmgrApcieStSysBase 0x%lx disagrees with ADT-resolved APCIE_ST_SYS 0x%lx\n",
          PcdApcieStSysBase,
          PmgrApcieStSysBase
          ));
      }

      if (PcdApcieSt1SysBase != PmgrApcieSt1SysBase) {
        DEBUG ((
          DEBUG_WARN,
          "AppleANS ACPI: PcdAppleAnsPmgrApcieSt1SysBase 0x%lx disagrees with ADT-resolved APCIE_ST1_SYS 0x%lx\n",
          PcdApcieSt1SysBase,
          PmgrApcieSt1SysBase
          ));
      }
    }

    //
    // Defense in depth on top of name-based resolution: the resolved
    // addresses must still be four aligned, distinct words, exactly as
    // required before.
    //
    if ((PmgrResetBase == 0) || (PmgrApcieStBase == 0) ||
        (PmgrApcieStSysBase == 0) || (PmgrApcieSt1SysBase == 0) ||
        ((PmgrResetBase & (APPLE_ANS_PMGR_RESET_SIZE - 1)) != 0) ||
        ((PmgrApcieStBase & (APPLE_ANS_PMGR_RESET_SIZE - 1)) != 0) ||
        ((PmgrApcieStSysBase & (APPLE_ANS_PMGR_RESET_SIZE - 1)) != 0) ||
        ((PmgrApcieSt1SysBase & (APPLE_ANS_PMGR_RESET_SIZE - 1)) != 0) ||
        (PmgrResetBase == PmgrApcieStBase) ||
        (PmgrResetBase == PmgrApcieStSysBase) ||
        (PmgrResetBase == PmgrApcieSt1SysBase) ||
        (PmgrApcieStBase == PmgrApcieStSysBase) ||
        (PmgrApcieStBase == PmgrApcieSt1SysBase) ||
        (PmgrApcieStSysBase == PmgrApcieSt1SysBase))
    {
      DEBUG ((
        DEBUG_ERROR,
        "AppleANS ACPI: NTAS2003 requires four aligned distinct PMGR words\n"
        ));
      return EFI_UNSUPPORTED;
    }
    SartMinimumSize = APPLE_ANS_SART_V3_MIN_SIZE;
  } else {
    DEBUG ((
      DEBUG_ERROR,
      "AppleANS ACPI: unsupported legacy=%d SART v%d profile\n",
      Legacy,
      SartVersion
      ));
    return EFI_UNSUPPORTED;
  }

  if (!AppleAnsMmioRangeValid (CpuBase, CpuSize, APPLE_ANS_CPU_MIN_SIZE) ||
      !AppleAnsMmioRangeValid (NvmeBase, NvmeSize, NvmeMinimumSize) ||
      !AppleAnsMmioRangeValid (SartBase, SartSize, SartMinimumSize))
  {
    DEBUG ((
      DEBUG_ERROR,
      "AppleANS ACPI: refusing MMIO cpu=%Lx/%Lx nvme=%Lx/%Lx sart=%Lx/%Lx minimum=%Lx/%Lx/%Lx\n",
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

  Status = AmlCodeGenDefinitionBlock (
             "SSDT",
             APPLE_ANS_ACPI_OEM_ID,
             APPLE_ANS_ACPI_OEM_TABLE_ID,
             1,
             &RootNode
             );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenScope ("\\_SB_", RootNode, &ScopeNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenDevice ("ANS0", ScopeNode, &DeviceNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameString ("_HID", HardwareId, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameInteger ("_UID", 0, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameInteger ("_CCA", 1, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameInteger ("_STA", 0x0F, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AmlCodeGenNameResourceTemplate ("_CRS", DeviceNode, &CrsNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AppleAnsAddMemoryResource (CrsNode, CpuBase, CpuSize);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AppleAnsAddMemoryResource (CrsNode, NvmeBase, NvmeSize);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = AppleAnsAddMemoryResource (CrsNode, SartBase, SartSize);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  //
  // THE FOUR PMGR WORDS ARE NO LONGER _CRS RESOURCES (fixed 2026-07-30).
  //
  // They used to be appended here as four exact 4-byte ResourceConsumer
  // QWordMemory descriptors at 0x2902801A8 (ps_ans2), 0x2902801A0
  // (ps_apcie_st), 0x290280408 (ps_apcie_st_sys) and 0x290280410
  // (ps_apcie_st1_sys). Every one of those is INSIDE the 4 KiB page
  // [0x290280000, 0x290280FFF] that KBL0 (NTAS0051, keyboard backlight)
  // already claims as its own ResourceConsumer memory (KBL.asl:75-87) to
  // reach ps_sio at +0x1c0 and ps_fpwm0 at +0x1e8.
  //
  // Two _STA=0x0F devices claiming the same physical memory exclusively is a
  // genuine ACPI resource conflict, and it is one this firmware created:
  // commit bef9067 (2026-07-30 01:29) moved these four words from the "pmgr"
  // block at 0x28E080xxx -- where they did not overlap anything -- to
  // "pmgr_east" at 0x290280xxx, which is correct for the hardware and
  // collided with KBL0.
  //
  // WHY THE FIX IS HERE AND NOT IN KBL.asl: PMGR power-state pages are
  // inherently shared on this silicon -- one page carries the power words of
  // many unrelated devices -- so ANY exclusive consumer claim over part of one
  // is wrong in principle. KBL0's whole-page claim is coarse, but its driver
  // is deployed and working and maps that descriptor to reach two fixed
  // offsets; narrowing it would break a live driver's ABI to fix a conflict
  // this device introduced. NTAS2003's claims are the newer and more clearly
  // mistaken ones: firmware only ever READS these words (see
  // AppleAnsPmgrReportDomain()), and no consumer needs the OS resource
  // arbiter to hand them out.
  //
  // The addresses are still published, as _DSD integer properties below.
  // _DSD carries data, not resource claims, so the arbiter never sees them --
  // which is the correct shape for "here is where this register lives" as
  // opposed to "grant this device exclusive ownership of these bytes".
  //
  // ABI NOTE: this changes NTAS2003's _CRS from seven memory resources to
  // three. Nothing consumes the old layout today -- AppleNvmeSart3 is
  // disabled (Start=4) and never loads -- so the change costs nothing now,
  // but a future ANS driver must read the four addresses from _DSD rather
  // than from _CRS indices 3-6.
  //

  Status = AmlCodeGenRdInterrupt (
             TRUE,                       // ResourceConsumer
             FALSE,                      // Level triggered
             FALSE,                      // Active high
             FALSE,                      // Exclusive
             &AcpiInterrupt,
             1,
             CrsNode,
             NULL
             );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  //
  // The PMGR power-state register addresses, as data rather than as resource
  // claims. See the comment above the _CRS memory resources for why these are
  // not QWordMemory descriptors any more.
  //
  if (SartVersion == 3) {
    AML_OBJECT_NODE_HANDLE  DsdNode;
    AML_OBJECT_NODE_HANDLE  DsdPackageNode;

    DsdNode        = NULL;
    DsdPackageNode = NULL;

    Status = AmlCodeGenNamePackage ("_DSD", DeviceNode, &DsdNode);
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    Status = AmlAddDeviceDataDescriptorPackage (
               &gAppleAnsDsdPropertiesGuid,
               DsdNode,
               &DsdPackageNode
               );
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    Status = AmlAddNameIntegerPackage ("ntasp,pmgr-ans2", PmgrResetBase, DsdPackageNode);
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    Status = AmlAddNameIntegerPackage ("ntasp,pmgr-apcie-st", PmgrApcieStBase, DsdPackageNode);
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    Status = AmlAddNameIntegerPackage ("ntasp,pmgr-apcie-st-sys", PmgrApcieStSysBase, DsdPackageNode);
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    Status = AmlAddNameIntegerPackage ("ntasp,pmgr-apcie-st1-sys", PmgrApcieSt1SysBase, DsdPackageNode);
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    Status = AmlAddNameIntegerPackage ("ntasp,pmgr-word-size", APPLE_ANS_PMGR_RESET_SIZE, DsdPackageNode);
    if (EFI_ERROR (Status)) {
      goto Exit;
    }
  }

  Status = AmlSerializeDefinitionBlock (RootNode, &Table);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  TableHandle = 0;
  Status = AcpiTable->InstallAcpiTable (
                        AcpiTable,
                        Table,
                        Table->Length,
                        &TableHandle
                        );
  if (!EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "AppleANS ACPI: %a cpu=%lx/%lx nvme=%lx/%lx sart=%lx/%lx pmgr-ans2=%lx apcie-st=%lx st-sys=%lx st1-sys=%lx "
      "pmgr-word-size=%x (published via _DSD, NOT _CRS -- those addresses are inside KBL0's page) "
      "irq=%u physical=%u contract=%a\n",
      HardwareId,
      CpuBase,
      CpuSize,
      NvmeBase,
      NvmeSize,
      SartBase,
      SartSize,
      PmgrResetBase,
      PmgrApcieStBase,
      PmgrApcieStSysBase,
      PmgrApcieSt1SysBase,
      SartVersion == 3 ? (UINT32)APPLE_ANS_PMGR_RESET_SIZE : 0,
      AcpiInterrupt,
      PhysicalInterrupt,
      InterruptContract
      ));
  }

Exit:
  if (Table != NULL) {
    FreePool (Table);
  }

  if (RootNode != NULL) {
    DeleteStatus = AmlDeleteTree (RootNode);
    if (!EFI_ERROR (Status) && EFI_ERROR (DeleteStatus)) {
      Status = DeleteStatus;
    }
  }

  return Status;
}

/**
  Locate the first instance of a protocol.  If the protocol requested is an
  FV protocol, then it will return the first FV that contains the device-specific ACPI table
  storage file.

  @param  Instance      Return pointer to the first instance of the protocol

  @return EFI_SUCCESS           The function completed successfully.
  @return EFI_NOT_FOUND         The protocol could not be located.
  @return EFI_OUT_OF_RESOURCES  There are not enough resources to find the protocol.

**/
EFI_STATUS
LocateFvInstanceWithDeviceTables (
  OUT EFI_FIRMWARE_VOLUME2_PROTOCOL  **DeviceInstance
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *HandleBuffer;
  UINTN                          NumberOfHandles;
  EFI_FV_FILETYPE                FileType;
  UINT32                         FvStatus;
  EFI_FV_FILE_ATTRIBUTES         Attributes;
  UINTN                          Size;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FvInstance;

  FvStatus = 0;

  //
  // Locate protocol.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &NumberOfHandles,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    //
    // Defined errors at this time are not found and out of resources.
    //
    return Status;
  }

  //
  // Looking for FV with device-specific ACPI table storage file
  //

  for (Index = 0; Index < NumberOfHandles; Index++) {
    //
    // Get the protocol on this handle
    // This should not fail because of LocateHandleBuffer
    //
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&FvInstance
                    );
    ASSERT_EFI_ERROR (Status);

    //
    // See if it has the ACPI storage file
    //
    Status = FvInstance->ReadFile (
                           FvInstance,
                           (EFI_GUID *)PcdGetPtr (PcdDeviceAcpiTableStorageFile),
                           NULL,
                           &Size,
                           &FileType,
                           &Attributes,
                           &FvStatus
                           );

    //
    // If we found it, then we are done
    //
    if (Status == EFI_SUCCESS) {
      *DeviceInstance = FvInstance;
      break;
    }
  }

  //
  // Our exit status is determined by the success of the previous operations
  // If the protocol was found, Instance already points to it.
  //

  //
  // Free any allocated buffers
  //
  gBS->FreePool (HandleBuffer);

  return Status;
}


/**
  Locate the first instance of a protocol.  If the protocol requested is an
  FV protocol, then it will return the first FV that contains the device family-specific ACPI table
  storage file.

  @param  Instance      Return pointer to the first instance of the protocol

  @return EFI_SUCCESS           The function completed successfully.
  @return EFI_NOT_FOUND         The protocol could not be located.
  @return EFI_OUT_OF_RESOURCES  There are not enough resources to find the protocol.

**/
EFI_STATUS
LocateFvInstanceWithDeviceFamilyTables (
  OUT EFI_FIRMWARE_VOLUME2_PROTOCOL  **DeviceInstance
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *HandleBuffer;
  UINTN                          NumberOfHandles;
  EFI_FV_FILETYPE                FileType;
  UINT32                         FvStatus;
  EFI_FV_FILE_ATTRIBUTES         Attributes;
  UINTN                          Size;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FvInstance;

  FvStatus = 0;

  //
  // Locate protocol.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &NumberOfHandles,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    //
    // Defined errors at this time are not found and out of resources.
    //
    return Status;
  }

  //
  // Looking for FV with device-specific ACPI table storage file
  //

  for (Index = 0; Index < NumberOfHandles; Index++) {
    //
    // Get the protocol on this handle
    // This should not fail because of LocateHandleBuffer
    //
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&FvInstance
                    );
    ASSERT_EFI_ERROR (Status);

    //
    // See if it has the ACPI storage file
    //
    Status = FvInstance->ReadFile (
                           FvInstance,
                           (EFI_GUID *)PcdGetPtr (PcdDeviceFamilyAcpiTableStorageFile),
                           NULL,
                           &Size,
                           &FileType,
                           &Attributes,
                           &FvStatus
                           );

    //
    // If we found it, then we are done
    //
    if (Status == EFI_SUCCESS) {
      *DeviceInstance = FvInstance;
      break;
    }
  }

  //
  // Our exit status is determined by the success of the previous operations
  // If the protocol was found, Instance already points to it.
  //

  //
  // Free any allocated buffers
  //
  gBS->FreePool (HandleBuffer);

  return Status;
}



/**
  Locate the first instance of a protocol.  If the protocol requested is an
  FV protocol, then it will return the first FV that contains the SoC-specific ACPI table
  storage file.

  @param  Instance      Return pointer to the first instance of the protocol

  @return EFI_SUCCESS           The function completed successfully.
  @return EFI_NOT_FOUND         The protocol could not be located.
  @return EFI_OUT_OF_RESOURCES  There are not enough resources to find the protocol.

**/
EFI_STATUS
LocateFvInstanceWithSocTables (
  OUT EFI_FIRMWARE_VOLUME2_PROTOCOL  **SocInstance
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *HandleBuffer;
  UINTN                          NumberOfHandles;
  EFI_FV_FILETYPE                FileType;
  UINT32                         FvStatus;
  EFI_FV_FILE_ATTRIBUTES         Attributes;
  UINTN                          Size;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FvInstance;

  FvStatus = 0;

  //
  // Locate protocol.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &NumberOfHandles,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    //
    // Defined errors at this time are not found and out of resources.
    //
    return Status;
  }

  //
  // Looking for FV with device-specific ACPI table storage file
  //

  for (Index = 0; Index < NumberOfHandles; Index++) {
    //
    // Get the protocol on this handle
    // This should not fail because of LocateHandleBuffer
    //
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&FvInstance
                    );
    ASSERT_EFI_ERROR (Status);

    //
    // See if it has the ACPI storage file
    //
    Status = FvInstance->ReadFile (
                           FvInstance,
                           (EFI_GUID *)PcdGetPtr (PcdSocAcpiTableStorageFile),
                           NULL,
                           &Size,
                           &FileType,
                           &Attributes,
                           &FvStatus
                           );

    //
    // If we found it, then we are done
    //
    if (Status == EFI_SUCCESS) {
      *SocInstance = FvInstance;
      break;
    }
  }

  //
  // Our exit status is determined by the success of the previous operations
  // If the protocol was found, Instance already points to it.
  //

  //
  // Free any allocated buffers
  //
  gBS->FreePool (HandleBuffer);

  return Status;
}

/**
  Locate the first instance of a protocol.  If the protocol requested is an
  FV protocol, then it will return the first FV that contains the SoC-specific ACPI table
  storage file.

  @param  Instance      Return pointer to the first instance of the protocol

  @return EFI_SUCCESS           The function completed successfully.
  @return EFI_NOT_FOUND         The protocol could not be located.
  @return EFI_OUT_OF_RESOURCES  There are not enough resources to find the protocol.

**/
EFI_STATUS
LocateFvInstanceWithGenericTables (
  OUT EFI_FIRMWARE_VOLUME2_PROTOCOL  **GenericInstance
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *HandleBuffer;
  UINTN                          NumberOfHandles;
  EFI_FV_FILETYPE                FileType;
  UINT32                         FvStatus;
  EFI_FV_FILE_ATTRIBUTES         Attributes;
  UINTN                          Size;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FvInstance;

  FvStatus = 0;

  //
  // Locate protocol.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &NumberOfHandles,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    //
    // Defined errors at this time are not found and out of resources.
    //
    return Status;
  }

  //
  // Looking for FV with device-specific ACPI table storage file
  //

  for (Index = 0; Index < NumberOfHandles; Index++) {
    //
    // Get the protocol on this handle
    // This should not fail because of LocateHandleBuffer
    //
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&FvInstance
                    );
    ASSERT_EFI_ERROR (Status);

    //
    // See if it has the ACPI storage file
    //
    Status = FvInstance->ReadFile (
                           FvInstance,
                           (EFI_GUID *)PcdGetPtr (PcdGenericAcpiTableStorageFile),
                           NULL,
                           &Size,
                           &FileType,
                           &Attributes,
                           &FvStatus
                           );

    //
    // If we found it, then we are done
    //
    if (Status == EFI_SUCCESS) {
      *GenericInstance = FvInstance;
      break;
    }
  }

  //
  // Our exit status is determined by the success of the previous operations
  // If the protocol was found, Instance already points to it.
  //

  //
  // Free any allocated buffers
  //
  gBS->FreePool (HandleBuffer);

  return Status;
}

/**
  This function calculates and updates an UINT8 checksum.

  @param  Buffer          Pointer to buffer to checksum
  @param  Size            Number of bytes to checksum

**/
VOID
AppleAcpiPlatformChecksum (
  IN UINT8  *Buffer,
  IN UINTN  Size
  )
{
  UINTN  ChecksumOffset;

  ChecksumOffset = OFFSET_OF (EFI_ACPI_DESCRIPTION_HEADER, Checksum);

  //
  // Set checksum to 0 first
  //
  Buffer[ChecksumOffset] = 0;

  //
  // Update checksum value
  //
  Buffer[ChecksumOffset] = CalculateCheckSum8 (Buffer, Size);
}

// EFI_STATUS AcpiPlatformInstallMadtTable(VOID) {
//   //
//   // We need to install an MADT - we can use the same table regardless of
//   // whether we're using a vGIC or not.
//   //
  
//   return EFI_UNSUPPORTED;
// }

/**
  Entrypoint of Acpi Platform driver.

  @param  ImageHandle
  @param  SystemTable

  @return EFI_SUCCESS
  @return EFI_LOAD_ERROR
  @return EFI_OUT_OF_RESOURCES

**/
EFI_STATUS
EFIAPI
AcpiPlatformEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                     Status;
  EFI_ACPI_TABLE_PROTOCOL        *AcpiTable;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FwVol;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FwVol2;
  INTN                           Instance;
  EFI_ACPI_COMMON_HEADER         *CurrentTable;
  UINTN                          TableHandle;
  UINT32                         FvStatus;
  UINTN                          TableSize;
  UINTN                          Size;

  Instance     = 0;
  CurrentTable = NULL;
  TableHandle  = 0;

  DEBUG((DEBUG_ERROR, "%a: AcpiPlatform driver started\n", __FUNCTION__));

  //
  // Find the AcpiTable protocol
  //
  DEBUG((DEBUG_ERROR, "%a: Locating ACPI table protocol\n", __FUNCTION__));
  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable);
  if (EFI_ERROR (Status)) {
    DEBUG((DEBUG_ERROR, "%a: Failed to locate ACPI protocol, status %r (error code %llx)\n", __FUNCTION__, Status, Status));
    ASSERT(FALSE);
    return EFI_ABORTED;
  }

  //
  // Locate the firmware volume protocol
  //
  DEBUG((DEBUG_ERROR, "%a: Locating device specific ACPI tables\n", __FUNCTION__));
  Status = LocateFvInstanceWithDeviceTables (&FwVol);
  if (EFI_ERROR (Status)) {
    DEBUG((DEBUG_ERROR, "%a: Failed to locate device specific tables, status %r (error code %llx)\n", __FUNCTION__, Status, Status));
    ASSERT(FALSE);
    return EFI_ABORTED;
  }

  //
  // Read tables from the storage file.
  //
  while (Status == EFI_SUCCESS) {
    Status = FwVol->ReadSection (
                      FwVol,
                      (EFI_GUID *)PcdGetPtr (PcdDeviceAcpiTableStorageFile),
                      EFI_SECTION_RAW,
                      Instance,
                      (VOID **)&CurrentTable,
                      &Size,
                      &FvStatus
                      );
    if (!EFI_ERROR (Status)) {
      //
      // Add the table
      //
      TableHandle = 0;

      TableSize = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Length;
      ASSERT (Size >= TableSize);

      //
      // Checksum ACPI table
      //
      AppleAcpiPlatformChecksum ((UINT8 *)CurrentTable, TableSize);

      //
      // Install ACPI table
      //
      Status = AcpiTable->InstallAcpiTable (
                            AcpiTable,
                            CurrentTable,
                            TableSize,
                            &TableHandle
                            );

      //
      // Free memory allocated by ReadSection
      //
      gBS->FreePool (CurrentTable);

      if (EFI_ERROR (Status)) {
        return EFI_ABORTED;
      }

      //
      // Increment the instance
      //
      Instance++;
      CurrentTable = NULL;
    }
  }

  Instance     = 0;
  CurrentTable = NULL;
  TableHandle  = 0;
  //
  // Locate the firmware volume protocol
  //
  DEBUG((DEBUG_ERROR, "%a: Locating SoC specific ACPI tables\n", __FUNCTION__));
  Status = LocateFvInstanceWithSocTables (&FwVol2);
  if (EFI_ERROR (Status)) {
    DEBUG((DEBUG_ERROR, "%a: Failed to locate SoC specific tables, status %r (error code %llx)\n", __FUNCTION__, Status, Status));
    ASSERT(FALSE);
    return EFI_ABORTED;
  }

  //
  // Read tables from the storage file.
  //
  while (Status == EFI_SUCCESS) {
    Status = FwVol2->ReadSection (
                      FwVol2,
                      (EFI_GUID *)PcdGetPtr (PcdSocAcpiTableStorageFile),
                      EFI_SECTION_RAW,
                      Instance,
                      (VOID **)&CurrentTable,
                      &Size,
                      &FvStatus
                      );
    if (!EFI_ERROR (Status)) {
      //
      // Add the table
      //
      TableHandle = 0;

      TableSize = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Length;
      ASSERT (Size >= TableSize);

      //
      // Checksum ACPI table
      //
      AppleAcpiPlatformChecksum ((UINT8 *)CurrentTable, TableSize);

      //
      // Install ACPI table
      //
      Status = AcpiTable->InstallAcpiTable (
                            AcpiTable,
                            CurrentTable,
                            TableSize,
                            &TableHandle
                            );

      //
      // Free memory allocated by ReadSection
      //
      gBS->FreePool (CurrentTable);

      if (EFI_ERROR (Status)) {
        return EFI_ABORTED;
      }

      //
      // Increment the instance
      //
      Instance++;
      CurrentTable = NULL;
    }
  }


  //
  // Locate the firmware volume protocol
  //
  DEBUG((DEBUG_ERROR, "%a: Locating generic ACPI tables\n", __FUNCTION__));
  Status = LocateFvInstanceWithGenericTables (&FwVol2);
  if (EFI_ERROR (Status)) {
    DEBUG((DEBUG_ERROR, "%a: Failed to locate generic tables, status %r (error code %llx)\n", __FUNCTION__, Status, Status));
    ASSERT(FALSE);
    return EFI_ABORTED;
  }

  //
  // Read tables from the storage file.
  //
  while (Status == EFI_SUCCESS) {
    Status = FwVol2->ReadSection (
                      FwVol2,
                      (EFI_GUID *)PcdGetPtr (PcdGenericAcpiTableStorageFile),
                      EFI_SECTION_RAW,
                      Instance,
                      (VOID **)&CurrentTable,
                      &Size,
                      &FvStatus
                      );
    if (!EFI_ERROR (Status)) {
      //
      // Add the table
      //
      TableHandle = 0;

      TableSize = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Length;
      ASSERT (Size >= TableSize);

      //
      // Checksum ACPI table
      //
      AppleAcpiPlatformChecksum ((UINT8 *)CurrentTable, TableSize);

      //
      // Install ACPI table
      //
      Status = AcpiTable->InstallAcpiTable (
                            AcpiTable,
                            CurrentTable,
                            TableSize,
                            &TableHandle
                            );

      //
      // Free memory allocated by ReadSection
      //
      gBS->FreePool (CurrentTable);

      if (EFI_ERROR (Status)) {
        return EFI_ABORTED;
      }

      //
      // Increment the instance
      //
      Instance++;
      CurrentTable = NULL;
    }
  }

  //
  // Locate the firmware volume protocol
  //
  DEBUG((DEBUG_ERROR, "%a: Locating device family ACPI tables\n", __FUNCTION__));
  Status = LocateFvInstanceWithDeviceFamilyTables (&FwVol2);
  if (EFI_ERROR (Status)) {
    DEBUG((DEBUG_ERROR, "%a: Failed to locate device family tables, status %r (error code %llx)\n", __FUNCTION__, Status, Status));
    ASSERT(FALSE);
    return EFI_ABORTED;
  }

  //
  // Read tables from the storage file.
  //
  while (Status == EFI_SUCCESS) {
    Status = FwVol2->ReadSection (
                      FwVol2,
                      (EFI_GUID *)PcdGetPtr (PcdDeviceFamilyAcpiTableStorageFile),
                      EFI_SECTION_RAW,
                      Instance,
                      (VOID **)&CurrentTable,
                      &Size,
                      &FvStatus
                      );
    if (!EFI_ERROR (Status)) {
      //
      // Add the table
      //
      TableHandle = 0;

      TableSize = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Length;
      ASSERT (Size >= TableSize);

      //
      // Checksum ACPI table
      //
      AppleAcpiPlatformChecksum ((UINT8 *)CurrentTable, TableSize);

      //
      // Install ACPI table
      //
      Status = AcpiTable->InstallAcpiTable (
                            AcpiTable,
                            CurrentTable,
                            TableSize,
                            &TableHandle
                            );

      //
      // Free memory allocated by ReadSection
      //
      gBS->FreePool (CurrentTable);

      if (EFI_ERROR (Status)) {
        return EFI_ABORTED;
      }

      //
      // Increment the instance
      //
      Instance++;
      CurrentTable = NULL;
    }
  }

  //
  // Dynamically generate and install the MADT table.
  // We have to do this because we will only know the number of cores
  // (which is needed to allocate for redistributors correctly) at runtime.
  //

  // Temporarily disabled - using a static MADT for now

  // Status = AcpiPlatformInstallMadtTable();

  // Publish ANS after the static namespace has been installed.  Failure is
  // fatal when the ADT contains ANS: silently omitting the boot controller
  // would make the Windows storage driver impossible to bind.
  Status = AcpiPlatformInstallAppleAnsTable (AcpiTable);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    DEBUG ((DEBUG_ERROR, "AppleANS ACPI: SSDT installation failed: %r\n", Status));
    return EFI_ABORTED;
  }

#if NTASI_J414S_GPU_RESOURCE_PROFILE
  //
  // Moved from PEI's MemoryInitPeiLib.c on 2026-07-30 (see
  // NtasiGpuReservationGuard.h for why): a bug here must never be able to
  // take down the whole boot the way it could when this ran with no
  // console and no exception vector table. NtasiResolveAndReserveGpuCarveouts()
  // logs a breadcrumb before every step and never returns a fatal status.
  //
  {
    NTASI_GPU_HANDOFF  GpuHandoff;

    NtasiResolveAndReserveGpuCarveouts (&GpuHandoff);
 #if NTASI_ENABLE_GPU_ACPI_PUBLICATION
    //
    // Publishing NTAS0023 is non-fatal by construction, like DRT0 and the
    // media tables: a firmware bug here must never take down a boot that would
    // otherwise reach Windows. AppleAgxGpu is a demand-start, non-WDDM,
    // ErrorControl=NORMAL service, so a devnode that fails to start costs the
    // GPU and nothing else -- it is never loaded by winload and cannot
    // participate in boot-device selection.
    //
    NtasiPublishGpu (AcpiTable, &GpuHandoff);
 #endif
  }
#endif

#if NTASI_ENABLE_WIRELESS_DART_HANDOFF
  //
  // Publish DRT0 so AppleDart/AppleBcmWifi can adopt the SID-1 page-table
  // reservation m1n1 built and PEI authenticated. EFI_NOT_FOUND (no
  // reservation published this boot -- PEI withheld it) is expected and not
  // fatal: wireless then simply behaves as if the build had this feature
  // off. Any other failure is logged but still non-fatal -- consistent
  // with "never let a firmware bug here take down a boot that would
  // otherwise reach Windows" for everything after the mandatory ANS table.
  //
  Status = NtasiInstallWirelessDartTable (AcpiTable);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    DEBUG ((DEBUG_ERROR, "WirelessDART ACPI: SSDT installation failed: %r\n", Status));
  }

#endif

#if NTASI_ENABLE_MEDIA_PUBLICATION
  //
  // Publish MCA0/AOPA/ISP0 for the media profile.  Non-fatal by construction:
  // NtasiInstallMediaTables() logs and continues, consistent with "never let a
  // firmware bug here take down a boot that would otherwise reach Windows" for
  // everything after the mandatory ANS table.  The matching CSRT ALI2 entries
  // for MCA0's published GSIVs come from CSRT.aslc, gated on the same flag.
  //
  NtasiInstallMediaTables (AcpiTable);
#endif

  //
  // Dump every non-conventional memory region, in EVERY profile.
  //
  // Added 2026-07-30 for the BUGCODE_USB3_DRIVER 0x144 investigation. XHC1 was
  // captured halted on USBSTS.HSE (Host System Error -- the host bus rejected
  // its DMA) with DWC3 buserr_valid=1, and the failure correlated with
  // firmware profiles. Firmware's most plausible route to another master's DMA
  // fault is the memory map it hands the OS, so every profile now states
  // exactly what it asked the OS to treat specially. AcpiPlatformDxe is the
  // right home because it is in EVERY profile -- including baseline (which
  // boots) and wireless (which does not, and carries no ANS driver at all) --
  // so the maps can be diffed directly against each other.
  //
  // Purely observational. The ans profiles dump again from
  // AppleNANDStorageDxe after its own reserved buffers exist.
  //
  NtasiDumpReservedMemoryMap ("AcpiPlatform");

  //
  // The driver does not require to be kept loaded.
  //
  return EFI_REQUEST_UNLOAD_IMAGE;
}

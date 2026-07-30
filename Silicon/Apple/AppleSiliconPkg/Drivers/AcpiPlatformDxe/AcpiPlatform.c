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
#include <Library/AppleDTLib.h>
#include <Library/AmlLib/AmlLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>

#include <IndustryStandard/Acpi.h>
#include <Drivers/AppleAnsHardware.h>
//
// Moved out of this directory on 2026-07-30 so AppleNANDStorageDxe can share
// the one copy instead of a second module growing its own PMGR resolution.
// AppleAnsPmgrDomain.h is the EDK2-side wrapper; it includes the
// dependency-free arithmetic header the host test compiles verbatim.
//
#include <Drivers/AppleAnsPmgrDomain.h>

#define APPLE_ANS_ACPI_OEM_ID        "NTASP "
#define APPLE_ANS_ACPI_OEM_TABLE_ID  "APPLEANS"
#define APPLE_ANS_PMGR_RESET_SIZE     sizeof (UINT32)

STATIC CONST CHAR8  mAppleAnsAcpiTag[] = "AppleANS ACPI";

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
VOID
NtasiReserveGpuAdtCarveout (
  IN dt_node_t             *SgxNode,
  IN CONST CHAR8           *AdtPropertyPrefix,
  IN CONST CHAR8           *Label,
  IN EFI_PHYSICAL_ADDRESS  SystemMemoryBase,
  IN EFI_PHYSICAL_ADDRESS  SystemMemoryTop,
  IN UINT64                DramWindowBase,
  IN UINT64                DramWindowTop,
  IN UINT64                CurrentStackPointer
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
    return;
  }

  AsciiSPrint (PropName, sizeof (PropName), "%a-size", AdtPropertyPrefix);
  if (!NtasiGpuDtNodeU64 (SgxNode, PropName, &Size)) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: %a: missing ADT property \"%a\" on /arm-io/sgx; GPU degraded\n", Label, PropName));
    return;
  }

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: %a: ADT reports 0x%lx/+0x%lx\n", Label, Base, Size));

  if ((Size == 0) || (Base > MAX_UINT64 - (Size - 1))) {
    DEBUG ((DEBUG_ERROR, "AppleAgxGpu: %a: implausible ADT region 0x%lx/+0x%lx; GPU degraded\n", Label, Base, Size));
    return;
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
    return;
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
    return;
  }

  DEBUG ((DEBUG_INFO, "AppleAgxGpu: %a: reserved 0x%lx/+0x%lx (out-of-window carveout, GCD)\n", Label, Base, Size));
}

//
// hw_data_a / hw_data_b / globals have no known live source (no ADT
// property, and m1n1's dt_set_gpu() -- the only code that computes them --
// is never called on this project's chainload/HV boot path). See the
// 2026-07-30 incident history in NtasiGpuReservationGuard.h: a prior
// attempt to derive them from Mu's own memory window crashed the machine
// twice. Until m1n1 publishes them somewhere Mu can read them, or a
// fresh hardware-captured constant is authenticated the way
// NtasiValidateWirelessHandoffV2() authenticates the wireless DART
// handoff (see MemoryInitPeiLib.c), Mu does not reserve them at all.
// AppleAgxGpu will not find valid pre-computed firmware init data there
// and will not initialize -- degraded GPU, never a boot risk.
//
STATIC
VOID
NtasiLogGpuHandoffDataGap (
  VOID
  )
{
  DEBUG ((
    DEBUG_ERROR,
    "AppleAgxGpu: hw_data_a/hw_data_b/globals have no known live source; not reserved. "
    "GPU firmware init data will not be pre-populated; GPU degraded, boot continues. "
    "See NtasiLogGpuHandoffDataGap() history in AcpiPlatform.c.\n"
    ));
}

/**
  Resolve and reserve the GPU's out-of-window ADT carveouts. Called once
  from AcpiPlatformEntryPoint, late in DXE dispatch.
**/
STATIC
VOID
NtasiResolveAndReserveGpuCarveouts (
  VOID
  )
{
  dt_node_t  *SgxNode;
  UINT64     CurrentSp;
  UINT64     SystemMemoryBase;
  UINT64     SystemMemoryTop;
  UINT64     DramWindowBase;
  UINT64     DramWindowTop;

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
    NtasiLogGpuHandoffDataGap ();
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
    NtasiReserveGpuAdtCarveout (SgxNode, "gpu-region", "uat_ttbs", SystemMemoryBase, SystemMemoryTop, DramWindowBase, DramWindowTop, CurrentSp);
    NtasiReserveGpuAdtCarveout (SgxNode, "gfx-shared-region", "uat_pagetables", SystemMemoryBase, SystemMemoryTop, DramWindowBase, DramWindowTop, CurrentSp);
    NtasiReserveGpuAdtCarveout (SgxNode, "gfx-handoff", "uat_handoff", SystemMemoryBase, SystemMemoryTop, DramWindowBase, DramWindowTop, CurrentSp);
  }

  NtasiLogGpuHandoffDataGap ();
  DEBUG ((DEBUG_INFO, "AppleAgxGpu: bring-up finished\n"));
}
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
  AML_OBJECT_NODE_HANDLE       CrsNode;
  EFI_ACPI_DESCRIPTION_HEADER  *Table;
  UINTN                        TableHandle;
  UINT64                       ReservationBase;
  UINT32                       ReservationSize;

  RootNode = NULL;
  Table    = NULL;

  DEBUG ((DEBUG_INFO, "WirelessDART ACPI: stage \"read-reservation-pcds\"\n"));
  ReservationBase = PcdGet64 (PcdAppleWirelessDartPageTableBase);
  ReservationSize = PcdGet32 (PcdAppleWirelessDartPageTableSize);
  if ((ReservationBase == 0) || (ReservationSize == 0)) {
    //
    // KNOWN GAP, identified 2026-07-30 and NOT yet fixed -- do not read this
    // "withheld" line as proof that PEI's derivation failed.
    //
    // PcdAppleWirelessDartPageTableBase/Size are declared in
    // [PcdsPatchableInModule] (AppleSiliconPkg.dec). A PatchableInModule PCD
    // is a PER-MODULE copy: MemoryInitPeiLib's PatchPcdSet64/32 writes the
    // copy linked into PrePi, and this driver's PcdGet64/32 reads its own
    // never-patched copy, which is always the DEC default of zero. So this
    // path is taken on EVERY boot regardless of what PEI derived, and DRT0 is
    // never published.
    //
    // The same trap was confirmed on hardware for PcdSystemMemoryBase/Size,
    // which read the DSC defaults here (see NtasiDeriveBootArgsWindows()
    // above, which is why the GPU carveout guard no longer uses them). The
    // correct fix is a GUID HOB from PEI -- MemoryInitPeiLib.c already
    // publishes one for the appended ramdisk
    // (NTASI_APPENDED_RAMDISK_LOCATION_HOB_GUID), so the plumbing exists --
    // or an independent boot_args-derived recomputation here. Deliberately
    // left alone tonight rather than changed untested alongside the ANS and
    // GPU fixes: this failure mode is fail-safe (no DRT0, wireless simply
    // does not adopt the handoff), so it costs a feature, not a boot.
    //
    DEBUG ((
      DEBUG_ERROR,
      "WirelessDART ACPI: reservation PCDs read 0x%lx/+0x%x in this module; DRT0 withheld. "
      "NOTE: these are PatchableInModule, so PEI's PatchPcdSet never reaches this driver -- "
      "this is a known firmware gap, not evidence that PEI's derivation failed. "
      "See the comment at this DEBUG in AcpiPlatform.c.\n",
      ReservationBase,
      ReservationSize
      ));
    return EFI_NOT_FOUND;
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

  Status = AmlCodeGenNameResourceTemplate ("_CRS", DeviceNode, &CrsNode);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  // 0: DART aperture (fixed hardware MMIO, not part of the derived
  // reservation).
  Status = AppleAnsAddMemoryResource (CrsNode, NTASI_WIRELESS_DART_APERTURE_BASE, NTASI_WIRELESS_DART_APERTURE_SIZE);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  // 1: the derived SID-1 page-table/descriptor reservation.
  Status = AppleAnsAddMemoryResource (CrsNode, ReservationBase, ReservationSize);
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
      "WirelessDART ACPI: DRT0 published, dart-aperture=0x%lx/+0x%lx reservation=0x%lx/+0x%x\n",
      NTASI_WIRELESS_DART_APERTURE_BASE,
      NTASI_WIRELESS_DART_APERTURE_SIZE,
      ReservationBase,
      ReservationSize
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

/**
  Publish the native Apple ANS controller to Windows.  Addresses and the
  hardware profile are derived from the live Apple Device Tree so one firmware
  binary does not bake in a board-specific MMIO map.

  The first three memory resources have a stable ABI with the Windows miniport:
    0: ASC CPU/mailbox aperture (mailbox registers are at +0x8000)
    1: ANS NVMe aperture (ADT reg[3])
    2: SART aperture
  NTAS2003 appends four deliberately narrow resources in driver ABI order:
    3: exact 4-byte ps_ans2 PMGR power/reset word
    4: exact 4-byte ps_apcie_st parent power word
    5: exact 4-byte ps_apcie_st_sys power word
    6: exact 4-byte ps_apcie_st1_sys power word
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

  if (SartVersion == 3) {
    Status = AppleAnsAddMemoryResource (
               CrsNode,
               PmgrResetBase,
               APPLE_ANS_PMGR_RESET_SIZE
               );
    if (EFI_ERROR (Status)) {
      goto Exit;
    }
    Status = AppleAnsAddMemoryResource (
               CrsNode,
               PmgrApcieStBase,
               APPLE_ANS_PMGR_RESET_SIZE
               );
    if (EFI_ERROR (Status)) {
      goto Exit;
    }
    Status = AppleAnsAddMemoryResource (
               CrsNode,
               PmgrApcieStSysBase,
               APPLE_ANS_PMGR_RESET_SIZE
               );
    if (EFI_ERROR (Status)) {
      goto Exit;
    }
    Status = AppleAnsAddMemoryResource (
               CrsNode,
               PmgrApcieSt1SysBase,
               APPLE_ANS_PMGR_RESET_SIZE
               );
    if (EFI_ERROR (Status)) {
      goto Exit;
    }
  }

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
      "AppleANS ACPI: %a cpu=%lx/%lx nvme=%lx/%lx sart=%lx/%lx pmgr-ans2=%lx apcie-st=%lx st-sys=%lx st1-sys=%lx size=%x irq=%u physical=%u contract=%a\n",
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
  NtasiResolveAndReserveGpuCarveouts ();
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

  //
  // The driver does not require to be kept loaded.
  //
  return EFI_REQUEST_UNLOAD_IMAGE;
}

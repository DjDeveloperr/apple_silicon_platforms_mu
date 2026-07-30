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
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>

#include <IndustryStandard/Acpi.h>
#include <Drivers/AppleAnsHardware.h>
#include "NtasiAnsPmgrResolve.h"

#define APPLE_ANS_ACPI_OEM_ID        "NTASP "
#define APPLE_ANS_ACPI_OEM_TABLE_ID  "APPLEANS"
#define APPLE_ANS_PMGR_RESET_SIZE     sizeof (UINT32)

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

#define NTASI_PMGR_MAX_REG_TUPLES  16u

/**
  Resolve one "/arm-io/pmgr" PMGR power-state register address live from
  the ADT, by exact device name -- mirrors m1n1's pmgr_find_device() +
  pmgr_device_get_addr() (src/pmgr.c). See NtasiAnsPmgrResolve.h for the
  portable arithmetic this wraps (shared verbatim with the host test
  Tests/test_ans_pmgr_resolve.c) and for why this exists.

  NEVER falls back to a hardcoded address. On 2026-07-30 the DSC's
  hardcoded PcdAppleAnsPmgr*Base values pointed at DCS_09/DCS_10 -- DRAM
  controller power domains -- instead of ANS2/APCIE_ST/APCIE_ST_SYS/
  APCIE_ST1_SYS, because they were computed against the wrong "/arm-io/
  pmgr" register block ("pmgr" instead of "pmgr_east") and happened to
  still pass every alignment/distinctness sanity check below. Resolving
  strictly by name against the live ADT device table makes that class of
  address confusion impossible by construction: this function can only
  ever return an address it found attached to the exact name it was asked
  to look for, read fresh from this boot's ADT, never a computed guess.
**/
STATIC
EFI_STATUS
NtasiResolveAnsPmgrDomain (
  IN  CONST CHAR8  *DomainName,
  OUT UINT64       *Address
  )
{
  dt_node_t       *PmgrNode;
  CONST UINT8     *Devices;
  UINTN            DevicesLength;
  CONST UINT32    *PsRegs;
  UINTN            PsRegsLength;
  UINT64           RegTupleBases[NTASI_PMGR_MAX_REG_TUPLES];
  UINT32           RegTupleCount;
  UINT32           TupleIndex;

  PmgrNode = dt_get ("/arm-io/pmgr");
  if (PmgrNode == NULL) {
    DEBUG ((DEBUG_ERROR, "AppleANS ACPI: \"/arm-io/pmgr\" ADT node not found; cannot resolve %a\n", DomainName));
    return EFI_NOT_FOUND;
  }

  Devices = dt_node_prop (PmgrNode, "devices", &DevicesLength);
  if ((Devices == NULL) || (DevicesLength < NTASI_PMGR_DEVICE_SIZE)) {
    DEBUG ((DEBUG_ERROR, "AppleANS ACPI: \"/arm-io/pmgr\" has no usable \"devices\" property\n"));
    return EFI_NOT_FOUND;
  }

  PsRegs = (CONST UINT32 *)dt_node_prop (PmgrNode, "ps-regs", &PsRegsLength);
  if ((PsRegs == NULL) || (PsRegsLength < (NTASI_PMGR_PSREG_STRIDE * sizeof (UINT32)))) {
    DEBUG ((DEBUG_ERROR, "AppleANS ACPI: \"/arm-io/pmgr\" has no usable \"ps-regs\" property\n"));
    return EFI_NOT_FOUND;
  }

  //
  // Resolve every "reg" tuple on the pmgr node itself up front -- small
  // and bounded, and every psreg_idx we might see indexes into this
  // array. This is the multi-block piece the old hardcoded constants
  // skipped by pointing at one block's base address directly.
  //
  RegTupleCount = 0;
  for (TupleIndex = 0; TupleIndex < NTASI_PMGR_MAX_REG_TUPLES; TupleIndex++) {
    UINT64  TupleBase;
    UINT64  TupleSize;

    if (dt_node_reg (PmgrNode, TupleIndex, &TupleBase, &TupleSize) != 0) {
      break;
    }

    RegTupleBases[TupleIndex] = TupleBase;
    RegTupleCount++;
  }

  if (RegTupleCount == 0) {
    DEBUG ((DEBUG_ERROR, "AppleANS ACPI: \"/arm-io/pmgr\" has no readable \"reg\" tuples\n"));
    return EFI_NOT_FOUND;
  }

  if (NtasiPmgrFindDomainAddress (
        Devices,
        (UINT32)(DevicesLength / NTASI_PMGR_DEVICE_SIZE),
        RegTupleBases,
        RegTupleCount,
        PsRegs,
        (UINT32)(PsRegsLength / sizeof (UINT32)),
        DomainName,
        Address
        ) != NTASI_PMGR_TRUE)
  {
    DEBUG ((
      DEBUG_ERROR,
      "AppleANS ACPI: could not uniquely resolve PMGR domain \"%a\" from the live ADT\n",
      DomainName
      ));
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

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
    // NtasiResolveAnsPmgrDomain() above for why: a hardcoded constant is
    // exactly what pointed these four words at DCS_09/DCS_10 (DRAM
    // controller power domains) on 2026-07-30. Any single domain failing
    // to resolve uniquely withholds NTAS2003 entirely (EFI_NOT_FOUND,
    // handled by the caller as "no ANS device today") rather than
    // publishing three good addresses and one wrong or missing one.
    //
    if (EFI_ERROR (NtasiResolveAnsPmgrDomain ("ANS2", &PmgrResetBase)) ||
        EFI_ERROR (NtasiResolveAnsPmgrDomain ("APCIE_ST", &PmgrApcieStBase)) ||
        EFI_ERROR (NtasiResolveAnsPmgrDomain ("APCIE_ST_SYS", &PmgrApcieStSysBase)) ||
        EFI_ERROR (NtasiResolveAnsPmgrDomain ("APCIE_ST1_SYS", &PmgrApcieSt1SysBase)))
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


  //
  // The driver does not require to be kept loaded.
  //
  return EFI_REQUEST_UNLOAD_IMAGE;
}

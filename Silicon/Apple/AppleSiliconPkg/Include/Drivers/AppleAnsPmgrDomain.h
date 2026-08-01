/** @file
  Live-ADT resolution and read-only inspection of the Apple PMGR
  power-state words the ANS/NVMe coprocessor depends on.

  ONE COPY, TWO CONSUMERS. AcpiPlatformDxe publishes these four words to
  Windows as NTAS2003 _DSD integer properties -- NOT as _CRS resources, and
  never again: every one of them sits inside the 4 KiB page NTAS0051/KBL0
  claims exclusively, so claiming them here is an arbiter conflict that costs
  one of the two devices a code 12. AppleNANDStorageDxe reads them before it
  touches any ANS MMIO. Both used to be impossible to share because the
  resolution wrapper lived inside AcpiPlatform.c as a STATIC function. It is
  a static inline here instead, so there is still exactly one copy of the
  logic and no second chance to get a PMGR address wrong in a second place.

  The portable arithmetic stays in <Drivers/NtasiAnsPmgrResolve.h>, which has
  zero dependencies on purpose (see its own header comment) and is compiled
  verbatim by the host test Tests/test_ans_pmgr_resolve.c. This file adds
  only the EDK2-side ADT walk and the PMGR register field decode, both of
  which need AppleDTLib/DebugLib and therefore cannot live there.

  NO PMGR *ENABLE* ANYWHERE IN THIS FIRMWARE, AND EXACTLY ONE PMGR *RESET*:

  m1n1 -- the authoritative reference for this silicon -- performs NO power
  enable for ANS at all. Its nvme_init() (src/nvme.c) never calls
  pmgr_adt_power_enable()/pmgr_power_enable(); the only PMGR calls in the
  whole file are pmgr_reset(die, "ANS")/pmgr_reset(die, "ANS2") on teardown
  and failure paths. It relies on iBoot leaving ANS2 and its AFNC fabric
  parents enabled, which iBoot always does because it booted from this very
  NVMe. On T602X it could not do otherwise even if it wanted to:
  pmgr_adt_power_enable() reads the target node's "clock-gates" property, and
  /arm-io/ans's "clock-gates" is zero-length on this SoC (its "clock-ids"
  carries the domain id 0x8c instead).

  This was also measured directly on J414s on 2026-07-30: ANS2, APCIE_ST,
  APCIE_ST_SYS and APCIE_ST1_SYS were all already ACTUAL=0xf TARGET=0xf
  before any software touched them, and a "touching ANS MMIO before a PMGR
  sequence stalls the AMBA bus" theory was explicitly falsified (see
  AppleSiliconPkg.dec). So there is nothing to enable.

  AppleAnsPmgrReportDomain() below is therefore read-only and is what runs on
  every ANS boot. AppleAnsPmgrResetDomain() is the single exception: it is the
  teardown reset m1n1 performs and Mu previously omitted, it runs only from
  the opt-in DXE bring-up path, and it is guarded by an
  ExpectedAddress cross-check because a wrong PMGR address is precisely the
  operation that once landed on DCS_09/DCS_10 (DRAM controller power domains).
  See its own comment for the full safety argument.

  SPDX-License-Identifier: MIT
**/

#ifndef APPLE_ANS_PMGR_DOMAIN_H_
#define APPLE_ANS_PMGR_DOMAIN_H_

#include <Library/AppleDTLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#include <Drivers/NtasiAnsPmgrResolve.h>

//
// PMGR power-state register fields, from m1n1's src/pmgr.h.
//
#define APPLE_PMGR_PS_ACTUAL_SHIFT  4u
#define APPLE_PMGR_PS_ACTUAL_MASK   (0xFu << APPLE_PMGR_PS_ACTUAL_SHIFT)  // GENMASK(7,4)
#define APPLE_PMGR_PS_TARGET_SHIFT  0u
#define APPLE_PMGR_PS_TARGET_MASK   (0xFu << APPLE_PMGR_PS_TARGET_SHIFT)  // GENMASK(3,0)
#define APPLE_PMGR_PS_ACTIVE        0xFu
#define APPLE_PMGR_RESET            0x80000000u  // BIT(31), m1n1 src/pmgr.h:9
#define APPLE_PMGR_DEV_DISABLE      0x00000400u  // BIT(10), m1n1 src/pmgr.h:13
#define APPLE_PMGR_PS_CLKGATE       0x4u
#define APPLE_PMGR_PS_PWRGATE       0x0u

//
// Upper bound on the number of "reg" tuples the "/arm-io/pmgr" node itself
// may carry that this code will resolve. Every psreg_idx seen on T602X
// indexes 0..2 (main pmgr, pmgr_west-ish, pmgr_east) plus one high index for
// the NUB/AOP-side block; 16 is comfortable headroom and keeps the array on
// the stack.
//
#define APPLE_ANS_PMGR_MAX_REG_TUPLES  16u

//
// Reset timing. m1n1 holds RESET for a flat 10 us (src/pmgr.c
// pmgr_reset_device) and separately polls PS_ACTUAL with a 10000 us budget
// whenever it changes a power state (pmgr_set_mode, PMGR_POLL_TIMEOUT). This
// firmware does BOTH: the 10 us minimum hold, and then a bounded poll for the
// register to actually converge -- because a blind delay proves nothing about
// what the hardware did, and this reset runs inside the ExitBootServices
// callback where there is no second chance and no driver left to complain.
//
#define APPLE_ANS_PMGR_RESET_HOLD_US     10u
#define APPLE_ANS_PMGR_POLL_TIMEOUT_US   10000u
#define APPLE_ANS_PMGR_POLL_STEP_US      10u

/**
  Resolve one "/arm-io/pmgr" PMGR power-state register address live from the
  ADT, by exact device name -- mirrors m1n1's pmgr_find_device() +
  pmgr_device_get_addr() (src/pmgr.c).

  NEVER falls back to a hardcoded address. On 2026-07-30 the DSC's hardcoded
  PcdAppleAnsPmgr*Base values pointed at DCS_09/DCS_10 -- DRAM controller
  power domains -- instead of ANS2/APCIE_ST/APCIE_ST_SYS/APCIE_ST1_SYS,
  because they were computed against the wrong "/arm-io/pmgr" register block
  ("pmgr" instead of "pmgr_east") and happened to still pass every
  alignment/distinctness sanity check. Resolving strictly by name against the
  live ADT device table makes that class of address confusion impossible by
  construction: this function can only ever return an address it found
  attached to the exact name it was asked to look for, read fresh from this
  boot's ADT, never a computed guess.

  @param[in]  Tag         Short subsystem tag for log lines ("AppleANS ACPI",
                          "AppleANS", ...).
  @param[in]  DomainName  Exact uppercase PMGR device name, e.g. "ANS2".
  @param[out] Address     Resolved power-state register address.

  @retval EFI_SUCCESS    *Address holds the resolved register address.
  @retval EFI_NOT_FOUND  The ADT lacks the node/properties, or the name did
                         not resolve to exactly one device.
**/
STATIC
inline
EFI_STATUS
AppleAnsPmgrResolveDomain (
  IN  CONST CHAR8  *Tag,
  IN  CONST CHAR8  *DomainName,
  OUT UINT64       *Address
  )
{
  dt_node_t     *PmgrNode;
  CONST UINT8   *Devices;
  UINTN         DevicesLength;
  CONST UINT32  *PsRegs;
  UINTN         PsRegsLength;
  UINT64        RegTupleBases[APPLE_ANS_PMGR_MAX_REG_TUPLES];
  UINT32        RegTupleCount;
  UINT32        TupleIndex;

  PmgrNode = dt_get ("/arm-io/pmgr");
  if (PmgrNode == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: \"/arm-io/pmgr\" ADT node not found; cannot resolve %a\n", Tag, DomainName));
    return EFI_NOT_FOUND;
  }

  Devices = dt_node_prop (PmgrNode, "devices", &DevicesLength);
  if ((Devices == NULL) || (DevicesLength < NTASI_PMGR_DEVICE_SIZE)) {
    DEBUG ((DEBUG_ERROR, "%a: \"/arm-io/pmgr\" has no usable \"devices\" property\n", Tag));
    return EFI_NOT_FOUND;
  }

  PsRegs = (CONST UINT32 *)dt_node_prop (PmgrNode, "ps-regs", &PsRegsLength);
  if ((PsRegs == NULL) || (PsRegsLength < (NTASI_PMGR_PSREG_STRIDE * sizeof (UINT32)))) {
    DEBUG ((DEBUG_ERROR, "%a: \"/arm-io/pmgr\" has no usable \"ps-regs\" property\n", Tag));
    return EFI_NOT_FOUND;
  }

  //
  // Resolve every "reg" tuple on the pmgr node itself up front -- small and
  // bounded, and every psreg_idx we might see indexes into this array. This
  // is the multi-block piece the old hardcoded constants skipped by pointing
  // at one block's base address directly.
  //
  RegTupleCount = 0;
  for (TupleIndex = 0; TupleIndex < APPLE_ANS_PMGR_MAX_REG_TUPLES; TupleIndex++) {
    UINT64  TupleBase;
    UINT64  TupleSize;

    if (dt_node_reg (PmgrNode, TupleIndex, &TupleBase, &TupleSize) != 0) {
      break;
    }

    RegTupleBases[TupleIndex] = TupleBase;
    RegTupleCount++;
  }

  if (RegTupleCount == 0) {
    DEBUG ((DEBUG_ERROR, "%a: \"/arm-io/pmgr\" has no readable \"reg\" tuples\n", Tag));
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
      "%a: could not uniquely resolve PMGR domain \"%a\" from the live ADT\n",
      Tag,
      DomainName
      ));
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

/**
  Read one ADT-resolved PMGR power-state word and report whether the domain
  is fully powered (PS_ACTUAL == PS_ACTIVE).

  Read-only by construction: no caller can turn this into a PMGR write. See
  the file header for why this firmware never writes a PMGR word.

  @param[in]  Tag          Short subsystem tag for log lines.
  @param[in]  DomainName   Exact uppercase PMGR device name.
  @param[in]  ExpectedAddress  A hardware-confirmed expectation for the
                               resolved address, or 0 to skip the
                               cross-check. A mismatch is reported and makes
                               *Resolved FALSE -- an address this firmware
                               cannot corroborate is not one to draw
                               conclusions from.
  @param[out] Resolved     TRUE when the address resolved (and matched
                           ExpectedAddress, when supplied) and the register
                           was read.
  @param[out] Active       TRUE when PS_ACTUAL == PS_ACTIVE. Meaningless
                           unless *Resolved is TRUE.
**/
STATIC
inline
VOID
AppleAnsPmgrReportDomain (
  IN  CONST CHAR8  *Tag,
  IN  CONST CHAR8  *DomainName,
  IN  UINT64       ExpectedAddress,
  OUT BOOLEAN      *Resolved,
  OUT BOOLEAN      *Active
  )
{
  EFI_STATUS  Status;
  UINT64      Address;
  UINT32      Value;
  UINT32      Actual;
  UINT32      Target;

  *Resolved = FALSE;
  *Active   = FALSE;

  Address = 0;
  Status  = AppleAnsPmgrResolveDomain (Tag, DomainName, &Address);
  if (EFI_ERROR (Status) || (Address == 0) || ((Address & (sizeof (UINT32) - 1)) != 0)) {
    return;
  }

  if ((ExpectedAddress != 0) && (Address != ExpectedAddress)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: PMGR domain \"%a\" resolved to 0x%Lx but 0x%Lx was expected; refusing to "
      "read an address this firmware cannot corroborate\n",
      Tag,
      DomainName,
      Address,
      ExpectedAddress
      ));
    return;
  }

  Value  = MmioRead32 ((UINTN)Address);
  Actual = (Value & APPLE_PMGR_PS_ACTUAL_MASK) >> APPLE_PMGR_PS_ACTUAL_SHIFT;
  Target = (Value & APPLE_PMGR_PS_TARGET_MASK) >> APPLE_PMGR_PS_TARGET_SHIFT;

  *Resolved = TRUE;
  *Active   = (Actual == APPLE_PMGR_PS_ACTIVE);

  DEBUG ((
    *Active ? DEBUG_INFO : DEBUG_ERROR,
    "%a: PMGR \"%a\" @0x%Lx = 0x%08x (actual=0x%x target=0x%x) %a\n",
    Tag,
    DomainName,
    Address,
    Value,
    Actual,
    Target,
    *Active ? "ACTIVE" : "NOT ACTIVE"
    ));
}

/**
  Reset one ADT-resolved PMGR device, byte for byte m1n1's
  pmgr_reset_device() (src/pmgr.c): refuse unless PS_ACTUAL is ACTIVE, then
  DEV_DISABLE, RESET, 10 us, clear RESET, clear DEV_DISABLE.

  THE ONLY PMGR WRITE IN THIS FIRMWARE, and it exists for one reason.

  m1n1's nvme_shutdown() (src/nvme.c) ends with:

      rtkit_sleep(nvme_rtkit);
      pmgr_reset(nvme_die, "ANS");
      pmgr_reset(nvme_die, "ANS2");

  Mu used to stop at rtkit_sleep -- clearing the ASC run bit and nothing more.
  Clearing the run bit halts the coprocessor's CPU; it does NOT quiesce the
  block's AXI/fabric interface. pmgr_reset is what does that: DEV_DISABLE
  detaches the device from the fabric and RESET returns its master interface
  to a known state, so any transaction still outstanding when the CPU stopped
  cannot remain dangling. A block left halted-but-not-reset is a plausible
  route to another bus master seeing an unexplained bus error later, under
  load -- which is the shape of the 2026-07-30 XHC1 USBSTS.HSE failure that
  correlated with ANS-carrying firmware.

  SAFETY, because a wrong PMGR address writes to a DRAM controller and this
  file's own history includes exactly that mistake (the old hardcoded
  PcdAppleAnsPmgr*Base constants resolved to DCS_09/DCS_10):

    * The address comes from AppleAnsPmgrResolveDomain(), which can only ever
      return an address it found attached to the exact device name it was
      asked for, in this boot's live ADT.
    * ExpectedAddress must ALSO match, or nothing is written. Callers pass the
      hardware-confirmed PCD, so a resolution that drifts writes nothing.
    * The register is re-read and refused unless PS_ACTUAL == PS_ACTIVE,
      exactly as m1n1 refuses to reset a gated device.
    * Read-modify-write of single defined bits only; no field is invented.

  CONVERGENCE, NOT A BLIND DELAY. m1n1 can afford `udelay(10)` and no readback:
  it is a debug loader that continues running afterwards and can be told to try
  again. This copy runs inside the ExitBootServices callback, microseconds
  before Windows owns the machine, and it is the last chance to quiesce the ANS
  fabric interface. So it:

    * holds RESET for m1n1's 10 us minimum, then READS THE REGISTER BACK to
      confirm DEV_DISABLE and RESET actually latched -- MmioOr32() returns the
      value it wrote, not what the hardware took, so nothing before that read
      has established anything;
    * after clearing both bits, POLLS PS_ACTUAL (and PS_TARGET, and the two
      control bits) back to ACTIVE with m1n1's own 10 ms budget, instead of
      assuming;
    * keeps every readback in a real variable. The previous version's final
      readback lived inside a DEBUG() argument, which means it did not happen
      at all in a RELEASE build -- the one build that ships. A verification
      that compiles out is not a verification.

  @param[out] FinalValue  OPTIONAL. Receives the last power-state word read.
                          Written on every path that read the register at all,
                          including the failure paths, so a caller can report
                          the hardware's own account rather than a status code.

  @retval EFI_SUCCESS       The reset sequence completed AND the domain
                            converged back to PS_ACTUAL == PS_TARGET == ACTIVE
                            with RESET and DEV_DISABLE clear.
  @retval EFI_NOT_FOUND     Name did not resolve, or disagreed with
                            ExpectedAddress.
  @retval EFI_NOT_READY     The domain is not ACTIVE; refused (m1n1 does the
                            same).
  @retval EFI_DEVICE_ERROR  The RESET/DEV_DISABLE bits did not read back as
                            set; the sequence was unwound and the block was
                            NOT reset.
  @retval EFI_TIMEOUT       The bits were driven correctly but the domain did
                            not converge within the poll budget. The block is
                            in an unknown state -- say so, do not claim a
                            quiesced fabric interface.
**/
STATIC
inline
EFI_STATUS
AppleAnsPmgrResetDomain (
  IN  CONST CHAR8  *Tag,
  IN  CONST CHAR8  *DomainName,
  IN  UINT64       ExpectedAddress,
  OUT UINT32       *FinalValue  OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT64      Address;
  UINT32      Value;
  UINT32      Actual;
  UINT32      Target;
  UINT32      Elapsed;
  BOOLEAN     Converged;

  if (FinalValue != NULL) {
    *FinalValue = 0;
  }

  Address = 0;
  Status  = AppleAnsPmgrResolveDomain (Tag, DomainName, &Address);
  if (EFI_ERROR (Status) || (Address == 0) || ((Address & (sizeof (UINT32) - 1)) != 0)) {
    return EFI_NOT_FOUND;
  }

  if ((ExpectedAddress == 0) || (Address != ExpectedAddress)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: PMGR \"%a\" resolved to 0x%Lx but 0x%Lx was expected; refusing to WRITE an "
      "address this firmware cannot corroborate\n",
      Tag,
      DomainName,
      Address,
      ExpectedAddress
      ));
    return EFI_NOT_FOUND;
  }

  Value = MmioRead32 ((UINTN)Address);
  if (FinalValue != NULL) {
    *FinalValue = Value;
  }

  if (((Value & APPLE_PMGR_PS_ACTUAL_MASK) >> APPLE_PMGR_PS_ACTUAL_SHIFT) != APPLE_PMGR_PS_ACTIVE) {
    DEBUG ((
      DEBUG_WARN,
      "%a: PMGR \"%a\" @0x%Lx = 0x%08x is not ACTIVE; will not reset (matches m1n1)\n",
      Tag,
      DomainName,
      Address,
      Value
      ));
    return EFI_NOT_READY;
  }

  DEBUG ((DEBUG_INFO, "%a: PMGR \"%a\" @0x%Lx: resetting (0x%08x)\n", Tag, DomainName, Address, Value));

  MmioOr32 ((UINTN)Address, APPLE_PMGR_DEV_DISABLE);
  MmioOr32 ((UINTN)Address, APPLE_PMGR_RESET);

  //
  // m1n1's minimum hold, then confirm the hardware actually took both bits.
  // This read is unconditional: it is the only evidence that anything was
  // driven at all, and it must exist in RELEASE.
  //
  MicroSecondDelay (APPLE_ANS_PMGR_RESET_HOLD_US);
  Value = MmioRead32 ((UINTN)Address);
  if (FinalValue != NULL) {
    *FinalValue = Value;
  }

  if ((Value & (APPLE_PMGR_RESET | APPLE_PMGR_DEV_DISABLE)) !=
      (APPLE_PMGR_RESET | APPLE_PMGR_DEV_DISABLE))
  {
    //
    // The register refused the control bits (a locked or already-gated
    // domain would do this). Unwind what was attempted and report honestly:
    // the block was NOT reset, so the caller must not claim a quiesced
    // fabric interface.
    //
    MmioAnd32 ((UINTN)Address, (UINT32) ~APPLE_PMGR_RESET);
    MmioAnd32 ((UINTN)Address, (UINT32) ~APPLE_PMGR_DEV_DISABLE);
    Value = MmioRead32 ((UINTN)Address);
    if (FinalValue != NULL) {
      *FinalValue = Value;
    }

    DEBUG ((
      DEBUG_ERROR,
      "%a: PMGR \"%a\" @0x%Lx: RESET|DEV_DISABLE did not read back as set "
      "(0x%08x); the block was NOT reset\n",
      Tag,
      DomainName,
      Address,
      Value
      ));
    return EFI_DEVICE_ERROR;
  }

  MmioAnd32 ((UINTN)Address, (UINT32) ~APPLE_PMGR_RESET);
  MmioAnd32 ((UINTN)Address, (UINT32) ~APPLE_PMGR_DEV_DISABLE);

  //
  // Poll to convergence rather than assuming. Budget and step are m1n1's
  // (PMGR_POLL_TIMEOUT). Convergence means all four things at once: both
  // control bits clear, PS_ACTUAL back to ACTIVE, and PS_TARGET agreeing --
  // a domain whose ACTUAL trails its TARGET is still moving.
  //
  Converged = FALSE;
  for (Elapsed = 0; Elapsed < APPLE_ANS_PMGR_POLL_TIMEOUT_US; Elapsed += APPLE_ANS_PMGR_POLL_STEP_US) {
    Value  = MmioRead32 ((UINTN)Address);
    Actual = (Value & APPLE_PMGR_PS_ACTUAL_MASK) >> APPLE_PMGR_PS_ACTUAL_SHIFT;
    Target = (Value & APPLE_PMGR_PS_TARGET_MASK) >> APPLE_PMGR_PS_TARGET_SHIFT;
    if (((Value & (APPLE_PMGR_RESET | APPLE_PMGR_DEV_DISABLE)) == 0) &&
        (Actual == APPLE_PMGR_PS_ACTIVE) &&
        (Target == APPLE_PMGR_PS_ACTIVE))
    {
      Converged = TRUE;
      break;
    }

    MicroSecondDelay (APPLE_ANS_PMGR_POLL_STEP_US);
  }

  //
  // One more unconditional read so the reported value is the settled one and
  // exists in every build flavour.
  //
  Value = MmioRead32 ((UINTN)Address);
  if (FinalValue != NULL) {
    *FinalValue = Value;
  }

  if (!Converged) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: PMGR \"%a\" @0x%Lx: did NOT converge within %Lu us (last 0x%08x, "
      "actual=0x%x target=0x%x); the block is in an unknown state\n",
      Tag,
      DomainName,
      Address,
      (UINT64)APPLE_ANS_PMGR_POLL_TIMEOUT_US,
      Value,
      (Value & APPLE_PMGR_PS_ACTUAL_MASK) >> APPLE_PMGR_PS_ACTUAL_SHIFT,
      (Value & APPLE_PMGR_PS_TARGET_MASK) >> APPLE_PMGR_PS_TARGET_SHIFT
      ));
    return EFI_TIMEOUT;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: PMGR \"%a\" @0x%Lx: reset done and converged in <=%Lu us (now 0x%08x)\n",
    Tag,
    DomainName,
    Address,
    (UINT64)Elapsed + APPLE_ANS_PMGR_POLL_STEP_US,
    Value
    ));
  return EFI_SUCCESS;
}

#endif // APPLE_ANS_PMGR_DOMAIN_H_

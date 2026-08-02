/**
 * Copyright (c) 2024, amarioguy (AppleWOA authors).
 * 
 * Module Name:
 *     AppleUsbTypeCBringupDxe.c
 * 
 * Abstract:
 *     Platform specific driver for Apple silicon platforms to bring up the USB-C ports.
 * 
 * Environment:
 *     UEFI DXE (Driver Execution Environment).
 * 
 * License:
 *     SPDX-License-Identifier: (BSD-2-Clause-Patent OR MIT) AND GPL-2.0
 * 
 *     Original code basis is from the Asahi Linux u-boot project, original copyright and author notices below.
 *     Copyright (C) 2022 Mark Kettenis <kettenis@openbsd.org>
 *     Copyright (C) The Asahi Linux Contributors.
 *     
 *     Parts of DWC3 bringup code brought in from edk2-platforms, original copyright notice below.
 *     Copyright 2017, 2020 NXP
*/

#include <PiDxe.h>
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/ArmLib.h>
#include <Library/PrintLib.h>
#include <Library/PcdLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/TimerLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/AppleDTLib.h>

#include <Drivers/AppleUsbTypeCBringupDxe.h>

//
// This driver is just a stub to bringup register the DWC3 controller(s) as a non-discoverable XHCI controller(s), which
// the built-in XHCI DXE driver should be able to bring up more or less normally.
//
// Historically the PHY only ever did USB 2.0 speeds here, because iBoot only brings it up to that
// state and the USB 3.0 tunables (several of them fuse-derived) were out of scope. That is still
// true of anything this driver does on its own.
//
// It is NO LONGER true of the machine as a whole: m1n1 now has a full ATC PHY driver and can
// configure a port for USB3 before handing off. When it does, it deliberately stops one step short
// and leaves the pipehandler PIPE mux parked on DUMMY, because the mux switch has to happen after
// dwc3 core init (Asahi dwc3-apple.c:29) and dwc3 core init happens *here*. Finishing that handoff
// is what AtcPhyFinishDeferredUsb3Switch below is for. See the block comment on it.
//
// As for actual bringup of the DWC3 controllers, the device registration sequence should be almost exactly the same as that of the sequence used on NXP's UsbHcd driver,
// Only difference here is that we do more of those bringups. Note that the Synopsys bringup will be based on the sequence done in 
// u-boot's DWC3 driver to ensure Apple platform compatibility. (which the NXP bringup sequence is based off of.)
//

//
// ===========================================================================
// Apple vendor DWC3 registers and the deferred USB3 PIPE handoff
// ===========================================================================
//
// Offsets below are DWC3-ABSOLUTE, i.e. relative to the controller base and
// NOT to DWC3_CONTROLLER (which starts at base + DWC3_REG_OFFSET == 0xC100).
// This is the same convention Linux uses when it writes DWC3_GCTL as 0xC110,
// and it is verified against this file's own struct: DWC3_CONTROLLER.GCtl sits
// at struct offset 0x10, so 0xC100 + 0x10 == 0xC110. Likewise GUsb3PipeCtl[0]
// is at struct offset 0x1C0 -> 0xC2C0, matching DWC3_GUSB3PIPECTL(0).
//
#define DWC3_APPLE_CIO_UNK_CD38      0xCD38
#define DWC3_APPLE_CIO_UNK_CD38_VAL  0x0F800F80
#define DWC3_APPLE_CIO_UNK_CD3C      0xCD3C
#define DWC3_APPLE_CIO_UNK_CD3C_VAL  0x0FC00FC0

//
// Link timer register. Field layout and values from dwc3-apple.c:122-128,155-162.
//
#define DWC3_APPLE_CIO_LINK_TIMERS         0xCD40
#define DWC3_APPLE_LINK_HP_TIMER_SHIFT     16
#define DWC3_APPLE_LINK_HP_TIMER_MASK      0x00FF0000
#define DWC3_APPLE_LINK_HP_TIMER_VAL       0x14
#define DWC3_APPLE_LINK_PM_LC_TIMER_SHIFT  8
#define DWC3_APPLE_LINK_PM_LC_TIMER_MASK   0x0000FF00
#define DWC3_APPLE_LINK_PM_LC_TIMER_VAL    0x0A
#define DWC3_APPLE_LINK_PM_ENTRY_SHIFT     0
#define DWC3_APPLE_LINK_PM_ENTRY_MASK      0x000000FF
#define DWC3_APPLE_LINK_PM_ENTRY_VAL       0x10

//
// SUSPHY. Linux core.c:111-136 (dwc3_enable_susphy).
//
#define DWC3_GUSB3PIPECTL_SUSPHY  BIT17
#define DWC3_GUSB2PHYCFG_SUSPHY   BIT6

//
// ---------------------------------------------------------------------------
// Apple ATC PHY register windows, transcribed from m1n1 src/atcphy_core.h,
// which in turn cites Asahi Linux drivers/phy/apple/atc.c line by line.
//
// Two windows are involved, resolved from the guest ADT exactly as m1n1
// resolves them (m1n1 src/atcphy.c:9-18):
//   pipehandler : /arm-io/usb-drdN  reg[3]
//   phy core    : /arm-io/atc-phyN  reg[3]
// ---------------------------------------------------------------------------
//
#define ATCPHY_DRD_REG_PIPEHANDLER  3
#define ATCPHY_ATC_REG_CORE         3

#define ATCPHY_PIPEHANDLER_OVERRIDE                 0x00
#define ATCPHY_PIPEHANDLER_OVERRIDE_RXVALID         BIT0
#define ATCPHY_PIPEHANDLER_OVERRIDE_RXDETECT        BIT2
#define ATCPHY_PIPEHANDLER_OVERRIDE_VALUES          0x04
#define ATCPHY_PIPEHANDLER_OVERRIDE_VAL_RXDETECT0   BIT1
#define ATCPHY_PIPEHANDLER_OVERRIDE_VAL_RXDETECT1   BIT2
#define ATCPHY_PIPEHANDLER_MUX_CTRL                 0x0C
#define ATCPHY_PIPEHANDLER_MUX_DATA_MASK            0x7
#define ATCPHY_PIPEHANDLER_MUX_DATA_SHIFT           0
#define ATCPHY_PIPEHANDLER_MUX_DATA_USB3            0
#define ATCPHY_PIPEHANDLER_MUX_DATA_DUMMY           2
#define ATCPHY_PIPEHANDLER_MUX_CLK_MASK             0x38
#define ATCPHY_PIPEHANDLER_MUX_CLK_SHIFT            3
#define ATCPHY_PIPEHANDLER_MUX_CLK_OFF              0
#define ATCPHY_PIPEHANDLER_MUX_CLK_USB3             1
#define ATCPHY_PIPEHANDLER_MUX_CLK_DUMMY            4
#define ATCPHY_PIPEHANDLER_LOCK_REQ                 0x10
#define ATCPHY_PIPEHANDLER_LOCK_ACK                 0x14
#define ATCPHY_PIPEHANDLER_LOCK_EN                  BIT0
#define ATCPHY_PIPEHANDLER_LOCK_TIMEOUT_US          1000
#define ATCPHY_PIPEHANDLER_NONSELECTED_OVERRIDE     0x20
#define ATCPHY_PIPEHANDLER_NATIVE_POWER_DOWN_MASK   0xF
#define ATCPHY_PIPEHANDLER_NATIVE_POWER_DOWN_SHIFT  0
#define ATCPHY_PIPEHANDLER_NATIVE_RESET             BIT12

#define ATCPHY_CORE_BIST_CIOPHY_CFG1                0x84
#define ATCPHY_CORE_BIST_CIOPHY_CFG1_CLK_EN         BIT27
#define ATCPHY_CORE_BIST_CIOPHY_CFG1_BIST_EN        BIT28
#define ATCPHY_CORE_BIST_OV_CFG                     0x8C
#define ATCPHY_CORE_BIST_OV_CFG_LN0_RESET_N_OV      BIT13
#define ATCPHY_CORE_BIST_OV_CFG_LN0_PWR_DOWN_OV     BIT25
#define ATCPHY_CORE_BIST_READ_CTRL                  0x90
#define ATCPHY_CORE_BIST_READ_CTRL_LN0_PHY_STATUS_RE BIT2
#define ATCPHY_CORE_PHY_STAT                        0x9C
#define ATCPHY_CORE_PHY_STAT_LN0_UNK0               BIT0
#define ATCPHY_CORE_PHY_STAT_LN0_UNK23              BIT23
#define ATCPHY_CORE_BIST_PHY_CFG0                   0xA8
#define ATCPHY_CORE_BIST_PHY_CFG0_LN0_RESET_N       BIT0
#define ATCPHY_CORE_BIST_PHY_CFG1                   0xAC
#define ATCPHY_CORE_BIST_PHY_CFG1_LN0_PWR_DOWN_MASK 0x3C00
#define ATCPHY_CORE_BIST_PHY_CFG1_LN0_PWR_DOWN_SHIFT 10

#define ATCPHY_CORE_POWER_CTRL                      0x20000
#define ATCPHY_CORE_POWER_APB_RESET_N               BIT3
#define ATCPHY_CORE_POWER_PHY_RESET_N               BIT4

#define ATCPHY_PHY_STAT_TIMEOUT_US                  10000

//
// Poll Addr until (read & Mask) == Target, or TimeoutUs elapses.
//
STATIC EFI_STATUS AtcPhyPoll32(IN UINTN Addr, IN UINT32 Mask, IN UINT32 Target, IN UINT32 TimeoutUs) {
  UINT32 Elapsed;

  for (Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 10) {
    if ((MmioRead32(Addr) & Mask) == Target) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay(10);
  }

  return ((MmioRead32(Addr) & Mask) == Target) ? EFI_SUCCESS : EFI_TIMEOUT;
}

//
// Apple vendor CIO registers. dwc3_apple_setup_cio, dwc3-apple.c:150-163,
// carrying the upstream comment "without these USB3 devices sometimes don't
// work" (dwc3-apple.c:110-111). Nothing in our chain used to write these.
//
STATIC VOID Dwc3AppleSetupCio(IN UINTN Dwc3ControllerBaseReg) {
  UINT32 LinkTimers;

  MmioWrite32(Dwc3ControllerBaseReg + DWC3_APPLE_CIO_UNK_CD38, DWC3_APPLE_CIO_UNK_CD38_VAL);
  MmioWrite32(Dwc3ControllerBaseReg + DWC3_APPLE_CIO_UNK_CD3C, DWC3_APPLE_CIO_UNK_CD3C_VAL);

  //
  // Read-modify-write: upstream sets named fields rather than the whole
  // register, so bits outside these three are left as the hardware had them.
  //
  LinkTimers = MmioRead32(Dwc3ControllerBaseReg + DWC3_APPLE_CIO_LINK_TIMERS);
  LinkTimers &= ~(UINT32)(DWC3_APPLE_LINK_HP_TIMER_MASK |
                          DWC3_APPLE_LINK_PM_LC_TIMER_MASK |
                          DWC3_APPLE_LINK_PM_ENTRY_MASK);
  LinkTimers |= (DWC3_APPLE_LINK_HP_TIMER_VAL << DWC3_APPLE_LINK_HP_TIMER_SHIFT) |
                (DWC3_APPLE_LINK_PM_LC_TIMER_VAL << DWC3_APPLE_LINK_PM_LC_TIMER_SHIFT) |
                (DWC3_APPLE_LINK_PM_ENTRY_VAL << DWC3_APPLE_LINK_PM_ENTRY_SHIFT);
  MmioWrite32(Dwc3ControllerBaseReg + DWC3_APPLE_CIO_LINK_TIMERS, LinkTimers);

  DEBUG((DEBUG_INFO, "Dwc3AppleSetupCio: CIO regs programmed (link timers now 0x%x)\n", LinkTimers));
}

//
// dwc3_enable_susphy, core.c:111-136. Called from dwc3-apple.c:271 with the
// comment "This platform requires SUSPHY to be enabled here already in order
// to properly configure the PHY and switch dwc3's PIPE interface to USB3 PHY."
// So this MUST run before the PIPE switch below, not after.
//
STATIC VOID Dwc3EnableSusphy(IN DWC3_CONTROLLER *Controller) {
  MmioOr32((UINTN)&Controller->GUsb3PipeCtl[0], DWC3_GUSB3PIPECTL_SUSPHY);
  MmioOr32((UINTN)&Controller->GUsb2PhyCfg[0], DWC3_GUSB2PHYCFG_SUSPHY);
  MemoryFence();
}

//
// Park the PIPE mux back on the dummy backend. Used as the failure path below:
// a half-switched mux is worse than no switch at all, and dummy is the state
// the rest of the boot chain expects for a USB2-only port.
//
STATIC VOID AtcPhyPipeParkDummy(IN UINTN PipeHandler) {
  UINT32 MuxCtrl;

  MuxCtrl = MmioRead32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL);
  MuxCtrl &= ~(UINT32)(ATCPHY_PIPEHANDLER_MUX_CLK_MASK | ATCPHY_PIPEHANDLER_MUX_DATA_MASK);
  MuxCtrl |= (ATCPHY_PIPEHANDLER_MUX_CLK_DUMMY << ATCPHY_PIPEHANDLER_MUX_CLK_SHIFT) |
             (ATCPHY_PIPEHANDLER_MUX_DATA_DUMMY << ATCPHY_PIPEHANDLER_MUX_DATA_SHIFT);
  MmioWrite32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL, MuxCtrl);
  MemoryFence();
}

//
// Switch the pipehandler PIPE mux from the dummy backend to the live USB3 PHY.
//
// This is a transcription of atcphy_configure_pipehandler_usb3 (Asahi atc.c:
// 975-1079, host path), by way of m1n1's op-table implementation of the same
// sequence in src/atcphy_core.c:853-942. Line citations are upstream atc.c.
//
// PRECONDITION: the ATC PHY itself is already configured and out of reset.
// This function does NOT configure the PHY -- it cannot, that needs the
// tunable blobs and PLL sequencing that live in m1n1. It only moves the mux.
//
STATIC EFI_STATUS AtcPhyPipeSwitchToUsb3(IN UINTN PipeHandler, IN UINTN PhyCore) {
  EFI_STATUS Status;
  UINT32     RegVal;

  //
  // atcphy_pipehandler_check, atc.c:956-973: a previous attempt may have left
  // the lock held. Release it before requesting it again, or the request below
  // never completes.
  //
  if (MmioRead32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_ACK) & ATCPHY_PIPEHANDLER_LOCK_EN) {
    DEBUG((DEBUG_WARN, "AtcPhyPipeSwitchToUsb3: lock already held, clearing first\n"));
    MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_REQ, ~(UINT32)ATCPHY_PIPEHANDLER_LOCK_EN);
    AtcPhyPoll32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_ACK, ATCPHY_PIPEHANDLER_LOCK_EN, 0,
                 ATCPHY_PIPEHANDLER_LOCK_TIMEOUT_US);
  }

  //
  // Force-disable link detection while the mux moves, atc.c:989-995.
  //
  MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_OVERRIDE_VALUES,
            ~(UINT32)(ATCPHY_PIPEHANDLER_OVERRIDE_VAL_RXDETECT0 |
                      ATCPHY_PIPEHANDLER_OVERRIDE_VAL_RXDETECT1));
  MmioOr32(PipeHandler + ATCPHY_PIPEHANDLER_OVERRIDE, ATCPHY_PIPEHANDLER_OVERRIDE_RXVALID);
  MmioOr32(PipeHandler + ATCPHY_PIPEHANDLER_OVERRIDE, ATCPHY_PIPEHANDLER_OVERRIDE_RXDETECT);

  //
  // atcphy_pipehandler_lock, atc.c:920-940.
  //
  MmioOr32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_REQ, ATCPHY_PIPEHANDLER_LOCK_EN);
  Status = AtcPhyPoll32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_ACK, ATCPHY_PIPEHANDLER_LOCK_EN,
                        ATCPHY_PIPEHANDLER_LOCK_EN, ATCPHY_PIPEHANDLER_LOCK_TIMEOUT_US);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "AtcPhyPipeSwitchToUsb3: pipehandler lock not acked, aborting\n"));
    return Status;
  }

  //
  // The BIST dance, atc.c:1004-1037. This is what actually brings lane 0 of the
  // USB3 PHY into a state the pipehandler will accept as a clock source.
  //
  MmioOr32(PhyCore + ATCPHY_CORE_BIST_PHY_CFG0, ATCPHY_CORE_BIST_PHY_CFG0_LN0_RESET_N);
  MmioOr32(PhyCore + ATCPHY_CORE_BIST_OV_CFG, ATCPHY_CORE_BIST_OV_CFG_LN0_RESET_N_OV);
  Status = AtcPhyPoll32(PhyCore + ATCPHY_CORE_PHY_STAT, ATCPHY_CORE_PHY_STAT_LN0_UNK23, 0,
                        ATCPHY_PHY_STAT_TIMEOUT_US);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "AtcPhyPipeSwitchToUsb3: PHY_STAT.LN0_UNK23 never cleared (PHY not "
                        "configured? m1n1 must have run the ATC PHY bringup first)\n"));
    goto Unlock;
  }

  MmioOr32(PhyCore + ATCPHY_CORE_BIST_READ_CTRL, ATCPHY_CORE_BIST_READ_CTRL_LN0_PHY_STATUS_RE);
  MmioAnd32(PhyCore + ATCPHY_CORE_BIST_READ_CTRL, ~(UINT32)ATCPHY_CORE_BIST_READ_CTRL_LN0_PHY_STATUS_RE);

  RegVal = MmioRead32(PhyCore + ATCPHY_CORE_BIST_PHY_CFG1);
  RegVal &= ~(UINT32)ATCPHY_CORE_BIST_PHY_CFG1_LN0_PWR_DOWN_MASK;
  RegVal |= 3u << ATCPHY_CORE_BIST_PHY_CFG1_LN0_PWR_DOWN_SHIFT;
  MmioWrite32(PhyCore + ATCPHY_CORE_BIST_PHY_CFG1, RegVal);

  MmioOr32(PhyCore + ATCPHY_CORE_BIST_OV_CFG, ATCPHY_CORE_BIST_OV_CFG_LN0_PWR_DOWN_OV);
  MmioOr32(PhyCore + ATCPHY_CORE_BIST_CIOPHY_CFG1, ATCPHY_CORE_BIST_CIOPHY_CFG1_CLK_EN);
  MmioOr32(PhyCore + ATCPHY_CORE_BIST_CIOPHY_CFG1, ATCPHY_CORE_BIST_CIOPHY_CFG1_BIST_EN);
  MmioWrite32(PhyCore + ATCPHY_CORE_BIST_CIOPHY_CFG1, 0);

  Status = AtcPhyPoll32(PhyCore + ATCPHY_CORE_PHY_STAT, ATCPHY_CORE_PHY_STAT_LN0_UNK0,
                        ATCPHY_CORE_PHY_STAT_LN0_UNK0, ATCPHY_PHY_STAT_TIMEOUT_US);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "AtcPhyPipeSwitchToUsb3: PHY_STAT.LN0_UNK0 never set\n"));
    goto Unlock;
  }
  Status = AtcPhyPoll32(PhyCore + ATCPHY_CORE_PHY_STAT, ATCPHY_CORE_PHY_STAT_LN0_UNK23, 0,
                        ATCPHY_PHY_STAT_TIMEOUT_US);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "AtcPhyPipeSwitchToUsb3: PHY_STAT.LN0_UNK23 never re-cleared\n"));
    goto Unlock;
  }

  //
  // Clear reset for the non-selected USB3 PHY, atc.c:1043-1046.
  //
  RegVal = MmioRead32(PipeHandler + ATCPHY_PIPEHANDLER_NONSELECTED_OVERRIDE);
  RegVal &= ~(UINT32)ATCPHY_PIPEHANDLER_NATIVE_POWER_DOWN_MASK;
  RegVal |= 3u << ATCPHY_PIPEHANDLER_NATIVE_POWER_DOWN_SHIFT;
  MmioWrite32(PipeHandler + ATCPHY_PIPEHANDLER_NONSELECTED_OVERRIDE, RegVal);
  MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_NONSELECTED_OVERRIDE,
            ~(UINT32)ATCPHY_PIPEHANDLER_NATIVE_RESET);

  //
  // More BIST, atc.c:1049-1053.
  //
  MmioWrite32(PhyCore + ATCPHY_CORE_BIST_OV_CFG, 0);
  MmioOr32(PhyCore + ATCPHY_CORE_BIST_CIOPHY_CFG1, ATCPHY_CORE_BIST_CIOPHY_CFG1_CLK_EN);
  MmioOr32(PhyCore + ATCPHY_CORE_BIST_CIOPHY_CFG1, ATCPHY_CORE_BIST_CIOPHY_CFG1_BIST_EN);

  //
  // The mux itself, atc.c:1056-1065. Clock off, then data to USB3, then clock
  // to USB3, 10 us apart. Order and spacing are upstream's.
  //
  RegVal = MmioRead32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL);
  RegVal &= ~(UINT32)ATCPHY_PIPEHANDLER_MUX_CLK_MASK;
  RegVal |= ATCPHY_PIPEHANDLER_MUX_CLK_OFF << ATCPHY_PIPEHANDLER_MUX_CLK_SHIFT;
  MmioWrite32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL, RegVal);
  MicroSecondDelay(10);

  RegVal = MmioRead32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL);
  RegVal &= ~(UINT32)ATCPHY_PIPEHANDLER_MUX_DATA_MASK;
  RegVal |= ATCPHY_PIPEHANDLER_MUX_DATA_USB3 << ATCPHY_PIPEHANDLER_MUX_DATA_SHIFT;
  MmioWrite32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL, RegVal);
  MicroSecondDelay(10);

  RegVal = MmioRead32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL);
  RegVal &= ~(UINT32)ATCPHY_PIPEHANDLER_MUX_CLK_MASK;
  RegVal |= ATCPHY_PIPEHANDLER_MUX_CLK_USB3 << ATCPHY_PIPEHANDLER_MUX_CLK_SHIFT;
  MmioWrite32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL, RegVal);
  MicroSecondDelay(10);

  //
  // Remove the link detection override, atc.c:1068-1069.
  //
  MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_OVERRIDE, ~(UINT32)ATCPHY_PIPEHANDLER_OVERRIDE_RXVALID);
  MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_OVERRIDE, ~(UINT32)ATCPHY_PIPEHANDLER_OVERRIDE_RXDETECT);

  Status = EFI_SUCCESS;

Unlock:
  //
  // atcphy_pipehandler_unlock, host mode only, atc.c:947-949,1073.
  //
  MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_REQ, ~(UINT32)ATCPHY_PIPEHANDLER_LOCK_EN);
  AtcPhyPoll32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_ACK, ATCPHY_PIPEHANDLER_LOCK_EN, 0,
               ATCPHY_PIPEHANDLER_LOCK_TIMEOUT_US);

  if (EFI_ERROR(Status)) {
    //
    // A half-switched mux is worse than none: park it back on dummy so the
    // port degrades to USB2 rather than to an undefined PIPE topology.
    //
    DEBUG((DEBUG_ERROR, "AtcPhyPipeSwitchToUsb3: FAILED, parking mux back on dummy\n"));
    AtcPhyPipeParkDummy(PipeHandler);
  }

  return Status;
}

//
// Finish the USB3 handoff m1n1 deliberately left half-done for this port.
//
// WHY THIS EXISTS
// ---------------
// Asahi's dwc3-apple.c:29 states the ordering rule: the PIPE mux switch has to
// happen AFTER dwc3 core init. m1n1 cannot satisfy that, because dwc3 core init
// happens here in Mu, after m1n1 is gone. When m1n1 switched the mux itself, the
// result was that Dwc3ControllerSoftReset above then asserted
// GUSB3PIPECTL.PHYSOFTRST for 100 ms on an already-live USB3 PIPE -- something no
// Asahi code path ever does, and a good explanation for SuperSpeed never training
// on this port despite the PHY reporting itself configured.
//
// So the work is split. m1n1 does the part only it can do (tunables, PLLs, lanes,
// crossbar, PHY_RESET_N) and parks the mux on DUMMY. We do the part that must
// come after our own core init: CIO regs, SUSPHY, then the mux.
//
// HOW WE KNOW IT IS OUR TURN
// --------------------------
// Two conditions, both required, and we do nothing unless both hold:
//   1. the platform opted this port in via PcdAppleUsb3PipeSwitchPortMask;
//   2. the ATC PHY reports powered and out of reset (POWER_CTRL APB_RESET_N and
//      PHY_RESET_N both set), which is the state m1n1 leaves behind and is not
//      the state a USB2-only iBoot handoff leaves behind.
// If either fails we leave the port exactly as it was: USB2-only, working.
//
STATIC VOID AtcPhyFinishDeferredUsb3Switch(IN UINT32 PortIndex, IN UINTN Dwc3ControllerBaseReg,
                                           IN DWC3_CONTROLLER *Dwc3Controller) {
  CHAR8       NodeName[31];
  dt_node_t   *DrdNode;
  dt_node_t   *PhyNode;
  UINT64      PipeHandlerBase;
  UINT64      PhyCoreBase;
  UINT32      PowerCtrl;
  EFI_STATUS  Status;

  if ((PcdGet32(PcdAppleUsb3PipeSwitchPortMask) & (1u << PortIndex)) == 0) {
    return;
  }

  AsciiSPrint(NodeName, ARRAY_SIZE(NodeName), "usb-drd%d", PortIndex);
  DrdNode = dt_get(NodeName);
  AsciiSPrint(NodeName, ARRAY_SIZE(NodeName), "atc-phy%d", PortIndex);
  PhyNode = dt_get(NodeName);
  if (DrdNode == NULL || PhyNode == NULL) {
    DEBUG((DEBUG_WARN, "AtcPhyFinishDeferredUsb3Switch: port %d missing usb-drd or atc-phy node, "
                       "skipping USB3 switch\n", PortIndex));
    return;
  }

  if (dt_node_reg(DrdNode, ATCPHY_DRD_REG_PIPEHANDLER, &PipeHandlerBase, NULL) < 0 ||
      dt_node_reg(PhyNode, ATCPHY_ATC_REG_CORE, &PhyCoreBase, NULL) < 0) {
    DEBUG((DEBUG_WARN, "AtcPhyFinishDeferredUsb3Switch: port %d missing pipehandler/core reg, "
                       "skipping USB3 switch\n", PortIndex));
    return;
  }

  PowerCtrl = MmioRead32((UINTN)PhyCoreBase + ATCPHY_CORE_POWER_CTRL);
  if ((PowerCtrl & (ATCPHY_CORE_POWER_APB_RESET_N | ATCPHY_CORE_POWER_PHY_RESET_N)) !=
      (ATCPHY_CORE_POWER_APB_RESET_N | ATCPHY_CORE_POWER_PHY_RESET_N)) {
    DEBUG((DEBUG_INFO, "AtcPhyFinishDeferredUsb3Switch: port %d ATC PHY not configured "
                       "(POWER_CTRL=0x%x); leaving port on USB2\n", PortIndex, PowerCtrl));
    return;
  }

  DEBUG((DEBUG_INFO, "AtcPhyFinishDeferredUsb3Switch: port %d PHY is configured "
                     "(POWER_CTRL=0x%x), finishing USB3 handoff\n", PortIndex, PowerCtrl));

  //
  // P3 then P2 then P1, in dwc3_apple_init's order (dwc3-apple.c:260-272):
  // CIO regs, PRTCAP (already set by our caller), SUSPHY, then the mux.
  //
  Dwc3AppleSetupCio(Dwc3ControllerBaseReg);
  Dwc3EnableSusphy(Dwc3Controller);

  Status = AtcPhyPipeSwitchToUsb3((UINTN)PipeHandlerBase, (UINTN)PhyCoreBase);
  DEBUG((DEBUG_INFO, "AtcPhyFinishDeferredUsb3Switch: port %d USB3 PIPE switch %r "
                     "(MUX_CTRL now 0x%x)\n", PortIndex, Status,
                     MmioRead32((UINTN)PipeHandlerBase + ATCPHY_PIPEHANDLER_MUX_CTRL)));
}

STATIC VOID Dwc3XhciSetBeatBurstLength(IN DWC3_CONTROLLER *Controller) {
  MmioAndThenOr32 ((UINTN)&Controller->GSBusCfg0, ~USB3_ENABLE_BEAT_BURST_MASK,
    USB3_ENABLE_BEAT_BURST);

  MmioOr32 ((UINTN)&Controller->GSBusCfg1, USB3_SET_BEAT_BURST_LIMIT);
}

STATIC VOID Dwc3SetFladj(IN DWC3_CONTROLLER *Controller, IN UINT32 Value) {
  MmioOr32 ((UINTN)&Controller->GFLAdj, GFLADJ_30MHZ_REG_SEL | GFLADJ_30MHZ (Value));
}

STATIC VOID Dwc3SetMode(IN DWC3_CONTROLLER *Controller, IN UINT32 Mode) {
  MmioAndThenOr32 ((UINTN)&Controller->GCtl, ~(DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_OTG)), DWC3_GCTL_PRTCAPDIR (Mode));
}


STATIC VOID Dwc3ControllerSoftReset(IN DWC3_CONTROLLER *Controller) {
  //
  // put the core in reset first.
  //
  MmioOr32((UINTN)&Controller->GCtl, DWC3_GCTL_CORESOFTRESET);

  //
  // Assert USB 2 and USB 3 PHY reset here.
  // There doesn't seem to be Apple-specific carveouts for USB2 or USB3 PHY reset only in u-boot so reset both as per canonical
  // DWC3 u-boot/NXP EDK2 implementation.
  //
  MmioOr32((UINTN)&Controller->GUsb3PipeCtl[0], DWC3_GUSB3PIPECTL_PHYSOFTRST);

  MmioOr32((UINTN)&Controller->GUsb2PhyCfg, DWC3_GUSB2PHYCFG_PHYSOFTRST);

  MemoryFence();

  MicroSecondDelay(100 * 1000);

  //
  // Clear USB 2 and USB 3 PHY reset.
  // Note that this doesn't actually bring up the USB 3 PHY, that's separate ATC setup which we're not doing here for USB 3.
  //

  MmioAnd32((UINTN)&Controller->GUsb3PipeCtl[0], ~DWC3_GUSB3PIPECTL_PHYSOFTRST);

  MmioAnd32 ((UINTN)&Controller->GUsb2PhyCfg, ~DWC3_GUSB2PHYCFG_PHYSOFTRST);

  MemoryFence();

  MicroSecondDelay(100 * 1000);

  //
  // PHYs are stable, take core out of reset.
  //

  MmioAnd32 ((UINTN)&Controller->GCtl, ~DWC3_GCTL_CORESOFTRESET);

}

STATIC EFI_STATUS Dwc3XhciCoreInit(IN DWC3_CONTROLLER *Controller)
{
  UINT32 Dwc3Revision;
  UINT32 Dwc3RegVal;
  UINTN Dwc3HwParams1Reg;
  Dwc3Revision = MmioRead32((UINTN)&Controller->GSnpsId);

  if((Dwc3Revision & DWC3_GSNPSID_MASK) != DWC3_SYNOPSYS_ALT_ID) {
    DEBUG((DEBUG_ERROR, "Dwc3XhciCoreInit: Revision 0x%x Not a Synopsys DWC3 core, aborting\n", Dwc3Revision));
    return EFI_NOT_FOUND;
  }

  //
  // soft reset the DWC3 here.
  //
  Dwc3ControllerSoftReset(Controller);

  Dwc3HwParams1Reg = MmioRead32((UINTN)&Controller->GHwParams1);

  Dwc3RegVal = MmioRead32((UINTN)&Controller->GCtl);
  Dwc3RegVal &= ~DWC3_GCTL_SCALEDOWN_MASK;
  Dwc3RegVal &= ~DWC3_GCTL_DISSCRAMBLE;

  if(DWC3_GHWPARAMS1_EN_PWROPT(Dwc3HwParams1Reg) == DWC3_GHWPARAMS1_EN_PWROPT_CLK) {
    Dwc3RegVal &= ~DWC3_GCTL_DSBLCLKGTNG;
  } else {
    DEBUG((DEBUG_INFO,"Dwc3XhciCoreInit: Power optimization unavailable\n"));
  }
  //
  // Both U-Boot and the NXP UsbHcd driver check for DWC3 errata on revisions < 1.90a - do likewise.
  //
  if((Dwc3Revision & DWC3_RELEASE_MASK) < DWC3_RELEASE_190a) {
    Dwc3RegVal |= DWC3_GCTL_U2RSTECN;
  }
  MmioWrite32((UINTN)&Controller->GCtl, Dwc3RegVal);

  return EFI_SUCCESS;
}


//
// This function actually brings up the DWC3 controller. The PHY is already set up by iBoot so we don't need
// to deal with that here.
//
NON_DISCOVERABLE_DEVICE_INIT 
EFIAPI 
AppleUsbTypeCBringupDxeInitializeUsbController(IN UINTN Dwc3ControllerBaseReg)
{
  EFI_STATUS Status;
  DWC3_CONTROLLER *Dwc3Controller;
  UINT32 Usb2PhyCfgReg;
  //
  // PHY reset/clock is brought up by iBoot, no need to do it here.
  //

  Dwc3Controller = (VOID *)(Dwc3ControllerBaseReg + DWC3_REG_OFFSET);

  Status = Dwc3XhciCoreInit(Dwc3Controller);
  if(EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "AppleUsbTypeCBringupDxeInitializeUsbController: USB controller init failed, status %r\n", Status));
    return (VOID *)EFI_DEVICE_ERROR;
  }

  //
  // the core is initialized at this point, U-Boot sets USB2 PHY config based on quirks in the device tree.
  // Since Apple platforms have none of those quirks defined in known device trees, just read and write back the PHY config to be safe.
  //
  Usb2PhyCfgReg = MmioRead32((UINTN)&Dwc3Controller->GUsb2PhyCfg[0]);
  MmioWrite32((UINTN)&Dwc3Controller->GUsb2PhyCfg[0], Usb2PhyCfgReg);

  //
  // Set the DWC3 to host mode.
  //
  Dwc3SetMode(Dwc3Controller, DWC3_GCTL_PRTCAP_HOST);

  //
  // Disabling this for now but per XHCI spec, this should be set per U-Boot comments? do this as a troubleshooting step if things don't work out. also seems to be set in the "core" dwc3 code in U-Boot.
  //
  Dwc3SetFladj(Dwc3Controller, GFLADJ_30MHZ_DEFAULT);
  Dwc3XhciSetBeatBurstLength(Dwc3Controller);
  return (VOID*)Status;
  
}


VOID 
EFIAPI 
AppleUsbTypeCBringupDxeBringupCallback(IN EFI_EVENT Event, IN VOID *Context)
{
  EFI_STATUS Status;
  UINT32 NumDwc3Controllers;
  UINT64 Dwc3ControllerBaseAddr;
  CHAR8 Dwc3RegNodeName[31];
  UINT32 Dwc3ControllerRegSize;
  NON_DISCOVERABLE_DEVICE_INIT DeviceInit;
  //
  // Close the event so that we don't have duplicate events floating around.
  //
  gBS->CloseEvent(Event);

  DEBUG((DEBUG_INFO, "AppleUsbTypeCBringupDxeBringupCallback started\n"));

  NumDwc3Controllers = PcdGet32(PcdAppleNumDwc3Controllers);

  for(UINT32 Dwc3Index = 0; Dwc3Index < NumDwc3Controllers; Dwc3Index++) {
    AsciiSPrint(Dwc3RegNodeName, ARRAY_SIZE(Dwc3RegNodeName), "usb-drd%d", Dwc3Index);
    dt_node_t *Dwc3Node = dt_get(Dwc3RegNodeName);

    //
    // m1n1 removes the controller used by its proxy transport from the guest
    // ADT. Treat that absence as the ownership signal instead of assuming
    // fixed DFU ports: on machines with several Type-C ports, any other
    // surviving controller can contain the boot disk.
    //
    if (Dwc3Node == NULL) {
      DEBUG((DEBUG_INFO, "AppleUsbTypeCBringupDxeBringupCallback: skipping absent/owned controller %a\n", Dwc3RegNodeName));
      continue;
    }
 
    dt_node_reg(Dwc3Node, 0, &Dwc3ControllerBaseAddr, NULL);

    Dwc3ControllerRegSize = 0x100000;//TODO: get from ADT
    DEBUG((DEBUG_INFO, "AppleUsbTypeCBringupDxeBringupCallback: DWC3_%d base address: 0x%llx, size = 0x%x\n", Dwc3Index, Dwc3ControllerBaseAddr, Dwc3ControllerRegSize));
    
    //
    // Register the controller as a non-registerable XHCI DMA-coherent controller. (All DMA on Apple systems must be cache-coherent)
    // Note: if this doesn't end up working, change the DMA type to non-coherent as one of the first steps to try.
    //
    DeviceInit = AppleUsbTypeCBringupDxeInitializeUsbController(Dwc3ControllerBaseAddr);

    //
    // dwc3 core init has just run (inside the call above) and xhci cannot bind
    // until RegisterNonDiscoverableMmioDevice below. That makes this exact spot
    // the only window in the whole boot chain that satisfies Asahi's ordering
    // rule for the USB3 PIPE switch: after core init, before xhci. No-op unless
    // m1n1 configured this port's PHY and deferred the switch to us.
    //
    AtcPhyFinishDeferredUsb3Switch(
      Dwc3Index,
      (UINTN)Dwc3ControllerBaseAddr,
      (DWC3_CONTROLLER *)(UINTN)(Dwc3ControllerBaseAddr + DWC3_REG_OFFSET));

    Status = RegisterNonDiscoverableMmioDevice(NonDiscoverableDeviceTypeXhci,
             NonDiscoverableDeviceDmaTypeCoherent,
             DeviceInit,
             NULL,
             1,
             Dwc3ControllerBaseAddr,
             Dwc3ControllerRegSize);
  }
  return;
} 


EFI_STATUS
EFIAPI 
AppleUsbTypeCBringupDxeInitialize(
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
) 
{
    EFI_STATUS               Status;
    EFI_EVENT                EndOfDxeEvent;
    //
    // The UsbHcd code in edk2-platforms for NXP platforms (which also use DWC3 controllers) registers the initialization to take place at the end of DXE phase.
    // I'm not quite sure why this is the case, but to avoid problems, do likewise.
    //
    DEBUG((DEBUG_INFO, "AppleUsbTypeCBringupDxeInitialize started\n"));

    Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,
                                TPL_CALLBACK,
                                AppleUsbTypeCBringupDxeBringupCallback,
                                NULL,
                                &gEfiEndOfDxeEventGroupGuid,
                                &EndOfDxeEvent);

    return Status;
}

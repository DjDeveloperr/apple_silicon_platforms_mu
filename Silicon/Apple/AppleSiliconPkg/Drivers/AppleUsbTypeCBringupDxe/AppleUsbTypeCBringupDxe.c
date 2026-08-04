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

// Current Asahi dwc3_core_soft_reset() device-side reset contract.
#define DWC3_DCTL_RUN_STOP         BIT31
#define DWC3_DCTL_CSFTRST          BIT30
#define DWC3_DCTL_ULSTCHNGREQ_MASK (0x0F << 5)
#define DWC3_DCTL_RESET_RETRIES    10
#define DWC3_DCTL_RESET_POLL_US    (20 * 1000)

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
#define ATCPHY_ATC_REG_USB2PHY      0

#define ATCPHY_USB2PHY_USBCTL                 0x00
#define ATCPHY_USB2PHY_USBCTL_MODE_MASK       0x7
#define ATCPHY_USB2PHY_USBCTL_RUN             2
#define ATCPHY_USB2PHY_CTL                    0x04
#define ATCPHY_USB2PHY_CTL_RESET              BIT0
#define ATCPHY_USB2PHY_CTL_PORT_RESET         BIT1
#define ATCPHY_USB2PHY_CTL_APB_RESET_N        BIT2
#define ATCPHY_USB2PHY_CTL_SIDDQ              BIT3
#define ATCPHY_USB2PHY_SIG                    0x08
#define ATCPHY_USB2PHY_SIG_VBUS               (BIT0 | BIT1 | BIT2 | BIT3)
#define ATCPHY_USB2PHY_MISCTUNE               0x1C
#define ATCPHY_USB2PHY_MISCTUNE_APB_GATE_OFF  BIT29
#define ATCPHY_USB2PHY_MISCTUNE_REF_GATE_OFF  BIT30

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
#define ATCPHY_PIPEHANDLER_MUX_DATA_USB4            1
#define ATCPHY_PIPEHANDLER_MUX_DATA_DUMMY           2
#define ATCPHY_PIPEHANDLER_MUX_CLK_MASK             0x38
#define ATCPHY_PIPEHANDLER_MUX_CLK_SHIFT            3
#define ATCPHY_PIPEHANDLER_MUX_CLK_OFF              0
#define ATCPHY_PIPEHANDLER_MUX_CLK_USB3             1
#define ATCPHY_PIPEHANDLER_MUX_CLK_USB4             2
#define ATCPHY_PIPEHANDLER_MUX_CLK_DUMMY            4
//
// Whole-register mux states, matching m1n1 src/atcphy_core.h:
//   DUMMY = CLK_DUMMY|DATA_DUMMY = 0x22
//   USB3  = CLK_USB3 |DATA_USB3  = 0x08
//   USB4  = CLK_USB4 |DATA_USB4  = 0x11   (routed: producer is the ACIO router)
//
#define ATCPHY_PIPEHANDLER_MUX_VALUE_DUMMY          0x22
#define ATCPHY_PIPEHANDLER_MUX_VALUE_USB4_ROUTED    0x11
#define ATCPHY_PIPEHANDLER_LOCK_REQ                 0x10
#define ATCPHY_PIPEHANDLER_LOCK_ACK                 0x14
#define ATCPHY_PIPEHANDLER_LOCK_EN                  BIT0
#define ATCPHY_PIPEHANDLER_LOCK_TIMEOUT_US          1000
//
// The ROUTED-USB4 mux uses Apple's own macOS budget, not atc.c's 1 ms. Decoded
// from AppleT8142USBXHCI::setUSB3Mode's USB4 branch (T6050 BootKC
// com.apple.driver.usb.AppleSynopsysUSB40XHCI): both LOCK_PIPE_IF_ACK polls are
// clock_interval_to_deadline(6, NSEC_PER_MSEC) = 6 ms. Kept as its own constant
// so the working USB3 path above keeps its own proven 1 ms verbatim.
//
#define ATCPHY_PIPEHANDLER_LOCK_ROUTED_TIMEOUT_US   6000
#define ATCPHY_PIPEHANDLER_AON_GEN                  0x1C
#define ATCPHY_PIPEHANDLER_AON_DWC3_FORCE_CLAMP_EN  BIT4
#define ATCPHY_PIPEHANDLER_AON_DWC3_RESET_N         BIT0
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

//
// Which transport the PHY lanes are actually programmed for.
//
// POWER_CTRL (powered, out of reset) plus MUX_CTRL == 0x22 is BYTE-IDENTICAL
// between m1n1's USB4/TBT routed prepare and its direct-USB3 deferred prepare:
// both release APB_RESET_N|PHY_RESET_N and both deliberately park the mux on
// DUMMY for us to finish.  Those two checks therefore establish what STATE the
// PHY is in; they say nothing about which TRANSPORT it carries.  Completing a
// USB3 PIPE switch against lanes crossbarred for USB4 is the failure that
// distinction exists to prevent.
//
// The crossbar protocol field is read-only here and is the discriminator.
//
// *** NAMING TRAP -- read this before touching the values below. ***
// In m1n1's header the constant called PROTOCOL_USB3 (0x0A) is what
// ATCPHY_MODE_OFF programs.  ATCPHY_MODE_USB3 programs PROTOCOL_USB3_DP
// (0x10), because USB3/USB3 is unsupported at 20Gbps so the companion lane is
// programmed as DP.  Writing this gate against the constant whose NAME says
// "USB3" produces an exactly inverted gate: it accepts the OFF state and
// refuses real USB3.  These values were read out of atcphy_modes[] in
// m1n1/src/atcphy_core.c, not out of the header.
//
//   0x10 / 0x11  ATCPHY_MODE_USB3 (USB3 + DP companion)  -> ours, proceed
//   0x00 / 0x01  ATCPHY_MODE_USB4 / ATCPHY_MODE_TBT      -> refuse
//   0x0A / 0x0B  ATCPHY_MODE_OFF (also parks DUMMY)      -> refuse
//   0x14         ATCPHY_MODE_DP                          -> refuse
//
// This is a WHITELIST.  Any unrecognised encoding refuses, for the same reason
// as every other gate in this stack: an unknown value is not evidence that the
// lanes are USB3.
//
#define ATCPHY_CORE_ACIOPHY_CROSSBAR                0x4C
#define ATCPHY_CORE_ACIOPHY_CROSSBAR_PROTOCOL_MASK  0x1F
#define ATCPHY_CROSSBAR_PROTOCOL_USB3_DP            0x10
#define ATCPHY_CROSSBAR_PROTOCOL_USB3_DP_SWAPPED    0x11
//
// The routed (USB4/TBT) crossbar encodings. These are the values the USB3
// whitelist above deliberately REFUSES, and they stay refused there: they are
// accepted only by AtcPhyFinishDeferredUsb4Switch, under its own separate PCD.
// Same whitelist discipline -- an unrecognised encoding refuses.
//
#define ATCPHY_CROSSBAR_PROTOCOL_USB4               0x00
#define ATCPHY_CROSSBAR_PROTOCOL_USB4_SWAPPED       0x01
#define ATCPHY_CORE_ACIOPHY_LANE_MODE               0x48

#define ATCPHY_PHY_STAT_TIMEOUT_US                  10000

//
// USB DART completion contract.  m1n1 keeps each guest DWC3 reset and
// clamped while AppleDartIoMmuDxe replaces its translation state.  Do not
// release a controller merely because the DART driver is earlier in the
// apriori list: prove that both DART instances for this controller have
// reached all-stream bypass first.
//
#define USB_DART_REG_COUNT                    2
#define USB_DART_PARAMS2                      0x0004
#define USB_DART_PARAMS2_BYPASS_SUPPORT       BIT0
#define USB_DART_T8020_NSID                   16
#define USB_DART_T8020_TCR_BASE               0x0100
#define USB_DART_T8020_TCR_BYPASS             (BIT8 | BIT12)
#define USB_DART_T8110_PARAMS4                0x000C
#define USB_DART_T8110_PARAMS4_NSID_MASK      0x1FF
#define USB_DART_T8110_TCR_BASE               0x1000
#define USB_DART_T8110_TCR_BYPASS             (BIT1 | BIT2)

STATIC EFI_STATUS UsbDartVerifyControllerBypass(IN UINT32 PortIndex) {
  CHAR8      NodeName[31];
  dt_node_t  *DartNode;
  CHAR8      *Compatible;
  UINTN      CompatibleLength;
  UINT32     Nsid;
  UINT32     TcrBase;
  UINT32     ExpectedTcr;

  AsciiSPrint(NodeName, ARRAY_SIZE(NodeName), "dart-usb%d", PortIndex);
  DartNode = dt_get(NodeName);
  if (DartNode == NULL) {
    DEBUG((DEBUG_ERROR, "UsbDartVerifyControllerBypass: missing %a\n", NodeName));
    return EFI_NOT_FOUND;
  }

  CompatibleLength = 0;
  Compatible = dt_node_prop(DartNode, "compatible", &CompatibleLength);
  if ((Compatible == NULL) || (CompatibleLength == 0)) {
    DEBUG((DEBUG_ERROR, "UsbDartVerifyControllerBypass: %a has no compatible\n", NodeName));
    return EFI_COMPROMISED_DATA;
  }

  if (AsciiStrCmp(Compatible, "dart,t8110") == 0) {
    TcrBase = USB_DART_T8110_TCR_BASE;
    ExpectedTcr = USB_DART_T8110_TCR_BYPASS;
  } else if (AsciiStrCmp(Compatible, "dart,t6000") == 0) {
    Nsid = USB_DART_T8020_NSID;
    TcrBase = USB_DART_T8020_TCR_BASE;
    ExpectedTcr = USB_DART_T8020_TCR_BYPASS;
  } else {
    DEBUG((DEBUG_ERROR, "UsbDartVerifyControllerBypass: %a unsupported compatible %a\n",
           NodeName, Compatible));
    return EFI_UNSUPPORTED;
  }

  for (UINT32 Instance = 0; Instance < USB_DART_REG_COUNT; Instance++) {
    UINT64 DartBase;

    if (dt_node_reg(DartNode, Instance, &DartBase, NULL) < 0) {
      DEBUG((DEBUG_ERROR, "UsbDartVerifyControllerBypass: %a missing reg[%d]\n",
             NodeName, Instance));
      return EFI_COMPROMISED_DATA;
    }
    if ((MmioRead32((UINTN)DartBase + USB_DART_PARAMS2) &
         USB_DART_PARAMS2_BYPASS_SUPPORT) == 0) {
      DEBUG((DEBUG_ERROR, "UsbDartVerifyControllerBypass: %a reg[%d] lacks bypass\n",
             NodeName, Instance));
      return EFI_UNSUPPORTED;
    }

    if (AsciiStrCmp(Compatible, "dart,t8110") == 0) {
      Nsid = MmioRead32((UINTN)DartBase + USB_DART_T8110_PARAMS4) &
             USB_DART_T8110_PARAMS4_NSID_MASK;
      if (Nsid == 0) {
        DEBUG((DEBUG_ERROR, "UsbDartVerifyControllerBypass: %a reg[%d] reports zero SIDs\n",
               NodeName, Instance));
        return EFI_DEVICE_ERROR;
      }
    }

    for (UINT32 Sid = 0; Sid < Nsid; Sid++) {
      UINT32 Tcr = MmioRead32((UINTN)DartBase + TcrBase + 4 * Sid);
      if (Tcr != ExpectedTcr) {
        DEBUG((DEBUG_ERROR, "UsbDartVerifyControllerBypass: %a reg[%d] SID %d "
                 "TCR=0x%x, expected bypass 0x%x; keeping DWC3 reset\n",
               NodeName, Instance, Sid, Tcr, ExpectedTcr));
        return EFI_NOT_READY;
      }
    }
  }

  DEBUG((DEBUG_INFO, "UsbDartVerifyControllerBypass: port %d both DARTs are in "
                     "all-stream bypass\n", PortIndex));
  return EFI_SUCCESS;
}

STATIC VOID AtcPhyHoldDwc3Reset(IN UINTN PipeHandler) {
  MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_AON_GEN,
            ~(UINT32)ATCPHY_PIPEHANDLER_AON_DWC3_RESET_N);
  MmioOr32(PipeHandler + ATCPHY_PIPEHANDLER_AON_GEN,
           ATCPHY_PIPEHANDLER_AON_DWC3_FORCE_CLAMP_EN);
  MemoryFence();
}

STATIC EFI_STATUS AtcPhyReleaseDwc3AfterDart(IN UINT32 PortIndex,
                                             OUT UINTN *PipeHandlerOut,
                                             OUT UINTN *Usb2PhyOut) {
  CHAR8      NodeName[31];
  dt_node_t  *DrdNode;
  dt_node_t  *PhyNode;
  UINT64     PipeHandlerBase;
  UINT64     Usb2PhyBase;
  UINT32     Aon;
  EFI_STATUS Status;

  Status = UsbDartVerifyControllerBypass(PortIndex);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  AsciiSPrint(NodeName, ARRAY_SIZE(NodeName), "usb-drd%d", PortIndex);
  DrdNode = dt_get(NodeName);
  if ((DrdNode == NULL) ||
      (dt_node_reg(DrdNode, ATCPHY_DRD_REG_PIPEHANDLER, &PipeHandlerBase, NULL) < 0)) {
    DEBUG((DEBUG_ERROR, "AtcPhyReleaseDwc3AfterDart: cannot resolve %a pipehandler\n",
           NodeName));
    return EFI_NOT_FOUND;
  }

  AsciiSPrint(NodeName, ARRAY_SIZE(NodeName), "atc-phy%d", PortIndex);
  PhyNode = dt_get(NodeName);
  if ((PhyNode == NULL) ||
      (dt_node_reg(PhyNode, ATCPHY_ATC_REG_USB2PHY, &Usb2PhyBase, NULL) < 0)) {
    DEBUG((DEBUG_ERROR, "AtcPhyReleaseDwc3AfterDart: cannot resolve %a USB2 PHY\n",
           NodeName));
    return EFI_NOT_FOUND;
  }

  Aon = MmioRead32((UINTN)PipeHandlerBase + ATCPHY_PIPEHANDLER_AON_GEN);
  if ((Aon & (ATCPHY_PIPEHANDLER_AON_DWC3_FORCE_CLAMP_EN |
              ATCPHY_PIPEHANDLER_AON_DWC3_RESET_N)) !=
      ATCPHY_PIPEHANDLER_AON_DWC3_FORCE_CLAMP_EN) {
    DEBUG((DEBUG_ERROR, "AtcPhyReleaseDwc3AfterDart: port %d did not arrive reset+clamped "
           "from m1n1 (AON_GEN=0x%x); refusing release\n", PortIndex, Aon));
    AtcPhyHoldDwc3Reset((UINTN)PipeHandlerBase);
    return EFI_NOT_READY;
  }

  if (MmioRead32((UINTN)PipeHandlerBase + ATCPHY_PIPEHANDLER_MUX_CTRL) !=
      (ATCPHY_PIPEHANDLER_MUX_CLK_DUMMY << ATCPHY_PIPEHANDLER_MUX_CLK_SHIFT |
       ATCPHY_PIPEHANDLER_MUX_DATA_DUMMY << ATCPHY_PIPEHANDLER_MUX_DATA_SHIFT)) {
    DEBUG((DEBUG_ERROR, "AtcPhyReleaseDwc3AfterDart: port %d PIPE is not DUMMY; "
           "refusing release\n", PortIndex));
    AtcPhyHoldDwc3Reset((UINTN)PipeHandlerBase);
    return EFI_NOT_READY;
  }

  // Asahi atcphy_dwc3_reset_deassert(): unclamp first, then release reset.
  MmioAnd32((UINTN)PipeHandlerBase + ATCPHY_PIPEHANDLER_AON_GEN,
            ~(UINT32)ATCPHY_PIPEHANDLER_AON_DWC3_FORCE_CLAMP_EN);
  MmioOr32((UINTN)PipeHandlerBase + ATCPHY_PIPEHANDLER_AON_GEN,
           ATCPHY_PIPEHANDLER_AON_DWC3_RESET_N);
  MemoryFence();

  Aon = MmioRead32((UINTN)PipeHandlerBase + ATCPHY_PIPEHANDLER_AON_GEN);
  if ((Aon & (ATCPHY_PIPEHANDLER_AON_DWC3_FORCE_CLAMP_EN |
              ATCPHY_PIPEHANDLER_AON_DWC3_RESET_N)) !=
      ATCPHY_PIPEHANDLER_AON_DWC3_RESET_N) {
    DEBUG((DEBUG_ERROR, "AtcPhyReleaseDwc3AfterDart: port %d AON release did not stick "
           "(AON_GEN=0x%x)\n", PortIndex, Aon));
    AtcPhyHoldDwc3Reset((UINTN)PipeHandlerBase);
    return EFI_DEVICE_ERROR;
  }

  *PipeHandlerOut = (UINTN)PipeHandlerBase;
  *Usb2PhyOut = (UINTN)Usb2PhyBase;
  DEBUG((DEBUG_INFO, "AtcPhyReleaseDwc3AfterDart: port %d DARTs complete; DWC3 "
                     "released for core init (AON_GEN=0x%x)\n", PortIndex, Aon));
  return EFI_SUCCESS;
}

//
// Asahi's lifecycle keeps USB2 powered off while the host role is selected
// and the external DWC3 reset is asserted.  Only after reset deassertion does
// generic DWC3 bringup power the PHY on.  m1n1 deliberately leaves the PHY in
// that off/host state; reproduce the power-on half here at the actual consumer
// boundary so the stateful eUSB2 repeater stays synchronized with DWC3.
//
STATIC EFI_STATUS AtcPhyPowerOnUsb2AfterDwc3Release(IN UINT32 PortIndex,
                                                    IN UINTN Usb2Phy) {
  UINT32 Ctl;

  MmioOr32(Usb2Phy + ATCPHY_USB2PHY_SIG, ATCPHY_USB2PHY_SIG_VBUS);
  MicroSecondDelay(10);
  MmioAnd32(Usb2Phy + ATCPHY_USB2PHY_CTL, ~(UINT32)ATCPHY_USB2PHY_CTL_SIDDQ);
  MicroSecondDelay(10);
  MmioAnd32(Usb2Phy + ATCPHY_USB2PHY_CTL, ~(UINT32)ATCPHY_USB2PHY_CTL_RESET);
  MicroSecondDelay(10);
  MmioAnd32(Usb2Phy + ATCPHY_USB2PHY_CTL, ~(UINT32)ATCPHY_USB2PHY_CTL_PORT_RESET);
  MicroSecondDelay(10);
  MmioOr32(Usb2Phy + ATCPHY_USB2PHY_CTL, ATCPHY_USB2PHY_CTL_APB_RESET_N);
  MicroSecondDelay(10);
  MmioAnd32(Usb2Phy + ATCPHY_USB2PHY_MISCTUNE,
            ~(UINT32)(ATCPHY_USB2PHY_MISCTUNE_APB_GATE_OFF |
                      ATCPHY_USB2PHY_MISCTUNE_REF_GATE_OFF));
  MmioWrite32(Usb2Phy + ATCPHY_USB2PHY_USBCTL, ATCPHY_USB2PHY_USBCTL_RUN);
  MemoryFence();

  Ctl = MmioRead32(Usb2Phy + ATCPHY_USB2PHY_CTL);
  if (((MmioRead32(Usb2Phy + ATCPHY_USB2PHY_USBCTL) &
        ATCPHY_USB2PHY_USBCTL_MODE_MASK) != ATCPHY_USB2PHY_USBCTL_RUN) ||
      ((Ctl & (ATCPHY_USB2PHY_CTL_RESET |
               ATCPHY_USB2PHY_CTL_PORT_RESET |
               ATCPHY_USB2PHY_CTL_APB_RESET_N |
               ATCPHY_USB2PHY_CTL_SIDDQ)) != ATCPHY_USB2PHY_CTL_APB_RESET_N)) {
    DEBUG((DEBUG_ERROR, "AtcPhyPowerOnUsb2AfterDwc3Release: port %d power-on did not "
           "stick (USBCTL=0x%x CTL=0x%x)\n", PortIndex,
           MmioRead32(Usb2Phy + ATCPHY_USB2PHY_USBCTL), Ctl));
    return EFI_DEVICE_ERROR;
  }

  DEBUG((DEBUG_INFO, "AtcPhyPowerOnUsb2AfterDwc3Release: port %d USB2 PHY live "
                     "after DWC3 release (CTL=0x%x)\n", PortIndex, Ctl));
  return EFI_SUCCESS;
}

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
STATIC VOID Dwc3DisableSusphyForCoreInit(IN DWC3_CONTROLLER *Controller) {
  UINT32 Usb3Before;
  UINT32 Usb2Before;
  UINT32 Usb3After;
  UINT32 Usb2After;

  Usb3Before = MmioRead32((UINTN)&Controller->GUsb3PipeCtl[0]);
  Usb2Before = MmioRead32((UINTN)&Controller->GUsb2PhyCfg[0]);
  MmioAnd32((UINTN)&Controller->GUsb3PipeCtl[0],
            ~(UINT32)DWC3_GUSB3PIPECTL_SUSPHY);
  MmioAnd32((UINTN)&Controller->GUsb2PhyCfg[0],
            ~(UINT32)DWC3_GUSB2PHYCFG_SUSPHY);
  MemoryFence();

  Usb3After = MmioRead32((UINTN)&Controller->GUsb3PipeCtl[0]);
  Usb2After = MmioRead32((UINTN)&Controller->GUsb2PhyCfg[0]);
  DEBUG((DEBUG_INFO, "Dwc3DisableSusphyForCoreInit: USB3PIPECTL 0x%x->0x%x, "
                     "USB2PHYCFG 0x%x->0x%x\n",
         Usb3Before, Usb3After, Usb2Before, Usb2After));
}

STATIC VOID Dwc3EnableSusphy(IN DWC3_CONTROLLER *Controller) {
  UINT32 Usb3Before;
  UINT32 Usb2Before;

  Usb3Before = MmioRead32((UINTN)&Controller->GUsb3PipeCtl[0]);
  Usb2Before = MmioRead32((UINTN)&Controller->GUsb2PhyCfg[0]);
  MmioOr32((UINTN)&Controller->GUsb3PipeCtl[0], DWC3_GUSB3PIPECTL_SUSPHY);
  MmioOr32((UINTN)&Controller->GUsb2PhyCfg[0], DWC3_GUSB2PHYCFG_SUSPHY);
  MemoryFence();
  DEBUG((DEBUG_INFO, "Dwc3EnableSusphy: USB3PIPECTL 0x%x->0x%x, "
                     "USB2PHYCFG 0x%x->0x%x\n",
         Usb3Before, MmioRead32((UINTN)&Controller->GUsb3PipeCtl[0]),
         Usb2Before, MmioRead32((UINTN)&Controller->GUsb2PhyCfg[0])));
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

  Status = EFI_SUCCESS;

  //
  // atcphy_pipehandler_check, atc.c:956-973: a previous attempt may have left
  // the lock held. Release it before requesting it again, or the request below
  // never completes.
  //
  if (MmioRead32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_ACK) & ATCPHY_PIPEHANDLER_LOCK_EN) {
    DEBUG((DEBUG_WARN, "AtcPhyPipeSwitchToUsb3: lock already held, clearing first\n"));
    MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_REQ, ~(UINT32)ATCPHY_PIPEHANDLER_LOCK_EN);
    Status = AtcPhyPoll32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_ACK,
                          ATCPHY_PIPEHANDLER_LOCK_EN, 0,
                          ATCPHY_PIPEHANDLER_LOCK_TIMEOUT_US);
    if (EFI_ERROR(Status)) {
      DEBUG((DEBUG_ERROR, "AtcPhyPipeSwitchToUsb3: stale lock did not clear\n"));
      goto Unlock;
    }
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
    goto Unlock;
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
  if (EFI_ERROR(AtcPhyPoll32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_ACK,
                             ATCPHY_PIPEHANDLER_LOCK_EN, 0,
                             ATCPHY_PIPEHANDLER_LOCK_TIMEOUT_US))) {
    DEBUG((DEBUG_ERROR, "AtcPhyPipeSwitchToUsb3: pipehandler unlock not acked\n"));
    Status = EFI_TIMEOUT;
  }

  if (EFI_ERROR(Status)) {
    //
    // A half-switched mux is worse than none: park it back on dummy so the
    // port degrades to USB2 rather than to an undefined PIPE topology. Remove
    // the temporary link-detection overrides even when lock acquisition or a
    // BIST poll failed before the normal success-path cleanup.
    //
    DEBUG((DEBUG_ERROR, "AtcPhyPipeSwitchToUsb3: FAILED, parking mux back on dummy\n"));
    MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_OVERRIDE,
              ~(UINT32)(ATCPHY_PIPEHANDLER_OVERRIDE_RXVALID |
                        ATCPHY_PIPEHANDLER_OVERRIDE_RXDETECT));
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
// result was that the former generic reset path then asserted
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
  UINT32      MuxCtrl;
  UINT32      Crossbar;
  UINT32      Protocol;
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

  MuxCtrl = MmioRead32((UINTN)PipeHandlerBase + ATCPHY_PIPEHANDLER_MUX_CTRL);
  if (MuxCtrl != (ATCPHY_PIPEHANDLER_MUX_CLK_DUMMY << ATCPHY_PIPEHANDLER_MUX_CLK_SHIFT |
                  ATCPHY_PIPEHANDLER_MUX_DATA_DUMMY << ATCPHY_PIPEHANDLER_MUX_DATA_SHIFT)) {
    DEBUG((DEBUG_ERROR, "AtcPhyFinishDeferredUsb3Switch: port %d expected deferred DUMMY "
                        "mux 0x22, got 0x%x; parking DUMMY and refusing ambiguous PIPE state\n",
           PortIndex, MuxCtrl));
    AtcPhyPipeParkDummy((UINTN)PipeHandlerBase);
    return;
  }

  //
  // The lanes must be crossbarred for USB3, not merely powered and parked.
  // See the NAMING TRAP note beside the constants: 0x10/0x11 is USB3, and the
  // constant named "PROTOCOL_USB3" is the OFF state.
  //
  Crossbar = MmioRead32((UINTN)PhyCoreBase + ATCPHY_CORE_ACIOPHY_CROSSBAR);
  Protocol = Crossbar & ATCPHY_CORE_ACIOPHY_CROSSBAR_PROTOCOL_MASK;
  if (Protocol != ATCPHY_CROSSBAR_PROTOCOL_USB3_DP &&
      Protocol != ATCPHY_CROSSBAR_PROTOCOL_USB3_DP_SWAPPED) {
    DEBUG((DEBUG_ERROR, "AtcPhyFinishDeferredUsb3Switch: port %d lanes are NOT crossbarred "
                        "for USB3 (CROSSBAR=0x%x, protocol=0x%x, LANE_MODE=0x%x); refusing "
                        "to complete a USB3 PIPE switch. DUMMY mux plus a powered PHY is the "
                        "same state a USB4/TBT routed prepare leaves behind, so it cannot "
                        "authorise this switch on its own.\n",
           PortIndex, Crossbar, Protocol,
           MmioRead32((UINTN)PhyCoreBase + ATCPHY_CORE_ACIOPHY_LANE_MODE)));
    AtcPhyPipeParkDummy((UINTN)PipeHandlerBase);
    return;
  }

  DEBUG((DEBUG_INFO, "AtcPhyFinishDeferredUsb3Switch: port %d PHY is configured "
                     "(POWER_CTRL=0x%x, CROSSBAR protocol=0x%x), finishing USB3 handoff\n",
         PortIndex, PowerCtrl, Protocol));

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

//
// Move the PIPE mux to the ROUTED USB4 backend (whole-register 0x11).
//
// PROVENANCE: this is Apple's own sequence, not a reconstruction. macOS's owner
// of this mux is the xHCI/dwc3 driver -- AppleT8142USBXHCI::setUSB3Mode (T6050
// BootKC com.apple.driver.usb.AppleSynopsysUSB40XHCI, fn @0xfffffe000b0a3df8,
// USB4 branch @0xfffffe000b0a9ea4) -- which drives the pipehandler
// (mapDeviceMemoryWithIndex(3), i.e. usb-drdN reg[3]) directly. T6050 encodes
// PIPE_CLK_EN as GENMASK(6,4) and T6020 as GENMASK(5,3); the field VALUES are
// identical, so Apple's T6050 whole-register 0x21 is this platform's 0x11.
//
// It differs from the USB3 path above in three ways, each read off the decode:
//   1. NO BIST dance. The USB3 branch brings lane 0 of the *native* USB3 PHY up
//      as a clock source; the routed producer is the ACIO host router, already
//      running, so Apple does none of it.
//   2. NO NONSELECTED_OVERRIDE (+0x20) write. Apple's +0x20 RMWs live in the
//      DUMMY and USB3 branches only; the USB4 branch jumps to the tail.
//   3. The LOCK_PIPE_IF_ACK polls use the 6 ms routed budget.
// There is also no fixed settle delay between the three MUX_CTRL writes: Apple
// issues CLK-off -> DATA -> CLK back-to-back (kc 0xb0a9f34/0xb0aa160/0xb0aa390).
//
// PRECONDITION: the ATC PHY is configured for a routed mode and out of reset,
// and m1n1 has brought the ACIO router and its USB3 tunnel up. This function
// only moves the mux; it cannot create a tunnel and does not try.
//
STATIC EFI_STATUS AtcPhyPipeSwitchToUsb4Routed(IN UINTN PipeHandler) {
  EFI_STATUS Status;
  UINT32     RegVal;
  UINT32     MuxCtrl;

  //
  // A previous attempt may have left the lock held; release it before
  // requesting it again, or the request below never completes. Same
  // precaution as the USB3 path (atc.c:956-973).
  //
  if (MmioRead32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_ACK) & ATCPHY_PIPEHANDLER_LOCK_EN) {
    DEBUG((DEBUG_WARN, "AtcPhyPipeSwitchToUsb4Routed: lock already held, clearing first\n"));
    MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_REQ, ~(UINT32)ATCPHY_PIPEHANDLER_LOCK_EN);
    Status = AtcPhyPoll32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_ACK,
                          ATCPHY_PIPEHANDLER_LOCK_EN, 0,
                          ATCPHY_PIPEHANDLER_LOCK_ROUTED_TIMEOUT_US);
    if (EFI_ERROR(Status)) {
      DEBUG((DEBUG_ERROR, "AtcPhyPipeSwitchToUsb4Routed: stale lock did not clear\n"));
      return Status;
    }
  }

  //
  // Force the link inputs inactive while the mux moves (kc 0xb0a810c/0xb0a8330).
  //
  MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_OVERRIDE_VALUES,
            ~(UINT32)(ATCPHY_PIPEHANDLER_OVERRIDE_VAL_RXDETECT0 |
                      ATCPHY_PIPEHANDLER_OVERRIDE_VAL_RXDETECT1));
  MmioOr32(PipeHandler + ATCPHY_PIPEHANDLER_OVERRIDE,
           ATCPHY_PIPEHANDLER_OVERRIDE_RXVALID | ATCPHY_PIPEHANDLER_OVERRIDE_RXDETECT);

  //
  // LOCK_PIPE_IF_REQ + 6 ms ACK poll (kc 0xb0a8564 / 0xb0a9550).
  //
  MmioOr32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_REQ, ATCPHY_PIPEHANDLER_LOCK_EN);
  Status = AtcPhyPoll32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_ACK, ATCPHY_PIPEHANDLER_LOCK_EN,
                        ATCPHY_PIPEHANDLER_LOCK_EN, ATCPHY_PIPEHANDLER_LOCK_ROUTED_TIMEOUT_US);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "AtcPhyPipeSwitchToUsb4Routed: lock not acked, aborting\n"));
    goto Unlock;
  }

  //
  // The three mux writes, back-to-back (kc 0xb0a9f34 / 0xb0aa160 / 0xb0aa390).
  //
  RegVal = MmioRead32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL);
  RegVal &= ~(UINT32)ATCPHY_PIPEHANDLER_MUX_CLK_MASK;
  RegVal |= ATCPHY_PIPEHANDLER_MUX_CLK_OFF << ATCPHY_PIPEHANDLER_MUX_CLK_SHIFT;
  MmioWrite32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL, RegVal);

  RegVal &= ~(UINT32)ATCPHY_PIPEHANDLER_MUX_DATA_MASK;
  RegVal |= ATCPHY_PIPEHANDLER_MUX_DATA_USB4 << ATCPHY_PIPEHANDLER_MUX_DATA_SHIFT;
  MmioWrite32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL, RegVal);

  RegVal &= ~(UINT32)ATCPHY_PIPEHANDLER_MUX_CLK_MASK;
  RegVal |= ATCPHY_PIPEHANDLER_MUX_CLK_USB4 << ATCPHY_PIPEHANDLER_MUX_CLK_SHIFT;
  MmioWrite32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL, RegVal);
  MemoryFence();

  //
  // Release the link-input overrides (kc 0xb0aa5c0).
  //
  MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_OVERRIDE,
            ~(UINT32)(ATCPHY_PIPEHANDLER_OVERRIDE_RXVALID |
                      ATCPHY_PIPEHANDLER_OVERRIDE_RXDETECT));

Unlock:
  //
  // Clear LOCK_PIPE_IF_REQ + 6 ms ACK-clear poll (kc 0xb0aaa0c / 0xb0aac04).
  //
  MmioAnd32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_REQ, ~(UINT32)ATCPHY_PIPEHANDLER_LOCK_EN);
  if (EFI_ERROR(AtcPhyPoll32(PipeHandler + ATCPHY_PIPEHANDLER_LOCK_ACK,
                             ATCPHY_PIPEHANDLER_LOCK_EN, 0,
                             ATCPHY_PIPEHANDLER_LOCK_ROUTED_TIMEOUT_US)) &&
      !EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "AtcPhyPipeSwitchToUsb4Routed: unlock never acked\n"));
    Status = EFI_TIMEOUT;
  }
  if (EFI_ERROR(Status)) {
    return Status;
  }

  //
  // Verify by READBACK, never by assuming the write took. This is the same rule
  // m1n1 applies on its side; a mux that ACKs is not a mux that switched.
  //
  MuxCtrl = MmioRead32(PipeHandler + ATCPHY_PIPEHANDLER_MUX_CTRL);
  if (MuxCtrl != ATCPHY_PIPEHANDLER_MUX_VALUE_USB4_ROUTED) {
    DEBUG((DEBUG_ERROR, "AtcPhyPipeSwitchToUsb4Routed: mux read back 0x%x, expected 0x%x; "
                        "refusing to claim a switch that did not take\n",
           MuxCtrl, ATCPHY_PIPEHANDLER_MUX_VALUE_USB4_ROUTED));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

//
// Finish (or re-establish) the ROUTED USB4 PIPE switch for a tunnelled port.
//
// WHY THIS EXISTS -- and why m1n1 committing 0x11 is not enough on its own.
// ----------------------------------------------------------------------
// m1n1 can bring the ACIO router and its USB3 tunnel up and commit the mux to
// 0x11, but dwc3 core init happens HERE, after m1n1 is gone, and it resets the
// PIPE. Worse, before this function existed the only Mu code that looked at the
// mux was the USB3 finisher, whose contract is "anything that is not DUMMY is
// ambiguous -- park it back on DUMMY". A port that m1n1 had correctly committed
// to 0x11 would therefore have been actively RESET to USB2 here, silently.
//
// So this runs at the same post-core-init/pre-xhci point as the USB3 finisher
// and accepts BOTH handoff shapes:
//   * mux on DUMMY (0x22) -- m1n1 deferred the switch to us, the same split the
//     USB3 path uses; we perform it.
//   * mux already 0x11 -- m1n1 committed it; we re-apply after core init, since
//     the reset in between may have disturbed it. Re-running the sequence on an
//     already-routed mux is idempotent (it ends in the same three field writes).
//
// HOW WE KNOW IT IS OUR TURN -- three conditions, all required:
//   1. the platform opted this port in via PcdAppleUsb4RoutedPipeSwitchPortMask
//      (a DIFFERENT PCD from the USB3 one; a port must never be in both);
//   2. the ATC PHY reports powered and out of reset (POWER_CTRL APB_RESET_N and
//      PHY_RESET_N), the state m1n1's routed prepare leaves behind;
//   3. the lanes are crossbarred for a ROUTED mode (USB4/TBT protocol 0x0/0x1).
//      Condition 2 alone cannot authorise this -- a powered PHY with a DUMMY mux
//      is also what a USB3 deferred prepare looks like, which is exactly why the
//      USB3 finisher checks its own crossbar encoding and why this one checks
//      the complementary pair. The two whitelists are disjoint by construction.
// If any fails we leave the port exactly as it was. USB2-only is a safe outcome.
//
// This function NEVER touches a port whose bit is clear, so with the default
// PCD value of 0 the entire boot chain behaves exactly as it did before.
//
STATIC VOID AtcPhyFinishDeferredUsb4Switch(IN UINT32 PortIndex, IN UINTN Dwc3ControllerBaseReg,
                                           IN DWC3_CONTROLLER *Dwc3Controller) {
  CHAR8       NodeName[31];
  dt_node_t   *DrdNode;
  dt_node_t   *PhyNode;
  UINT64      PipeHandlerBase;
  UINT64      PhyCoreBase;
  UINT32      PowerCtrl;
  UINT32      MuxCtrl;
  UINT32      Crossbar;
  UINT32      Protocol;
  EFI_STATUS  Status;

  if ((PcdGet32(PcdAppleUsb4RoutedPipeSwitchPortMask) & (1u << PortIndex)) == 0) {
    return;
  }

  AsciiSPrint(NodeName, ARRAY_SIZE(NodeName), "usb-drd%d", PortIndex);
  DrdNode = dt_get(NodeName);
  AsciiSPrint(NodeName, ARRAY_SIZE(NodeName), "atc-phy%d", PortIndex);
  PhyNode = dt_get(NodeName);
  if (DrdNode == NULL || PhyNode == NULL) {
    DEBUG((DEBUG_WARN, "AtcPhyFinishDeferredUsb4Switch: port %d missing usb-drd or atc-phy node, "
                       "skipping routed switch\n", PortIndex));
    return;
  }

  if (dt_node_reg(DrdNode, ATCPHY_DRD_REG_PIPEHANDLER, &PipeHandlerBase, NULL) < 0 ||
      dt_node_reg(PhyNode, ATCPHY_ATC_REG_CORE, &PhyCoreBase, NULL) < 0) {
    DEBUG((DEBUG_WARN, "AtcPhyFinishDeferredUsb4Switch: port %d missing pipehandler/core reg, "
                       "skipping routed switch\n", PortIndex));
    return;
  }

  PowerCtrl = MmioRead32((UINTN)PhyCoreBase + ATCPHY_CORE_POWER_CTRL);
  if ((PowerCtrl & (ATCPHY_CORE_POWER_APB_RESET_N | ATCPHY_CORE_POWER_PHY_RESET_N)) !=
      (ATCPHY_CORE_POWER_APB_RESET_N | ATCPHY_CORE_POWER_PHY_RESET_N)) {
    DEBUG((DEBUG_INFO, "AtcPhyFinishDeferredUsb4Switch: port %d ATC PHY not configured "
                       "(POWER_CTRL=0x%x); leaving port on USB2\n", PortIndex, PowerCtrl));
    return;
  }

  //
  // The lanes must be crossbarred for a ROUTED mode. This is the complement of
  // the USB3 finisher's whitelist and is what makes the two unambiguous.
  //
  Crossbar = MmioRead32((UINTN)PhyCoreBase + ATCPHY_CORE_ACIOPHY_CROSSBAR);
  Protocol = Crossbar & ATCPHY_CORE_ACIOPHY_CROSSBAR_PROTOCOL_MASK;
  if (Protocol != ATCPHY_CROSSBAR_PROTOCOL_USB4 &&
      Protocol != ATCPHY_CROSSBAR_PROTOCOL_USB4_SWAPPED) {
    DEBUG((DEBUG_ERROR, "AtcPhyFinishDeferredUsb4Switch: port %d lanes are NOT crossbarred for "
                        "USB4/TBT (CROSSBAR=0x%x, protocol=0x%x, LANE_MODE=0x%x); refusing the "
                        "routed PIPE switch\n",
           PortIndex, Crossbar, Protocol,
           MmioRead32((UINTN)PhyCoreBase + ATCPHY_CORE_ACIOPHY_LANE_MODE)));
    return;
  }

  //
  // Accept only the two handoff shapes we understand. Anything else is an
  // ambiguous PIPE state and is left strictly alone -- note we deliberately do
  // NOT park DUMMY here: on a routed port the mux may legitimately already be
  // live, and parking it would be the very destruction this function exists to
  // prevent.
  //
  MuxCtrl = MmioRead32((UINTN)PipeHandlerBase + ATCPHY_PIPEHANDLER_MUX_CTRL);
  if (MuxCtrl != ATCPHY_PIPEHANDLER_MUX_VALUE_DUMMY &&
      MuxCtrl != ATCPHY_PIPEHANDLER_MUX_VALUE_USB4_ROUTED) {
    DEBUG((DEBUG_ERROR, "AtcPhyFinishDeferredUsb4Switch: port %d unexpected mux 0x%x (expected "
                        "DUMMY 0x%x or already-routed 0x%x); leaving it untouched\n",
           PortIndex, MuxCtrl, ATCPHY_PIPEHANDLER_MUX_VALUE_DUMMY,
           ATCPHY_PIPEHANDLER_MUX_VALUE_USB4_ROUTED));
    return;
  }

  DEBUG((DEBUG_INFO, "AtcPhyFinishDeferredUsb4Switch: port %d routed PHY is configured "
                     "(POWER_CTRL=0x%x, protocol=0x%x, mux=0x%x -> %a), finishing routed "
                     "USB4 handoff\n",
         PortIndex, PowerCtrl, Protocol, MuxCtrl,
         MuxCtrl == ATCPHY_PIPEHANDLER_MUX_VALUE_USB4_ROUTED ? "re-applying m1n1's commit"
                                                             : "performing deferred switch"));

  //
  // Same ordering as the USB3 path (dwc3-apple.c:260-272): CIO regs, PRTCAP
  // (already set by our caller), SUSPHY, then the mux.
  //
  Dwc3AppleSetupCio(Dwc3ControllerBaseReg);
  Dwc3EnableSusphy(Dwc3Controller);

  Status = AtcPhyPipeSwitchToUsb4Routed((UINTN)PipeHandlerBase);
  DEBUG((DEBUG_INFO, "AtcPhyFinishDeferredUsb4Switch: port %d routed USB4 PIPE switch %r "
                     "(MUX_CTRL now 0x%x). NOTE this only means the mux reads 0x11 -- it is "
                     "NOT a claim that any tunnelled device enumerated.\n",
         PortIndex, Status,
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

STATIC EFI_STATUS Dwc3DeviceSideSoftReset(IN DWC3_CONTROLLER *Controller) {
  UINT32 Dctl;

  //
  // Apple glue invokes core init before selecting HOST PRTCAP, so current
  // Asahi still performs this device-side DCTL reset. The safe write removes
  // any stale link-state request, asserts CSFTRST, and clears RUN_STOP. It
  // deliberately never pulses GCTL.CORESOFTRESET or either PHY reset.
  //
  Dctl = MmioRead32((UINTN)&Controller->DCtl);
  Dctl |= DWC3_DCTL_CSFTRST;
  Dctl &= ~(UINT32)(DWC3_DCTL_RUN_STOP | DWC3_DCTL_ULSTCHNGREQ_MASK);
  MmioWrite32((UINTN)&Controller->DCtl, Dctl);

  // DWC31 1.90a+ may need slightly over 50 ms. Match Asahi's 20 ms poll,
  // bounded to ten attempts.
  for (UINT32 Retry = 0; Retry < DWC3_DCTL_RESET_RETRIES; Retry++) {
    Dctl = MmioRead32((UINTN)&Controller->DCtl);
    if ((Dctl & DWC3_DCTL_CSFTRST) == 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay(DWC3_DCTL_RESET_POLL_US);
  }

  DEBUG((DEBUG_ERROR, "Dwc3DeviceSideSoftReset: DCTL.CSFTRST did not clear "
         "(DCTL=0x%x)\n", Dctl));
  return EFI_TIMEOUT;
}


STATIC EFI_STATUS Dwc3XhciCoreInit(IN DWC3_CONTROLLER *Controller)
{
  EFI_STATUS Status;
  UINT32 Dwc3Revision;
  UINT32 Dwc3RegVal;
  UINTN Dwc3HwParams1Reg;
  Dwc3Revision = MmioRead32((UINTN)&Controller->GSnpsId);

  if((Dwc3Revision & DWC3_GSNPSID_MASK) != DWC3_SYNOPSYS_ALT_ID) {
    DEBUG((DEBUG_ERROR, "Dwc3XhciCoreInit: Revision 0x%x Not a Synopsys DWC3 core, aborting\n", Dwc3Revision));
    return EFI_NOT_FOUND;
  }

  // Asahi dwc3_phy_setup() clears both SUSPHY bits before core reset/init.
  // Apple glue re-enables them after init, immediately before USB3 PIPE setup.
  Dwc3DisableSusphyForCoreInit(Controller);

  Status = Dwc3DeviceSideSoftReset(Controller);
  if (EFI_ERROR(Status)) {
    return Status;
  }

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
STATIC EFI_STATUS
AppleUsbTypeCBringupDxeInitializeUsbController(IN UINT32 PortIndex,
                                                IN UINTN Dwc3ControllerBaseReg)
{
  EFI_STATUS Status;
  DWC3_CONTROLLER *Dwc3Controller;
  UINT32 Usb2PhyCfgReg;
  UINTN PipeHandler;
  UINTN Usb2Phy;
  //
  // PHY reset/clock is brought up by iBoot, no need to do it here.
  //

  Dwc3Controller = (VOID *)(Dwc3ControllerBaseReg + DWC3_REG_OFFSET);

  //
  // This is the cross-stage ownership boundary.  m1n1 deliberately left the
  // Apple AON reset asserted; AppleDartIoMmuDxe must have completed both DART
  // instances before this helper releases it.  Release happens immediately
  // before generic DWC3 core init and is reasserted if that init fails.
  //
  Status = AtcPhyReleaseDwc3AfterDart(PortIndex, &PipeHandler, &Usb2Phy);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "AppleUsbTypeCBringupDxeInitializeUsbController: port %d "
           "reset/DART handoff failed: %r\n", PortIndex, Status));
    return Status;
  }

  Status = AtcPhyPowerOnUsb2AfterDwc3Release(PortIndex, Usb2Phy);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "AppleUsbTypeCBringupDxeInitializeUsbController: port %d "
           "USB2 PHY power-on failed: %r\n", PortIndex, Status));
    AtcPhyHoldDwc3Reset(PipeHandler);
    return Status;
  }

  Status = Dwc3XhciCoreInit(Dwc3Controller);
  if(EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "AppleUsbTypeCBringupDxeInitializeUsbController: USB controller init failed, status %r\n", Status));
    AtcPhyHoldDwc3Reset(PipeHandler);
    return EFI_DEVICE_ERROR;
  }

  //
  // the core is initialized at this point, U-Boot sets USB2 PHY config based on quirks in the device tree.
  // Since Apple platforms have none of those quirks defined in known device trees, just read and write back the PHY config to be safe.
  //
  Usb2PhyCfgReg = MmioRead32((UINTN)&Dwc3Controller->GUsb2PhyCfg[0]);
  MmioWrite32((UINTN)&Dwc3Controller->GUsb2PhyCfg[0], Usb2PhyCfgReg);

  // Asahi's Apple glue selects HOST only after core init/soft reset.
  Dwc3SetMode(Dwc3Controller, DWC3_GCTL_PRTCAP_HOST);

  //
  // Disabling this for now but per XHCI spec, this should be set per U-Boot comments? do this as a troubleshooting step if things don't work out. also seems to be set in the "core" dwc3 code in U-Boot.
  //
  Dwc3SetFladj(Dwc3Controller, GFLADJ_30MHZ_DEFAULT);
  Dwc3XhciSetBeatBurstLength(Dwc3Controller);
  return Status;
  
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
    Status = AppleUsbTypeCBringupDxeInitializeUsbController(
               Dwc3Index, Dwc3ControllerBaseAddr);
    if (EFI_ERROR(Status)) {
      DEBUG((DEBUG_ERROR, "AppleUsbTypeCBringupDxeBringupCallback: controller %d "
             "failed closed before registration: %r\n", Dwc3Index, Status));
      continue;
    }

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

    //
    // Same window, same ordering rule, for a port carrying a USB4 tunnel
    // instead of direct USB3. Gated by its own PCD and its own crossbar
    // whitelist, so it is a strict no-op for every port the USB3 path owns and
    // for every port not explicitly opted in. A port must never appear in both
    // masks -- they are different PIPE mux values on one PHY.
    //
    AtcPhyFinishDeferredUsb4Switch(
      Dwc3Index,
      (UINTN)Dwc3ControllerBaseAddr,
      (DWC3_CONTROLLER *)(UINTN)(Dwc3ControllerBaseAddr + DWC3_REG_OFFSET));

    Status = RegisterNonDiscoverableMmioDevice(NonDiscoverableDeviceTypeXhci,
             NonDiscoverableDeviceDmaTypeCoherent,
             NULL,
             NULL,
             1,
             Dwc3ControllerBaseAddr,
             Dwc3ControllerRegSize);
    if (EFI_ERROR(Status)) {
      DEBUG((DEBUG_ERROR, "AppleUsbTypeCBringupDxeBringupCallback: controller %d "
             "registration failed: %r\n", Dwc3Index, Status));
    }
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

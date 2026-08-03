## @file
#  MacBook Pro Early 2023 DSC file, borrowing aspects from SurfaceDuo2.dsc from WOA-Project/SurfaceDuoPkg
#  Copyright (c) 2011-2015, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2016, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################

[Defines]
  PLATFORM_NAME                  = MacBookProEarly2023
  DEFINE NTASI_ENABLE_WIRELESS_DART_HANDOFF = 0
  DEFINE NTASI_ENABLE_XHC2 = 1
  # The PIPE switch is a firmware capability, selected per sealed profile.
  # Right-enabled profiles finish port 2; phase-1 no-XHC2 profiles finish only
  # the left boot-volume port 1. PlatformBuild.py supplies the profile value.
  DEFINE NTASI_USB3_PIPE_SWITCH_PORT_MASK = 0x4
  DEFINE NTASI_J414S_GPU_RESOURCE_PROFILE = 0
  # Publish NTAS0023 (AppleAgxGpu) from AcpiPlatformDxe. Strictly narrower than
  # NTASI_J414S_GPU_RESOURCE_PROFILE, which only reserves the ADT-derived
  # carveouts in the GCD: this one additionally emits the device, its eight
  # _CRS resources and its GSIV-46 interrupt. Kept separate so "reserve the
  # carveouts" and "publish the ACPI device" stay independently selectable --
  # the same split ans/ans_acpi already has, and the reason the ans-noacpi
  # control could isolate the 0x144. Setting this without the resource profile
  # does nothing: the whole block is nested inside it.
  DEFINE NTASI_ENABLE_GPU_ACPI_PUBLICATION = 0
  DEFINE NTASI_ANS_DXE_BRINGUP = FALSE
  DEFINE NTASI_ANS_PUBLISH_BLOCK_IO = FALSE
  DEFINE NTASI_ANS_PRESERVE_FOR_OS = FALSE
  DEFINE NTASI_ANS_PUBLISH_ACPI = FALSE
  # Media profile: publish MCA0 (NTAS0080), AOPA (NTAS0081) and ISP0
  # (NTAS0090) from AcpiPlatformDxe. OFF by default, and off means
  # preprocessor-excluded, so a profile without it produces byte-identical
  # firmware rather than merely equivalent firmware. Adds no FFS module and no
  # ACPI table to the FV (the SSDTs are generated at DXE runtime, like ANS0 and
  # DRT0), so expected_ffs_count and the 94-image count are unchanged. Publishes
  # ZERO interrupt resources and touches no CSRT byte -- see
  # NtasiInstallMediaTables() in AcpiPlatform.c.
  DEFINE NTASI_ENABLE_MEDIA_PUBLICATION = 0
  # Battery: publish BAT0 (NTAS0053) from AcpiPlatformDxe, the devnode the
  # AppleSmcBattery battc miniport binds to so Windows shows a real battery.
  # OFF by default, and off means preprocessor-excluded, so a profile without
  # it produces byte-identical firmware. Adds no FFS module and no ACPI table
  # to the FV (the SSDT is generated at DXE runtime, like ANS0, DRT0 and the
  # media tables), so expected_ffs_count and the 94-image count are unchanged.
  # Publishes ZERO interrupts and ZERO memory windows -- its _CRS is an empty
  # resource template -- so it allocates no GSIV, touches no CSRT byte, and
  # cannot collide with SMCG's exclusive claim on the SMC ASC and SRAM
  # windows. See NtasiInstallBatteryTable() in AcpiPlatform.c.
  DEFINE NTASI_ENABLE_BATTERY_PUBLICATION = 0
  # WinPE deploy-verdict echo (BootRamdiskHelperDxe). OFF by default: it adds a
  # participant to the ReadyToBoot event group and opens every attached FAT
  # volume microseconds before the OS loader starts, on a machine whose boot disk
  # is USB and whose DEBUG build deadloops on any ASSERT. Turn it on for a
  # deploy-verification boot with
  #   NTASI_DEPLOY_EVIDENCE_ECHO=1 ./Tools/build-j414s-windows-native.sh <profile>
  # and leave it off for every boot whose result is meant to be attributable.
  DEFINE NTASI_DEPLOY_EVIDENCE_ECHO = 0
  PLATFORM_GUID                  = d70b31ca-2cbc-433b-885f-b8bbda409959
  PLATFORM_VERSION               = 1.0
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/MacBookProEarly2023-$(ARCH)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = MacBookProEarly2023Pkg/MacBookProEarly2023.fdf
  SECURE_BOOT_ENABLE             = FALSE #disable secure boot for now
  AIC_BUILD                      = TRUE  # Mu uses native AIC; m1n1 supplies only the later Windows startup carrier
  NETWORK_TLS_ENABLE             = TRUE
  # Experimental only. Build with
  #   BLD_*_NTASI_T6020_J414S_HOMOGENEOUS_EFFICIENCY=1
  # to publish PEC 0 for all ten processors without changing MPIDRs, CPU UIDs,
  # or PPTT topology. PlatformBuild.py supplies the default value of 0.

[BuildOptions.common]
  GCC:*_*_AARCH64_CC_FLAGS = -DSILICON_PLATFORM=6020
  *_*_*_CC_FLAGS = -D DISABLE_NEW_DEPRECATED_INTERFACES -D HAS_MEMCPY_INTRINSICS -DNTASI_T6020_J414S_HOMOGENEOUS_EFFICIENCY=$(NTASI_T6020_J414S_HOMOGENEOUS_EFFICIENCY) -DNTASI_ENABLE_WIRELESS_DART_HANDOFF=$(NTASI_ENABLE_WIRELESS_DART_HANDOFF) -DNTASI_J414S_GPU_RESOURCE_PROFILE=$(NTASI_J414S_GPU_RESOURCE_PROFILE) -DNTASI_ENABLE_GPU_ACPI_PUBLICATION=$(NTASI_ENABLE_GPU_ACPI_PUBLICATION) -DNTASI_ENABLE_MEDIA_PUBLICATION=$(NTASI_ENABLE_MEDIA_PUBLICATION) -DNTASI_ENABLE_BATTERY_PUBLICATION=$(NTASI_ENABLE_BATTERY_PUBLICATION) -DNTASI_DEPLOY_EVIDENCE_ECHO=$(NTASI_DEPLOY_EVIDENCE_ECHO)
  *_*_*_ASLPP_FLAGS = -DNTASI_ENABLE_XHC2=$(NTASI_ENABLE_XHC2)



[PcdsFixedAtBuild.common]
  # This firmware is RAM-loaded by m1n1 and has no persistent UEFI variable
  # store. The normal first-boot memory-type update reset would therefore
  # repeat on every launch instead of stabilizing after one reboot.
  gEfiMdeModulePkgTokenSpaceGuid.PcdResetOnMemoryTypeInformationChange|FALSE
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemModel|"MacBook Pro (Early 2023)"
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemModelNumber|"Mac14,5/Mac14,6/Mac14,9/Mac14,10"
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemSku|"MacBook Pro (Early 2023) (Mac14,5/Mac14,6/Mac14,9/Mac14,10)"
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleNumDwc3Controllers|3 # M2 Pro case is hardcoded for now.
  #
  # Finish exactly the deferred USB3 PIPE selected by the sealed profile:
  # 0x2 = left boot-volume port for phase-1 no-XHC2 isolation;
  # 0x4 = right port for right-enabled profiles. Never select both implicitly.
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleUsb3PipeSwitchPortMask|$(NTASI_USB3_PIPE_SWITCH_PORT_MASK)
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleNumDwc3Darts|6 # M2 Pro case is hardcoded for now.
  # Windows consumes GSIV 38; the AIC2 CSRT translates it to T6020 line 1832.
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPublishedInterrupt|38
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsExpectedPhysicalInterrupt|1832
  # ANS publication is the only storage-firmware experiment.  The unified
  # baseline leaves this FALSE; the ans build profile overrides it to TRUE.
  # DECOUPLED 2026-07-30 from NTASI_ENABLE_ANS (which gates the driver FFS) so
  # "the ANS driver is in the FV" and "NTAS2003 is published to Windows" can be
  # varied independently. The `ans-noacpi` profile is exactly that experiment:
  # identical FFS set to `ans`, no NTAS2003.
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPublishAcpiDevice|$(NTASI_ANS_PUBLISH_ACPI)
  # Mu-side ANS bring-up. FALSE leaves the coprocessor exactly as iBoot left it
  # (running), which is the state every booting profile has. Flip
  # NTASI_ANS_DXE_BRINGUP to TRUE and rebuild to restore the full bring-up.
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPerformDxeBringUp|$(NTASI_ANS_DXE_BRINGUP)
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPublishBlockIo|$(NTASI_ANS_PUBLISH_BLOCK_IO)
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPreserveForOs|$(NTASI_ANS_PRESERVE_FOR_OS)
  # CORRECTED 2026-07-30 (hardware-confirmed): these four were resolved
  # against the wrong PMGR register block. /arm-io/pmgr's "ps-regs" table
  # has multiple blocks (reg tuples); ANS2/APCIE_ST/APCIE_ST_SYS/
  # APCIE_ST1_SYS live in the "pmgr_east" block (base 0x290280000), not the
  # main "pmgr" block (base 0x28E080000) these values used to point into.
  # The old 0x28E080-prefixed addresses land on DCS_09/DCS_10 -- DRAM
  # controller power domains -- at the exact same low offsets, which is why
  # they passed every alignment/spacing sanity check while being
  # catastrophically wrong. Live ADT walk of /arm-io/pmgr "devices",
  # matching by name, confirms these four; TODO(Delivery 2): resolve them
  # from the ADT at runtime instead of trusting this constant again.
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPmgrResetBase|0x2902801A8
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPmgrApcieStBase|0x2902801A0
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPmgrApcieStSysBase|0x290280408
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPmgrApcieSt1SysBase|0x290280410

[Components.common]

  MacBookProEarly2023Pkg/AcpiTables/DeviceAcpiTables.inf

!include MacBookProFamilyPkg/MacBookProFamilyPkg.dsc.inc
!include T602XFamilyPkg/T602XFamilyPkg.dsc.inc
!include AppleSiliconPkg/AppleSiliconPkg.dsc.inc
!include AppleSiliconPkg/FrontpageDsc.inc

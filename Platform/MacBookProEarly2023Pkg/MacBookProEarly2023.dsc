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
  *_*_*_CC_FLAGS = -D DISABLE_NEW_DEPRECATED_INTERFACES -D HAS_MEMCPY_INTRINSICS -DNTASI_T6020_J414S_HOMOGENEOUS_EFFICIENCY=$(NTASI_T6020_J414S_HOMOGENEOUS_EFFICIENCY)



[PcdsFixedAtBuild.common]
  # This firmware is RAM-loaded by m1n1 and has no persistent UEFI variable
  # store. The normal first-boot memory-type update reset would therefore
  # repeat on every launch instead of stabilizing after one reboot.
  gEfiMdeModulePkgTokenSpaceGuid.PcdResetOnMemoryTypeInformationChange|FALSE
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemModel|"MacBook Pro (Early 2023)"
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemModelNumber|"Mac14,5/Mac14,6/Mac14,9/Mac14,10"
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemSku|"MacBook Pro (Early 2023) (Mac14,5/Mac14,6/Mac14,9/Mac14,10)"
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleNumDwc3Controllers|3 # M2 Pro case is hardcoded for now.
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleNumDwc3Darts|6 # M2 Pro case is hardcoded for now.
  # Windows consumes GSIV 38; the AIC2 CSRT translates it to T6020 line 1832.
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPublishedInterrupt|38
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsExpectedPhysicalInterrupt|1832
  # This branch's FV carries AppleNANDStorageDxe, so the SSDT may publish.
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPublishAcpiDevice|TRUE

  #
  # BCM4388 SID-1 DART page-table carveout.  m1n1's wireless handoff installs
  # a deny-all SID-1 domain whose L1 / dedicated MSI L2 tables live here, and
  # they must outlive m1n1: Windows' pci.sys enables bus mastering on both
  # BCM4388 functions before any KMDF driver runs, so the domain has to be
  # live and its tables non-conventional across the whole handoff.
  #
  # Keep in lockstep with WLAN_PT_CARVEOUT_PHYS / WLAN_PT_CARVEOUT_SIZE in
  # the m1n1 patch and the second DRT0 _CRS memory resource in DSDT.asl.
  #
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleWirelessDartPageTableBase|0x10022000000
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleWirelessDartPageTableSize|0x10000

[Components.common]

  MacBookProEarly2023Pkg/AcpiTables/DeviceAcpiTables.inf

!include MacBookProFamilyPkg/MacBookProFamilyPkg.dsc.inc
!include T602XFamilyPkg/T602XFamilyPkg.dsc.inc
!include AppleSiliconPkg/AppleSiliconPkg.dsc.inc
!include AppleSiliconPkg/FrontpageDsc.inc

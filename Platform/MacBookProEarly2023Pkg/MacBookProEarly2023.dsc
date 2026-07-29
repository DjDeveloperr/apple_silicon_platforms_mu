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

[BuildOptions.common]
  GCC:*_*_AARCH64_CC_FLAGS = -DSILICON_PLATFORM=6020
  *_*_*_CC_FLAGS = -D DISABLE_NEW_DEPRECATED_INTERFACES -D HAS_MEMCPY_INTRINSICS



[PcdsFixedAtBuild.common]
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemModel|"MacBook Pro (Early 2023)"
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemModelNumber|"Mac14,5/Mac14,6/Mac14,9/Mac14,10"
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemSku|"MacBook Pro (Early 2023) (Mac14,5/Mac14,6/Mac14,9/Mac14,10)"
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleNumDwc3Controllers|3 # M2 Pro case is hardcoded for now.
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleNumDwc3Darts|6 # M2 Pro case is hardcoded for now.
  # Windows' GIC arbiter cannot allocate T6020's physical ANS line 1832.
  # Publish the platform-reserved GSIV 38; the AIC2 CSRT maps it back to 1832.
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPublishedInterrupt|38
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsExpectedPhysicalInterrupt|1832
  # Physical xHCI line 1274 is published as arbiter-legal GSIV 37; AIC2 ALI2
  # translates it back before native-controller MMIO.
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleXhciPublishedInterrupt|37

[Components.common]

  MacBookProEarly2023Pkg/AcpiTables/DeviceAcpiTables.inf

!include MacBookProFamilyPkg/MacBookProFamilyPkg.dsc.inc
!include T602XFamilyPkg/T602XFamilyPkg.dsc.inc
!include AppleSiliconPkg/AppleSiliconPkg.dsc.inc
!include AppleSiliconPkg/FrontpageDsc.inc

[PcdsFixedAtBuild.common]
  # J414s has four E-cores and two three-core P clusters (4+3+3).  This must
  # follow the T602x family include so the platform-specific value wins over
  # the family's twelve-core maximum.
  gAppleSiliconPkgTokenSpaceGuid.PcdCoreCount|10
  # Publish the architectural ARM generic-timer PPIs.  The T602x family
  # defaults are historical arbitrary placeholders and do not describe the
  # Windows/native-AIC carrier contract.
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|29
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|30
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|27
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|26

[PcdsFeatureFlag.common]
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsAcpiEnabled|FALSE

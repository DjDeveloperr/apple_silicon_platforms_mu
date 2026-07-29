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
  DEFINE NTASI_ENABLE_WIRELESS_DART_HANDOFF = FALSE
  DEFINE NTASI_J414S_GPU_RESOURCE_PROFILE = FALSE
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
  *_*_*_CC_FLAGS = -D DISABLE_NEW_DEPRECATED_INTERFACES -D HAS_MEMCPY_INTRINSICS -DNTASI_T6020_J414S_HOMOGENEOUS_EFFICIENCY=$(NTASI_T6020_J414S_HOMOGENEOUS_EFFICIENCY) -DNTASI_ENABLE_WIRELESS_DART_HANDOFF=$(NTASI_ENABLE_WIRELESS_DART_HANDOFF) -DNTASI_J414S_GPU_RESOURCE_PROFILE=$(NTASI_J414S_GPU_RESOURCE_PROFILE)



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
  # ANS publication is the only storage-firmware experiment.  The unified
  # baseline leaves this FALSE; the ans build profile overrides it to TRUE.
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPublishAcpiDevice|$(NTASI_ENABLE_ANS)
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPmgrResetBase|0x28E0801A8
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPmgrApcieStBase|0x28E0801A0
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPmgrApcieStSysBase|0x28E080408
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleAnsPmgrApcieSt1SysBase|0x28E080410
  # Exact m1n1 wireless_handoff_init() carveout. Mu only reserves and
  # publishes it; it never creates or modifies the DART tables.
!if $(NTASI_ENABLE_WIRELESS_DART_HANDOFF) == TRUE
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleWirelessDartPageTableBase|0x10022000000
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleWirelessDartPageTableSize|0x10000
!else
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleWirelessDartPageTableBase|0
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleWirelessDartPageTableSize|0
!endif

[Components.common]

  MacBookProEarly2023Pkg/AcpiTables/DeviceAcpiTables.inf
!if $(NTASI_ENABLE_WIRELESS_DART_HANDOFF) == TRUE
  MacBookProEarly2023Pkg/AcpiTables/WirelessDartAcpiTables.inf
!endif
!if $(NTASI_J414S_GPU_RESOURCE_PROFILE) == TRUE
  MacBookProEarly2023Pkg/AcpiTables/GpuAcpiTables.inf
!endif

!include MacBookProFamilyPkg/MacBookProFamilyPkg.dsc.inc
!include T602XFamilyPkg/T602XFamilyPkg.dsc.inc
!include AppleSiliconPkg/AppleSiliconPkg.dsc.inc
!include AppleSiliconPkg/FrontpageDsc.inc

# @file
# Script to Build MacBook Pro (Early 2023) Mu UEFI firmware 
# (this script is based off WOA-Project/SurfaceDuoPkg PlatformBuild.py)
#
# Copyright (c) Microsoft Corporation.
# SPDX-License-Identifier: BSD-2-Clause-Patent
##
import datetime
import logging
import os
import uuid
from io import StringIO

from edk2toolext.environment import shell_environment
from edk2toolext.environment.uefi_build import UefiBuilder
from edk2toolext.invocables.edk2_platform_build import BuildSettingsManager
from edk2toolext.invocables.edk2_pr_eval import PrEvalSettingsManager
from edk2toolext.invocables.edk2_setup import (RequiredSubmodule,
                                               SetupSettingsManager)
from edk2toolext.invocables.edk2_update import UpdateSettingsManager
from edk2toolext.invocables.edk2_parse import ParseSettingsManager
from edk2toollib.utility_functions import RunCmd


    # ####################################################################################### #
    #                                Common Configuration                                     #
    # ####################################################################################### #
class CommonPlatform():
    ''' Common settings for this platform.  Define static data here and use
        for the different parts of stuart
    '''
    PackagesSupported = ("MacBookProEarly2023Pkg",)
    ArchSupported = ("AARCH64",)
    TargetsSupported = ("DEBUG", "RELEASE", "NOOPT")
    Scopes = ('MacBookProEarly2023', 'gcc_aarch64_linux')
    WorkspaceRoot = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    PackagesPath = ("Platform", "MU_BASECORE", "Common/MU", "Common/TIANO", "Common/MU_OEM_SAMPLE", "Silicon/ARM/TIANO", "Silicon/Apple", "Common/MU_DFCI", "mu_feature_debugger")


    # ####################################################################################### #
    #                         Configuration for Update & Setup                                #
    # ####################################################################################### #
class SettingsManager(UpdateSettingsManager, SetupSettingsManager, PrEvalSettingsManager):

    def GetPackagesSupported(self):
        ''' return iterable of edk2 packages supported by this build.
        These should be edk2 workspace relative paths '''
        return CommonPlatform.PackagesSupported

    def GetArchitecturesSupported(self):
        ''' return iterable of edk2 architectures supported by this build '''
        return CommonPlatform.ArchSupported

    def GetTargetsSupported(self):
        ''' return iterable of edk2 target tags supported by this build '''
        return CommonPlatform.TargetsSupported

    def GetRequiredSubmodules(self):
        """Use this disposable local source snapshot as-is."""
        return []

    def SetArchitectures(self, list_of_requested_architectures):
        ''' Confirm the requests architecture list is valid and configure SettingsManager
        to run only the requested architectures.

        Raise Exception if a list_of_requested_architectures is not supported
        '''
        unsupported = set(list_of_requested_architectures) - \
            set(self.GetArchitecturesSupported())
        if(len(unsupported) > 0):
            errorString = (
                "Unsupported Architecture Requested: " + " ".join(unsupported))
            logging.critical( errorString )
            raise Exception( errorString )
        self.ActualArchitectures = list_of_requested_architectures

    def GetWorkspaceRoot(self):
        ''' get WorkspacePath '''
        return CommonPlatform.WorkspaceRoot

    def GetActiveScopes(self):
        ''' return tuple containing scopes that should be active for this process '''
        return CommonPlatform.Scopes

    def FilterPackagesToTest(self, changedFilesList: list, potentialPackagesList: list) -> list:
        ''' Filter other cases that this package should be built
        based on changed files. This should cover things that can't
        be detected as dependencies. '''
        build_these_packages = []
        possible_packages = potentialPackagesList.copy()
        for f in changedFilesList:
            # BaseTools files that might change the build
            if "BaseTools" in f:
                if os.path.splitext(f) not in [".txt", ".md"]:
                    build_these_packages = possible_packages
                    break

            # if the azure pipeline platform template file changed
            if "platform-build-run-steps.yml" in f:
                build_these_packages = possible_packages
                break

        return build_these_packages

    def GetPlatformDscAndConfig(self) -> tuple:
        ''' If a platform desires to provide its DSC then Policy 4 will evaluate if
        any of the changes will be built in the dsc.

        The tuple should be (<workspace relative path to dsc file>, <input dictionary of dsc key value pairs>)
        '''
        return ("MacBookProEarly2023Pkg/MacBookProEarly2023.dsc", {})

    def GetName(self):
        return "MacBookProEarly2023"

    def GetPackagesPath(self):
        ''' Return a list of paths that should be mapped as edk2 PackagesPath '''
        return CommonPlatform.PackagesPath

    # ####################################################################################### #
    #                         Actual Configuration for Platform Build                         #
    # ####################################################################################### #
class PlatformBuilder( UefiBuilder, BuildSettingsManager):
    def __init__(self):
        UefiBuilder.__init__(self)

    def AddCommandLineOptions(self, parserObj):
        ''' Add command line options to the argparser '''

        # In an effort to support common server based builds this parameter is added.  It is
        # checked for correctness but is never uses as this platform only supports a single set of
        # architectures.
        parserObj.add_argument('-a', "--arch", dest="build_arch", type=str, default="AARCH64",
            help="Optional - CSV of architecture to build.  AARCH64 is used for PEI and "
            "DXE and is the only valid option for this platform.")

    def RetrieveCommandLineOptions(self, args):
        '''  Retrieve command line options from the argparser '''
        if args.build_arch.upper() != "AARCH64":
            raise Exception("Invalid Arch Specified.  Please see comments in PlatformBuild.py::PlatformBuilder::AddCommandLineOptions")

    def GetWorkspaceRoot(self):
        ''' get WorkspacePath '''
        return CommonPlatform.WorkspaceRoot

    def GetPackagesPath(self):
        ''' Return a list of paths that should be mapped as edk2 PackagesPath '''
        feature_config_path = shell_environment.GetBuildVars().GetValue(
            "FEATURE_CONFIG_PATH", ""
        )
        # An unset optional feature path must not become an empty package-root
        # entry.  edk2-pytool-extensions 0.27.6 later resolves Conf templates
        # through this list and otherwise passes None to os.path.join(), making
        # a clean Linux/container build fail before any EDK2 source compiles.
        result = [feature_config_path] if feature_config_path else []
        for a in CommonPlatform.PackagesPath:
            result.append(a)
        return result

    def GetActiveScopes(self):
        ''' return tuple containing scopes that should be active for this process '''
        return CommonPlatform.Scopes

    def GetName(self):
        ''' Get the name of the repo, platform, or product being build '''
        ''' Used for naming the log file, among others '''
        return "MacBookProEarly2023"

    def GetLoggingLevel(self, loggerType):
        ''' Get the logging level for a given type
        base == lowest logging level supported
        con  == Screen logging
        txt  == plain text file logging
        md   == markdown file logging
        '''
        return logging.DEBUG
        #return super().GetLoggingLevel(loggerType)

    def SetPlatformEnv(self):
        logging.debug("PlatformBuilder SetPlatformEnv")
        profile = os.environ.get("NTASI_MU_PROFILE", "baseline").strip().lower()
        profile_values = {
            "baseline": {"ans": "FALSE", "gpu": "0", "wireless": "0"},
            "ans": {"ans": "TRUE", "gpu": "0", "wireless": "0"},
            "gpu": {"ans": "FALSE", "gpu": "1", "wireless": "0"},
            "ans-gpu": {"ans": "TRUE", "gpu": "1", "wireless": "0"},
            "wireless": {"ans": "FALSE", "gpu": "0", "wireless": "1"},
            "ans-gpu-wireless": {"ans": "TRUE", "gpu": "1", "wireless": "1"},
        }
        if profile not in profile_values:
            raise ValueError(
                "NTASI_MU_PROFILE must be one of: "
                "baseline, ans, gpu, ans-gpu, wireless, ans-gpu-wireless"
            )
        logging.info("Building the J414s Windows Mu profile: %s", profile)

        self.env.SetValue("PRODUCT_NAME", "MacBookProEarly2023", "Platform Hardcoded")
        self.env.SetValue("ACTIVE_PLATFORM", "MacBookProEarly2023Pkg/MacBookProEarly2023.dsc", "Platform Hardcoded")
        self.env.SetValue("TARGET_ARCH", "AARCH64", "Platform Hardcoded")
        self.env.SetValue("TOOL_CHAIN_TAG", "CLANGPDB", "set default to clangpdb")
        self.env.SetValue("EMPTY_DRIVE", "FALSE", "Default to false")
        self.env.SetValue("RUN_TESTS", "FALSE", "Default to false")
        self.env.SetValue("SHUTDOWN_AFTER_RUN", "FALSE", "Default to false")
        # needed to make FV size build report happy
        # self.env.SetValue("BLD_*_BUILDID_STRING", "Unknown", "Default")
        # # Default turn on build reporting.
        self.env.SetValue("BUILDREPORTING", "TRUE", "Enabling build report")
        self.env.SetValue("BUILDREPORT_TYPES", "PCD DEPEX FLASH BUILD_FLAGS LIBRARY FIXED_ADDRESS HASH", "Setting build report types")
        # Include the MFCI test cert by default, override on the commandline with "BLD_*_SHIP_MODE=TRUE" if you want the retail MFCI cert
        self.env.SetValue("BLD_*_SHIP_MODE", "FALSE", "Default")
        # Experimental Windows 26200 scheduler containment.  Keep the normal
        # heterogeneous MADT efficiency classes unless the build explicitly
        # supplies BLD_*_NTASI_T6020_J414S_HOMOGENEOUS_EFFICIENCY=1.
        self.env.SetValue(
            "BLD_*_NTASI_T6020_J414S_HOMOGENEOUS_EFFICIENCY",
            "1",
            "Default",
        )
        self.env.SetValue(
            "BLD_*_NTASI_ENABLE_ANS",
            profile_values[profile]["ans"],
            "Selected by NTASI_MU_PROFILE",
        )
        self.env.SetValue(
            "BLD_*_NTASI_J414S_GPU_RESOURCE_PROFILE",
            profile_values[profile]["gpu"],
            "Selected by NTASI_MU_PROFILE",
        )
        # CORRECTED 2026-07-30: wireless used to require a same-instance,
        # hardware-captured handoff manifest so this build could bake an
        # exact reservation base/size/limit into PatchPcd overrides -- the
        # coordinator's own hand-picked 0x103e0000000 test address, sealed
        # after the fact. The end user asked "wouldn't that be hard coding
        # it?" and was right: MemoryInitPeiLib.c now derives the reservation
        # at PEI runtime from that boot's own boot_args (mirroring how
        # SystemMemoryTop is already computed), so there is nothing left for
        # a build-time manifest to bake. PcdAppleWirelessDartPageTableBase/
        # Size are PatchableInModule and simply keep their AppleSiliconPkg.dec
        # default of 0 in every build produced by this script; only a live
        # boot's PEI phase ever writes a nonzero value. Enabling the
        # NTASI_ENABLE_WIRELESS_DART_HANDOFF code paths is now a pure
        # source-flag decision, exactly like NTASI_ENABLE_ANS and
        # NTASI_J414S_GPU_RESOURCE_PROFILE above.
        self.env.SetValue(
            "BLD_*_NTASI_ENABLE_WIRELESS_DART_HANDOFF",
            profile_values[profile]["wireless"],
            "Selected by NTASI_MU_PROFILE",
        )

        return 0

    def PlatformPreBuild(self):
        return 0

    def PlatformPostBuild(self):
        return 0

    def FlashRomImage(self):
        return 0

if __name__ == "__main__":
    import argparse
    import sys
    from edk2toolext.invocables.edk2_update import Edk2Update
    from edk2toolext.invocables.edk2_setup import Edk2PlatformSetup
    from edk2toolext.invocables.edk2_platform_build import Edk2PlatformBuild
    print("Invoking Stuart")
    print("     ) _     _")
    print("    ( (^)-~-(^)")
    print("__,-.\_( 0 0 )__,-.___")
    print("  'W'   \   /   'W'")
    print("         >o<")
    SCRIPT_PATH = os.path.relpath(__file__)
    parser = argparse.ArgumentParser(add_help=False)
    parse_group = parser.add_mutually_exclusive_group()
    parse_group.add_argument("--update", "--UPDATE",
                             action='store_true', help="Invokes stuart_update")
    parse_group.add_argument("--setup", "--SETUP",
                             action='store_true', help="Invokes stuart_setup")
    args, remaining = parser.parse_known_args()
    new_args = ["stuart", "-c", SCRIPT_PATH]
    new_args = new_args + remaining
    sys.argv = new_args
    if args.setup:
        print("Running stuart_setup -c " + SCRIPT_PATH)
        Edk2PlatformSetup().Invoke()
    elif args.update:
        print("Running stuart_update -c " + SCRIPT_PATH)
        Edk2Update().Invoke()
    else:
        print("Running stuart_build -c " + SCRIPT_PATH)
        Edk2PlatformBuild().Invoke()

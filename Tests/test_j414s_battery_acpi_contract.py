# SPDX-License-Identifier: MIT
"""Contract tests for the J414s battery devnode, BAT0 (NTAS0053).

Windows will not show a battery unless something in the ACPI namespace gives
its driver a devnode to bind to.  On this machine the battery data lives in the
SMC, which AML cannot read, so firmware's job is narrow and precise: publish a
vendor devnode that claims *nothing*, and let AppleSmcBattery.sys (a battc.sys
miniport) do the reading over the SMC transport that SMCG already owns.

"Claims nothing" is the whole design, so it is what these tests pin:

  - an EMPTY _CRS.  No memory window, because SMCG (NTAS0052) holds the SMC ASC
    and SRAM ranges as exclusive claims and re-claiming either is the
    CM_PROB_NORMAL_CONFLICT (Code 12) class this tree has already hit twice.
  - ZERO interrupts, so no GSIV is allocated.  AIC2 GSIVs above 1019 need a
    CSRT ALI2 alias and a published-GSIV collision is an active suspect in an
    unrelated boot failure; this feature must not add to that surface.
  - no write path.  Battery reporting needs READ_KEY and nothing else, and the
    charge-control keys change real power hardware.

Plus the usual profile-gate discipline: default off must mean byte-identical
firmware, and the two independent profile dicts must not be able to drift (on
2026-07-31 changing only one produced a manifest that lied about the binary).
"""

from __future__ import annotations

import importlib.util
import re
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
ACPI_PLATFORM = (
    REPO
    / "Silicon"
    / "Apple"
    / "AppleSiliconPkg"
    / "Drivers"
    / "AcpiPlatformDxe"
    / "AcpiPlatform.c"
)
DSC = REPO / "Platform" / "MacBookProEarly2023Pkg" / "MacBookProEarly2023.dsc"
PLATFORM_BUILD = (
    REPO / "Platform" / "MacBookProEarly2023Pkg" / "PlatformBuild.py"
)
BUILDER = REPO / "Tools" / "build-j414s-windows-native.sh"
SMCG_ASL = (
    REPO / "Platform" / "MacBookProEarly2023Pkg" / "AcpiTables" / "SMCG.asl"
)

MODULE_PATH = REPO / "Tools" / "j414s_mu_profile_manifest.py"
SPEC = importlib.util.spec_from_file_location("j414s_mu_profile_manifest", MODULE_PATH)
M = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(M)

HID = "NTAS0053"
DEVICE = "BAT0"
FLAG = "NTASI_ENABLE_BATTERY_PUBLICATION"

# SMCG's two exclusive _CRS claims (SMCG.asl).  Neither may reappear anywhere
# in the battery generator.
SMC_ASC_BASE = 0x2A2400000
SMC_SRAM_BASE = 0x2A3E00000

# Every SMC key that changes power hardware, plus the notification-arming key.
# None may appear in AcpiPlatform.c's battery block: firmware must not be able
# to hand the driver a key it is forbidden to write.
FORBIDDEN_SMC_KEYS = (
    "CH0I", "CH0C", "CHTE", "CH0B", "CH0K", "CHWA", "CHLS", "NTAP",
)


def battery_block() -> str:
    """The text between the battery generator's #if and its #endif."""
    text = ACPI_PLATFORM.read_text(encoding="utf-8")
    start = text.index(f"#if {FLAG}")
    end = text.index(f"#endif // {FLAG}", start)
    return text[start:end]


def strip_comments(source: str) -> str:
    """Drop // and /* */ comments.

    Needed because the block deliberately *names* the forbidden keys in prose
    to explain why they are absent; only the code may be searched for them.
    """
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", source)


class BatteryClaimsNothing(unittest.TestCase):
    def test_crs_is_an_empty_resource_template(self):
        block = strip_comments(battery_block())
        # The _CRS node is created and then nothing is ever added to it.
        self.assertIn('AmlCodeGenNameResourceTemplate ("_CRS"', block)
        self.assertNotIn("AppleAnsAddMemoryResource", block)
        self.assertNotIn("AmlCodeGenRdQWordMemory", block)
        self.assertNotIn("AmlCodeGenRdInterrupt", block)

    def test_no_gsiv_is_allocated(self):
        block = strip_comments(battery_block())
        # Any interrupt at all would need one of these, and would need adding
        # to tools/verify-j414s-gsiv-allocation.py's allocation set.
        for token in ("Interrupt", "Irq", "GSIV", "Gsiv"):
            with self.subTest(token=token):
                self.assertNotIn(f"AmlCodeGenRd{token}", block)
        self.assertIn('{ "ntasp,battery-interrupt-count",   0     }', block)

    def test_smcg_windows_are_never_re_claimed(self):
        block = strip_comments(battery_block())
        for base in (SMC_ASC_BASE, SMC_SRAM_BASE):
            with self.subTest(base=hex(base)):
                self.assertNotIn(f"{base:X}", block.upper())
        self.assertIn('{ "ntasp,battery-memory-windows",    0     }', block)

    def test_smcg_still_owns_the_smc_windows_exclusively(self):
        # The premise of the empty _CRS: if SMCG ever stopped claiming these,
        # this test should fail loudly rather than let the battery silently
        # become the only claimant of a window it does not map.
        asl = SMCG_ASL.read_text(encoding="utf-8")
        self.assertIn(f"0x0000000{SMC_ASC_BASE:X}", asl)
        self.assertIn(f"0x0000000{SMC_SRAM_BASE:X}", asl)


class BatteryIsReadOnly(unittest.TestCase):
    def test_no_charge_control_key_appears_in_firmware(self):
        block = strip_comments(battery_block())
        for key in FORBIDDEN_SMC_KEYS:
            with self.subTest(key=key):
                self.assertNotIn(key, block)

    def test_write_gate_is_published_closed(self):
        block = battery_block()
        self.assertIn('{ "ntasp,battery-write-keys-allowed", 0    }', block)

    def test_manifest_records_the_write_gate_closed_in_every_profile(self):
        for profile in M.PROFILES:
            with self.subTest(profile=profile):
                features = M.profile_policy(profile)["experimental_features"]
                self.assertIs(features["battery_smc_write_enabled"], False)


class BatteryDeviceShape(unittest.TestCase):
    def test_device_identity(self):
        block = battery_block()
        self.assertIn(f'AmlCodeGenDevice ("{DEVICE}"', block)
        self.assertIn(f'AmlCodeGenNameString ("_HID", "{HID}"', block)
        self.assertIn('AmlCodeGenNameInteger ("_UID", 0', block)
        self.assertIn('AmlCodeGenNameInteger ("_STA", 0x0F', block)
        # Windows ARM64 fails device start on an absent _CCA; SMCG publishes
        # Zero and this device matches it.
        self.assertIn('AmlCodeGenNameInteger ("_CCA", 0', block)
        self.assertIn('AmlCodeGenScope ("\\\\_SB_"', block)

    def test_hid_is_not_already_taken(self):
        # NTAS0053 was free when this landed; every other NTASP id in the tree
        # belongs to a different device.  A duplicate _HID would make two
        # drivers race for one devnode.
        text = ACPI_PLATFORM.read_text(encoding="utf-8")
        self.assertEqual(text.count(f'"{HID}"'), 1)
        acpi_tables = REPO / "Platform" / "MacBookProEarly2023Pkg" / "AcpiTables"
        for asl in acpi_tables.rglob("*.asl"):
            with self.subTest(asl=asl.name):
                self.assertNotIn(HID, asl.read_text(encoding="utf-8"))

    def test_dsd_is_data_and_carries_the_driver_cross_check(self):
        block = battery_block()
        self.assertIn('AmlCodeGenNamePackage ("_DSD"', block)
        self.assertIn("AmlAddDeviceDataDescriptorPackage", block)
        # The properties a driver can disagree with firmware about.
        for prop in (
            "ntasp,battery-index",
            "ntasp,smc-transport-owner-hid",
            "ntasp,smc-rtkit-endpoint",
            "ntasp,battery-nominal-cell-mv",
            "ntasp,battery-poll-idle-ms",
            "ntasp,battery-poll-active-ms",
            "ntasp,battery-poll-low-ms",
        ):
            with self.subTest(prop=prop):
                self.assertIn(prop, block)

    def test_energy_scale_and_endpoint_agree_with_smcg_and_the_driver(self):
        block = battery_block()
        # 3800 mV/cell is macsmc-power.c:30; the Windows decoder duplicates it
        # as NTASI_SMC_NOMINAL_CELL_VOLTAGE_MV.
        self.assertIn('{ "ntasp,battery-nominal-cell-mv",   3800  }', block)
        # The SMC's RTKit endpoint, the same value SMCG publishes.
        self.assertIn('{ "ntasp,smc-rtkit-endpoint",        0x20  }', block)
        self.assertIn('"ntasp,smc-rtkit-endpoint", 0x20', SMCG_ASL.read_text(encoding="utf-8"))
        # 0x52 names NTAS0052 = \_SB.SMCG, the devnode that owns the mailbox.
        self.assertIn('{ "ntasp,smc-transport-owner-hid",   0x52  }', block)

    def test_publication_is_non_fatal(self):
        # Only ANS may abort the boot.  A firmware bug here must never take
        # down a boot that would otherwise reach Windows.
        text = ACPI_PLATFORM.read_text(encoding="utf-8")
        call = text.index("NtasiInstallBatteryTable (AcpiTable);")
        tail = text[call:call + 400]
        self.assertIn("DEBUG_ERROR", tail)
        self.assertNotIn("return EFI_ABORTED", tail)

    def test_generator_does_not_depend_on_the_media_block(self):
        # media and battery are independent switches; the battery build must
        # not reach into a type or table defined inside #if MEDIA.
        block = strip_comments(battery_block())
        self.assertIn("NTASI_BATTERY_PROPERTY", block)
        self.assertNotIn("NTASI_MEDIA", block)


class BatteryProfileGate(unittest.TestCase):
    def test_only_the_battery_profile_publishes_the_battery(self):
        for profile in M.PROFILES:
            with self.subTest(profile=profile):
                features = M.profile_policy(profile)["experimental_features"]
                expected = bool(M.PROFILES[profile]["battery"])
                self.assertIs(features["battery_publication"], expected)
                self.assertEqual(
                    features["battery_acpi_devices"], [HID] if expected else []
                )

    def test_no_profile_ever_publishes_a_battery_gsiv(self):
        for profile in M.PROFILES:
            with self.subTest(profile=profile):
                features = M.profile_policy(profile)["experimental_features"]
                self.assertEqual(features["battery_published_gsivs"], [])
                self.assertEqual(features["battery_memory_windows"], 0)

    def test_battery_adds_no_ffs_module(self):
        # The SSDT is generated at DXE runtime inside the always-built
        # AcpiPlatformDxe, so the FV inventory is unchanged.
        self.assertEqual(
            M.PROFILES["battery"]["expected_ffs_count"],
            M.PROFILES["baseline"]["expected_ffs_count"],
        )

    def test_battery_leaves_the_csrt_untouched(self):
        # Publishing no interrupt means no ALI2 alias, so the battery profile
        # must carry the ordinary 3-alias table, byte-for-byte as baseline.
        battery = M.profile_policy("battery")["experimental_features"]
        baseline = M.profile_policy("baseline")["experimental_features"]
        self.assertEqual(battery["csrt_variant"], baseline["csrt_variant"])
        self.assertEqual(
            battery["csrt_ali2_alias_count"], baseline["csrt_ali2_alias_count"]
        )

    def test_battery_is_a_single_variable_on_top_of_baseline(self):
        battery = M.PROFILES["battery"]
        for other in ("ans", "ans_acpi", "gpu", "gpu_acpi", "wireless", "media"):
            with self.subTest(feature=other):
                self.assertFalse(battery[other])

    def test_profile_abi_is_distinct_and_correctly_namespaced(self):
        abis = [entry["profile_abi"] for entry in M.PROFILES.values()]
        self.assertEqual(len(abis), len(set(abis)))
        self.assertEqual(
            M.PROFILES["battery"]["profile_abi"],
            "ntasi.j414s.windows.battery-publication.v1",
        )

    def test_generator_is_preprocessor_gated(self):
        # Default OFF has to mean byte-identical firmware, not merely
        # equivalent behaviour.
        text = ACPI_PLATFORM.read_text(encoding="utf-8")
        self.assertIn(f"#if {FLAG}", text)
        self.assertIn(f"#endif // {FLAG}", text)
        self.assertIn(
            f"-D{FLAG}=$({FLAG})", DSC.read_text(encoding="utf-8")
        )
        self.assertIn(f"DEFINE {FLAG} = 0", DSC.read_text(encoding="utf-8"))

    def test_expected_defines_track_the_profile_table(self):
        source = MODULE_PATH.read_text(encoding="utf-8")
        self.assertIn(
            f'"{FLAG}": "1" if PROFILES[profile]["battery"] else "0"', source
        )


class BatteryBuilderAgreesWithTheManifest(unittest.TestCase):
    """The two profile dicts are independent; only one reaches the compiler.

    Changing the manifest alone produces a manifest that lies about the binary
    (measured 2026-07-31), which is why this cross-check exists at all.
    """

    def _builder_profile_values(self) -> dict[str, dict[str, str]]:
        text = PLATFORM_BUILD.read_text(encoding="utf-8")
        start = text.index("profile_values = {")
        end = text.index("\n        }", start)
        body = text[start:end]
        return {
            name: dict(
                re.findall(r'"([a-z_]+)": "([A-Za-z01]+)"', entry)
            )
            for name, entry in re.findall(
                r'"([a-z0-9-]+)": \{([^}]*)\}', body
            )
        }

    def test_every_manifest_profile_is_buildable(self):
        builder = self._builder_profile_values()
        for profile in M.PROFILES:
            with self.subTest(profile=profile):
                self.assertIn(profile, builder)

    def test_the_battery_flag_reaches_the_compiler(self):
        builder = self._builder_profile_values()
        self.assertEqual(builder["battery"].get("battery"), "1")
        text = PLATFORM_BUILD.read_text(encoding="utf-8")
        self.assertIn(f'"BLD_*_{FLAG}"', text)
        self.assertIn('profile_values[profile]["battery"]', text)
        # Defaulted off rather than repeated, so a profile added later cannot
        # inherit a battery publication by omission.
        self.assertIn('values.setdefault("battery", "0")', text)

    def test_the_wrapper_script_accepts_the_profile(self):
        text = BUILDER.read_text(encoding="utf-8")
        self.assertIn('profile manifest has no PROFILES table', text)
        self.assertIn('if sys.argv[2] not in profiles', text)


if __name__ == "__main__":
    unittest.main()

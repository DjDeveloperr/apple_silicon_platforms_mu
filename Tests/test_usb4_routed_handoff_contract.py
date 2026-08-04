"""Static contract for the ROUTED (USB4/Thunderbolt) deferred PIPE switch.

Companion to `test_usb3_deferred_handoff_contract.py`, and deliberately a
separate file: the direct-USB3 path is working today and carries this machine's
Ethernet and SSH, so the rule this suite exists to enforce is that the routed
addition can never reach a USB3-owned port, and can never park a live routed
mux back on DUMMY.

Everything here is static source/manifest parsing. Nothing touches hardware, and
nothing asserts that a tunnel works -- only that the gates are shaped so a wrong
transport cannot be switched. `TUNNEL_READY` already taught this project that a
phase our own code sets is not evidence of a device; the same discipline applies
to a mux value.
"""
from __future__ import annotations

import importlib.util
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
DRIVER = ROOT / (
    "Silicon/Apple/AppleSiliconPkg/Drivers/AppleUsbTypeCBringupDxe/"
    "AppleUsbTypeCBringupDxe.c"
)
DRIVER_INF = ROOT / (
    "Silicon/Apple/AppleSiliconPkg/Drivers/AppleUsbTypeCBringupDxe/"
    "AppleUsbTypeCBringupDxe.inf"
)
PACKAGE_DEC = ROOT / "Silicon/Apple/AppleSiliconPkg/AppleSiliconPkg.dec"
PLATFORM_DSC = ROOT / "Platform/MacBookProEarly2023Pkg/MacBookProEarly2023.dsc"
MANIFEST_MODULE = ROOT / "Tools/j414s_mu_profile_manifest.py"

# m1n1 owns the mux encoding; parse it rather than transcribe it, so the two
# repositories cannot drift silently.
M1N1_ATCPHY_HEADER = ROOT.parent / "m1n1" / "src" / "atcphy_core.h"

SPEC = importlib.util.spec_from_file_location("j414s_mu_profile_manifest_usb4", MANIFEST_MODULE)
MANIFEST = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MANIFEST)

PCD = "PcdAppleUsb4RoutedPipeSwitchPortMask"


def function_body(source: str, signature: str) -> str:
    """Extract one C function body by brace matching from its signature."""
    start = source.index(signature)
    open_brace = source.index("{", start)
    depth = 0
    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unbalanced braces after {signature!r}")


class RoutedGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.source = DRIVER.read_text(encoding="utf-8")
        self.routed = function_body(self.source, "STATIC VOID AtcPhyFinishDeferredUsb4Switch")
        self.switch = function_body(self.source, "STATIC EFI_STATUS AtcPhyPipeSwitchToUsb4Routed")
        self.usb3 = function_body(self.source, "STATIC VOID AtcPhyFinishDeferredUsb3Switch")

    def test_the_routed_switch_is_gated_on_its_own_pcd(self):
        """A different token from the USB3 mask, checked before anything else."""
        self.assertIn(f"PcdGet32({PCD})", self.routed)
        # And the USB3 finisher must NOT have grown a dependency on it.
        self.assertNotIn(PCD, self.usb3)

    def test_the_routed_gate_requires_usb4_crossbarred_lanes(self):
        """POWER_CTRL + DUMMY mux is byte-identical between a USB3 deferred
        prepare and a routed prepare. Only the crossbar tells them apart."""
        self.assertIn("ATCPHY_CROSSBAR_PROTOCOL_USB4", self.routed)
        self.assertIn("ATCPHY_CROSSBAR_PROTOCOL_USB4_SWAPPED", self.routed)
        self.assertIn("ATCPHY_CORE_POWER_APB_RESET_N", self.routed)
        self.assertIn("ATCPHY_CORE_POWER_PHY_RESET_N", self.routed)

    def test_the_two_crossbar_whitelists_are_disjoint(self):
        """The routed gate must never accept USB3 lanes, and vice versa."""
        self.assertNotIn("ATCPHY_CROSSBAR_PROTOCOL_USB3_DP", self.routed)
        self.assertNotIn("ATCPHY_CROSSBAR_PROTOCOL_USB4", self.usb3.replace(
            "ATCPHY_CROSSBAR_PROTOCOL_USB4", "", 0))
        # The USB3 finisher still whitelists exactly USB3_DP / USB3_DP_SWAPPED.
        self.assertIn("ATCPHY_CROSSBAR_PROTOCOL_USB3_DP", self.usb3)
        self.assertIn("ATCPHY_CROSSBAR_PROTOCOL_USB3_DP_SWAPPED", self.usb3)

    def test_the_routed_gate_never_parks_a_live_mux_on_dummy(self):
        """This is the regression the routed path exists to prevent.

        Before it existed, the only Mu code that read the mux was the USB3
        finisher, whose contract is "not DUMMY means ambiguous, park DUMMY". A
        port m1n1 had correctly committed to 0x11 would have been silently reset
        to USB2 here. The routed finisher must therefore never call the park.
        """
        self.assertNotIn("AtcPhyPipeParkDummy", self.routed)
        self.assertNotIn("AtcPhyPipeParkDummy", self.switch)

    def test_the_routed_gate_accepts_both_handoff_shapes(self):
        """DUMMY (m1n1 deferred) and already-0x11 (m1n1 committed, re-apply
        after dwc3 core init, which resets the PIPE)."""
        self.assertIn("ATCPHY_PIPEHANDLER_MUX_VALUE_DUMMY", self.routed)
        self.assertIn("ATCPHY_PIPEHANDLER_MUX_VALUE_USB4_ROUTED", self.routed)

    def test_the_switch_verifies_by_readback_not_by_assumption(self):
        """A mux write that ACKs is not a mux that switched."""
        self.assertRegex(
            self.switch,
            r"MuxCtrl\s*=\s*MmioRead32\([^;]*ATCPHY_PIPEHANDLER_MUX_CTRL\);",
        )
        self.assertIn("MuxCtrl != ATCPHY_PIPEHANDLER_MUX_VALUE_USB4_ROUTED", self.switch)
        self.assertIn("EFI_DEVICE_ERROR", self.switch)

    def test_the_routed_switch_omits_the_bist_dance_and_nonselected_write(self):
        """Apple's setUSB3Mode USB4 branch does neither: the producer is the
        already-running ACIO router, not the native USB3 PHY."""
        self.assertNotIn("ATCPHY_PIPEHANDLER_NONSELECTED_OVERRIDE", self.switch)
        self.assertNotIn("BIST", self.switch)
        # The USB3 path keeps both, unchanged.
        usb3_switch = function_body(self.source, "STATIC EFI_STATUS AtcPhyPipeSwitchToUsb3")
        self.assertIn("ATCPHY_PIPEHANDLER_NONSELECTED_OVERRIDE", usb3_switch)
        self.assertIn("BIST", usb3_switch)

    def test_the_routed_switch_uses_apples_six_millisecond_lock_budget(self):
        """atc.c's 1 ms is the dummy/USB3 value; the routed path is a separate,
        longer Apple budget and must not borrow the USB3 constant."""
        self.assertIn("ATCPHY_PIPEHANDLER_LOCK_ROUTED_TIMEOUT_US", self.switch)
        self.assertNotIn("ATCPHY_PIPEHANDLER_LOCK_TIMEOUT_US,", self.switch)
        match = re.search(
            r"#define\s+ATCPHY_PIPEHANDLER_LOCK_ROUTED_TIMEOUT_US\s+(\d+)", self.source
        )
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1)), 6000)

    def test_the_switch_orders_clk_off_then_data_then_clk(self):
        order = [
            self.switch.index("ATCPHY_PIPEHANDLER_MUX_CLK_OFF"),
            self.switch.index("ATCPHY_PIPEHANDLER_MUX_DATA_USB4"),
            self.switch.index("ATCPHY_PIPEHANDLER_MUX_CLK_USB4"),
        ]
        self.assertEqual(order, sorted(order))

    def test_it_runs_after_core_init_in_the_same_window_as_usb3(self):
        """Asahi's ordering rule: the PIPE switch happens after dwc3 core init
        and before xhci binds."""
        # Anchor on the CALL sites, not on prose: the surrounding comment names
        # RegisterNonDiscoverableMmioDevice too, and matching that made this
        # assertion pass or fail on comment wording rather than on the order the
        # code actually runs in.
        usb3_call = self.source.index("    AtcPhyFinishDeferredUsb3Switch(\n")
        usb4_call = self.source.index("    AtcPhyFinishDeferredUsb4Switch(\n")
        register = self.source.index("Status = RegisterNonDiscoverableMmioDevice(")
        self.assertLess(usb3_call, usb4_call)
        self.assertLess(usb4_call, register)
        self.assertIn("Dwc3AppleSetupCio", self.routed)
        self.assertIn("Dwc3EnableSusphy", self.routed)


class MuxEncodingAgreesWithM1n1Tests(unittest.TestCase):
    """The two repositories must not drift on what 0x11 and 0x22 mean."""

    def test_mux_values_match_m1n1s_header(self):
        if not M1N1_ATCPHY_HEADER.is_file():
            self.skipTest("m1n1 checkout not adjacent")
        m1n1 = M1N1_ATCPHY_HEADER.read_text(encoding="utf-8")
        mu = DRIVER.read_text(encoding="utf-8")

        def define(source: str, name: str) -> int:
            match = re.search(rf"#define\s+{name}\s+(0x[0-9A-Fa-f]+)", source)
            assert match, f"{name} not found"
            return int(match.group(1), 0)

        self.assertEqual(
            define(mu, "ATCPHY_PIPEHANDLER_MUX_VALUE_USB4_ROUTED"),
            define(m1n1, "ATCPHY_PIPEHANDLER_MUX_VALUE_USB4_TUNNEL"),
        )
        self.assertEqual(
            define(mu, "ATCPHY_PIPEHANDLER_MUX_VALUE_DUMMY"),
            define(m1n1, "ATCPHY_PIPEHANDLER_MUX_VALUE_DUMMY"),
        )
        self.assertEqual(define(mu, "ATCPHY_PIPEHANDLER_MUX_VALUE_USB4_ROUTED"), 0x11)


class PcdPlumbingTests(unittest.TestCase):
    def test_the_pcd_is_declared_defaulted_off_and_wired(self):
        dec = PACKAGE_DEC.read_text(encoding="utf-8")
        self.assertRegex(dec, rf"{PCD}\|0x00000000\|UINT32\|")
        self.assertIn(PCD, DRIVER_INF.read_text(encoding="utf-8"))
        self.assertRegex(
            PLATFORM_DSC.read_text(encoding="utf-8"),
            rf"{PCD}\s*\|\s*\$\(NTASI_USB4_ROUTED_PIPE_SWITCH_PORT_MASK\)",
        )

    def test_no_profile_claims_a_port_for_both_transports(self):
        """Direct USB3 (0x08) and routed USB4 (0x11) are different values of the
        SAME mux. An overlap would have the two finishers fight, and the loser
        parks the port on DUMMY."""
        for name, profile in MANIFEST.PROFILES.items():
            with self.subTest(profile=name):
                self.assertEqual(
                    profile["usb3_pipe_switch_port_mask"]
                    & profile["usb4_routed_pipe_switch_port_mask"],
                    0,
                )

    def test_every_shipped_profile_defaults_the_routed_mask_off(self):
        """Only a profile that explicitly opts in may switch a routed mux."""
        for name, profile in MANIFEST.PROFILES.items():
            with self.subTest(profile=name):
                if name != "internal-storage-usb4":
                    self.assertEqual(profile["usb4_routed_pipe_switch_port_mask"], 0)

    def test_the_usb4_profile_matches_internal_storage_except_for_the_mask(self):
        """The known-good boot profile's policy must be preserved exactly; the
        only difference is the routed opt-in."""
        base = MANIFEST.PROFILES["internal-storage"]
        usb4 = MANIFEST.PROFILES["internal-storage-usb4"]
        self.assertEqual(usb4["usb4_routed_pipe_switch_port_mask"], 0x2)
        self.assertEqual(base["usb4_routed_pipe_switch_port_mask"], 0x0)
        # The right-port direct-USB3 link carries Ethernet and SSH; it must not move.
        self.assertEqual(
            usb4["usb3_pipe_switch_port_mask"], base["usb3_pipe_switch_port_mask"]
        )
        for key in ("ans", "ans_acpi", "ans_dxe", "ans_block_io", "ans_preserve",
                    "gpu", "wireless", "xhc2", "expected_ffs_count"):
            self.assertEqual(usb4[key], base[key], key)

    def test_the_manifest_records_the_routed_mask_as_evidence(self):
        source = MANIFEST_MODULE.read_text(encoding="utf-8")
        self.assertIn('"usb4_routed_pipe_switch_port_mask"', source)
        self.assertIn(f'"{PCD}"', source)


if __name__ == "__main__":
    unittest.main()

"""Host-side contract tests for the split m1n1 -> Mu USB3 handoff."""

from pathlib import Path
import importlib.util
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / (
    "Silicon/Apple/AppleSiliconPkg/Drivers/AppleUsbTypeCBringupDxe/"
    "AppleUsbTypeCBringupDxe.c"
)
PLATFORM_DSC = ROOT / "Platform/MacBookProEarly2023Pkg/MacBookProEarly2023.dsc"
PLATFORM_BUILD = ROOT / "Platform/MacBookProEarly2023Pkg/PlatformBuild.py"
MANIFEST_MODULE = ROOT / "Tools/j414s_mu_profile_manifest.py"
SPEC = importlib.util.spec_from_file_location("j414s_mu_profile_manifest_usb3", MANIFEST_MODULE)
MANIFEST = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MANIFEST)


def function_body(source: str, name: str) -> str:
    """Return a C function body, balancing braces instead of relying on regex."""
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"function {name} not found")

    start = source.index("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1 : index]
    raise AssertionError(f"unterminated function {name}")


class DeferredUsb3HandoffContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.driver = DRIVER.read_text(encoding="utf-8")
        cls.dsc = PLATFORM_DSC.read_text(encoding="utf-8")

    def test_profile_seals_the_exact_single_or_dual_pipe_mask(self) -> None:
        self.assertRegex(
            self.dsc,
            r"PcdAppleUsb3PipeSwitchPortMask\s*\|\s*\$\(NTASI_USB3_PIPE_SWITCH_PORT_MASK\)",
        )

        for name, profile in MANIFEST.PROFILES.items():
            with self.subTest(profile=name):
                expected = (
                    0x6 if name.endswith("-usb3-dual")
                    else 0x4 if profile["xhc2"]
                    else 0x2
                )
                self.assertEqual(profile["usb3_pipe_switch_port_mask"], expected)
                self.assertEqual(
                    MANIFEST.profile_policy(name)["baseline_capabilities"][
                        "usb3_deferred_pipe_switch_port_mask"
                    ],
                    expected,
                )

    def test_builder_exports_profile_mask_to_the_dsc(self) -> None:
        source = PLATFORM_BUILD.read_text(encoding="utf-8")
        self.assertRegex(
            source,
            r'"usb3_pipe_mask",\s*"0x4" if values\["xhc2"\] == "1" else "0x2"',
        )
        self.assertIn("BLD_*_NTASI_USB3_PIPE_SWITCH_PORT_MASK", source)
        self.assertIn('profile_values[profile]["usb3_pipe_mask"]', source)

    def test_switch_happens_after_dwc3_init_and_before_xhci_registration(self) -> None:
        callback = function_body(self.driver, "AppleUsbTypeCBringupDxeBringupCallback")
        calls = (
            "Status = AppleUsbTypeCBringupDxeInitializeUsbController",
            "AtcPhyFinishDeferredUsb3Switch(",
            "Status = RegisterNonDiscoverableMmioDevice(",
        )
        positions = tuple(callback.index(call) for call in calls)
        self.assertEqual(positions, tuple(sorted(positions)))

    def test_asahi_device_reset_precedes_host_mode_without_phy_resets(self) -> None:
        core = function_body(self.driver, "Dwc3XhciCoreInit")
        self.assertLess(
            core.index("Dwc3DisableSusphyForCoreInit(Controller)"),
            core.index("Dwc3DeviceSideSoftReset(Controller)"),
        )
        self.assertIn("Dwc3DeviceSideSoftReset(Controller)", core)
        self.assertNotRegex(
            self.driver, r"STATIC\s+VOID\s+Dwc3ControllerSoftReset\s*\("
        )
        self.assertNotIn("DWC3_GCTL_CORESOFTRESET", core)
        self.assertNotIn("DWC3_GUSB3PIPECTL_PHYSOFTRST", core)
        self.assertNotIn("DWC3_GUSB2PHYCFG_PHYSOFTRST", core)

        initialize = function_body(
            self.driver, "AppleUsbTypeCBringupDxeInitializeUsbController"
        )
        self.assertLess(
            initialize.index("AtcPhyPowerOnUsb2AfterDwc3Release"),
            initialize.index("Dwc3XhciCoreInit"),
        )
        self.assertLess(
            initialize.index("Dwc3XhciCoreInit"),
            initialize.index("Dwc3SetMode(Dwc3Controller, DWC3_GCTL_PRTCAP_HOST)"),
        )

        reset = function_body(self.driver, "Dwc3DeviceSideSoftReset")
        required = (
            "Dctl = MmioRead32",
            "Dctl |= DWC3_DCTL_CSFTRST",
            "Dctl &= ~(UINT32)(DWC3_DCTL_RUN_STOP | DWC3_DCTL_ULSTCHNGREQ_MASK)",
            "MmioWrite32",
            "DWC3_DCTL_RESET_RETRIES",
            "MicroSecondDelay(DWC3_DCTL_RESET_POLL_US)",
        )
        positions = tuple(reset.index(token) for token in required)
        self.assertEqual(positions, tuple(sorted(positions)))
        self.assertNotIn("DWC3_GCTL_CORESOFTRESET", reset)
        self.assertNotIn("DWC3_GUSB3PIPECTL_PHYSOFTRST", reset)
        self.assertNotIn("DWC3_GUSB2PHYCFG_PHYSOFTRST", reset)

        disable = function_body(self.driver, "Dwc3DisableSusphyForCoreInit")
        self.assertIn("DWC3_GUSB3PIPECTL_SUSPHY", disable)
        self.assertIn("DWC3_GUSB2PHYCFG_SUSPHY", disable)
        self.assertIn("Usb3Before", disable)
        self.assertIn("Usb3After", disable)
        self.assertIn("Usb2Before", disable)
        self.assertIn("Usb2After", disable)

        finish = function_body(self.driver, "AtcPhyFinishDeferredUsb3Switch")
        self.assertLess(
            finish.index("Dwc3EnableSusphy(Dwc3Controller)"),
            finish.index("AtcPhyPipeSwitchToUsb3"),
        )
        enable = function_body(self.driver, "Dwc3EnableSusphy")
        self.assertIn("Usb3Before", enable)
        self.assertIn("Usb2Before", enable)

    def test_dwc3_release_is_held_until_both_darts_are_in_bypass(self) -> None:
        initialize = function_body(
            self.driver, "AppleUsbTypeCBringupDxeInitializeUsbController"
        )
        self.assertLess(
            initialize.index("AtcPhyReleaseDwc3AfterDart"),
            initialize.index("AtcPhyPowerOnUsb2AfterDwc3Release"),
        )
        self.assertLess(
            initialize.index("AtcPhyPowerOnUsb2AfterDwc3Release"),
            initialize.index("Dwc3XhciCoreInit"),
        )
        self.assertIn("AtcPhyHoldDwc3Reset(PipeHandler)", initialize)

        release = function_body(self.driver, "AtcPhyReleaseDwc3AfterDart")
        required = (
            "UsbDartVerifyControllerBypass",
            "ATCPHY_PIPEHANDLER_AON_DWC3_FORCE_CLAMP_EN",
            "ATCPHY_PIPEHANDLER_AON_DWC3_RESET_N",
            "ATCPHY_PIPEHANDLER_MUX_CTRL",
            "MmioAnd32",
            "MmioOr32",
        )
        positions = tuple(release.index(token) for token in required)
        self.assertEqual(positions, tuple(sorted(positions)))

        dart = function_body(self.driver, "UsbDartVerifyControllerBypass")
        self.assertIn("USB_DART_REG_COUNT", dart)
        self.assertIn("Tcr != ExpectedTcr", dart)
        self.assertIn("return EFI_NOT_READY", dart)

        usb2 = function_body(self.driver, "AtcPhyPowerOnUsb2AfterDwc3Release")
        required_usb2 = (
            "ATCPHY_USB2PHY_SIG_VBUS",
            "ATCPHY_USB2PHY_CTL_SIDDQ",
            "ATCPHY_USB2PHY_CTL_RESET",
            "ATCPHY_USB2PHY_CTL_PORT_RESET",
            "ATCPHY_USB2PHY_CTL_APB_RESET_N",
            "ATCPHY_USB2PHY_MISCTUNE_APB_GATE_OFF",
            "ATCPHY_USB2PHY_USBCTL_RUN",
        )
        positions = tuple(usb2.index(token) for token in required_usb2)
        self.assertEqual(positions, tuple(sorted(positions)))
        self.assertIn("ATCPHY_USB2PHY_USBCTL_MODE_MASK", usb2)

    def test_manifest_seals_reset_held_dart_contract(self) -> None:
        for name in MANIFEST.PROFILES:
            with self.subTest(profile=name):
                self.assertEqual(
                    MANIFEST.profile_policy(name)["baseline_capabilities"][
                        "usb_dwc3_reset_dart_handoff"
                    ],
                    "m1n1_reset_clamped_mu_dart_bypass_release_v1",
                )

    def test_finish_is_opt_in_and_fails_closed_without_a_live_phy(self) -> None:
        finish = function_body(self.driver, "AtcPhyFinishDeferredUsb3Switch")
        required = (
            "PcdGet32(PcdAppleUsb3PipeSwitchPortMask)",
            "ATCPHY_CORE_POWER_APB_RESET_N",
            "ATCPHY_CORE_POWER_PHY_RESET_N",
            "ATCPHY_PIPEHANDLER_MUX_CTRL",
            "MuxCtrl !=",
            "Dwc3AppleSetupCio",
            "Dwc3EnableSusphy",
            "AtcPhyPipeSwitchToUsb3",
        )
        positions = tuple(finish.index(token) for token in required)
        self.assertEqual(positions, tuple(sorted(positions)))
        self.assertRegex(
            finish,
            r"if\s*\(\s*\(PowerCtrl\s*&.*?APB_RESET_N.*?PHY_RESET_N.*?\)\s*!=",
        )
        mismatch = finish.index("MuxCtrl !=")
        self.assertGreater(
            finish.index("AtcPhyPipeParkDummy((UINTN)PipeHandlerBase)", mismatch),
            mismatch,
        )

    def test_failed_mux_switch_parks_the_pipe_on_dummy(self) -> None:
        switch = function_body(self.driver, "AtcPhyPipeSwitchToUsb3")
        self.assertEqual(switch.count("return Status;"), 1)
        self.assertIn("goto Unlock;", switch)
        error = switch.index("Unlock:")
        park = switch.rindex("AtcPhyPipeParkDummy(PipeHandler)")
        self.assertGreater(park, error)
        self.assertGreater(
            switch.rindex("ATCPHY_PIPEHANDLER_OVERRIDE_RXDETECT"),
            error,
        )


if __name__ == "__main__":
    unittest.main()

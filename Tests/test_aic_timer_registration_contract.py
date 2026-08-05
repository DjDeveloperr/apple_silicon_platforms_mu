"""Host contract for the Mu/m1n1 native-AIC timer registration boundary."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
AIC_V2 = ROOT / (
    "Silicon/Apple/AppleSiliconPkg/Drivers/AppleAicDxe/AicV2/"
    "AppleAicV2Dxe.c"
)
TIMER_DXE = ROOT / "Silicon/ARM/TIANO/ArmPkg/Drivers/TimerDxe/TimerDxe.c"


def function_body(source: str, name: str) -> str:
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


@dataclass
class TimerRegistrationModel:
    timer_ticks: int = 0
    compare: int = 0
    current: int = 0x1606D36C
    reflected_pending: bool = True
    reflected_masked: bool = True
    reflection_gate_open: bool = False
    handler_calls: int = 0

    def prepare_source(self) -> None:
        self.reflected_pending = False
        self.reflected_masked = False

    def final_enable(self, timer_ticks: int) -> None:
        if timer_ticks <= 0:
            raise ValueError("TimerDxe must publish a nonzero period")
        self.timer_ticks = timer_ticks
        self.compare = self.current + timer_ticks
        self.reflection_gate_open = True

    def deliver_reflected_tick(self) -> int:
        if not self.reflection_gate_open or self.reflected_masked:
            return 0
        self.handler_calls += 1
        iterations = 0
        while self.compare < self.current:
            if self.timer_ticks == 0:
                raise RuntimeError("non-progressing TimerInterruptHandler loop")
            self.compare += self.timer_ticks
            iterations += 1
        return iterations


class AicTimerRegistrationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.aic = AIC_V2.read_text(encoding="utf-8")
        cls.timer = TIMER_DXE.read_text(encoding="utf-8")

    def test_timer_period_is_initialized_after_interrupt_registration(self) -> None:
        initialize = function_body(self.timer, "TimerInitialize")
        register = initialize.index("RegisterInterruptSource")
        default_period = initialize.index(
            "TimerDriverSetTimerPeriod (&gTimer, FixedPcdGet32 (PcdTimerPeriod))"
        )
        self.assertLess(register, default_period)

    def test_registration_clears_before_unmask_and_never_synthesizes(self) -> None:
        prepare = function_body(self.aic, "AppleAicV2PrepareTimerInterrupt")
        clear = prepare.index("AppleAicV2ClearSoftwareInterrupt")
        unmask = prepare.index("AppleAicUnmaskInterrupt")
        self.assertLess(clear, unmask)
        self.assertNotIn("mAicV2SoftwareSetRegOffset", prepare)
        self.assertNotIn("mDeferredTimer", self.aic)
        self.assertNotIn("ReplayDeferredTimerInterrupt", self.aic)

    def test_reflection_stays_quiet_until_final_timer_enable(self) -> None:
        model = TimerRegistrationModel()
        model.prepare_source()
        self.assertEqual(model.deliver_reflected_tick(), 0)
        self.assertEqual(model.handler_calls, 0)

        model.final_enable(timer_ticks=0x18000)
        self.assertEqual(model.deliver_reflected_tick(), 0)
        self.assertEqual(model.handler_calls, 1)

    def test_zero_tick_catchup_is_explicitly_non_progressing(self) -> None:
        model = TimerRegistrationModel(
            compare=0,
            current=0x1606D36C,
            reflected_pending=False,
            reflected_masked=False,
            reflection_gate_open=True,
        )
        with self.assertRaisesRegex(RuntimeError, "non-progressing"):
            model.deliver_reflected_tick()


if __name__ == "__main__":
    unittest.main()

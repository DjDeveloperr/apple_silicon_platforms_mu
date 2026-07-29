/** @file
  J414s internal keyboard backlight (FPWM) ACPI device.

  The keyboard backlight on the MacBook Pro 14" (M2 Pro, J414s) is channel 0
  of the SoC fixed-PWM block fpwm0.  It is not an SMC key and not an
  MTP/DockChannel HID report: Asahi Linux drives it as a pwm-leds consumer of
  fpwm0 and macOS drives the same generation of hardware through an
  AppleARMPWMDevice child of the ADT pwm node.  It therefore needs no
  interrupt (nothing here touches the CSRT ALI2 GSIV alias table), no DART,
  no firmware, and no coprocessor, and it is fully independent of the MTP
  input bring-up.

  AppleKbdBacklight consumes exactly two memory windows, in this order:

    0: fpwm0 register block, 0x39b030000 + 0x4000.  CTRL at +0x00,
       OFF_CYCLES at +0x18, ON_CYCLES at +0x1c.
    1: pmgr_east power-state page, 0x290280000 + 0x1000.  ps_sio at +0x1c0
       and ps_fpwm0 at +0x1e8 are the pinned power chain the driver raises
       before touching window 0 (fabric parent ps_afnc2_lw1 is always-on).

  The _DSD entries mirror the compiled-in driver contract for verification
  tooling; none of them is a _CRS resource.  There is no preboot handoff:
  the driver powers the domain itself.

  Provenance:
    Linux arch/arm64/boot/dts/apple/t600x-j314-j316.dtsi (led-controller,
      pwms = <&fpwm0 0 40000>, max-brightness 255, 24 MHz clkref)
    Linux arch/arm64/boot/dts/apple/t602x-j414-j416.dtsi (includes the above
      for J414s/J416s)
    Linux arch/arm64/boot/dts/apple/t602x-die0.dtsi (fpwm0 at 0x39b030000)
    Linux arch/arm64/boot/dts/apple/t602x-dieX.dtsi (pmgr_east at
      0x290280000)
    Linux arch/arm64/boot/dts/apple/t602x-pmgr.dtsi (ps_sio 0x1c0,
      ps_fpwm0 0x1e8)
    all at Asahi linux-asahi 030248d39b401c94695c9f7df2fed630d35120cd
    mainline drivers/pwm/pwm-apple.c (register model)
    live Mac ADT pwm0/kbd-backlight node (pwm-frequency 24000000,
      high+low period 960 cycles)

  SPDX-License-Identifier: MIT
**/

DefinitionBlock ("KBL.aml", "SSDT", 0x02, "Apple", "J414KBL", 0x00000001)
{
    Scope (\_SB)
    {
        Device (KBL0)
        {
            Name (_HID, "NTAS0051")
            Name (_UID, Zero)
            Name (_CCA, One)

            //
            // Keep the ordering in lockstep with AppleKblEvtPrepareHardware:
            //   0: fpwm0 register block
            //   1: pmgr_east PS page (ps_sio / ps_fpwm0)
            // No interrupt: the FPWM has no AIC line, so no GSIV and no
            // CSRT ALI2 alias is involved.
            //
            Name (_CRS, ResourceTemplate ()
            {
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x000000039B030000,
                    0x000000039B033FFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x0000000290280000,
                    0x0000000290280FFF,
                    0x0000000000000000,
                    0x0000000000001000
                    )
            })

            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "ntasp,kbl-pwm-clock-hz", 24000000 },
                    Package () { "ntasp,kbl-pwm-period-ns", 40000 },
                    Package () { "ntasp,kbl-max-brightness", 255 },
                    Package () { "ntasp,kbl-ps-sio-offset", 0x1C0 },
                    Package () { "ntasp,kbl-ps-fpwm0-offset", 0x1E8 },
                    Package () { "ntasp,preboot-owner", "none" },
                    Package () { "ntasp,preboot-handoff-required", Zero }
                }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}

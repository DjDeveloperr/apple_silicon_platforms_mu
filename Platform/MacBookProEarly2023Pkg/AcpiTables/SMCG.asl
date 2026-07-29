/** @file
  J414s SMC GPIO controller (NTAS0052).

  The MTP multitouch endpoint asks the host to pulse two reset lines during
  its INIT sequence, and on this machine both live behind the SMC, not on the
  SoC pin controller:

    t602x-j414-j416.dtsi:125  apple,afe-reset-gpios = <&smc_gpio 25 ...>
    t602x-j414-j416.dtsi:126  apple,stm-reset-gpios = <&smc_gpio 26 ...>

  AppleMtpHid is fail-closed and will only pulse through a Resource Hub GPIO
  connection, so a GpioClx controller has to exist for those connections to
  resolve.  This device is what AppleSmcGpio binds to; it owns the SMC ASC
  transport and exposes the SMC's GPIO keys as output-only pins.

  Resource shape follows the AppleNvme/ANS convention already proven on this
  platform: one combined window for the ASC CPU block plus its mailbox, a
  second for the coprocessor's SRAM, then the mailbox interrupt.  The SMC
  mailbox sits at +0x8000 from the ASC base exactly as the ANS one does
  (t602x-die0.dtsi:317 mbox@2a2408000 vs :330 smc@2a2400000), so a single
  0xC000 window covers both.

  Addresses and the interrupt come from Asahi's device tree for this SoC:

    t602x-die0.dtsi:330-335  smc@2a2400000
                             reg = <0x2 0xa2400000 0x0 0x4000>    "smc"
                                   <0x2 0xa3e00000 0x0 0x100000>  "sram"
                             mboxes = <&smc_mbox>
    t602x-die0.dtsi:317-326  smc_mbox: mbox@2a2408000
                             reg = <0x2 0xa2408000 0x0 0x4000>
                             interrupts = 862, 863, 864, 865
                             names      = send-empty, send-not-empty,
                                          recv-empty, recv-not-empty

  No interrupt is published, deliberately.  AppleSmcGpio's SMC transport is
  strictly polled and never connects one.  Publishing the level-triggered
  recv-not-empty line (AIC 865) anyway would let GpioClx connect a line that
  nothing drains -- SMC notifications assert recv-not-empty -- and a connected,
  never-acknowledged level interrupt is an interrupt storm.  If a future
  interrupt-driven transport needs it, add 865 back together with the code that
  services it.  All four mailbox lines are below 1019, so unlike the xHCI and
  ANS lines they would need no CSRT ALI2 alias.

  Copyright (c) Apple Silicon NT Drivers contributors
  SPDX-License-Identifier: MIT
**/

DefinitionBlock ("SMCG.aml", "SSDT", 2, "NTASI ", "SMCGPIO ", 0x00000001)
{
    Scope (\_SB)
    {
        Device (SMCG)
        {
            Name (_HID, "NTAS0052")
            Name (_UID, Zero)
            Name (_CCA, Zero)

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }

            Name (_CRS, ResourceTemplate ()
            {
                //
                // SMC ASC CPU block plus its mailbox at +0x8000.
                //
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x00000002A2400000,
                    0x00000002A240BFFF,
                    0x0000000000000000,
                    0x000000000000C000
                    )
                //
                // SMC coprocessor SRAM.  Key values too large to ride inline
                // in the 64-bit command/result message are exchanged here.
                //
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x00000002A3E00000,
                    0x00000002A3EFFFFF,
                    0x0000000000000000,
                    0x0000000000100000
                    )
            })

            //
            // None of these is a _CRS resource.  They record the pin/key
            // contract so the miniport can cross-check its algorithmic map
            // instead of trusting it: gpio-macsmc.c derives SMC pin n's key
            // as "gP%02x", giving gP19 for 25 and gP1a for 26, and writes
            // CMD_OUTPUT (1 << 24) ORed with the level.  If the live ADT ever
            // disagrees with that derivation the miniport must refuse the pin
            // rather than guess.
            //
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "ntasp,smc-asc-base", 0x00000002A2400000 },
                    Package () { "ntasp,smc-asc-size", 0x4000 },
                    Package () { "ntasp,smc-mailbox-base", 0x00000002A2408000 },
                    Package () { "ntasp,smc-mailbox-size", 0x4000 },
                    Package () { "ntasp,smc-sram-base", 0x00000002A3E00000 },
                    Package () { "ntasp,smc-sram-size", 0x100000 },
                    Package () { "ntasp,smc-rtkit-endpoint", 0x20 },
                    Package () { "ntasp,smc-gpio-key-format", "gP%02x" },
                    Package () { "ntasp,smc-gpio-cmd-output", 0x01000000 },
                    Package () { "ntasp,smc-gpio-pin-afe-reset", 25 },
                    Package () { "ntasp,smc-gpio-pin-stm-reset", 26 },
                    Package () { "ntasp,preboot-owner", "iBoot" }
                }
            })
        }
    }
}

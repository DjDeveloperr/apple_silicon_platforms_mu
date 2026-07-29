/** @file
  J414s MTP/DockChannel input transport ACPI device.

  AppleMtpHid consumes the three DockChannel apertures, one optional
  firmware-staging memory window, and one AIC interrupt in _CRS.  The ASC,
  DART, reset GPIO, and firmware facts are intentionally device properties
  instead: they describe the preboot handoff that m1n1 owns and must not
  become extra translated resources for the Windows driver.

  The fourth memory resource is the multitouch firmware staging window.  Its
  AddressMinimum is the MTP DART stream 1 bus address the driver sends in
  command 0x95; AddressTranslation raises it to the reserved CPU physical
  carveout the driver maps and fills.  The carveout is DRAM at 0x10020000000
  (DRAM base + 512 MiB): below the Mu FD at 0x10030000000, far above the
  m1n1/iBoot bottom-of-DRAM footprint, and reserved from the UEFI memory map
  by the matching MemoryInitPeiLib overlay so Windows never allocates it.
  m1n1 maps bus 0x1800000 -> 0x10020000000 (1 MiB) in MTP DART stream 1
  during the preboot handoff; the bus address sits above the IOP firmware
  segment VAs and below m1n1's RTKit IOVA window at 0x2000000.

  Provenance:
    Linux arch/arm64/boot/dts/apple/t602x-die0.dtsi
    Linux arch/arm64/boot/dts/apple/t602x-j414-j416.dtsi
    Linux arch/arm64/boot/dts/apple/t6020-j414s.dts
    m1n1 proxyclient/experiments/mtp.py

  SPDX-License-Identifier: MIT
**/

DefinitionBlock ("MTP.aml", "SSDT", 0x02, "Apple", "J414MTP", 0x00000001)
{
    Scope (\_SB)
    {
        //
        // The built-in keyboard and trackpad are served by the MTP ASC over
        // DockChannel.  This is not the older AppleSPI trackpad topology.
        //
        Device (MTP0)
        {
            Name (_HID, "NTAS0050")
            Name (_UID, Zero)
            Name (_CCA, One)

            //
            // Keep the ordering in lockstep with AppleMtpEvtPrepareHardware:
            //   0: parent interrupt/status registers
            //   1: remote FIFO-1 configuration window
            //   2: remote FIFO-1 data window
            //   3: multitouch firmware staging window (raw = DART stream 1
            //      bus address for command 0x95, translated = reserved CPU
            //      carveout)
            //   4: physical AIC line 677
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
                    0x00000002A9B14000,
                    0x00000002A9B14FFF,
                    0x0000000000000000,
                    0x0000000000001000
                    )
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x00000002A9B30000,
                    0x00000002A9B30FFF,
                    0x0000000000000000,
                    0x0000000000001000
                    )
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x00000002A9B34000,
                    0x00000002A9B34FFF,
                    0x0000000000000000,
                    0x0000000000001000
                    )
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x0000000001800000,
                    0x00000000018FFFFF,
                    0x000001001E800000,
                    0x0000000000100000
                    )
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) {
                    677
                }
                //
                // GPIO connection 0 is afe-reset (SMC pin 25) and
                // connection 1 is stm-reset (SMC pin 26).  The driver
                // matches the INIT request by (name, id) and refuses
                // any other pairing, so this order is a contract.
                //
                GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly,
                    "\\_SB.SMCG", 0, ResourceConsumer)
                { 25 }
                GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly,
                    "\\_SB.SMCG", 0, ResourceConsumer)
                { 26 }
            })

            //
            // Standard ACPI device-properties UUID.  None of these entries
            // is a _CRS resource: preboot m1n1 code initializes the ASC/DART
            // and reset path, then releases DockChannel ownership before
            // Windows starts AppleMtpHid.
            //
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "ntasp,dockchannel-fifo-index", One },
                    Package () { "ntasp,dockchannel-fifo-size", 0x800 },
                    Package () { "ntasp,mtp-asc-base", 0x00000002A9400000 },
                    Package () { "ntasp,mtp-asc-size", 0x4000 },
                    Package () { "ntasp,mtp-mailbox-base", 0x00000002A9408000 },
                    Package () { "ntasp,mtp-mailbox-size", 0x4000 },
                    Package () { "ntasp,mtp-dart-base", 0x00000002A9808000 },
                    Package () { "ntasp,mtp-dart-size", 0x4000 },
                    Package () { "ntasp,mtp-dart-stream-id", One },
                    Package () { "ntasp,mtp-dart-fault-gsiv", 676 },
                    Package () { "ntasp,mtp-afe-reset-gpio", 25 },
                    Package () { "ntasp,mtp-stm-reset-gpio", 26 },
                    Package () { "ntasp,mtp-firmware-name", "apple/tpmtfw-j414s.bin" },
                    Package () { "ntasp,mtp-fw-staging-dva", 0x0000000001800000 },
                    Package () { "ntasp,mtp-fw-staging-phys", 0x0000010020000000 },
                    Package () { "ntasp,mtp-fw-staging-size", 0x100000 },
                    Package () { "ntasp,preboot-owner", "m1n1" },
                    Package () { "ntasp,preboot-handoff-required", One }
                }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}

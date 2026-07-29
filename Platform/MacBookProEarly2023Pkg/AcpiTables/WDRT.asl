/** @file
  J414s APCIE0 DART publication for the explicit m1n1 wireless handoff.

  This table is compiled only when NTASI_ENABLE_WIRELESS_DART_HANDOFF is
  TRUE. The normal PCIe baseline keeps PCI0 and MCFG but does not publish an
  unbacked DRT0 page-table resource or a dependency on it.

  SPDX-License-Identifier: MIT
**/

DefinitionBlock ("WDRT.aml", "SSDT", 0x02, "Apple", "J414WDR", 0x00000001)
{
    External (\_SB.PCI0, DeviceObj)

    Scope (\_SB)
    {
        Device (DRT0)
        {
            Name (_HID, "NTAS0011")
            Name (_UID, Zero)
            Name (_CCA, One)
            Name (_CRS, ResourceTemplate ()
            {
                QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite, 0,
                    0x0000000594000000, 0x0000000594003fff, 0, 0x4000)
                QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite, 0,
                    FixedPcdGet64 (PcdAppleWirelessDartPageTableBase),
                    (FixedPcdGet64 (PcdAppleWirelessDartPageTableBase) +
                     FixedPcdGet32 (PcdAppleWirelessDartPageTableSize) - 1),
                    0, FixedPcdGet32 (PcdAppleWirelessDartPageTableSize))
            })
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "ntasi,wireless-handoff-abi", 2 },
                    Package () { "ntasi,wireless-handoff-descriptor-offset", 0xc000 },
                    Package () { "ntasi,wireless-handoff-descriptor-size", 96 }
                }
            })
            Method (_STA) { Return (0x0f) }
        }
    }

    Scope (\_SB.PCI0)
    {
        Name (_DEP, Package () { \_SB.DRT0 })
    }
}

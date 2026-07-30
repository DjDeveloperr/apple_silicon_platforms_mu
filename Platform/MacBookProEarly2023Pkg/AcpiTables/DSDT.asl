/**
 * Copyright (c) 2023, amarioguy (AppleWOA authors).
 * 
 * Module Name:
 *     DSDT.asl
 * 
 * Abstract:
 *     Differentiated System Description Table. This source file implements the DSDT table
 *     for the base MacBook Pro (Early 2023) platform.
 *     Variants will be handled via SSDTs loaded depending on the platform.
 * 
 * Environment:
 *     UEFI firmware/runtime services.
 * 
 * License:
 *     SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
 * 
 **/
#include <IndustryStandard/Acpi65.h>

 DefinitionBlock("DSDT.aml", "DSDT", 0x02, "Apple", "J41x", 0x6020) {
    Scope(\_SB) {

        //
        // Cluster Low Power States defined here. Seem to be the same for E-cores/P-cores?
        // Note: there is a state where the cluster can be powered off, unsure how to use so not implemented.
        // If all cores in a cluster are in "deep WFI" mode, the cluster enters "deep WFI" as well.
        //

        Name (CLPI, Package() {
            0, // Version
            0, // Level Index
            1, // Count
            Package() { // Power Gating state for Cluster
            1, // Min residency (uS)
            1, // Wake latency (uS)
            1, // Flags
            1, // Arch Context Flags
            0, //Residency Counter Frequency
            0, // No Parent State
            0x00000000, // Integer Entry method (currently NULL, TODO actually add an entry method)
            ResourceTemplate() { // Null Residency Counter
                Register (SystemMemory, 0, 0, 0, 0)
            },
            ResourceTemplate() { // Null Usage Counter
                Register (SystemMemory, 0, 0, 0, 0)
            },
            "ClusterRetention"
            },
        })

        //
        // Per processor low power states. Currently shallow/deep WFI, suspend to ram not implemented yet.
        // Side note, not gonna be fun having ACPI handle the reconfig engine...
        // TODO: actually implement this.
        //
        Name(PLPI, Package() {
            0, // Version
            0, // Level Index
            2, // Count
            Package() { // WFI for CPU
            1, // Min residency (uS)
            1, // Wake latency (uS)
            1, // Flags
            0, // Arch Context Flags
            0, //Residency Counter Frequency
            0, // No parent state
            ResourceTemplate () {
                // Register Entry method
                Register (SystemMemory,
                0x00,               // Bit Width
                0x00,               // Bit Offset
                0x00,         // Address
                0x00,               // Access Size
                )
            },
            ResourceTemplate() { // Null Residency Counter
                Register (SystemMemory, 0, 0, 0, 0)
            },
            ResourceTemplate() { // Null Usage Counter
                Register (SystemMemory, 0, 0, 0, 0)
            },
            "WFI",
            },
            Package() { // Power Gating state for CPU
            1, // Min residency (uS)
            1, // Wake latency (uS)
            1, // Flags
            1, // Arch Context Flags
            0, //Residency Counter Frequency
            1, // Parent node can be in any state
            ResourceTemplate () {
                // Register Entry method
                Register (SystemMemory,
                0x00,               // Bit Width
                0x00,               // Bit Offset
                0x00000000,         // Address
                0x00,               // Access Size
                )
            },
            ResourceTemplate() { // Null Residency Counter
                Register (SystemMemory, 0, 0, 0, 0)
            },
            ResourceTemplate() { // Null Usage Counter
                Register (SystemMemory, 0, 0, 0, 0)
            },
            "CorePwrDn"
            },
        })


        Device (PCI0) {
            Name (_HID, EISAID ("PNP0A08"))
            Name (_CID, EISAID ("PNP0A03"))
            Name (_SEG, Zero)
            Name (_BBN, Zero)
            Name (_UID, "PCI0")
            Name (_CCA, One)
            Method (_CBA, 0, NotSerialized) {
                Return (FixedPcdGet64 (PcdPciExpressBaseAddress))
            }
            //
            // One identity-mapped (TRA = 0), 32-bit, non-prefetchable
            // producer window at bus == CPU 0xC0000000..0xCFFFFFFF.
            //
            // WHY THIS SHAPE (2026-07-30, code-12 root-cause work):
            //
            //  * Every resource Windows' PnP arbiters have ever granted on
            //    this machine is below 4 GiB (XHC1/XHC2 aliases, MTP, ANS,
            //    DISP, UART); the one _CRS consumer ever published above the
            //    proven range (native XHC1 at 0xB02280000) was refused by the
            //    root memory arbiter and had to be aliased to 0x60000000.
            //    The previous translated window (bus 0xC0000000 -> CPU
            //    0x5C0000000, TRA 0x500000000) put every BAR grant in exactly
            //    that unproven-high class, and additionally relied on the
            //    arbiter honouring a producer-side _TRA -- while ledger row
            //    "HV: Unmapped IPA 0x1800000" proves this Windows build maps
            //    the RAW base of consumer descriptors, so producer _TRA was
            //    the only untested translation path left in the boot.
            //  * m1n1 (tools/m1n1-windows-debug.py in the drivers repo) now
            //    installs a stage-2 alias guest 0xC0000000 -> host
            //    0x5C0000000 (256 MiB) whenever post-HV PCIe init runs, so
            //    guest-physical == PCI-bus address inside this window.  The
            //    hardware window itself is unchanged: the apcie fabric still
            //    forwards host 0x5C0000000+off as bus 0xC0000000+off (ADT
            //    apcie "ranges", measured), and Mu's own PciHostBridgeDxe
            //    still allocates BARs at bus 0xC0000000+ (translation-aware
            //    via PcdPciMmio32Translation), so Mu's boot configuration
            //    (WiFi BAR2=0xC0000000, BT BAR2=0xC1000000, SD
            //    BAR0=0xC2100000, all measured 2026-07-30) sits inside this
            //    window and stays valid for Windows to adopt.
            //  * 256 MiB comfortably covers the 34 MiB Mu actually assigns
            //    and stays far from the XHC1/XHC2 aliases at 0x60000000/
            //    0x61000000 and the MTP staging alias at 0x1800000.
            //
            // The 64-bit prefetchable window (bus == CPU 0x5A0000000,
            // 512 MiB) is deliberately NOT published: both BCM4388 functions
            // expose only 64-bit NON-prefetchable BARs (low bits 0x4) and the
            // GL9755 a 32-bit non-prefetchable BAR, so per PCI bridge rules
            // nothing on this fabric can be placed in a prefetchable window.
            // Publishing an unproven-high producer range Windows never needs
            // only re-introduces the exact arbitration doubt this change
            // removes.
            //
            Name (_CRS, ResourceTemplate () {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0, 0, 4, 0, 5)
                QWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite, 0,
                    0xc0000000, 0xcfffffff, 0, 0x10000000)
            })
            Method (_STA) { Return (0x0f) }
        }

        //
        // The DWC3 blocks expose a standards-compliant xHCI register interface.
        // m1n1 leaves usb-drd1 assigned to the guest, and Mu brings the controller
        // and its DART up before ExitBootServices.  m1n1 maps the exact contiguous
        // DWC3 core + Apple register span from the T6020 device tree at a free
        // 32-bit guest-physical alias because the Windows root memory arbiter
        // rejects the native 0xB02280000 fixed address before StartDevice.
        //
        Device (XHC1) {
            Name (_HID, "PNP0D15")
            Name (_UID, One)
            Name (_CCA, One)

            Name (_CRS, ResourceTemplate () {
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x0000000060000000,
                    0x000000006000FEFF,
                    0x0000000000000000,
                    0x000000000000FF00
                    )
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) {
                    37
                }
            })

            Method (_STA) {
                Return (0xF)
            }
        }

        //
        // J414s' right-side USB-C receptacle is usb-drd2.  Keep its native
        // T6020 controller separate from XHC1 and publish the m1n1-provided
        // low guest-physical alias.  GSIV 39 is translated to physical AIC
        // line 1292 by the AIC2 CSRT ALI2 table.
        //
        Device (XHC2) {
            Name (_HID, "PNP0D15")
            Name (_UID, 0x02)
            Name (_CCA, One)

            Name (_CRS, ResourceTemplate () {
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x0000000061000000,
                    0x000000006100FEFF,
                    0x0000000000000000,
                    0x000000000000FF00
                    )
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) {
                    39
                }
            })

            Method (_STA) {
                Return (0x0)
            }
        }

        //
        // All known Apple devices to date have used the Samsung based UART that debuted on the 8900 (or a compatible implementation).
        //
        Device(COM0) {
            Name(_HID, "APPL8900") // naming it APPL8900 since the Samsung based UART was used since the S5L8900
            Name(_UID, Zero)
            Name (_CRS, ResourceTemplate () {
                QWordMemory (
                ResourceProducer,     // ResourceUsage
                PosDecode,            // Decode
                MinFixed,             // IsMinFixed
                MaxFixed,             // IsMaxFixed
                NonCacheable,         // Cacheable
                ReadWrite,            // ReadAndWrite
                0x0000000000000000,   // AddressGranularity - GRA
                0x39b200000,   // AddressMinimum - MIN
                0x39b200fff,   // AddressMaximum - MAX
                0x0000000000000000,   // AddressTranslation - TRA
                0x0000000000001000    // RangeLength - LEN
                )
                Interrupt(ResourceConsumer, Level, ActiveHigh, Exclusive) { 1198 }            
            })
            Method (_STA) {
                Return (0xF)
            }
        }
        //
        // Die 0, always present.
        //
        Device(DIE0) {
            Name(_HID, "ACPI0010") // all "processor containers" must have this HID
            Name(_UID, Zero) // unique identifier of the container
            //
            // E-core cluster, present on all variants.
            //
            Device(CLU0) {
                Name(_HID, "ACPI0010") // all "processor containers" must have this HID
                Name(_UID, 0x1) // unique identifier of the container
                // Method (_LPI, 0, NotSerialized) {
                //     return(CLPI)
                // }
                //
                // Bootstrap cluster, E-core 0
                //
                Device(CPU0) {
                    Name(_HID, "ACPI0007")
                    Name(_UID, 0)
                    // Method (_LPI, 0, NotSerialized) {
                    // return(PLPI)
                    // }
                    Method (_STA) {
                        Return (0xF)
                    }
                }
                //
                // Bootstrap cluster, E-core 1
                //
                Device(CPU1) {
                    Name(_HID, "ACPI0007")
                    Name(_UID, 1)
                    // Method (_LPI, 0, NotSerialized) {
                    // return(PLPI)
                    // }
                    Method (_STA) {
                        Return (0xF)
                    }
                }
                //
                // Bootstrap cluster, E-core 2
                //
                Device(CPU2) {
                    Name(_HID, "ACPI0007")
                    Name(_UID, 2)
                    // Method (_LPI, 0, NotSerialized) {
                    // return(PLPI)
                    // }
                    Method (_STA) {
                        Return (0xF)
                    }
                }
                //
                // Bootstrap cluster, E-core 3
                //
                Device(CPU3) {
                    Name(_HID, "ACPI0007")
                    Name(_UID, 3)
                    // Method (_LPI, 0, NotSerialized) {
                    // return(PLPI)
                    // }
                    Method (_STA) {
                        Return (0xF)
                    }
                }
            }
            //
            // P-core cluster 1, present on all variants.
            //
            Device(CLU1) {
                Name(_HID, "ACPI0010") // all "processor containers" must have this HID
                Name(_UID, 0x2) // unique identifier of the container
                Method (_STA) {
                    Return (0xF)
                }
                // Method (_LPI, 0, NotSerialized) {
                //     return(CLPI)
                // }
                Device(CPU4) {
                    Name(_HID, "ACPI0007")
                    Name(_UID, 0x4)
                    // Method (_LPI, 0, NotSerialized) {
                    // return(PLPI)
                    // }
                    Method (_STA) {
                        Return (0xF)
                    }
                }
                Device(CPU5) {
                    Name(_HID, "ACPI0007")
                    Name(_UID, 0x5)
                    // Method (_LPI, 0, NotSerialized) {
                    // return(PLPI)
                    // }
                    Method (_STA) {
                        Return (0xF)
                    }
                }
                Device(CPU6) {
                    Name(_HID, "ACPI0007")
                    Name(_UID, 0x6)
                    // Method (_LPI, 0, NotSerialized) {
                    // return(PLPI)
                    // }
                    Method (_STA) {
                        Return (0xF)
                    }
                }
            }
            //
            // P-core cluster 2, present on all variants
            //
            Device(CLU2) {
                Name(_HID, "ACPI0010") // all "processor containers" must have this HID
                Name(_UID, 0x3) // unique identifier of the container
                Method (_STA) {
                    Return (0xF)
                }
                // Method (_LPI, 0, NotSerialized) {
                //     return(CLPI)
                // }
                Device(CPU7) {
                    Name(_HID, "ACPI0007")
                    Name(_UID, 0x7)
                    // Method (_LPI, 0, NotSerialized) {
                    // return(PLPI)
                    // }
                    Method (_STA) {
                        Return (0xF)
                    }
                }
                Device(CPU8) {
                    Name(_HID, "ACPI0007")
                    Name(_UID, 0x8)
                    // Method (_LPI, 0, NotSerialized) {
                    // return(PLPI)
                    // }
                    Method (_STA) {
                        Return (0xF)
                    }
                }
                Device(CPU9) {
                    Name(_HID, "ACPI0007")
                    Name(_UID, 0x9)
                    // Method (_LPI, 0, NotSerialized) {
                    // return(PLPI)
                    // }
                    Method (_STA) {
                        Return (0xF)
                    }
                }
            }
        }

        // //
        // // Die 1, only present on multi die SoCs.
        // // TODO: make this an SSDT, only being placed in DSDT because testing on an T6002.
        // //

        // Device(DIE1) {
        //     Name(_HID, "ACPI0010") // all "processor containers" must have this HID
        //     Name(_UID, 0x4) // unique identifier of the container
        //     //
        //     // E-core cluster, present on all variants.
        //     //
        //     Device(CLU3) {
        //         Name(_HID, "ACPI0010") // all "processor containers" must have this HID
        //         Name(_UID, 0x5) // unique identifier of the container
        //         // Method (_LPI, 0, NotSerialized) {
        //         //     return(CLPI)
        //         // }
        //         //
        //         // Bootstrap cluster, E-core 0
        //         //
        //         Device(CPUA) {
        //             Name(_HID, "ACPI0007")
        //             Name(_UID, 0xA)
        //             // Method (_LPI, 0, NotSerialized) {
        //             // return(PLPI)
        //             // }
        //             Method (_STA) {
        //                 Return (0xF)
        //             }
        //         }
        //         //
        //         // Bootstrap cluster, E-core 1
        //         //
        //         Device(CPUB) {
        //             Name(_HID, "ACPI0007")
        //             Name(_UID, 0xB)
        //             // Method (_LPI, 0, NotSerialized) {
        //             // return(PLPI)
        //             // }
        //             Method (_STA) {
        //                 Return (0xF)
        //             }
        //         }
        //     }
        //     //
        //     // P-core cluster 3, present on all variants.
        //     //
        //     Device(CLU4) {
        //         Name(_HID, "ACPI0010") // all "processor containers" must have this HID
        //         Name(_UID, 0x6) // unique identifier of the container
        //         // Method (_LPI, 0, NotSerialized) {
        //         //     return(CLPI)
        //         // }
        //         Device(CPUC) {
        //             Name(_HID, "ACPI0007")
        //             Name(_UID, 0xC)
        //             // Method (_LPI, 0, NotSerialized) {
        //             // return(PLPI)
        //             // }
        //             Method (_STA) {
        //                 Return (0xF)
        //             }
        //         }
        //         Device(CPUD) {
        //             Name(_HID, "ACPI0007")
        //             Name(_UID, 0xD)
        //             // Method (_LPI, 0, NotSerialized) {
        //             // return(PLPI)
        //             // }
        //             Method (_STA) {
        //                 Return (0xF)
        //             }
        //         }
        //         Device(CPUE) {
        //             Name(_HID, "ACPI0007")
        //             Name(_UID, 0xE)
        //             // Method (_LPI, 0, NotSerialized) {
        //             // return(PLPI)
        //             // }
        //             Method (_STA) {
        //                 Return (0xF)
        //             }
        //         }
        //         Device(CPUF) {
        //             Name(_HID, "ACPI0007")
        //             Name(_UID, 0xF)
        //             // Method (_LPI, 0, NotSerialized) {
        //             // return(PLPI)
        //             // }
        //             Method (_STA) {
        //                 Return (0xF)
        //             }
        //         }
        //     }
        //     //
        //     // P-core cluster 4, present on all variants
        //     //
        //     Device(CLU5) {
        //         Name(_HID, "ACPI0010") // all "processor containers" must have this HID
        //         Name(_UID, 0x7) // unique identifier of the container
        //         // Method (_LPI, 0, NotSerialized) {
        //         //     return(CLPI)
        //         // }
        //         Device(CU16) {
        //             Name(_HID, "ACPI0007")
        //             Name(_UID, 0x10)
        //             // Method (_LPI, 0, NotSerialized) {
        //             // return(PLPI)
        //             // }
        //             Method (_STA) {
        //                 Return (0xF)
        //             }
        //         }
        //         Device(CU17) {
        //             Name(_HID, "ACPI0007")
        //             Name(_UID, 0x11)
        //             // Method (_LPI, 0, NotSerialized) {
        //             // return(PLPI)
        //             // }
        //             Method (_STA) {
        //                 Return (0xF)
        //             }
        //         }
        //         Device(CU18) {
        //             Name(_HID, "ACPI0007")
        //             Name(_UID, 0x12)
        //             // Method (_LPI, 0, NotSerialized) {
        //             // return(PLPI)
        //             // }
        //             Method (_STA) {
        //                 Return (0xF)
        //             }
        //         }
        //         Device(CU19) {
        //             Name(_HID, "ACPI0007")
        //             Name(_UID, 0x13)
        //             // Method (_LPI, 0, NotSerialized) {
        //             // return(PLPI)
        //             // }
        //             Method (_STA) {
        //                 Return (0xF)
        //             }
        //         }
        //     }
        // }

    }
}

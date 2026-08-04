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
            // Both producer windows are a literal transcription of the
            // hardware's own description.  Decoded 2026-07-30 from the pinned
            // live J414s ADT capture (j414s-adt.bin, sha256
            // 93d96b4a3ea736288278606b723f263361c6ae6c3d5c4f24f08f6f7a73f4b66e),
            // node /arm-io/apcie, property "ranges" -- 2 entries x 7 cells:
            //
            //   phys.hi 0x02000000  bus 0x0_C0000000 -> parent 0x5C0000000
            //                       size 0x40000000   (32-bit, NON-prefetchable)
            //   phys.hi 0x43000000  bus 0x5_A0000000 -> parent 0x5A0000000
            //                       size 0x20000000   (64-bit, prefetchable)
            //
            // phys.hi bit 30 is the prefetchable bit and bits 25:24 the space
            // code, so 0x02000000 is 32-bit non-prefetchable and 0x43000000 is
            // 64-bit prefetchable.  The same node gives bus-range <0 8>,
            // #ports 4, msi-address 0xfffff000, #msi-vectors 32 and
            // msi-vector-offset 1672.
            //
            // Hence: _TRA 0x500000000 on the non-prefetchable window (the
            // apcie fabric forwards CPU 0x5C0000000+off as bus 0xC0000000+off,
            // which is what must land in the BARs) and _TRA 0 on the
            // prefetchable window, which the fabric maps identically.
            //
            // RESTORED 2026-07-30, reverting the DSDT hunk of 848b3b5.  That
            // commit replaced this with a single identity-mapped 256 MiB
            // window (bus 0xC0000000, _TRA 0) and deleted the prefetchable
            // window.  Three things were wrong with it:
            //
            //  * It rested on m1n1 installing a stage-2 alias guest
            //    0xC0000000 -> host 0x5C0000000.  No such alias exists.
            //    tools/m1n1-windows-debug.py calls hv.map_hw() exactly three
            //    times -- 0x60000000->0xB02280000 (XHC1), 0x61000000->
            //    0xF02280000 (XHC2) and 0x1800000->0x10020000000 (MTP
            //    staging).  With _TRA 0 and no alias, every BAR Windows
            //    granted would have been mapped at a guest-physical address
            //    nothing backs.
            //  * Its premise -- "Windows' root memory arbiter refuses this
            //    machine's high addresses, as shown by native XHC1 at
            //    0xB02280000" -- is falsified by this project's own A/B.  The
            //    only CM_PROB_NORMAL_CONFLICT ever measured here was on
            //    ACPI\PNP0D15\1, and removing the *Interrupt* descriptor (GSIV
            //    1274) from its _CRS cleared it to problem=0 with resources
            //    assigned.  That code 12 was interrupt arbitration, not
            //    memory; no memory descriptor has ever been refused.
            //  * It withheld 1 GiB of genuine non-prefetchable space and left
            //    only 256 MiB, while deleting the prefetchable window the
            //    fabric really does decode.
            //
            // Sizing, for the record: the ADT's non-prefetchable window is
            // 1 GiB and every BAR on this fabric is non-prefetchable (both
            // BCM4388 functions expose 64-bit non-prefetchable BAR0 64 KiB +
            // BAR2 16 MiB; the GL9755 a 32-bit non-prefetchable BAR0), so the
            // ~34 MiB actually needed has 30x headroom.  The prefetchable
            // window is published because the hardware provides it, not
            // because anything needs it.
            //
            Name (_CRS, ResourceTemplate () {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0, 0, 4, 0, 5)
                QWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite, 0,
                    0xc0000000, 0xffffffff, 0x500000000, 0x40000000)
                QWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    Prefetchable, ReadWrite, 0,
                    0x00000005a0000000, 0x00000005bfffffff, 0, 0x20000000)
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
                // 2026-08-02: RE-ENABLED, because the evidence that disabled it
                // is no longer trustworthy.
                //
                // The 2026-08-01 note read: "With it enabled Windows enumerated
                // the right-side controller but the boot stalled BEFORE any
                // driver started, which matches the recorded storage-death
                // signature for XHC2-on." That symptom -- an early stall with no
                // driver activity -- is exactly what m1n1's rendezvous defect
                // produced, and that defect was killing roughly half of ALL
                // boots regardless of what was enabled. It was diagnosed and
                // fixed today (m1n1 ed83a306): hv_cpus_in_guest is cleared only
                // by hv_exc_entry(), seven return-to-guest paths skipped it, and
                // hv_rendezvous() panicked the machine rather than waiting. So
                // the XHC2-on boots were competing with a coin flip.
                //
                // Nothing about this device looks wrong on inspection: GSIV 39
                // is a routable SPI, unlike COM0's 1198 and XHC1's 1274 which
                // fell in the reserved 1024..4095 gap and produced
                // CM_PROB_NORMAL_CONFLICT. m1n1 already stage-2 aliases the
                // MMIO (0x61000000 -> 0xF02280000). And the 0x144 bugcheck this
                // was blamed for is already recorded as probabilistic -- it
                // occurs with XHC2 OFF as well.
                //
                // With the rendezvous fix in place an A/B finally carries
                // information: a stall now is evidence against XHC2, not noise.
#if NTASI_ENABLE_XHC2
                Return (0x0F)
#else
                Return (Zero)
#endif
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
                ResourceConsumer,     // ResourceUsage
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
                //
                // NO Interrupt descriptor -- deliberate.
                //
                // This used to publish Interrupt(..., Exclusive) { 1198 }. GSIV
                // 1198 is UNROUTABLE on this platform: the GICv3 carrier
                // registers SPIs [32, 1024) and the architecture caps SPI at
                // 1019, so 1198 lands in the reserved 1024..4095 gap. The
                // identical situation on XHC1 (GSIV 1274) was A/B-proven to
                // produce CM_PROB_NORMAL_CONFLICT, and removing the Interrupt
                // descriptor was what cleared it to problem=0 with resources
                // assigned. Keeping it here would fail COM0's whole requirement
                // list the same way -- one unsatisfiable descriptor in a
                // single-alternative list fails everything.
                //
                // AppleSerial does not need it: TX is synchronous polled and RX
                // runs off its own poll timer. The driver accepts zero interrupt
                // descriptors and only rejects MORE than one; the two changes
                // are a matched pair and must ship together.
                //
                // ResourceUsage above is likewise corrected to ResourceConsumer,
                // which is what a leaf device should declare (XHC1/XHC2 already
                // do). Note this is NOT believed to be a blocker on its own:
                // per ACPI 6.0+ 6.4.3.5.1 the Consumer/Producer bit is ignored
                // for QWord descriptors, so do not credit it with any fix.
                //
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
                    Name(_STR, Unicode ("Apple M2 Pro Efficiency Core 0"))
                    Name(_DDN, "Apple M2 Pro Efficiency Core 0")
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
                    Name(_STR, Unicode ("Apple M2 Pro Efficiency Core 1"))
                    Name(_DDN, "Apple M2 Pro Efficiency Core 1")
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
                    Name(_STR, Unicode ("Apple M2 Pro Efficiency Core 2"))
                    Name(_DDN, "Apple M2 Pro Efficiency Core 2")
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
                    Name(_STR, Unicode ("Apple M2 Pro Efficiency Core 3"))
                    Name(_DDN, "Apple M2 Pro Efficiency Core 3")
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
                    Name(_STR, Unicode ("Apple M2 Pro Performance Core 0"))
                    Name(_DDN, "Apple M2 Pro Performance Core 0")
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
                    Name(_STR, Unicode ("Apple M2 Pro Performance Core 1"))
                    Name(_DDN, "Apple M2 Pro Performance Core 1")
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
                    Name(_STR, Unicode ("Apple M2 Pro Performance Core 2"))
                    Name(_DDN, "Apple M2 Pro Performance Core 2")
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
                    Name(_STR, Unicode ("Apple M2 Pro Performance Core 3"))
                    Name(_DDN, "Apple M2 Pro Performance Core 3")
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
                    Name(_STR, Unicode ("Apple M2 Pro Performance Core 4"))
                    Name(_DDN, "Apple M2 Pro Performance Core 4")
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
                    Name(_STR, Unicode ("Apple M2 Pro Performance Core 5"))
                    Name(_DDN, "Apple M2 Pro Performance Core 5")
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

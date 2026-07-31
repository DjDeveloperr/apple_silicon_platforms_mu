/** @file
  J414s (M2 Pro / T6020) FaceTime camera ISP coprocessor ACPI device.

  ==========================================================================
  THIS FILE IS THE SPECIFICATION.  THE BUILD DOES NOT COMPILE IT.
  ==========================================================================
  It is NOT listed in any INF [Sources] section and produces no .aml in the
  firmware volume.  NtasiInstallMediaTables() in
  Silicon/Apple/AppleSiliconPkg/Drivers/AcpiPlatformDxe/AcpiPlatform.c builds
  the equivalent SSDT with AmlLib at DXE runtime, gated on
  NTASI_ENABLE_MEDIA_PUBLICATION, and installs it -- the same relationship
  ANS0 and DRT0 already have with their (deleted) static ASL.  A static table
  was not used because it would land in the firmware volume of EVERY profile,
  including the baseline that boots today, and the media profile must be
  additive-when-on and byte-identical-when-off.

  Tests/test_j414s_media_acpi_contract.py machine-checks that the _CRS window
  list in this file and the C table in AcpiPlatform.c agree, in order, base and
  length, so this file cannot silently drift from what actually ships.  It also
  fails if an Interrupt() descriptor reappears in either place.

  The runtime generator omits only the STRING-valued _DSD properties below:
  this AmlLib has AmlAddNameIntegerPackage() but no string equivalent.  No
  driver reads _DSD at all (verified across AppleIsp, AppleMcaAudio and
  AppleAopAudio -- the single ntasp reference in any of them is a comment), so
  the omission is documentary, not functional.

  Originally authored as drivers-side specification in
  apple_silicon_nt_drivers/integration/hardware/m2-pro/isp/ISP.asl.

  ==========================================================================
  WHAT THE DRIVER DOES WITH THIS
  ==========================================================================
  AppleIsp is an AVStream (KS) capture minidriver.  Given these resources it
  adopts the DART page table iBoot left behind, checks that the ISP firmware
  carveout survived the Windows boot chain, raises the seven power domains,
  resets the coprocessor, runs the three-stage firmware handshake, identifies
  the sensor and configures a capture channel.  It does NOT deliver frames --
  see drivers/AppleIsp/README.md.

  ==========================================================================
  _CRS ORDER IS A CONTRACT
  ==========================================================================
  AppleIsp consumes eight memory windows, in exactly this order, matching the
  NTASI_ISP_RES_* indices in drivers/AppleIsp/AppleIsp.h.  The driver fails
  closed on a short list, but a REORDERED list would put a DART TTBR write into
  a coprocessor control register, so do not reorder these:

    0: ISP coprocessor,        0x384000000 + 0x2000000
    1: ISP mailbox,            0x386104000 + 0x100
    2: ISP scratch "gpio",     0x386104170 + 0x100
    3: ISP mailbox 2,          0x3861043F0 + 0x100
    4: pmgr_east PS page,      0x290280000 + 0x4034
    5: dart-isp0 DARTLLT,      0x3860E8000 + 0x4000
    6: dart-isp0 DARTBULK,     0x3860F4000 + 0x4000
    7: dart-isp0 DARTRT,       0x3860FC000 + 0x4000

  Window 4's length is not rounded: 0x4034 is exactly what isp0's own `reg`
  index 1 publishes in the live ADT, and it is one byte past ps_isp_clr at
  offset 0x4030 -- Apple's own description confirming that all seven ISP power
  states live inside that window.

  ==========================================================================
  KNOWN RESOURCE OVERLAP -- WINDOW 4 vs KBL0, AND vs MCA0
  ==========================================================================
  Window 4 spans [0x290280000, 0x290284033].  KBL0 (NTAS0051, keyboard
  backlight) already claims [0x290280000, 0x290280FFF] as an exclusive
  ResourceConsumer memory descriptor (KBL.asl), and MCA0 in this same media set
  claims that identical page as its own window 4.  PMGR power-state pages are
  inherently shared on this silicon -- one page carries the power words of many
  unrelated devices -- so three _STA=0x0F devices claiming overlapping bytes is
  a genuine conflict that Windows' root memory arbiter can resolve only by
  failing devnodes (CM_PROB_NORMAL_CONFLICT, Code 12).

  This is the SAME defect class that was found and fixed for NTAS2003 on
  2026-07-30, where four PMGR words inside KBL0's page were moved out of _CRS
  and republished as _DSD integer properties (AcpiPlatform.c, "THE FOUR PMGR
  WORDS ARE NO LONGER _CRS RESOURCES").  It is NOT fixed here, because the fix
  is not firmware's to make: AppleIsp matches its resource list POSITIONALLY by
  NTASI_ISP_RES_* index, so deleting window 4 would shift DARTLLT/DARTBULK/
  DARTRT down by one and put a DART TTBR write into a coprocessor register --
  precisely the failure this file's _CRS-order contract exists to prevent.

  The correct fix is driver-side and is the same one ANS took: read the
  pmgr_east base from _DSD instead of from _CRS index 4, then drop the window.
  Until then, expect one of KBL0 / MCA0 / ISP0 to fail with Code 12 in the
  media profile.  Nothing outside the media profile is affected: with
  NTASI_ENABLE_MEDIA_PUBLICATION off, none of these three devices exists.

  ==========================================================================
  THE THREE DART WINDOWS THAT ARE **NOT** HERE
  ==========================================================================
  The live ADT's /arm-io/dart-isp0 node has SIX `reg` windows, not three.  Its
  `instance` property names them; decoded from the pinned capture
  (sha256 93d96b4a...):

    0x3860E8000  'DART' idx 0  "DARTLLT"   -- published above, == isp_dart0
    0x3860F4000  'DART' idx 1  "DARTBULK"  -- published above, == isp_dart1
    0x3860FC000  'DART' idx 2  "DARTRT"    -- published above, == isp_dart2
    0x3860F0000  'SMMU' idx 1  "SMMUBULK"  -- NOT published
    0x3860F8000  'SMMU' idx 2  "SMMURT"    -- NOT published
    0x3860EC000  'DAPF' idx 0  "APF"       -- NOT published

  So Asahi's three `isp_dart0/1/2` devices and this one node describe the same
  silicon: three TRANSLATION instances named for three traffic classes
  (low-latency, bulk, real-time), plus two SMMU shadows and an address filter
  that the known-working Linux driver never touches.  The three unpublished
  windows are omitted precisely because the driver must never write to them; a
  window that is not in _CRS cannot be mapped by accident.

  ==========================================================================
  ONE INTERRUPT, PUBLISHED LAST
  ==========================================================================
  AIC line 569, appended AFTER the eight memory windows.  Position matters only
  in the sense that it must not come first: AppleIspMapResources() walks the
  translated list once, counts memory descriptors in their own index space, and
  handles CmResourceTypeInterrupt in a separate switch arm -- so an interrupt at
  the end cannot shift a window, and the _CRS memory contract above is exactly
  as it was.

  569 is below the GIC carrier's 1019 limit, so it is published identity-mapped
  and needs NO CSRT ALI2 entry.  Of the three media devices only MCA0's lines
  need translation; see CSRT.aslc.

  AppleIsp assigns rather than accumulates -- `Device->Gsiv =
  desc->u.Interrupt.Vector` -- so it keeps the LAST interrupt descriptor it
  sees.  With exactly one published that is unambiguous; adding 570/571/572
  later would silently change which line gets connected, so do not add them
  without also changing the driver.

  The driver treats a failed connect as non-fatal on purpose, and its entire
  bring-up remains polled, so this interrupt is an improvement to the streaming
  path rather than a precondition for enumeration.

  ==========================================================================
  WHAT MU MUST DO BESIDES PUBLISHING THIS DEVICE
  ==========================================================================
  Keep 0x100009FC000 .. 0x10001C80000 (0x1284000 bytes) OUT of the conventional
  UEFI memory map.  That is the union of the ISP firmware's __TEXT and __DATA
  segments plus the 0x63C000 physical hole between them.  The ISP firmware is
  placed there by the Apple boot chain (`pre-loaded = 1`) and the OS only resets
  and starts the core -- if Windows is ever handed that range as conventional
  memory the image is destroyed before the driver runs.

  As of the pinned boot_args capture this is ALREADY SATISFIED without any Mu
  change: the carveout ends 1.75 MiB below phys_base = 0x10001E40000, so Mu
  never describes it.  This note exists so that a future change to phys_base
  does not silently break the camera.  tests/test_j414s_av_memory_contract.py
  pins it.

  Provenance:
    Linux arch/arm64/boot/dts/apple/t602x-die0.dtsi:562-578 (isp@384000000)
    Linux arch/arm64/boot/dts/apple/t602x-j414-j416.dtsi:146-152
      (deletes the inherited preset table, includes isp-imx558-cfg0.dtsi)
    Linux drivers/media/platform/apple/isp/ (isp-drv.c, isp-fw.c, isp-regs.h)
    Linux drivers/iommu/apple-dart.c (T8110 register block)
    all at Asahi linux-asahi 030248d39b401c94695c9f7df2fed630d35120cd
    the pinned J414s live ADT capture, sha256 93d96b4a...
    docs/j414s-camera-isp-bringup.md

  SPDX-License-Identifier: MIT
**/

DefinitionBlock ("ISP.aml", "SSDT", 0x02, "Apple", "J414ISP", 0x00000001)
{
    Scope (\_SB)
    {
        Device (ISP0)
        {
            Name (_HID, "NTAS0090")
            Name (_UID, Zero)
            //
            // _CCA = 1.  The ISP's DMA is coherent with the CPU caches, which
            // is what lets AppleIsp map its frame and IPC surfaces MmCached and
            // skip explicit flushes.  If this is ever changed to Zero the
            // driver's surface allocator must change to MmNonCached in the same
            // commit -- the two are one decision.
            //
            Name (_CCA, One)

            Name (_CRS, ResourceTemplate ()
            {
                // 0: ISP coprocessor -- RVBAR, CONTROL/STATUS, FABRIC, IRQ masks
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000384000000,
                    0x0000000385FFFFFF,
                    0x0000000000000000,
                    0x0000000002000000
                    )
                // 1: ISP mailbox (IRQ interrupt / enable)
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000386104000,
                    0x00000003861040FF,
                    0x0000000000000000,
                    0x0000000000000100
                    )
                // 2: ISP scratch words.  The device tree calls these "gpio" but
                // they are NOT pinctrl GPIOs -- they are eight 32-bit words the
                // host and firmware use to trade magic numbers during boot.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000386104170,
                    0x000000038610426F,
                    0x0000000000000000,
                    0x0000000000000100
                    )
                // 3: ISP mailbox 2 (doorbell / ack)
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x00000003861043F0,
                    0x00000003861044EF,
                    0x0000000000000000,
                    0x0000000000000100
                    )
                // 4: pmgr_east power-state window.  Length 0x4034 verbatim from
                // isp0's own `reg` index 1; see the header comment.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000290280000,
                    0x0000000290284033,
                    0x0000000000000000,
                    0x0000000000004034
                    )
                // 5: dart-isp0 DARTLLT (low latency)  == Asahi isp_dart0
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x00000003860E8000,
                    0x00000003860EBFFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // 6: dart-isp0 DARTBULK                == Asahi isp_dart1
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x00000003860F4000,
                    0x00000003860F7FFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // 7: dart-isp0 DARTRT (real time)      == Asahi isp_dart2
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x00000003860FC000,
                    0x00000003860FFFFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                //
                // AIC line 569, published LAST -- after all eight memory
                // windows -- so the positional memory contract above is
                // untouched.  AppleIspMapResources() counts memory descriptors
                // in their own index space and handles CmResourceTypeInterrupt
                // in a separate switch arm, so an interrupt appended here
                // cannot shift any window.
                //
                // 569 is BELOW the GIC carrier's 1019 limit, so it is a legal
                // plain GSIV published identity-mapped, with NO CSRT ALI2
                // translation entry.  That is the opposite of the MCA lines
                // (1211-1231), every one of which needs one.
                //
                // The ADT also lists 570, 571 and 572 for isp0; Linux wires
                // only 569 and so does this.  If a future revision needs the
                // others they are additional Interrupt() entries -- but note
                // that AppleIsp keeps only the LAST interrupt descriptor it
                // sees (Device->Gsiv is assigned, not accumulated), so adding
                // more would silently change which line the driver connects.
                //
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, , , )
                {
                    569
                }
            })

            //
            // Descriptive properties for verification tooling.  None of these is
            // a _CRS resource and the driver does not depend on any of them --
            // they exist so that tools/verify-j414s-av-adt.py and a human
            // reading the SSDT see the same facts the driver was built against.
            //
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    // The rule that must never be relaxed.  Every sensor config
                    // other than 0 enables MLVNR, which runs on the Apple Neural
                    // Engine; the ISP firmware crashes mid-stream if one is
                    // selected without an ANE driver, and there is no ANE driver
                    // on Linux or here.
                    Package () { "ntasp,isp-camera-config-index", 0 },
                    Package () { "ntasp,isp-camera-config-index-pinned", One },
                    Package () { "ntasp,isp-platform-id", 7 },
                    Package () { "ntasp,isp-sensor", "IMX558" },
                    Package () { "ntasp,isp-sensor-native-dim", 1920 },

                    // Published capture modes: landscape only, entries 0/2/4/6/8
                    // of the ten-entry table.  Asahi shipped all ten and spent
                    // nine months on application bugs caused by portrait entries
                    // being chosen as "the biggest mode" (AsahiLinux/linux#384).
                    Package () { "ntasp,isp-mode-count", 10 },
                    Package () { "ntasp,isp-published-mode-count", 5 },
                    Package () { "ntasp,isp-stride-alignment", 64 },
                    Package () { "ntasp,isp-pixel-format", "NV12" },
                    Package () { "ntasp,isp-frame-rate-max", 30 },
                    Package () { "ntasp,isp-frame-rate-min", 15 },

                    // Firmware carveout.  Placed by iBoot; the OS must not be
                    // given it and does not load it.
                    Package () { "ntasp,isp-firmware-preloaded", One },
                    Package () { "ntasp,isp-firmware-carveout-base", 0x100009FC000 },
                    Package () { "ntasp,isp-firmware-carveout-size", 0x1284000 },
                    Package () { "ntasp,isp-firmware-text-iova", 0x0 },
                    Package () { "ntasp,isp-firmware-data-iova", 0x934000 },
                    Package () { "ntasp,isp-firmware-iova-span", 0xC48000 },

                    // DART topology.  One node, six windows, three of which
                    // translate; stream 0; 16 KiB pages; 42-bit PA.
                    Package () { "ntasp,isp-dart-node-count", 1 },
                    Package () { "ntasp,isp-dart-window-count", 6 },
                    Package () { "ntasp,isp-dart-translation-count", 3 },
                    Package () { "ntasp,isp-dart-windows-published", 3 },
                    Package () { "ntasp,isp-dart-sid", 0 },
                    Package () { "ntasp,isp-dart-page-shift", 14 },
                    Package () { "ntasp,isp-dart-pa-width", 42 },
                    Package () { "ntasp,isp-dart-vm-size", 0xA0000000 },
                    Package () { "ntasp,isp-dart-adopt-inherited-table", One },

                    // Interrupt posture.
                    Package () { "ntasp,isp-gsiv", 569 },
                    Package () { "ntasp,isp-gsiv-needs-ali2", Zero },
                    Package () { "ntasp,isp-interrupts-published", One },
                    Package () { "ntasp,isp-adt-irq-count", 4 },
                    Package () { "ntasp,isp-bringup-is-polled", One },

                    // Honest scope, so a tool reading the SSDT does not have to
                    // read the driver to learn this.
                    Package () { "ntasp,isp-delivers-frames", Zero },
                    Package () { "ntasp,isp-power-domain-count", 7 }
                }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}

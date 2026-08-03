/** @file
  J414s MCA audio complex ACPI device.

  ==========================================================================
  THIS FILE IS THE SPECIFICATION.  THE BUILD DOES NOT COMPILE IT.
  ==========================================================================
  It is NOT listed in any INF [Sources] section and produces no .aml in the
  firmware volume.  NtasiInstallMediaTables() in
  Silicon/Apple/AppleSiliconPkg/Drivers/AcpiPlatformDxe/AcpiPlatform.c builds
  the equivalent SSDT with AmlLib at DXE runtime, gated on
  NTASI_ENABLE_MEDIA_PUBLICATION.  See ISP.asl's header for why a static table
  was not used and for the string-_DSD omission.
  Tests/test_j414s_media_acpi_contract.py pins this file against that C table.

  Originally authored as drivers-side specification in
  the retained AuroraSilicon M2 Pro hardware evidence.

  ==========================================================================
  NINE WINDOWS, NOT TWELVE -- AND THAT IS A LEGAL SHAPE
  ==========================================================================
  AppleMcaMapResources() accepts exactly 8, 9 or 12 memory descriptors and
  returns STATUS_DEVICE_CONFIGURATION_ERROR for anything between, because the
  list is matched positionally and a partial capture set would silently shift
  the meaning of every later index.  This file publishes NINE, which sets
  CaptureResourcesPresent = FALSE.

  The three capture windows are deliberately withheld for first light.  They
  are all-or-nothing and, unlike everything above, they would arm code that
  MUTATES hardware the firmware has not otherwise touched: a CS42L84 reset over
  the pinctrl_nub GPIO and a sio_dart stream programming.  Add them as one
  change, never partially:

     9: i2c2,         0x39B048000 + 0x4000   -- CS42L84 jack codec at 0x4b
    10: pinctrl_nub,  0x29E1F0000 + 0x4000   -- codec reset line is pin 8
    11: sio_dart,     0x39B008000 + 0x4000   -- ADMAC's IOMMU, stream SID 2

  ==========================================================================
  KNOWN RESOURCE OVERLAP -- WINDOW 4 vs KBL0, AND vs ISP0
  ==========================================================================
  Window 4 is [0x290280000, 0x290280FFF], byte-for-byte the page KBL0
  (NTAS0051) already claims exclusively in KBL.asl, and a subset of ISP0's own
  window 4.  Read the identically-titled section of ISP.asl: this is the same
  defect class as the NTAS2003/KBL0 collision fixed on 2026-07-30, the fix is
  driver-side (_DSD instead of _CRS index 4), and it cannot be made here
  without breaking the positional _CRS contract.  Expect Code 12 on one of
  KBL0 / MCA0 / ISP0 in the media profile.

  AppleMcaAudio drives the speaker/headphone-jack half of the machine's audio:
  the MCA I2S/TDM SERDES complex, its ADMAC DMA controller, the NCO that clocks
  it, and the six TAS2764/SN012776 amplifiers on i2c1/i2c3.  The internal
  microphone array is NOT here: on this machine it lives behind the AOP
  coprocessor and needs a separate device (see drivers/AppleAopAudio).

  AppleMcaAudio consumes eight required memory windows, in this exact order,
  and one optional ninth.  Keep the ordering in lockstep with
  AppleMcaEvtPrepareHardware and the NTASI_MCA_RES_* indices:

    0: MCA cluster registers,   0x39b600000 + 0x10000   (4 clusters of 0x4000)
    1: MCA switch / DMA glue,   0x39b500000 + 0x20000
    2: ADMAC (audio DMA),       0x39b400000 + 0x34000
    3: NCO clock generator,     0x28e03c000 + 0x14000   (5 channels of 0x4000)
    4: pmgr_east PS page,       0x290280000 + 0x1000
    5: i2c1 (left amps),        0x39b044000 + 0x4000
    6: i2c3 (right amps),       0x39b04c000 + 0x4000
    7: pinctrl_ap GPIO block,   0x39b028000 + 0x4000
    8: OPTIONAL -- the /arm-io/mca-switch clock-mux window, 0x28e03807c + 0x18.

  RESOLVED 2026-07-30: window 8's address used to be an open question, because
  the Linux DT does not model the mux registers at all.  It is now read
  directly out of the pinned live ADT capture (sha256 93d96b4a...), and it is
  `reg` INDEX 2 of /arm-io/mca-switch -- NOT get_reg(0), which earlier notes
  wrongly suggested.  m1n1 src/clk.c:26 uses index 2 verbatim:

      adt_get_reg(adt, path, "reg", 2, &mca_clk_base, &mca_clk_size)

  giving 0x28e03807c + 0x18, i.e. six 32-bit mux registers.  m1n1 then writes
  CLK_MUX (bits [27:24]) of each word i to 5 + min(4, i) -> 5,6,7,8,9,9.
  That loop only runs from kboot_boot(), which the Windows boot path never
  reaches, so these muxes are NOT programmed when Windows starts.
  tools/verify-j414s-av-adt.py asserts the address and the register count.

  Note the window is deliberately sub-page and NOT page-aligned (offset 0x07c).
  It is a six-register island next to the NCO block at 0x28e03c000, and it does
  not overlap the NCO.  The driver still fails open if the window is absent.

  No interrupt.  Every AIC line in this subsystem (MCA 1211-1214, ADMAC 1218,
  i2c 1220/1222) is above 1019 and therefore illegal as a GSIV under the GIC
  carrier, so publishing one would require a CSRT ALI2 translation entry per
  line.  The driver is written to complete its entire bring-up by polling, so
  first light does not depend on that work.  Adding interrupts later is what
  the streaming path needs, not what enumeration needs.

  Note the deliberate overlap between window 1 (0x39b500000 + 0x20000) and the
  SoC's dpaudio0 block (0x39b500000 + 0x4000).  Both Asahi's t600x and t602x
  device trees describe them that way and dpaudio0 is disabled on this machine.
  Do not publish a dpaudio0 device alongside this one.

  Provenance:
    Linux arch/arm64/boot/dts/apple/t602x-die0.dtsi
      (mca@39b600000:923, admac@39b400000:888, nco@28e03c000:9,
       i2c1@39b044000:718, i2c3@39b04c000:746)
    Linux arch/arm64/boot/dts/apple/t602x-dieX.dtsi
      (pinctrl_ap@39b028000:269, pmgr_east@290280000:121)
    Linux arch/arm64/boot/dts/apple/t602x-pmgr.dtsi
      (ps_sio 0x1c0, ps_i2c1 0x208, ps_i2c3 0x218, ps_audio_p 0x270,
       ps_sio_adma 0x278, ps_mca0..3 0x398/0x3a0/0x3a8/0x3b0 -- all inside the
       pmgr_east overlay, lines 998-1973)
    Linux arch/arm64/boot/dts/apple/t600x-j314-j316.dtsi
      (six ti,sn012776 amps, the sound node's two dai-links, nco_clkref
       1068000000)
    Linux arch/arm64/boot/dts/apple/t602x-j414-j416.dtsi
      (J414s GPIO overrides: SDZ pinctrl_ap 57, amp IRQ 58, jack IRQ 59)
    Linux sound/soc/apple/mca.c, sound/soc/apple/macaudio.c,
      drivers/dma/apple-admac.c, drivers/clk/clk-apple-nco.c,
      sound/soc/codecs/tas2764.c
    all at Asahi linux-asahi 030248d39b401c94695c9f7df2fed630d35120cd
    m1n1 src/clk.c (the mca-switch mux writes, and the fact that only
      kboot_boot() calls them)

  SAFETY: this device's driver never unmutes an amplifier and never starts a
  DMA channel.  The internal speakers have no thermal protection on Windows --
  there is no equivalent of Asahi's speakersafetyd -- and the amplifiers power
  on at maximum analog gain and zero digital attenuation.  Read
  docs/j414s-audio-bringup.md section 8 before changing anything here.

  SPDX-License-Identifier: MIT
**/

DefinitionBlock ("MCA.aml", "SSDT", 0x02, "Apple", "J414MCA", 0x00000001)
{
    Scope (\_SB)
    {
        Device (MCA0)
        {
            Name (_HID, "NTAS0080")
            Name (_UID, Zero)
            Name (_CCA, One)

            Name (_CRS, ResourceTemplate ()
            {
                // 0: MCA cluster registers
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x000000039B600000,
                    0x000000039B60FFFF,
                    0x0000000000000000,
                    0x0000000000010000
                    )
                // 1: MCA switch / DMA glue
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x000000039B500000,
                    0x000000039B51FFFF,
                    0x0000000000000000,
                    0x0000000000020000
                    )
                // 2: ADMAC
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x000000039B400000,
                    0x000000039B433FFF,
                    0x0000000000000000,
                    0x0000000000034000
                    )
                // 3: NCO
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x000000028E03C000,
                    0x000000028E04FFFF,
                    0x0000000000000000,
                    0x0000000000014000
                    )
                // 4: pmgr_east power-state page
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000290280000,
                    0x0000000290280FFF,
                    0x0000000000000000,
                    0x0000000000001000
                    )
                // 5: i2c1 -- left woofers and tweeter (0x38, 0x39, 0x3a)
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x000000039B044000,
                    0x000000039B047FFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // 6: i2c3 -- right woofers and tweeter (0x3b, 0x3c, 0x3d)
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x000000039B04C000,
                    0x000000039B04FFFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // 7: pinctrl_ap -- the shared speaker SDZ line is pin 57
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x000000039B028000,
                    0x000000039B02BFFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // 8: OPTIONAL -- /arm-io/mca-switch clock-mux window.
                // Live ADT `reg` index 2 (m1n1 src/clk.c:26).  Six 32-bit mux
                // registers; sub-page and intentionally unaligned.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x000000028E03807C,
                    0x000000028E038093,
                    0x0000000000000000,
                    0x0000000000000018
                    )
                //
                // Five PUBLISHED GSIVs, appended after all nine memory
                // windows.  These are NOT the physical AIC lines: every line
                // in this subsystem is above the GIC carrier's 1019 limit and
                // is illegal as a GSIV, so the CSRT's ALI2 tail translates
                // them.  The media profile therefore selects the
                // "m2-pro-media" CSRT (8 aliases, 296 bytes) instead of the
                // ordinary "m2-pro" (3 aliases, 256 bytes); see CSRT.aslc.
                //
                //   40 -> 1218  admac-sio, IRQ output index 1
                //   41 -> 1211  mca0, I/V sense cluster
                //   42 -> 1213  mca2, jack cluster
                //   43 -> 1221  i2c2, CS42L84
                //   45 -> 1231  dart-sio
                //
                // NOT 44.  AIC 44 is claimed by /arm-io/i2c0/hpmBusManager in
                // the live ADT and tools/verify-j414s-gsiv-allocation.py
                // rejects it.  An earlier draft proposed 44 -> 1231; it was
                // withdrawn and must not come back.
                //
                // MEASURED CAVEAT: AppleMcaAudio contains no
                // CmResourceTypeInterrupt handling and no WdfInterrupt object
                // -- AppleMcaMapResources() skips every non-memory descriptor.
                // All five are inert for the shipped driver, whose bring-up is
                // wholly polled.  They are published so the streaming path has
                // its resources already arbitrated, and because a granted
                // descriptor now is evidence it will be granted later.  The
                // cost is that five more descriptors must be satisfiable for
                // MCA0 to start at all.
                //
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, , , )
                {
                    40,
                    41,
                    42,
                    43,
                    45
                }
            })

            //
            // Descriptive properties for verification tooling.  None of these
            // is a _CRS resource, and none of them can open the render gate:
            // ntasp,mca-allow-render is intentionally absent, and this build of
            // the driver hard-codes its ACPI opt-in to false regardless.
            //
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "ntasp,mca-cluster-count", 4 },
                    Package () { "ntasp,mca-speaker-cluster-left", 0 },
                    Package () { "ntasp,mca-speaker-cluster-right", 1 },
                    Package () { "ntasp,mca-jack-cluster", 2 },
                    Package () { "ntasp,nco-ref-hz", 1068000000 },
                    Package () { "ntasp,mca-slot-width", 32 },
                    Package () { "ntasp,mca-bclk-ratio-speakers", 256 },
                    Package () { "ntasp,mca-bclk-ratio-jack", 64 },
                    Package () { "ntasp,admac-irq-output-index", 1 },
                    Package () { "ntasp,speaker-amp-count", 6 },
                    Package () { "ntasp,speaker-sdz-gpio", 57 },
                    Package () { "ntasp,speaker-irq-gpio", 58 },
                    Package () { "ntasp,jack-irq-gpio", 59 },
                    Package () { "ntasp,speaker-safe-dvc-floor", 40 },
                    Package () { "ntasp,speaker-resting-dvc", 200 },
                    Package () { "ntasp,speaker-amp-gain-ceiling", 15 },
                    Package () { "ntasp,thermal-protection", "none" },
                    Package () { "ntasp,preboot-owner", "none" },
                    Package () { "ntasp,preboot-handoff-required", Zero },
                    Package () { "ntasp,clk-mux-window-published", One },
                    Package () { "ntasp,clk-mux-register-count", 6 },
                    // Five published GSIVs (40/41/42/43/45), all requiring
                    // CSRT ALI2 translation -- which is why the media profile
                    // selects the m2-pro-media CSRT.
                    Package () { "ntasp,mca-interrupts-published", 5 },
                    Package () { "ntasp,mca-csrt-ali2-required", One },
                    Package () { "ntasp,mca-capture-windows-published", Zero },

                    //
                    // Live-ADT facts (sha256 93d96b4a...), machine-checked by
                    // tools/verify-j414s-av-adt.py.  Where Apple's ADT and
                    // Asahi's hand-written Linux DT disagree, BOTH are
                    // recorded rather than one being silently preferred.
                    //
                    // Only three MCA cluster nodes are instantiated on J414s
                    // (mca0/mca1/mca2); mca-switch still reports numClusters=4
                    // and the register window still spans four clusters.
                    Package () { "ntasp,mca-clusters-instantiated", 3 },
                    // CONFLICT, unresolved, and it matters only on a path this
                    // project never takes: Apple's ADT puts ALL SIX amplifiers
                    // on cluster 0 (mca0a/audio-speaker, audio-data,sn012776)
                    // and uses cluster 1 as an internal loopback
                    // (mca1a/audio-loopback).  Asahi instead splits the amps
                    // across clusters 0 and 1 with TDM masks 0x15/0x2a.  The
                    // ntasp,mca-speaker-cluster-* values above follow Asahi,
                    // because Asahi is known to work on this exact machine.
                    // Do not "fix" either one without hardware evidence, and
                    // note that resolving it requires driving the amps, which
                    // is out of scope: see SAFETY above.
                    Package () { "ntasp,adt-speaker-cluster", 0 },
                    Package () { "ntasp,adt-loopback-cluster", 1 },
                    // Verified from the ADT's own per-DAI dma-channels
                    // records: ADMAC index = (SIO channel id - 40).
                    Package () { "ntasp,admac-channel-jack-capture", 11 },
                    Package () { "ntasp,admac-channel-speaker-play", 0 }
                }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}

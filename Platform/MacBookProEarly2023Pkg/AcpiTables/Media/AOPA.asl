/** @file
  J414s AOP (Always-On Processor) internal microphone array ACPI device.

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

  This is the only one of the three media devices with no pmgr_east window, so
  it is also the only one with no resource overlap against KBL0 -- see the
  "KNOWN RESOURCE OVERLAP" section of ISP.asl for what the other two hit.

  Originally authored as drivers-side specification in
  apple_silicon_nt_drivers/integration/hardware/m2-pro/aop-audio/AOPA.asl.

  AppleAopAudio drives the machine's INTERNAL PDM microphone array, which is
  not on the MCA I2S complex at all: it is a PDM front end owned by the AOP
  coprocessor, decimated in AOP firmware, and delivered over the AOP's own
  ADMAC instance.  The speaker and headphone-jack half of the machine's audio
  is a separate device -- see project-mu/MCA.asl and drivers/AppleMcaAudio.

  AppleAopAudio consumes FOUR memory windows, in this exact order.  Keep the
  ordering in lockstep with AppleAopEvtPrepareHardware and the
  NTASI_AOP_RES_* indices in drivers/AppleAopAudio/AppleAopAudio.h:

    0: aop ASC (coprocessor control), 0x2a6400000 + 0x6c000
    1: aop SRAM / mmio window,        0x2a6c00000 + 0x250000
    2: aop_dart (T8110),              0x2a6808000 + 0x4000
    3: aop_admac,                     0x2a6980000 + 0x34000

  THE MAILBOX IS NOT A FIFTH WINDOW.  /arm-io/aop-mbox is at 0x2a6408000, which
  is window 0 + 0x8000 and already inside it; the driver derives the mailbox
  pointer rather than taking an overlapping resource.  This holds on every SoC
  with an `iop,ascwrap-v4` AOP: t8103 0x24a408000, t600x 0x293408000,
  t602x 0x2a6408000, each exactly ASC + 0x8000.

  > #### Address trap on the `aop` node -- the inverse of the usual rule
  > `aop.get_reg(2)` returns 0x4a6c00000, which is WRONG.  The raw ADT `reg`
  > value is 0x2a6c00000, already above the `arm-io` range base, so the range
  > translation double-adds 0x2_0000_0000.  The true physical base of the AOP
  > SRAM is 0x2a6c00000, which is what Asahi's DT says and what the node's own
  > `segment-ranges` __TEXT entry gives.  This project's standing advice is
  > "use get_reg(), not the raw reg".  That advice is right for most nodes and
  > wrong here.  tools/verify-j414s-av-adt.py asserts the disagreement so it
  > cannot be silently "fixed".

  NO INTERRUPT IS PUBLISHED, and this is a different situation from MCA.asl.
  The AOP's AIC lines are 613/614/615/616 (mailbox), 628 (dart-aop) and 631
  (admac-aop-audio).  Every one of them is BELOW 1019, so unlike the MCA and
  ADMAC lines (1211-1218) they are legal GSIVs under the GIC carrier and would
  need NO CSRT ALI2 alias.  They are omitted anyway, for one reason: the GIC
  carrier's declared SPI count is not something this file can verify, and a
  _CRS the interrupt arbiter cannot satisfy stops the device from starting at
  all.  The driver completes its entire bring-up by polling -- the RTKit/AFK
  handshake polls the ASC mailbox and the capture ring is advanced by a 2 ms
  WDF timer -- so first light does not depend on any of this.  The line numbers
  are published as _DSD properties below so that adding the descriptors later
  is a one-line change with no rediscovery.

  NO POWER DOMAIN.  The `aop` node's `clock-gates` and `power-gates` are both
  empty and there is no `ps_aop` anywhere in t602x-pmgr.dtsi, so -- unlike
  MCA.asl, which publishes a pmgr_east page as window 4 -- there is nothing to
  raise before touching the block.

  Provenance:
    Linux arch/arm64/boot/dts/apple/t602x-die0.dtsi
      (aop@2a6c00000:422-423, aop_mbox@2a6408000:383,
       aop_dart@2a6808000, admac-aop-audio@2a6980000:416, dmas <&aop_admac 1>)
    Linux arch/arm64/boot/dts/apple/t600x-j314-j316.dtsi:597-603
      (&aop, &aop_admac, &aop_audio all enabled)
    Linux arch/arm64/boot/dts/apple/t6020-j414s.dts:34-37
      (apple,chassis-name = "J414", apple,machine-kind = "MacBook Pro")
    Linux drivers/soc/apple/aop.rs, sound/soc/apple/aop_audio.rs,
      drivers/dma/apple-admac.c, drivers/iommu/apple-dart.c
    all at Asahi linux-asahi 030248d39b401c94695c9f7df2fed630d35120cd
    m1n1 src/dart.c (T8110 TCR bit assignments, MIT)
    The pinned live-ADT capture sha256 93d96b4a..., asserted by
      tools/verify-j414s-av-adt.py --section mic (51 checks)

  SAFETY: this is a CAPTURE device.  Its driver has no render path, publishes
  no render pin, and contains no TAS2764 or MCA code.  It cannot drive the
  speakers.  Nothing in this file should ever grow a property that changes
  that.

  SPDX-License-Identifier: MIT
**/

DefinitionBlock ("AOPA.aml", "SSDT", 0x02, "Apple", "J414AOPA", 0x00000001)
{
    Scope (\_SB)
    {
        Device (AOPA)
        {
            Name (_HID, "NTAS0081")
            Name (_UID, Zero)
            Name (_CCA, One)

            Name (_CRS, ResourceTemplate ()
            {
                // 0: aop ASC control block.  CPU_CONTROL is at +0x44 and the
                // ASC mailbox at +0x8000; both are inside this window.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x00000002A6400000,
                    0x00000002A646BFFF,
                    0x0000000000000000,
                    0x000000000006C000
                    )
                // 1: aop SRAM.  The firmware image is already placed here by
                // the Apple boot chain; the two u32 at +0x22c and +0x230 hold
                // the offset and length of the bootargs blob.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x00000002A6C00000,
                    0x00000002A6E4FFFF,
                    0x0000000000000000,
                    0x0000000000250000
                    )
                // 2: dart-aop, apple,t8110-dart, page-size 16384.
                // Stream 0 = the AOP itself, stream 10 = the AOP ADMAC.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x00000002A6808000,
                    0x00000002A680BFFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // 3: admac-aop-audio.  16 channels; the microphone stream is
                // channel 1 (odd => RX) and the instance's IRQ *output* index
                // is 2, not the audio ADMAC's 1.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x00000002A6980000,
                    0x00000002A69B3FFF,
                    0x0000000000000000,
                    0x0000000000034000
                    )
                //
                // AIC line 631 (admac-aop-audio), published LAST, after all
                // four memory windows.  Below the carrier's 1019 limit, so it
                // is identity-mapped and needs NO CSRT ALI2 entry.
                //
                // MEASURED CAVEAT, recorded so nobody assumes otherwise:
                // AppleAopAudio contains no CmResourceTypeInterrupt handling
                // and no WdfInterrupt object at all.  This descriptor is
                // therefore inert for the shipped driver -- the capture ring
                // is still advanced by a 2 ms WDF timer and the RTKit/AFK
                // handshake still polls the ASC mailbox.  It is published so
                // the resource is already in the devnode when the driver grows
                // an ISR, and because the arbiter granting it now is evidence
                // it will grant it then.
                //
                // The mailbox lines 613/614/615/616 and dart-aop 628 are NOT
                // published.  They are equally legal, and each one is another
                // descriptor the arbiter must satisfy for a devnode that reads
                // none of them; 631 is the single line a future capture ISR
                // actually needs.  Adding the others is one line each.
                //
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, , , )
                {
                    631
                }
            })

            //
            // Descriptive properties for verification tooling and for the
            // interrupt work that is deliberately not done here.  None of
            // these is a _CRS resource and none can create a render path.
            //
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    // Stream format.  The AOP publishes exactly one and does
                    // no rate conversion; the driver refuses anything else.
                    Package () { "ntasp,aop-mic-rate-hz", 48000 },
                    Package () { "ntasp,aop-mic-channels", 3 },
                    Package () { "ntasp,aop-mic-sample-bits", 32 },
                    Package () { "ntasp,aop-mic-sample-format", "float32-le" },
                    Package () { "ntasp,aop-mic-period-bytes-min", 256 },
                    Package () { "ntasp,aop-mic-period-bytes-max", 16384 },

                    // DMA binding.  Both values are silent when wrong.
                    Package () { "ntasp,aop-admac-channel", 1 },
                    Package () { "ntasp,aop-admac-irq-output-index", 2 },
                    Package () { "ntasp,aop-dart-stream-aop", 0 },
                    Package () { "ntasp,aop-dart-stream-admac", 10 },
                    Package () { "ntasp,aop-dart-page-size", 16384 },

                    // AIC lines, published as data rather than as _CRS
                    // Interrupt() descriptors.  All are below 1019 and would
                    // therefore need no CSRT ALI2 alias.  See the header.
                    Package () { "ntasp,aop-aic-mailbox-0", 613 },
                    Package () { "ntasp,aop-aic-mailbox-1", 614 },
                    Package () { "ntasp,aop-aic-mailbox-2", 615 },
                    Package () { "ntasp,aop-aic-mailbox-3", 616 },
                    Package () { "ntasp,aop-aic-dart", 628 },
                    Package () { "ntasp,aop-aic-admac", 631 },
                    Package () { "ntasp,aop-interrupts-published", One },
                    Package () { "ntasp,aop-csrt-ali2-required", Zero },

                    // Coprocessor facts.
                    Package () { "ntasp,aop-mailbox-offset", 0x8000 },
                    Package () { "ntasp,aop-cpu-control-offset", 0x44 },
                    Package () { "ntasp,aop-cpu-run-bit", 0x10 },
                    Package () { "ntasp,aop-bootargs-ptr-offset", 0x22C },
                    Package () { "ntasp,aop-bootargs-size-offset", 0x230 },
                    Package () { "ntasp,aop-power-domain", "none" },
                    Package () { "ntasp,aop-firmware-preloaded", One },
                    // UNRESOLVED WITHOUT HARDWARE.  "unknown" is the honest
                    // value: `pre-loaded = 1` says the image is staged, not
                    // that the core runs, and the same flag is on ans, smc,
                    // sio, dcp0, isp0 and mtp, several of which this project
                    // demonstrably has to start itself.  The driver reads
                    // ASC + 0x44 and decides; BootPolicy = 3 (PROBE_ONLY)
                    // measures without writing anything.
                    Package () { "ntasp,aop-preboot-owner", "unknown" },

                    // PDM front end.  These are the dc-2400000 values from the
                    // machine's own ADT, which is the licence-clean source and
                    // also the correct one -- a different chassis carries a
                    // different table.
                    Package () { "ntasp,aop-pdm-frequency-hz", 2400000 },
                    Package () { "ntasp,aop-pdmc-frequency-hz", 24000000 },
                    Package () { "ntasp,aop-pdm-clock-source", "pll " },
                    Package () { "ntasp,aop-pdm-bytes-per-sample", 2 },
                    Package () { "ntasp,aop-pdm-filter-lengths", 0x00542C47 },
                    Package () { "ntasp,aop-pdm-ratio1", 15 },
                    Package () { "ntasp,aop-pdm-ratio2", 5 },
                    Package () { "ntasp,aop-pdm-ratio3", 2 },
                    Package () { "ntasp,aop-decimator-latency", 15 },
                    Package () { "ntasp,aop-mic-turn-on-time-ms", 20 },
                    Package () { "ntasp,aop-mic-settle-time-ms", 50 },
                    // RESOLVED: there is no 100-vs-120 conflict.  The wire
                    // field is a FIXED 120-slot u32 array (480 bytes) and the
                    // ADT's 400-byte table is the live tap set, which Asahi's
                    // literal reproduces byte-for-byte followed by 80 zero
                    // bytes.  The tap count is a function of filterLengths:
                    // 0x00542C47 -> (71,44,84) -> 36+22+42 = 100 u32.  The
                    // 120 that goes on the wire as `coeff_bulk` is the array
                    // capacity, not the tap count.
                    Package () { "ntasp,aop-pdm-coefficient-taps", 100 },
                    Package () { "ntasp,aop-pdm-coefficient-slots", 120 },
                    Package () { "ntasp,aop-pdm-coefficient-element", "u32" },
                    // Where the driver got its coefficients this boot.  The
                    // APDM buffer method below is NOT yet emitted, so the
                    // shipped driver uses its compiled-in J414s fallback and
                    // says so in IOCTL_NTASI_AOP_GET_PDM_PARAMS.
                    Package () { "ntasp,aop-pdm-source", "driver-fallback" },

                    // EPIC identifiers.  The ADT nodes are audio-pdm2 /
                    // audio-hp / audio-lp-mic-in, but their `identifier`
                    // properties -- which is what AUDIO_ATTACH_DEVICE carries
                    // -- are pdm0 / hpai / lpai.  Do NOT "correct" pdm0 to
                    // pdm2 because the node is called audio-pdm2.
                    Package () { "ntasp,aop-epic-device-pdm", "pdm0" },
                    Package () { "ntasp,aop-epic-device-hp", "hpai" },
                    Package () { "ntasp,aop-epic-device-lp", "lpai" },
                    Package () { "ntasp,aop-epic-service-name", "aop-audio" },

                    // Windows model.
                    Package () { "ntasp,aop-windows-model", "acx-capture" },
                    Package () { "ntasp,aop-render-path", "none" }
                }
            })

            //
            // APDM -- the whole PDM parameter set as one ACPI buffer, so the
            // driver makes a single IOCTL_ACPI_EVAL_METHOD round trip instead
            // of fifteen and so the entire decode is host-testable.  The
            // layout is documented in drivers/AppleAopAudio/AppleAopAudioCore.h
            // ("ACPI APDM platform-data blob"): a 0x40-byte little-endian
            // header beginning with the bytes 'A','P','D','M', then the
            // coefficient table.
            //
            // NOT YET EMITTED.  Filling this in means transcribing the
            // dc-2400000 `coefficients` property out of the pinned live-ADT
            // capture, which is private hardware evidence this file cannot
            // reach.  Until a generator exists, the method is deliberately
            // absent rather than present-and-wrong: the driver treats a
            // missing APDM as "use the compiled-in J414s table", records
            // FromAcpi = 0 in its telemetry, and validates whatever it ends up
            // with against filterLengths before configuring anything.
            //
            // Method (APDM, 0, NotSerialized) { Return (Buffer () { ... }) }
            //

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}

/** @file
  J414s internal display adapter ACPI device for the AppleDisplay WDDM
  display driver (rungs (a) and (b) of docs/j414s-display-driver.md).

  The inherited framebuffer is deliberately NOT an ACPI resource.  The WDDM
  display-only path takes its boot-specific physical address, geometry, and
  format from dxgkrnl at StartDevice via
  DxgkCbAcquirePostDisplayOwnership (the same POST-display buffer Mu's
  SimpleFbDxe published as the one-mode GOP). Embedding that framebuffer base
  here would be wrong twice over: the /vram carveout base is
  boot-specific (m1n1 rewrites boot_args.video.base each boot,
  src/display.c:586-606), and it would inject the scanout into Windows PnP
  arbitration for no benefit.

  The fixed _CRS ranges below are instead the exact six non-overlapping DCP
  and DART MMIO apertures required by rung (b). REG3 contains both the ASC
  CPU block at +0x400000 and mailbox at +0x408000, so those subranges are not
  published a second time. Keeping these resources on the display adapter
  avoids a second PnP driver and an undocumented cross-device lifetime
  protocol: DxgkDdiStartDevice receives the translated resource list and
  validates it before mapping anything.

  The _DSD properties are the pinned J414s geometry, provided ONLY as a
  cross-check for offline verification tooling. NOTE: as of this revision the
  AppleDisplay driver reads NONE of them -- it treats
  DxgkCbAcquirePostDisplayOwnership as authoritative and derives geometry from
  AppleDisplayModeCore. They are kept because the verification tooling and a
  human reading the SSDT both benefit, but no code path depends on them, and
  the earlier claim that they were "a cross-check for the driver" overstated
  what the driver does.

  INTERRUPTS ARE GATED, AND OFF BY DEFAULT.

  The five AIC lines the display path needs are the DCP ASC mailbox quad
  932-935 (send-empty, send-not-empty, recv-empty, recv-not-empty) and the
  DART fault line 911, which dcp_dart and disp0_dart share. All five are
  below the GIC carrier's 1019 limit, so each is a legal plain GSIV needing no
  CSRT ALI2 alias -- the same property AppleIsp relies on for AIC 569. None is
  claimed by any other ACPI device on this platform.

  They are nonetheless behind NTASI_ENABLE_DISPLAY_INTERRUPTS, off by default,
  because publishing a resource is a PROMISE PnP must be able to keep. If any
  one of the five cannot be routed, the device does not start AT ALL -- and
  that would cost the working rung-(a) path, whose whole design rule is that
  every failure leaves BasicDisplay owning the panel. Turning interrupts on to
  chase lower present latency, and losing the display in exchange, is a bad
  trade to make by default. MCA.asl documents the same hazard for its own five
  lines.

  Off means preprocessor-excluded, so a profile without it produces
  byte-identical firmware rather than merely equivalent firmware, and adds no
  GSIV allocation and no CSRT byte.

  The set is published atomically because the driver's inventory validator is
  all-or-none: zero interrupts selects bounded polling, exactly
  {911,932,933,934,935} enables the ISR path, and anything else is rejected
  (drivers/AppleDisplay/AppleDisplayDcpResourceCore.c). Partial publication
  cannot be expressed here and must not be attempted.

  The driver additionally requires each descriptor to be Level-triggered,
  ActiveHigh and Exclusive, and rejects message-signalled or latched
  descriptors, so the Interrupt() flags below are a contract, not a style
  choice. Level/ActiveHigh matches the device tree, which declares all five
  IRQ_TYPE_LEVEL_HIGH.

  UNPROVEN, and only hardware can settle it: that GSIV 935 is the line AIC
  actually raises for an inbound DCP message, and that a level-triggered AIC
  line routed to a display-only miniport is delivered at all under this HAL.
  Publishing them is necessary for that experiment and is not itself evidence.

  Provenance:
    Pinned scanout geometry 3024x1964 BGRA32 stride 12096: WIP.md display
      note (commit 474037b), tools/m1n1-windows-debug.py win_capture_framebuffer.
    Panel 302 x 196 mm: Asahi linux-asahi t6020-j414s.dts panel node; also
      the argument Mu SimpleFbDxe passes to its synthesized EDID
      (SimpleFbDxe.c:369).
    _HID NTAS0070: next free NTASP ACPI id (drivers/ grep NTAS0001..0067).

  SPDX-License-Identifier: MIT
**/

DefinitionBlock ("DISP.aml", "SSDT", 0x02, "Apple", "J414DSP", 0x00000002)
{
    Scope (\_SB)
    {
        Device (DISP)
        {
            Name (_HID, "NTAS0070")
            Name (_UID, Zero)
            Name (_CCA, One)

            Name (_CRS, ResourceTemplate ()
            {
                // DCP disp-0 register aperture.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000388000000,
                    0x000000038861BFFF,
                    0x0000000000000000,
                    0x000000000061C000
                    )
                // DCP firmware DART (stream 5).
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x000000038930C000,
                    0x000000038930FFFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // DISP0 scanout DART (stream 0; piodma stream 4).
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000389304000,
                    0x0000000389307FFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // DCP disp-1 register aperture.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000389320000,
                    0x0000000389323FFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // DCP disp-2 register aperture.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000389344000,
                    0x0000000389347FFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // DCP disp-3 aperture, including ASC CPU and mailbox.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000389800000,
                    0x0000000389FFFFFF,
                    0x0000000000000000,
                    0x0000000000800000
                    )
#if NTASI_ENABLE_DISPLAY_INTERRUPTS
                //
                // DART fault (911, shared by dcp_dart and disp0_dart) and the
                // DCP ASC mailbox quad: 932 send-empty, 933 send-not-empty,
                // 934 recv-empty, 935 recv-not-empty.  Inbound DCP traffic --
                // including the D589 swap-complete callback that is this
                // platform's only present-completion signal -- arrives on 935.
                //
                // Published as ONE descriptor with five vectors, which the
                // ACPI driver expands into five CmResourceTypeInterrupt
                // partial descriptors.  Order is irrelevant: the driver
                // normalises by value, not by position.
                //
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, , , )
                {
                    911,
                    932,
                    933,
                    934,
                    935
                }
#endif
            })

            //
            // Geometry cross-check only. Keep in lockstep
            // with drivers/AppleDisplay/AppleDisplayModeCore.h and the
            // firmware EDID Mu already publishes.
            //
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "ntasp,fb-width", 3024 },
                    Package () { "ntasp,fb-height", 1964 },
                    Package () { "ntasp,fb-stride", 12096 },
                    Package () { "ntasp,fb-format", "BGRA8888" },
                    Package () { "ntasp,panel-width-mm", 302 },
                    Package () { "ntasp,panel-height-mm", 196 },
                    Package () { "ntasp,preboot-owner", "m1n1-dcp-then-quiesced" },
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

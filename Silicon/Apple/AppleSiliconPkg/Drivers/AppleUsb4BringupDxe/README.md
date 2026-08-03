# Apple USB4 Type5 bring-up

Status: host-tested core only. The directory is intentionally absent from the
platform DSC/FDF and cannot execute during boot.

## Source pins

- Asahi Linux `sven/tbt-wip`: `5265e38457df79188be2e13870192a1d488831ac`
- Asahi m1n1 `tbt`: `3e781e2d2582c0c05d70a243d5fb40ad9e253d81`
- `T6020_TYPE5_ACIO_HAL_CONTRACT_2026-07-29.md`:
  `2f99767f10ccd898703593bd36a26496662a36ebf328739ce8cbce03878b4fb8`
- `T6020_TYPE5_NHI_RING_TRANSPORT_2026-07-29.md`:
  `76601a51a1d2de7443145cc12fedc0eca88e989f48045bbda5369e89ce580bc3`
- `T6020_TYPE5_CONNECTION_MANAGER_TUNNELS_2026-07-29.md`:
  `a6a97336539866d583d4755713e3535ad5dc6c41acff958299f9b4c073bfbf3b`

The three contracts live in the external Asahi research corpus under
`atcphy/`. They supersede generation-
incorrect values in the current Linux WIP for J414s: Type5 has six mapped
ranges, no CTRL/INIT aperture, 0x4000 ring pages, a Type5 PDF aperture, and a
separate `usb-auss` consumer for tunnelled USB3.

## Implemented and host tested

- power state 5 on domains 0/1/2, HW/PHY state 2, RTKit boot, exact firmware
  ready value, a six-range tunables callback, power state 7 on domains 0/1/2/4;
- rollback of attempted partial power, HW, PHY, RTKit, DART, and NHI starts;
- nonzero cable-state transition protection;
- Type5 NHI geometry, DART IOVA alignment, descriptors, doorbells, PDF mirror,
  interrupt mask, disable deadline, and completion ordering;
- endian-safe config read/write packets plus an explicitly named raw-host
  payload API matching Apple's descriptor-copy path, CRC32C, reply bit,
  route/address/sequence/payload matching, and Apple VSE traversal;
  CRC32C is sourced from the pinned Asahi Linux/spec implementation; the local
  BootKC contract does not independently resolve Apple's private CRC routine;
- grade-A hop descriptor encoding with unresolved credit bit 24 rejected by
  bounding credits to seven bits;
- USB3 minimum TMU mode 1, exact Tx/Rx hop-8 path parameters, capability 0x04
  discovery, upstream adapter enable, nonblocking 100 ms deadline, downstream
  adapter enable, and attempted-operation reverse-order rollback.

Run:

```sh
python3 -m unittest Tests.test_apple_usb4_core -v
```

## Runtime gates

Do not add this driver to the DSC/FDF until all are implemented and tested:

1. exact six-resource ACPI binding and map-only-after-power behavior;
2. `function-dart_force_active`, ADT-selected `iommu-parent` SID partition,
   and contiguous 4 KiB-aligned IOVAs. Apple accepts 1 shared, 2 directional,
   or 24 per-ring mappers; live J414s `acio1` has one mapper, SID 1;
3. ACIO firmware/RTKit ownership and SRAM bounds;
4. NHI reset, ring0 config control path, PDF/sequence dispatch, and interrupts;
5. static root-router construction from DROM/portmap and ready/CV/ack handshake;
6. router scan, hop/counter allocation, NFC atomic compare-swap, hop activation,
   and rollback;
7. `usb-auss` xHCI ownership and handoff to Windows;
8. ATCPHY routed PIPE state `0x11` only after the USB3 tunnel is ready;
9. cold/warm reset, disconnect, timeout, and cable-mode transition tests.

No register value may be inferred from the Type7/CIO80 generation. Unknowns
remain fail-closed.

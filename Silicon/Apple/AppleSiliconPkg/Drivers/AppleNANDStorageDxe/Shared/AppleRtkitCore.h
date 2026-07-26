/* SPDX-License-Identifier: MIT */
#ifndef NTASI_APPLE_RTKIT_H
#define NTASI_APPLE_RTKIT_H

/*
 * Apple RTKit coprocessor mailbox message codec, host-testable seam.
 *
 * Ported from the MIT-licensed m1n1 driver (repos/m1n1/src/rtkit.c, asc.c).
 * RTKit is the message transport almost every Apple coprocessor speaks (ANS/
 * NVMe, DCP display, SMC, SEP, ISP, SIO). A message is a 64-bit payload plus a
 * one-byte endpoint (the ASC mailbox carries msg0=payload, msg1=endpoint).
 *
 * This seam models the pure field packing of the management-endpoint (EP 0)
 * handshake — message type, HELLO version negotiation, and the EPMAP endpoint
 * bitmap — with no MMIO/UEFI/WDK deps. The Windows KMDF transport and the
 * firmware can share one verified codec.
 */

#include <stdbool.h>
#include <stdint.h>

#define NTASI_RTKIT_GENMASK(h, l) \
    ((((uint64_t)~0ull) - (((uint64_t)1 << (l)) - 1)) & \
     (((uint64_t)~0ull) >> (63 - (h))))

/* Management message type field (m1n1 MGMT_TYPE). */
#define NTASI_RTKIT_MGMT_TYPE_MASK  NTASI_RTKIT_GENMASK(59, 52)
#define NTASI_RTKIT_MGMT_TYPE_SHIFT 52u

/* System endpoints (m1n1). */
#define NTASI_RTKIT_EP_MGMT     0
#define NTASI_RTKIT_EP_CRASHLOG 1
#define NTASI_RTKIT_EP_SYSLOG   2
#define NTASI_RTKIT_EP_DEBUG    3
#define NTASI_RTKIT_EP_IOREPORT 4
#define NTASI_RTKIT_EP_OSLOG    8

/* Management message types. */
#define NTASI_RTKIT_MGMT_HELLO     1
#define NTASI_RTKIT_MGMT_HELLO_ACK 2
#define NTASI_RTKIT_MGMT_EPMAP     8

/* HELLO version subfields. */
#define NTASI_RTKIT_HELLO_MINVER_MASK  NTASI_RTKIT_GENMASK(15, 0)
#define NTASI_RTKIT_HELLO_MAXVER_MASK  NTASI_RTKIT_GENMASK(31, 16)
#define NTASI_RTKIT_HELLO_MAXVER_SHIFT 16u

/* Version window this codec supports (m1n1 RTKIT_MIN/MAX_VERSION). */
#define NTASI_RTKIT_MIN_VERSION 11u
#define NTASI_RTKIT_MAX_VERSION 12u

/* EPMAP subfields. */
#define NTASI_RTKIT_EPMAP_DONE        (1ull << 51)
#define NTASI_RTKIT_EPMAP_BASE_MASK   NTASI_RTKIT_GENMASK(34, 32)
#define NTASI_RTKIT_EPMAP_BASE_SHIFT  32u
#define NTASI_RTKIT_EPMAP_BITMAP_MASK NTASI_RTKIT_GENMASK(31, 0)

/* The ASC mailbox endpoint word is a byte; recv rejects values >= 0x100. */
#define NTASI_RTKIT_EP_LIMIT 0x100u

/* Management message type of a 64-bit payload. */
uint8_t ntasi_rtkit_mgmt_type(uint64_t msg);

/* Set the management type field into a base payload (clears then sets it). */
uint64_t ntasi_rtkit_with_mgmt_type(uint64_t base, uint8_t type);

/* Parse a HELLO message's supported [min,max] version window. */
void ntasi_rtkit_hello_parse(uint64_t msg, uint16_t *min_ver, uint16_t *max_ver);

/*
 * Negotiate the boot version against a peer's [min,max], mirroring m1n1:
 *   want = min(MAX_VERSION, peer_max); ok iff [MIN,MAX] overlaps [peer_min,peer_max].
 * *want_ver is always written; returns whether the windows overlap.
 */
bool ntasi_rtkit_hello_negotiate(uint16_t peer_min, uint16_t peer_max, uint16_t *want_ver);

/* Build the HELLO_ACK payload selecting `want_ver` for both min and max. */
uint64_t ntasi_rtkit_hello_ack(uint16_t want_ver);

/* Parse an EPMAP message. */
void ntasi_rtkit_epmap_parse(uint64_t msg, uint8_t *base, uint32_t *bitmap, bool *last);

/* Endpoint index for bit `bit` (0..31) at EPMAP base `base`: 32*base + bit. */
uint8_t ntasi_rtkit_epmap_endpoint(uint8_t base, unsigned int bit);

/* Whether an endpoint value is representable in the mailbox endpoint byte. */
bool ntasi_rtkit_ep_valid(uint32_t ep);

#endif

/** @file
  Portable USB4/Thunderbolt router configuration-channel codec, derived from
  Asahi Linux sven/tbt-wip 5265e38457df79188be2e13870192a1d488831ac
  drivers/thunderbolt/{ctl.c,ctl.h,tb_msgs.h}.

  SPDX-License-Identifier: GPL-2.0-only
**/

#ifndef AURORA_APPLE_USB4_ROUTER_CORE_H
#define AURORA_APPLE_USB4_ROUTER_CORE_H

#include <stddef.h>
#include <stdint.h>

#define AS_USB4_CFG_MAX_DWORDS 63u
#define AS_USB4_CFG_HEADER_SIZE 12u
#define AS_USB4_CFG_CRC_SIZE 4u

enum as_usb4_cfg_space {
    AS_USB4_CFG_HOPS = 0,
    AS_USB4_CFG_PORT = 1,
    AS_USB4_CFG_SWITCH = 2,
    AS_USB4_CFG_COUNTERS = 3,
};

enum as_usb4_cfg_operation {
    AS_USB4_CFG_READ = 0,
    AS_USB4_CFG_WRITE = 1,
};

enum as_usb4_cfg_result {
    AS_USB4_CFG_OK = 0,
    AS_USB4_CFG_ERR_ARGUMENT = -1,
    AS_USB4_CFG_ERR_ROUTE = -2,
    AS_USB4_CFG_ERR_CRC = -3,
    AS_USB4_CFG_ERR_RESPONSE = -4,
    AS_USB4_CFG_ERR_NOT_FOUND = -5,
    AS_USB4_CFG_ERR_CAPABILITY = -6,
};

#define AS_USB4_SWITCH_CAP_TMU 0x03u
#define AS_USB4_SWITCH_CAP_VSE 0x05u
#define AS_USB4_APPLE_VSE 0x00u
#define AS_USB4_APPLE_VSE_CABLE_INFO 0x01u
#define AS_USB4_ROUTER_CS_5 5u
#define AS_USB4_ROUTER_CS_6 6u
#define AS_USB4_ROUTER_READY (1u << 24)
#define AS_USB4_ROUTER_CONFIG_ACK (1u << 25)
#define AS_USB4_ROUTER_CV (1u << 31)
#define AS_USB4_ROUTER_USB_TUNNELS 0x03000000u
#define AS_USB4_ROUTER_NO_USB_TUNNELS 0x05000000u

struct as_usb4_cfg_address {
    uint16_t offset;
    uint8_t length;
    uint8_t port;
    uint8_t space;
    uint8_t sequence;
};

typedef int (*as_usb4_cfg_read_fn)(void *context, uint64_t route,
                                   uint8_t space, uint16_t offset,
                                   uint8_t length, uint32_t *words);

struct as_usb4_router_ops {
    int (*read32)(void *context, uint64_t route, uint8_t port,
                  uint8_t space, uint16_t offset, uint32_t *value);
    int (*write32)(void *context, uint64_t route, uint8_t port,
                   uint8_t space, uint16_t offset, uint32_t value);
    int (*poll32)(void *context, uint64_t route, uint8_t port,
                  uint8_t space, uint16_t offset, uint32_t value,
                  uint32_t mask);
};

uint32_t as_usb4_crc32c(const void *data, size_t length);
int as_usb4_cfg_encode_read(uint8_t *wire, size_t capacity, uint64_t route,
                            const struct as_usb4_cfg_address *address,
                            size_t *wire_length);
int as_usb4_cfg_encode_write(uint8_t *wire, size_t capacity, uint64_t route,
                             const struct as_usb4_cfg_address *address,
                             const uint32_t *data, size_t *wire_length);
int as_usb4_cfg_encode_write_raw_host(
    uint8_t *wire, size_t capacity, uint64_t route,
    const struct as_usb4_cfg_address *address, const uint32_t *host_memory,
    size_t *wire_length);
int as_usb4_cfg_decode(uint32_t *words, size_t word_capacity,
                       const uint8_t *wire, size_t wire_length,
                       int expect_reply,
                       uint64_t *route, struct as_usb4_cfg_address *address,
                       size_t *word_count);
int as_usb4_cfg_decode_response(
    uint32_t *words, size_t word_capacity, const uint8_t *wire,
    size_t wire_length, enum as_usb4_cfg_operation operation,
    uint64_t expected_route, const struct as_usb4_cfg_address *expected,
    size_t *word_count);
int as_usb4_find_switch_vse(as_usb4_cfg_read_fn read, void *context,
                            uint64_t route, uint16_t first_capability,
                            uint8_t vendor_capability, uint16_t *offset);
int as_usb4_router_configure(const struct as_usb4_router_ops *ops,
                             void *context, uint64_t route,
                             int all_parents_support_usb_tunnels);

#endif

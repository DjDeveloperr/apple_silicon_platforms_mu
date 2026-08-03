/** @file
  Host-testable Type5 hop encoder and USB3 tunnel activation state machine.

  Derived from the grade-A J414s Type5 BootKC contracts in
  T6020_TYPE5_CONNECTION_MANAGER_TUNNELS_2026-07-29.md. Runtime integration
  remains fail-closed until the platform ACIO, DART, RTKit, router, and
  usb-auss owners are all present.

  SPDX-License-Identifier: GPL-2.0-only
**/

#ifndef AURORA_APPLE_USB4_TUNNEL_CORE_H
#define AURORA_APPLE_USB4_TUNNEL_CORE_H

#include <stddef.h>
#include <stdint.h>

#define AS_USB4_USB3_HOP_ID 8u
#define AS_USB4_USB3_ADAPTER_CAPABILITY 0x04u
#define AS_USB4_USB3_ADAPTER_MASK 0xc0000000u
#define AS_USB4_USB3_ADAPTER_ENABLE 0xc0000000u
#define AS_USB4_USB3_ADAPTER_DISABLE 0x40000000u
#define AS_USB4_USB3_SETTLE_US 100000u

enum as_usb4_tunnel_result {
    AS_USB4_TUNNEL_OK = 0,
    AS_USB4_TUNNEL_PENDING = 1,
    AS_USB4_TUNNEL_ERR_ARGUMENT = -1,
    AS_USB4_TUNNEL_ERR_PATH = -2,
    AS_USB4_TUNNEL_ERR_ADAPTER = -3,
    AS_USB4_TUNNEL_ERR_TEARDOWN = -4,
};

enum as_usb4_usb3_path {
    AS_USB4_USB3_PATH_TX = 0,
    AS_USB4_USB3_PATH_RX = 1,
};

struct as_usb4_usb3_path_config;

struct as_usb4_hop_fields {
    uint16_t out_hop;
    uint16_t counter;
    uint8_t out_port;
    uint8_t initial_credits;
    uint8_t priority;
    uint8_t weight;
    uint8_t valid;
    uint8_t pm_packet;
    uint8_t suppress_route;
    uint8_t egress_shared_buffering;
    uint8_t ingress_shared_buffering;
    uint8_t egress_flow_control;
    uint8_t ingress_flow_control;
    uint8_t counter_enable;
    uint8_t drop_packet;
};

struct as_usb4_usb3_ops {
    int (*set_minimum_tmu_mode)(void *context, uint32_t mode);
    int (*path_activate)(void *context, enum as_usb4_usb3_path path,
                         const struct as_usb4_usb3_path_config *config);
    int (*path_deactivate)(void *context, enum as_usb4_usb3_path path);
    int (*adapter_find_capability)(void *context, uint64_t route,
                                   uint8_t port, uint16_t capability,
                                   uint16_t *offset);
    int (*adapter_enable)(void *context, uint64_t route, uint8_t port,
                          uint16_t capability_offset, int upstream,
                          int enable, uint32_t value, uint32_t mask);
    uint64_t (*now_us)(void *context);
};

struct as_usb4_usb3_path_config {
    uint16_t source_hop;
    uint16_t destination_hop;
    uint8_t path_type;
    uint8_t priority;
    uint8_t weight;
    uint8_t source_initial_credits;
    uint8_t initial_credits;
    uint8_t destination_initial_credits;
    uint8_t counter_enable;
    uint8_t ingress_flow_control;
    uint8_t egress_flow_control;
    uint8_t ingress_shared_buffering;
    uint8_t egress_shared_buffering;
    uint8_t routing_options;
    uint8_t credit_options;
};

struct as_usb4_usb3_tunnel {
    const struct as_usb4_usb3_ops *ops;
    void *context;
    uint64_t upstream_route;
    uint64_t downstream_route;
    uint64_t settle_deadline_us;
    uint16_t upstream_capability;
    uint16_t downstream_capability;
    uint8_t upstream_port;
    uint8_t downstream_port;
    uint8_t use_recommended_credits;
    uint8_t tx_attempted;
    uint8_t tx_active;
    uint8_t rx_attempted;
    uint8_t rx_active;
    uint8_t upstream_attempted;
    uint8_t upstream_enabled;
    uint8_t downstream_attempted;
    uint8_t downstream_enabled;
    uint8_t waiting_settle;
};

int as_usb4_hop_encode(uint8_t wire[8],
                       const struct as_usb4_hop_fields *fields);
int as_usb4_usb3_tunnel_start(struct as_usb4_usb3_tunnel *tunnel);
int as_usb4_usb3_tunnel_continue(struct as_usb4_usb3_tunnel *tunnel);
int as_usb4_usb3_tunnel_stop(struct as_usb4_usb3_tunnel *tunnel);

#endif

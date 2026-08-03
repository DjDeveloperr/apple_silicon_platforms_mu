/** @file
  SPDX-License-Identifier: GPL-2.0-only
**/

#include "AppleUsb4TunnelCore.h"

static void
store_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

int
as_usb4_hop_encode(uint8_t wire[8], const struct as_usb4_hop_fields *fields)
{
    uint32_t word0;
    uint32_t word1;

    if (wire == NULL || fields == NULL || fields->out_hop > 0x7ffu ||
        fields->counter > 0x7ffu || fields->out_port > 0x3fu ||
        fields->initial_credits > 0x7fu || fields->priority > 7u)
        return AS_USB4_TUNNEL_ERR_ARGUMENT;

    word0 = fields->valid ? 0x80000000u : 0;
    if (fields->pm_packet)
        word0 |= 1u << 24;
    word0 |= (uint32_t)fields->initial_credits << 17;
    if (!fields->suppress_route)
        word0 |= (uint32_t)fields->out_port << 11 | fields->out_hop;

    word1 = fields->egress_shared_buffering ? 1u << 27 : 0;
    word1 |= fields->ingress_shared_buffering ? 1u << 26 : 0;
    word1 |= fields->egress_flow_control ? 1u << 25 : 0;
    word1 |= fields->ingress_flow_control ? 1u << 24 : 0;
    word1 |= fields->counter_enable ? 1u << 23 : 0;
    word1 |= (uint32_t)fields->counter << 12;
    word1 |= fields->drop_packet ? 1u << 11 : 0;
    word1 |= (uint32_t)fields->priority << 8;
    word1 |= fields->weight;

    store_be32(wire, word0);
    store_be32(wire + 4, word1);
    return AS_USB4_TUNNEL_OK;
}

static struct as_usb4_usb3_path_config
usb3_path_config(enum as_usb4_usb3_path path, int recommended)
{
    struct as_usb4_usb3_path_config config = {
        .source_hop = AS_USB4_USB3_HOP_ID,
        .destination_hop = AS_USB4_USB3_HOP_ID,
        .path_type = path == AS_USB4_USB3_PATH_TX ? 8 : 7,
        .priority = 3,
        .weight = 3,
        .source_initial_credits = 2,
        .initial_credits = 14,
        .destination_initial_credits = 14,
        .counter_enable = 7,
        .ingress_flow_control = 7,
        .egress_flow_control = 3,
        .routing_options = 0x17,
        .credit_options = recommended ? 4 : 0,
    };
    return config;
}

int
as_usb4_usb3_tunnel_stop(struct as_usb4_usb3_tunnel *tunnel)
{
    int failed = 0;

    if (tunnel == NULL || tunnel->ops == NULL)
        return AS_USB4_TUNNEL_ERR_ARGUMENT;
    if (tunnel->downstream_attempted && tunnel->ops->adapter_enable != NULL &&
        tunnel->ops->adapter_enable(tunnel->context, tunnel->downstream_route,
                                    tunnel->downstream_port,
                                    tunnel->downstream_capability, 0, 0,
                                    AS_USB4_USB3_ADAPTER_DISABLE,
                                    AS_USB4_USB3_ADAPTER_MASK) != 0)
        failed = 1;
    tunnel->downstream_attempted = 0;
    tunnel->downstream_enabled = 0;
    if (tunnel->upstream_attempted && tunnel->ops->adapter_enable != NULL &&
        tunnel->ops->adapter_enable(tunnel->context, tunnel->upstream_route,
                                    tunnel->upstream_port,
                                    tunnel->upstream_capability, 1, 0,
                                    AS_USB4_USB3_ADAPTER_DISABLE,
                                    AS_USB4_USB3_ADAPTER_MASK) != 0)
        failed = 1;
    tunnel->upstream_attempted = 0;
    tunnel->upstream_enabled = 0;
    if (tunnel->rx_attempted && tunnel->ops->path_deactivate != NULL &&
        tunnel->ops->path_deactivate(tunnel->context,
                                     AS_USB4_USB3_PATH_RX) != 0)
        failed = 1;
    tunnel->rx_attempted = 0;
    tunnel->rx_active = 0;
    if (tunnel->tx_attempted && tunnel->ops->path_deactivate != NULL &&
        tunnel->ops->path_deactivate(tunnel->context,
                                     AS_USB4_USB3_PATH_TX) != 0)
        failed = 1;
    tunnel->tx_attempted = 0;
    tunnel->tx_active = 0;
    tunnel->waiting_settle = 0;
    tunnel->settle_deadline_us = 0;
    return failed ? AS_USB4_TUNNEL_ERR_TEARDOWN : AS_USB4_TUNNEL_OK;
}

int
as_usb4_usb3_tunnel_start(struct as_usb4_usb3_tunnel *tunnel)
{
    const struct as_usb4_usb3_ops *ops;

    if (tunnel == NULL || tunnel->ops == NULL)
        return AS_USB4_TUNNEL_ERR_ARGUMENT;
    ops = tunnel->ops;
    if (ops->set_minimum_tmu_mode == NULL || ops->path_activate == NULL ||
        ops->path_deactivate == NULL ||
        ops->adapter_find_capability == NULL || ops->adapter_enable == NULL ||
        ops->now_us == NULL || tunnel->upstream_route >> 54 ||
        tunnel->downstream_route >> 54 || tunnel->upstream_port > 0x3f ||
        tunnel->downstream_port > 0x3f ||
        tunnel->tx_active || tunnel->rx_active || tunnel->upstream_enabled ||
        tunnel->downstream_enabled || tunnel->waiting_settle)
        return AS_USB4_TUNNEL_ERR_ARGUMENT;

    if (ops->set_minimum_tmu_mode(tunnel->context, 1) != 0)
        return AS_USB4_TUNNEL_ERR_PATH;
    struct as_usb4_usb3_path_config tx = usb3_path_config(
        AS_USB4_USB3_PATH_TX, tunnel->use_recommended_credits);
    struct as_usb4_usb3_path_config rx = usb3_path_config(
        AS_USB4_USB3_PATH_RX, tunnel->use_recommended_credits);
    tunnel->tx_attempted = 1;
    if (ops->path_activate(tunnel->context, AS_USB4_USB3_PATH_TX, &tx) != 0) {
        as_usb4_usb3_tunnel_stop(tunnel);
        return AS_USB4_TUNNEL_ERR_PATH;
    }
    tunnel->tx_active = 1;
    tunnel->rx_attempted = 1;
    if (ops->path_activate(tunnel->context, AS_USB4_USB3_PATH_RX, &rx) != 0) {
        as_usb4_usb3_tunnel_stop(tunnel);
        return AS_USB4_TUNNEL_ERR_PATH;
    }
    tunnel->rx_active = 1;
    if (ops->adapter_find_capability(tunnel->context, tunnel->upstream_route,
                                     tunnel->upstream_port,
                                     AS_USB4_USB3_ADAPTER_CAPABILITY,
                                     &tunnel->upstream_capability) != 0 ||
        tunnel->upstream_capability == 0 ||
        tunnel->upstream_capability > 0x1fff) {
        as_usb4_usb3_tunnel_stop(tunnel);
        return AS_USB4_TUNNEL_ERR_ADAPTER;
    }
    tunnel->upstream_attempted = 1;
    if (ops->adapter_enable(tunnel->context, tunnel->upstream_route,
                            tunnel->upstream_port,
                            tunnel->upstream_capability, 1, 1,
                            AS_USB4_USB3_ADAPTER_ENABLE,
                            AS_USB4_USB3_ADAPTER_MASK) != 0) {
        as_usb4_usb3_tunnel_stop(tunnel);
        return AS_USB4_TUNNEL_ERR_ADAPTER;
    }
    tunnel->upstream_enabled = 1;
    tunnel->settle_deadline_us = ops->now_us(tunnel->context);
    if (tunnel->settle_deadline_us > UINT64_MAX - AS_USB4_USB3_SETTLE_US) {
        as_usb4_usb3_tunnel_stop(tunnel);
        return AS_USB4_TUNNEL_ERR_ARGUMENT;
    }
    tunnel->settle_deadline_us += AS_USB4_USB3_SETTLE_US;
    tunnel->waiting_settle = 1;
    return AS_USB4_TUNNEL_PENDING;
}

int
as_usb4_usb3_tunnel_continue(struct as_usb4_usb3_tunnel *tunnel)
{
    if (tunnel == NULL || tunnel->ops == NULL ||
        tunnel->ops->adapter_enable == NULL || tunnel->ops->now_us == NULL ||
        !tunnel->waiting_settle || !tunnel->upstream_enabled)
        return AS_USB4_TUNNEL_ERR_ARGUMENT;
    if (tunnel->ops->now_us(tunnel->context) < tunnel->settle_deadline_us)
        return AS_USB4_TUNNEL_PENDING;
    if (tunnel->ops->adapter_find_capability(
            tunnel->context, tunnel->downstream_route,
            tunnel->downstream_port, AS_USB4_USB3_ADAPTER_CAPABILITY,
            &tunnel->downstream_capability) != 0 ||
        tunnel->downstream_capability == 0 ||
        tunnel->downstream_capability > 0x1fff) {
        as_usb4_usb3_tunnel_stop(tunnel);
        return AS_USB4_TUNNEL_ERR_ADAPTER;
    }
    tunnel->downstream_attempted = 1;
    if (tunnel->ops->adapter_enable(tunnel->context,
                                    tunnel->downstream_route,
                                    tunnel->downstream_port,
                                    tunnel->downstream_capability, 0, 1,
                                    AS_USB4_USB3_ADAPTER_ENABLE,
                                    AS_USB4_USB3_ADAPTER_MASK) != 0) {
        as_usb4_usb3_tunnel_stop(tunnel);
        return AS_USB4_TUNNEL_ERR_ADAPTER;
    }
    tunnel->downstream_enabled = 1;
    tunnel->waiting_settle = 0;
    return AS_USB4_TUNNEL_OK;
}

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../Silicon/Apple/AppleSiliconPkg/Drivers/AppleUsb4BringupDxe/AppleUsb4Core.h"
#include "../Silicon/Apple/AppleSiliconPkg/Drivers/AppleUsb4BringupDxe/AppleUsb4RouterCore.h"
#include "../Silicon/Apple/AppleSiliconPkg/Drivers/AppleUsb4BringupDxe/AppleUsb4TunnelCore.h"

struct fake {
    uint32_t m3;
    uint32_t delays;
    uint32_t power_calls;
    uint32_t power_state[3];
    uint32_t power_mask[3];
    uint32_t hw_on;
    uint32_t phy_on;
    uint32_t stopped_nhi;
    uint32_t stopped_rtkit;
    uint32_t stopped_dma;
    int fail_nhi;
    uint64_t pdf_address;
    uint32_t pdf_value;
    uint32_t registers[0x100000 / 4];
};

static uint32_t read32(void *ctx, uint64_t address) {
    struct fake *f = ctx;
    if (address >= 0x300000 && address < 0x400000)
        return f->registers[(address - 0x300000) / 4];
    if (address >= 0x500000 && address < 0x540000)
        return address == f->pdf_address ? f->pdf_value : 0;
    return (address & 0xfff) == AS_USB4_ACIO_M3_STAT ? f->m3 : 0;
}
static void write32(void *ctx, uint64_t address, uint32_t value) {
    struct fake *f = ctx;
    if (address >= 0x300000 && address < 0x400000) {
        uint32_t offset = (uint32_t)(address - 0x300000);
        f->registers[offset / 4] = value;
        if (!(value & AS_USB4_RING_FLAG_ENABLE)) {
            if (offset >= AS_USB4_NHI_TX_RING_DESC_BASE &&
                offset < AS_USB4_NHI_RX_RING_DESC_BASE &&
                (offset - AS_USB4_NHI_TX_RING_DESC_BASE) %
                            AS_USB4_NHI_RING_STRIDE == 0x10)
                f->registers[(offset + 0x0c) / 4] |= 1;
            if (offset >= AS_USB4_NHI_RX_RING_DESC_BASE &&
                (offset - AS_USB4_NHI_RX_RING_DESC_BASE) %
                            AS_USB4_NHI_RING_STRIDE == 0x10)
                f->registers[(offset + 0x08) / 4] |= 1;
        }
        return;
    }
    if (address >= 0x500000 && address < 0x540000) {
        f->pdf_address = address;
        f->pdf_value = value;
        return;
    }
    assert((address & 0xfff) == AS_USB4_ACIO_M3_CTRL);
    assert(value == AS_USB4_ACIO_M3_CTRL_LSTX);
    f->m3 = AS_USB4_ACIO_M3_STAT_READY;
}
static void delay_us(void *ctx, uint32_t usec) { (void)usec; ((struct fake *)ctx)->delays++; }
static void barrier(void *ctx) { (void)ctx; }
static int set_power(void *ctx, uint32_t state, uint32_t mask) {
    struct fake *f = ctx;
    assert(f->power_calls < 3);
    f->power_state[f->power_calls] = state;
    f->power_mask[f->power_calls++] = mask;
    return 0;
}
static int set_hw(void *ctx, uint32_t state) {
    ((struct fake *)ctx)->hw_on = state != 0;
    return 0;
}
static int set_phy(void *ctx, uint32_t state) {
    ((struct fake *)ctx)->phy_on = state != 0;
    return 0;
}
static int rtkit_boot(void *ctx) { (void)ctx; return 0; }
static void rtkit_shutdown(void *ctx) { ((struct fake *)ctx)->stopped_rtkit++; }
static int tunables(void *ctx, const struct as_usb4_resources *resources) {
    (void)ctx; assert(resources->nhi_base && resources->pdf_base &&
                      resources->rc_base && resources->hbw_base &&
                      resources->lbw_base && resources->pcie_adapter_base);
    return 0;
}
static int dma_start(void *ctx) { (void)ctx; return 0; }
static void dma_stop(void *ctx) { ((struct fake *)ctx)->stopped_dma++; }
static int start_nhi(void *ctx, uint64_t base, uint32_t info) {
    (void)base;
    assert(info & AS_USB4_CABLE_INFO_PRESENT);
    return ((struct fake *)ctx)->fail_nhi;
}
static void stop_nhi(void *ctx) { ((struct fake *)ctx)->stopped_nhi++; }

struct fake_caps {
    uint32_t words[0x10000][2];
};

struct fake_tunnel {
    uint64_t now;
    uint32_t events[16];
    size_t event_count;
    int fail_tx;
    int fail_rx;
    int fail_up;
    int fail_down;
};

struct fake_router {
    uint32_t events[8];
    size_t count;
};

static int router_read(void *ctx, uint64_t route, uint8_t port,
                       uint8_t space, uint16_t offset, uint32_t *value) {
    struct fake_router *fake = ctx;
    assert(route == 0 && port == 0 && space == AS_USB4_CFG_SWITCH &&
           offset == AS_USB4_ROUTER_CS_5);
    fake->events[fake->count++] = 0x10000000u | offset;
    *value = 0;
    return 0;
}
static int router_write(void *ctx, uint64_t route, uint8_t port,
                        uint8_t space, uint16_t offset, uint32_t value) {
    struct fake_router *fake = ctx;
    assert(route == 0 && port == 0 && space == AS_USB4_CFG_SWITCH &&
           offset == AS_USB4_ROUTER_CS_5);
    fake->events[fake->count++] = value;
    return 0;
}
static int router_poll(void *ctx, uint64_t route, uint8_t port,
                       uint8_t space, uint16_t offset, uint32_t value,
                       uint32_t mask) {
    struct fake_router *fake = ctx;
    assert(route == 0 && port == 0 && space == AS_USB4_CFG_SWITCH &&
           offset == AS_USB4_ROUTER_CS_6 && value == mask);
    fake->events[fake->count++] = value;
    return 0;
}

static const struct as_usb4_router_ops router_ops = {
    .read32 = router_read, .write32 = router_write, .poll32 = router_poll,
};

static int tunnel_tmu(void *ctx, uint32_t mode) {
    struct fake_tunnel *fake = ctx;
    assert(mode == 1);
    fake->events[fake->event_count++] = 1;
    return 0;
}
static int tunnel_path_activate(
    void *ctx, enum as_usb4_usb3_path path,
    const struct as_usb4_usb3_path_config *config) {
    struct fake_tunnel *fake = ctx;
    assert(config->source_hop == 8 && config->destination_hop == 8 &&
           config->path_type == (path == AS_USB4_USB3_PATH_TX ? 8 : 7) &&
           config->priority == 3 && config->weight == 3 &&
           config->source_initial_credits == 2 &&
           config->initial_credits == 14 &&
           config->destination_initial_credits == 14 &&
           config->counter_enable == 7 &&
           config->ingress_flow_control == 7 &&
           config->egress_flow_control == 3 &&
           config->routing_options == 0x17 && config->credit_options == 4);
    fake->events[fake->event_count++] = 0x10u + path;
    return path == AS_USB4_USB3_PATH_TX ? fake->fail_tx : fake->fail_rx;
}
static int tunnel_path_deactivate(void *ctx, enum as_usb4_usb3_path path) {
    struct fake_tunnel *fake = ctx;
    fake->events[fake->event_count++] = 0x20u + path;
    return 0;
}
static int tunnel_find_cap(void *ctx, uint64_t route, uint8_t port,
                           uint16_t capability, uint16_t *offset) {
    struct fake_tunnel *fake = ctx;
    assert(capability == AS_USB4_USB3_ADAPTER_CAPABILITY);
    assert((route == 0 && port == 2) || (route == 1 && port == 3));
    fake->events[fake->event_count++] = route == 0 ? 0x42 : 0x43;
    *offset = 4;
    return 0;
}
static int tunnel_adapter(void *ctx, uint64_t route, uint8_t port,
                          uint16_t capability_offset, int upstream, int enable,
                          uint32_t value, uint32_t mask) {
    struct fake_tunnel *fake = ctx;
    assert(capability_offset == 4);
    assert((upstream && route == 0 && port == 2) ||
           (!upstream && route == 1 && port == 3));
    assert(mask == AS_USB4_USB3_ADAPTER_MASK);
    assert(value == (enable ? AS_USB4_USB3_ADAPTER_ENABLE
                            : AS_USB4_USB3_ADAPTER_DISABLE));
    fake->events[fake->event_count++] = 0x30u + (upstream ? 2u : 0u) +
                                        (enable ? 1u : 0u);
    return enable && (upstream ? fake->fail_up : fake->fail_down);
}
static uint64_t tunnel_now(void *ctx) { return ((struct fake_tunnel *)ctx)->now; }

static const struct as_usb4_usb3_ops tunnel_ops = {
    .set_minimum_tmu_mode = tunnel_tmu,
    .path_activate = tunnel_path_activate,
    .path_deactivate = tunnel_path_deactivate,
    .adapter_find_capability = tunnel_find_cap,
    .adapter_enable = tunnel_adapter,
    .now_us = tunnel_now,
};

static int read_cap(void *ctx, uint64_t route, uint8_t space,
                    uint16_t offset, uint8_t length, uint32_t *words) {
    struct fake_caps *caps = ctx;
    assert(route == 0 && space == AS_USB4_CFG_SWITCH && length == 2);
    words[0] = caps->words[offset][0];
    words[1] = caps->words[offset][1];
    return 0;
}

static const struct as_usb4_ops ops = {
    .read32 = read32, .write32 = write32, .delay_us = delay_us,
    .dma_write_barrier = barrier, .dma_read_barrier = barrier,
    .set_power_state = set_power, .set_hw_state = set_hw,
    .set_phy_state = set_phy, .rtkit_boot = rtkit_boot,
    .rtkit_shutdown = rtkit_shutdown, .apply_tunables = tunables,
    .dma_protection_start = dma_start, .dma_protection_stop = dma_stop,
    .start_nhi = start_nhi, .stop_nhi = stop_nhi,
};

static void make_reply(uint8_t *packet, size_t length) {
    packet[0] |= 0x80;
    uint32_t crc = as_usb4_crc32c(packet, length - AS_USB4_CFG_CRC_SIZE);
    packet[length - 4] = (uint8_t)(crc >> 24);
    packet[length - 3] = (uint8_t)(crc >> 16);
    packet[length - 2] = (uint8_t)(crc >> 8);
    packet[length - 1] = (uint8_t)crc;
}

int main(void) {
    struct fake f = {0};
    struct as_usb4_controller c = {
        .resources = { .rc_base = 0x200000, .nhi_base = 0x300000,
                       .pdf_base = 0x500000, .hbw_base = 0x600000,
                       .lbw_base = 0x700000, .pcie_adapter_base = 0x800000,
                       .power_domain_count = 5 },
        .ops = &ops, .context = &f,
    };
    uint32_t info = as_usb4_cable_info(AS_USB4_MODE_USB4, 1, 0, 0, 0, 0);

    assert(info == (AS_USB4_CABLE_INFO_PRESENT | AS_USB4_CABLE_INFO_REVERSE |
                    AS_USB4_CABLE_INFO_ACTIVE));
    assert(as_usb4_nhi_ring_desc(0x10000000, 3, 1) == 0x1001c000);
    assert(as_usb4_nhi_ring_desc(0x10000000, 3, 0) == 0x1008c000);
    assert(as_usb4_nhi_ring_irq_bit(2, 12, 1) == (1u << 2));
    assert(as_usb4_nhi_ring_irq_bit(2, 12, 0) == (1u << 14));
    assert(as_usb4_nhi_validate_hops(0xabc00c, 24) == AS_USB4_OK);
    assert(as_usb4_nhi_validate_hops(0xabc40c, 24) == AS_USB4_ERR_NHI);
    assert(as_usb4_nhi_validate_hops(0xabc80c, 24) == AS_USB4_OK);
    assert(as_usb4_nhi_validate_hops(12, 22) == AS_USB4_ERR_NHI);
    assert(as_usb4_nhi_validate_hops(11, 24) == AS_USB4_ERR_NHI);
    assert(as_usb4_crc32c("123456789", 9) == 0xe3069283);

    struct fake_router router = {0};
    assert(as_usb4_router_configure(&router_ops, &router, 0, 1) ==
           AS_USB4_CFG_OK);
    assert(router.count == 5 && router.events[0] == AS_USB4_ROUTER_READY &&
           router.events[1] == 0x10000005 &&
           router.events[2] == AS_USB4_ROUTER_USB_TUNNELS &&
           router.events[3] ==
               (AS_USB4_ROUTER_USB_TUNNELS | AS_USB4_ROUTER_CV) &&
           router.events[4] == AS_USB4_ROUTER_CONFIG_ACK);

    struct as_usb4_hop_fields hop = {
        .out_hop = 8, .counter = 5, .out_port = 3,
        .initial_credits = 14, .priority = 3, .weight = 3,
        .valid = 1, .egress_flow_control = 1,
        .ingress_flow_control = 1, .counter_enable = 1,
    };
    uint8_t hop_wire[8];
    assert(as_usb4_hop_encode(hop_wire, &hop) == AS_USB4_TUNNEL_OK);
    assert(hop_wire[0] == 0x80 && hop_wire[1] == 0x1c &&
           hop_wire[2] == 0x18 && hop_wire[3] == 0x08);
    assert(hop_wire[4] == 0x03 && hop_wire[5] == 0x80 &&
           hop_wire[6] == 0x53 && hop_wire[7] == 0x03);
    hop.initial_credits = 0x80;
    assert(as_usb4_hop_encode(hop_wire, &hop) ==
           AS_USB4_TUNNEL_ERR_ARGUMENT);

    struct fake_tunnel tunnel_fake = { .now = 1000 };
    struct as_usb4_usb3_tunnel tunnel = {
        .ops = &tunnel_ops, .context = &tunnel_fake,
        .upstream_route = 0, .upstream_port = 2,
        .downstream_route = 1, .downstream_port = 3,
        .use_recommended_credits = 1,
    };
    assert(as_usb4_usb3_tunnel_start(&tunnel) == AS_USB4_TUNNEL_PENDING);
    assert(tunnel_fake.event_count == 5 && tunnel_fake.events[0] == 1 &&
           tunnel_fake.events[1] == 0x10 && tunnel_fake.events[2] == 0x11 &&
           tunnel_fake.events[3] == 0x42 && tunnel_fake.events[4] == 0x33);
    tunnel_fake.now += AS_USB4_USB3_SETTLE_US - 1;
    assert(as_usb4_usb3_tunnel_continue(&tunnel) == AS_USB4_TUNNEL_PENDING);
    assert(tunnel_fake.event_count == 5);
    tunnel_fake.now++;
    assert(as_usb4_usb3_tunnel_continue(&tunnel) == AS_USB4_TUNNEL_OK);
    assert(tunnel_fake.events[5] == 0x43 && tunnel_fake.events[6] == 0x31);
    assert(as_usb4_usb3_tunnel_stop(&tunnel) == AS_USB4_TUNNEL_OK);
    assert(tunnel_fake.events[7] == 0x30 && tunnel_fake.events[8] == 0x32 &&
           tunnel_fake.events[9] == 0x21 && tunnel_fake.events[10] == 0x20);

    struct fake_tunnel failed_tunnel_fake = { .now = 2000, .fail_down = 1 };
    struct as_usb4_usb3_tunnel failed_tunnel = {
        .ops = &tunnel_ops, .context = &failed_tunnel_fake,
        .upstream_route = 0, .upstream_port = 2,
        .downstream_route = 1, .downstream_port = 3,
        .use_recommended_credits = 1,
    };
    assert(as_usb4_usb3_tunnel_start(&failed_tunnel) ==
           AS_USB4_TUNNEL_PENDING);
    failed_tunnel_fake.now += AS_USB4_USB3_SETTLE_US;
    assert(as_usb4_usb3_tunnel_continue(&failed_tunnel) ==
           AS_USB4_TUNNEL_ERR_ADAPTER);
    assert(!failed_tunnel.tx_attempted && !failed_tunnel.rx_attempted &&
           !failed_tunnel.upstream_attempted &&
           !failed_tunnel.downstream_attempted);

    struct fake_tunnel overflow_fake = { .now = UINT64_MAX - 10 };
    struct as_usb4_usb3_tunnel overflow_tunnel = {
        .ops = &tunnel_ops, .context = &overflow_fake,
        .upstream_route = 0, .upstream_port = 2,
        .downstream_route = 1, .downstream_port = 3,
        .use_recommended_credits = 1,
    };
    assert(as_usb4_usb3_tunnel_start(&overflow_tunnel) ==
           AS_USB4_TUNNEL_ERR_ARGUMENT);
    assert(!overflow_tunnel.tx_attempted && !overflow_tunnel.rx_attempted &&
           !overflow_tunnel.upstream_attempted);

    struct fake_tunnel partial_path_fake = { .fail_tx = 1 };
    struct as_usb4_usb3_tunnel partial_path_tunnel = {
        .ops = &tunnel_ops, .context = &partial_path_fake,
        .upstream_route = 0, .upstream_port = 2,
        .downstream_route = 1, .downstream_port = 3,
        .use_recommended_credits = 1,
    };
    assert(as_usb4_usb3_tunnel_start(&partial_path_tunnel) ==
           AS_USB4_TUNNEL_ERR_PATH);
    assert(partial_path_fake.event_count == 3 &&
           partial_path_fake.events[0] == 1 &&
           partial_path_fake.events[1] == 0x10 &&
           partial_path_fake.events[2] == 0x20 &&
           !partial_path_tunnel.tx_attempted);

    uint8_t packet[AS_USB4_CFG_HEADER_SIZE + 8 + AS_USB4_CFG_CRC_SIZE];
    size_t packet_size = 0;
    struct as_usb4_cfg_address cfg = {
        .offset = 0x123, .length = 2, .port = 5,
        .space = AS_USB4_CFG_SWITCH, .sequence = 3,
    };
    const uint32_t payload[2] = { 0x11223344, 0xaabbccdd };
    assert(as_usb4_cfg_encode_write(packet, sizeof(packet), 0x123456789ab,
                                    &cfg, payload, &packet_size) ==
           AS_USB4_CFG_OK);
    assert(packet_size == sizeof(packet));
    assert(packet[12] == 0x11 && packet[13] == 0x22 &&
           packet[14] == 0x33 && packet[15] == 0x44);
    uint32_t decoded[2] = {0};
    size_t decoded_count = 0;
    uint64_t decoded_route = 0;
    struct as_usb4_cfg_address decoded_cfg = {0};
    assert(as_usb4_cfg_decode(decoded, 2, packet, packet_size, 0,
                              &decoded_route, &decoded_cfg,
                              &decoded_count) == AS_USB4_CFG_OK);
    assert(decoded_route == 0x123456789ab && decoded_count == 2);
    assert(decoded_cfg.offset == cfg.offset && decoded_cfg.length == cfg.length &&
           decoded_cfg.port == cfg.port && decoded_cfg.space == cfg.space &&
           decoded_cfg.sequence == cfg.sequence);
    assert(decoded[0] == payload[0] && decoded[1] == payload[1]);
    packet[15] ^= 1;
    assert(as_usb4_cfg_decode(decoded, 2, packet, packet_size, 0,
                              &decoded_route, &decoded_cfg,
                              &decoded_count) == AS_USB4_CFG_ERR_CRC);
    cfg.length = 4;
    assert(as_usb4_cfg_encode_read(packet, sizeof(packet), 0, &cfg,
                                   &packet_size) == AS_USB4_CFG_OK);
    assert(packet_size == AS_USB4_CFG_HEADER_SIZE + AS_USB4_CFG_CRC_SIZE);
    assert(as_usb4_cfg_decode(decoded, 2, packet, packet_size, 0,
                              &decoded_route, &decoded_cfg,
                              &decoded_count) == AS_USB4_CFG_OK);
    assert(decoded_count == 0 && decoded_cfg.length == 4);
    /* A real reply carries tb_cfg_header.unknown == 1 << 9, serialized as
     * bit 31 of the first wire dword, and the CRC covers that reply bit. */
    make_reply(packet, packet_size);
    assert(as_usb4_cfg_decode(decoded, 2, packet, packet_size, 1,
                              &decoded_route, &decoded_cfg,
                              &decoded_count) == AS_USB4_CFG_OK);
    assert(decoded_route == 0);
    assert(as_usb4_cfg_decode(decoded, 2, packet, packet_size, 0,
                              &decoded_route, &decoded_cfg,
                              &decoded_count) == AS_USB4_CFG_ERR_RESPONSE);
    assert(as_usb4_cfg_decode_response(decoded, 2, packet, packet_size,
                                       AS_USB4_CFG_WRITE, 0, &cfg,
                                       &decoded_count) == AS_USB4_CFG_OK);
    assert(as_usb4_cfg_decode_response(decoded, 2, packet, packet_size,
                                       AS_USB4_CFG_READ, 0, &cfg,
                                       &decoded_count) ==
           AS_USB4_CFG_ERR_RESPONSE);
    cfg.length = 2;
    assert(as_usb4_cfg_encode_write(packet, sizeof(packet), 0, &cfg, payload,
                                    &packet_size) == AS_USB4_CFG_OK);
    /* A read response carries big-endian register dwords. */
    packet[12] = 0x11; packet[13] = 0x22;
    packet[14] = 0x33; packet[15] = 0x44;
    packet[16] = 0xaa; packet[17] = 0xbb;
    packet[18] = 0xcc; packet[19] = 0xdd;
    make_reply(packet, packet_size);
    assert(as_usb4_cfg_decode_response(decoded, 2, packet, packet_size,
                                       AS_USB4_CFG_READ, 0, &cfg,
                                       &decoded_count) == AS_USB4_CFG_OK);
    assert(decoded_count == 2 && decoded[0] == payload[0]);
    const uint32_t raw_bit31[1] = { 0x00000080 };
    cfg.length = 1;
    assert(as_usb4_cfg_encode_write_raw_host(packet, sizeof(packet), 0, &cfg,
                                              raw_bit31, &packet_size) ==
           AS_USB4_CFG_OK);
    assert(packet[12] == 0x80 && packet[13] == 0 &&
           packet[14] == 0 && packet[15] == 0);
    cfg.length = 64;
    assert(as_usb4_cfg_encode_read(packet, sizeof(packet), 0, &cfg,
                                   &packet_size) == AS_USB4_CFG_ERR_ARGUMENT);

    struct fake_caps caps = {0};
    /* Short TMU at 0x10 -> long non-Apple VSE at 0x20 -> Apple VSE 0x123. */
    caps.words[0x10][0] = 0x20 | (AS_USB4_SWITCH_CAP_TMU << 8);
    caps.words[0x20][0] = AS_USB4_SWITCH_CAP_VSE << 8 | (0x44u << 16);
    caps.words[0x20][1] = 0x123;
    caps.words[0x123][0] = AS_USB4_SWITCH_CAP_VSE << 8 |
                           (AS_USB4_APPLE_VSE << 16) | (2u << 24);
    uint16_t cap_offset = 0;
    assert(as_usb4_find_switch_vse(read_cap, &caps, 0, 0x10,
                                   AS_USB4_APPLE_VSE, &cap_offset) ==
           AS_USB4_CFG_OK);
    assert(cap_offset == 0x123);
    caps.words[0x123][0] = 0x123 | (AS_USB4_SWITCH_CAP_TMU << 8);
    assert(as_usb4_find_switch_vse(read_cap, &caps, 0, 0x123,
                                   AS_USB4_APPLE_VSE, &cap_offset) ==
           AS_USB4_CFG_ERR_CAPABILITY);
    assert(as_usb4_ring_metadata(0x345, 2, 1,
                                 AS_USB4_DESC_INTERRUPT) == 0x412345);

    struct as_usb4_ring tx = {
        .nhi_base = 0x300000,
        .descriptors_dma = 0x80000000,
        .hop = 0,
        .size = 16,
        .ring_count = 12,
        .transmit = 1,
        .tx_shared_buffer_allocation = 2,
    };
    assert(as_usb4_ring_start(&ops, &f, &tx) == AS_USB4_OK);
    assert(f.registers[AS_USB4_NHI_TX_RING_DESC_BASE / 4] == 0x80000000);
    assert(f.registers[(AS_USB4_NHI_TX_RING_DESC_BASE + 12) / 4] == 16);
    assert(f.registers[(AS_USB4_NHI_TX_RING_DESC_BASE + 0x10) / 4] ==
           AS_USB4_RING_FLAG_ENABLE);
    assert(f.registers[(AS_USB4_NHI_TX_RING_DESC_BASE + 0x14) / 4] == 2);
    assert(f.registers[AS_USB4_NHI_IRQ_ENABLE / 4] == 1);
    struct as_usb4_ring_descriptor descriptors[16] = {0};
    assert(as_usb4_ring_post(&ops, &f, &tx, descriptors, 0x90000000,
                             64, 3, 3) == AS_USB4_OK);
    assert(tx.head == 1);
    assert(f.registers[(AS_USB4_NHI_TX_RING_DESC_BASE + 8) / 4] ==
           (1u << 16));
    assert(descriptors[0].iova == 0x90000000);
    assert((descriptors[0].metadata & 0xfff) == 64);
    descriptors[0].metadata |= AS_USB4_DESC_COMPLETED << 20;
    struct as_usb4_frame_result frame = {0};
    assert(as_usb4_ring_poll(&ops, &f, &tx, descriptors, &frame) ==
           AS_USB4_OK);
    assert(frame.iova == 0x90000000 && frame.length == 64 &&
           frame.eof == 3 && frame.sof == 3 &&
           (frame.flags & AS_USB4_DESC_COMPLETED));
    assert(tx.tail == 1);
    assert(as_usb4_ring_stop(&ops, &f, &tx) == AS_USB4_OK);
    assert(f.registers[AS_USB4_NHI_IRQ_ENABLE / 4] == 0);
    assert(f.registers[AS_USB4_NHI_TX_RING_DESC_BASE / 4] == 0);

    struct as_usb4_ring rx = {
        .nhi_base = 0x300000,
        .pdf_base = 0x500000,
        .descriptors_dma = 0x80001000,
        .hop = 1,
        .size = 16,
        .ring_count = 12,
        .sof_mask = 0xffff,
        .eof_mask = 0xffff,
        .raw_mode = 1,
    };
    assert(as_usb4_ring_start(&ops, &f, &rx) == AS_USB4_OK);
    uint32_t rx_base = AS_USB4_NHI_RX_RING_DESC_BASE + AS_USB4_NHI_RING_STRIDE;
    assert(f.registers[(rx_base + 0x0c) / 4] == 16);
    assert(f.registers[(rx_base + 0x10) / 4] ==
           (AS_USB4_RING_FLAG_ENABLE | AS_USB4_RING_FLAG_RAW));
    assert(f.registers[(rx_base + 0x14) / 4] == 0xffffffff);
    assert(f.pdf_address == 0x500000 + AS_USB4_NHI_RING_STRIDE &&
           f.pdf_value == 0xffffffff);
    assert(as_usb4_ring_stop(&ops, &f, &rx) == AS_USB4_OK);

    assert(as_usb4_start(&c, info) == AS_USB4_OK);
    assert(f.power_calls == 2 && f.power_state[0] == 5 &&
           f.power_mask[0] == 0x7 && f.power_state[1] == 7 &&
           f.power_mask[1] == 0x17 && c.nhi_started && c.rtkit_started);
    as_usb4_stop(&c);
    assert(f.power_calls == 3 && f.power_state[2] == 0 &&
           f.power_mask[2] == 0x17 && f.stopped_nhi == 1 &&
           f.stopped_rtkit == 1 && f.stopped_dma == 1);

    memset(&f, 0, sizeof(f));
    memset(&c, 0, sizeof(c));
    c.resources.rc_base = 0x200000;
    c.resources.nhi_base = 0x300000;
    c.resources.pdf_base = 0x500000;
    c.resources.hbw_base = 0x600000;
    c.resources.lbw_base = 0x700000;
    c.resources.pcie_adapter_base = 0x800000;
    c.resources.power_domain_count = 5;
    c.ops = &ops;
    c.context = &f;
    f.fail_nhi = 1;
    assert(as_usb4_start(&c, info) == AS_USB4_ERR_NHI);
    assert(f.power_calls == 3 && f.stopped_rtkit == 1 &&
           f.stopped_nhi == 1 && f.stopped_dma == 1);

    puts("apple usb4 core tests passed");
    return 0;
}

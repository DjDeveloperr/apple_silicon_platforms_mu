/** @file
  Host-testable Apple USB4/Thunderbolt ACIO and NHI sequencing core.

  Cable-info encoding follows Asahi Linux sven/tbt-wip. The Type5 lifecycle,
  register map, rings, and DMA rules follow the local J414s BootKC/ADT reverse-
  engineering contracts pinned in this directory's README. In particular,
  Type5 does not use the t8103 Linux driver's CTRL/INIT aperture.

  SPDX-License-Identifier: GPL-2.0-only
**/

#ifndef AURORA_APPLE_USB4_CORE_H
#define AURORA_APPLE_USB4_CORE_H

#include <stddef.h>
#include <stdint.h>

#define AS_USB4_ACIO_M3_CTRL                 0x000cu
#define AS_USB4_ACIO_M3_CTRL_LSTX            2u
#define AS_USB4_ACIO_M3_STAT                 0x00a8u
#define AS_USB4_ACIO_M3_STAT_STATE_MASK      0x7f000000u
#define AS_USB4_ACIO_M3_STAT_READY           0x01000000u

#define AS_USB4_NHI_HOP_COUNT                0x00000u
#define AS_USB4_NHI_TX_RING_DESC_BASE        0x10000u
#define AS_USB4_NHI_RX_RING_DESC_BASE        0x80000u
#define AS_USB4_NHI_RING_STRIDE              0x04000u
#define AS_USB4_NHI_IRQ_STATUS               0xd0000u
#define AS_USB4_NHI_IRQ_ENABLE               0xd0010u
#define AS_USB4_TYPE5_RING_COUNT             12u
#define AS_USB4_TYPE5_IRQ_COUNT              24u

#define AS_USB4_RING_FLAG_RAW                (1u << 30)
#define AS_USB4_RING_FLAG_ENABLE             (1u << 31)
#define AS_USB4_DESC_COMPLETED               0x2u
#define AS_USB4_DESC_INTERRUPT               0x4u

#define AS_USB4_CABLE_INFO_PRESENT           (1u << 0)
#define AS_USB4_CABLE_INFO_REVERSE           (1u << 1)
#define AS_USB4_CABLE_INFO_ACTIVE            (1u << 2)
#define AS_USB4_CABLE_INFO_LINK_TRAINING     (1u << 3)
#define AS_USB4_CABLE_INFO_20_GBPS           (1u << 4)
#define AS_USB4_CABLE_INFO_LEGACY_ADAPTER    (1u << 9)
#define AS_USB4_CABLE_INFO_TBT2_3            (1u << 10)

#define AS_USB4_REQUIRED_POWER_DOMAINS        5u
#define AS_USB4_POWER_DOMAINS_INITIAL         0x07u
#define AS_USB4_POWER_DOMAINS_RUNNING         0x17u
#define AS_USB4_POWER_STATE_INITIAL           5u
#define AS_USB4_POWER_STATE_RUNNING           7u
#define AS_USB4_HW_STATE_ON                   2u
#define AS_USB4_PHY_STATE_ON                  2u
#define AS_USB4_M3_POLL_US                    5000u
#define AS_USB4_M3_TIMEOUT_US                 500000u
#define AS_USB4_RING_STOP_POLL_US             10u
#define AS_USB4_RING_STOP_TIMEOUT_US          100000u

enum as_usb4_result {
    AS_USB4_OK = 0,
    AS_USB4_ERR_ARGUMENT = -1,
    AS_USB4_ERR_POWER = -2,
    AS_USB4_ERR_ACIO_INIT = -3,
    AS_USB4_ERR_RTKIT = -4,
    AS_USB4_ERR_M3_READY = -5,
    AS_USB4_ERR_TUNABLES = -6,
    AS_USB4_ERR_NHI = -7,
};

enum as_usb4_mode {
    AS_USB4_MODE_OFF = 0,
    AS_USB4_MODE_USB4,
    AS_USB4_MODE_TBT,
};

struct as_usb4_resources {
    uint64_t nhi_base;
    uint64_t pdf_base;
    uint64_t rc_base;
    uint64_t hbw_base;
    uint64_t lbw_base;
    uint64_t pcie_adapter_base;
    uint32_t power_domain_count;
};

struct as_usb4_ops {
    uint32_t (*read32)(void *context, uint64_t address);
    void (*write32)(void *context, uint64_t address, uint32_t value);
    void (*delay_us)(void *context, uint32_t usec);
    void (*dma_write_barrier)(void *context);
    void (*dma_read_barrier)(void *context);
    int (*set_power_state)(void *context, uint32_t state,
                           uint32_t domain_mask);
    int (*set_hw_state)(void *context, uint32_t state);
    int (*set_phy_state)(void *context, uint32_t state);
    int (*rtkit_boot)(void *context);
    void (*rtkit_shutdown)(void *context);
    int (*apply_tunables)(void *context,
                          const struct as_usb4_resources *resources);
    int (*dma_protection_start)(void *context);
    void (*dma_protection_stop)(void *context);
    int (*start_nhi)(void *context, uint64_t nhi_base, uint32_t cable_info);
    void (*stop_nhi)(void *context);
};

struct as_usb4_controller {
    struct as_usb4_resources resources;
    const struct as_usb4_ops *ops;
    void *context;
    uint32_t power_state;
    uint32_t power_domain_mask;
    uint32_t cable_info;
    uint8_t hw_state_active;
    uint8_t phy_state_active;
    uint8_t rtkit_attempted;
    uint8_t rtkit_started;
    uint8_t dma_protection_attempted;
    uint8_t dma_protection_active;
    uint8_t nhi_attempted;
    uint8_t nhi_started;
};

struct as_usb4_ring {
    uint64_t nhi_base;
    uint64_t pdf_base;
    uint64_t descriptors_dma;
    uint32_t hop;
    uint16_t size;
    uint16_t ring_count;
    uint16_t sof_mask;
    uint16_t eof_mask;
    uint16_t tx_shared_buffer_allocation;
    uint16_t head;
    uint16_t tail;
    uint8_t transmit;
    uint8_t raw_mode;
    uint8_t running;
};

struct as_usb4_frame_result {
    uint64_t iova;
    uint16_t length;
    uint16_t flags;
    uint8_t eof;
    uint8_t sof;
};

struct as_usb4_ring_descriptor {
    uint64_t iova;
    uint32_t metadata;
    uint32_t time;
};

uint32_t as_usb4_cable_info(enum as_usb4_mode mode, int reverse,
                            int active, int link_training, int speed_20_gbps,
                            int legacy_adapter);
uint64_t as_usb4_nhi_ring_desc(uint64_t nhi_base, uint32_t hop, int transmit);
uint32_t as_usb4_nhi_ring_irq_bit(uint32_t hop, uint32_t ring_count,
                                  int transmit);
int as_usb4_nhi_validate_hops(uint32_t hardware_hops, uint32_t irq_count);
uint32_t as_usb4_ring_metadata(uint16_t length, uint8_t eof, uint8_t sof,
                               uint16_t flags);
int as_usb4_ring_start(const struct as_usb4_ops *ops, void *context,
                       struct as_usb4_ring *ring);
int as_usb4_ring_stop(const struct as_usb4_ops *ops, void *context,
                      struct as_usb4_ring *ring);
int as_usb4_ring_post(const struct as_usb4_ops *ops, void *context,
                      struct as_usb4_ring *ring,
                      struct as_usb4_ring_descriptor *descriptors,
                      uint64_t iova, uint16_t length,
                      uint8_t eof, uint8_t sof);
int as_usb4_ring_poll(const struct as_usb4_ops *ops, void *context,
                      struct as_usb4_ring *ring,
                      struct as_usb4_ring_descriptor *descriptors,
                      struct as_usb4_frame_result *result);
int as_usb4_start(struct as_usb4_controller *controller, uint32_t cable_info);
void as_usb4_stop(struct as_usb4_controller *controller);

#endif

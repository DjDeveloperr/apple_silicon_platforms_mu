/** @file
  SPDX-License-Identifier: GPL-2.0-only
**/

#include "AppleUsb4Core.h"

static int
poll_mask(struct as_usb4_controller *controller, uint64_t address,
          uint32_t mask, uint32_t expected, uint32_t interval_us,
          uint32_t timeout_us)
{
    uint32_t elapsed;

    for (elapsed = 0; elapsed <= timeout_us; elapsed += interval_us) {
        if ((controller->ops->read32(controller->context, address) & mask) == expected)
            return AS_USB4_OK;
        if (elapsed == timeout_us)
            break;
        controller->ops->delay_us(controller->context, interval_us);
    }
    return AS_USB4_ERR_ACIO_INIT;
}

uint32_t
as_usb4_cable_info(enum as_usb4_mode mode, int reverse, int active,
                   int link_training, int speed_20_gbps, int legacy_adapter)
{
    uint32_t value;

    if (mode == AS_USB4_MODE_OFF)
        return 0;

    value = AS_USB4_CABLE_INFO_PRESENT;
    if (reverse)
        value |= AS_USB4_CABLE_INFO_REVERSE;

    if (mode == AS_USB4_MODE_USB4) {
        /* Match Asahi: USB4 always presents the active-cable bit to ACIO. */
        return value | AS_USB4_CABLE_INFO_ACTIVE;
    }

    value |= AS_USB4_CABLE_INFO_TBT2_3;
    if (active)
        value |= AS_USB4_CABLE_INFO_ACTIVE;
    if (link_training)
        value |= AS_USB4_CABLE_INFO_LINK_TRAINING;
    if (speed_20_gbps)
        value |= AS_USB4_CABLE_INFO_20_GBPS;
    if (legacy_adapter)
        value |= AS_USB4_CABLE_INFO_LEGACY_ADAPTER;
    return value;
}

uint64_t
as_usb4_nhi_ring_desc(uint64_t nhi_base, uint32_t hop, int transmit)
{
    return nhi_base + (transmit ? AS_USB4_NHI_TX_RING_DESC_BASE
                                : AS_USB4_NHI_RX_RING_DESC_BASE) +
           (uint64_t)hop * AS_USB4_NHI_RING_STRIDE;
}

uint32_t
as_usb4_nhi_ring_irq_bit(uint32_t hop, uint32_t ring_count, int transmit)
{
    uint32_t index = transmit ? hop : hop + ring_count;
    return index < 32 ? (1u << index) : 0;
}

int
as_usb4_nhi_validate_hops(uint32_t hardware_hops, uint32_t irq_count)
{
    if (irq_count != AS_USB4_TYPE5_IRQ_COUNT)
        return AS_USB4_ERR_NHI;
    return (hardware_hops & 0x7ffu) == AS_USB4_TYPE5_RING_COUNT
               ? AS_USB4_OK
               : AS_USB4_ERR_NHI;
}

uint32_t
as_usb4_ring_metadata(uint16_t length, uint8_t eof, uint8_t sof,
                      uint16_t flags)
{
    return ((uint32_t)length & 0xfffu) |
           (((uint32_t)eof & 0xfu) << 12) |
           (((uint32_t)sof & 0xfu) << 16) |
           (((uint32_t)flags & 0xfffu) << 20);
}

static void
write64(const struct as_usb4_ops *ops, void *context, uint64_t address,
        uint64_t value)
{
    ops->write32(context, address, (uint32_t)value);
    ops->write32(context, address + 4, (uint32_t)(value >> 32));
}

int
as_usb4_ring_start(const struct as_usb4_ops *ops, void *context,
                   struct as_usb4_ring *ring)
{
    uint64_t descriptor_base;
    uint64_t options_base;
    uint32_t irq_bit;
    uint32_t flags;

    if (ops == NULL || ops->read32 == NULL || ops->write32 == NULL ||
        ops->delay_us == NULL ||
        ring == NULL || ring->running || ring->size < 2 ||
        ring->ring_count != AS_USB4_TYPE5_RING_COUNT ||
        ring->hop >= ring->ring_count ||
        ring->descriptors_dma == 0 || (ring->descriptors_dma & 0xfffu) != 0 ||
        (!ring->transmit && ring->pdf_base == 0))
        return AS_USB4_ERR_ARGUMENT;

    irq_bit = as_usb4_nhi_ring_irq_bit(ring->hop, ring->ring_count,
                                       ring->transmit);
    if (irq_bit == 0)
        return AS_USB4_ERR_ARGUMENT;

    descriptor_base = as_usb4_nhi_ring_desc(ring->nhi_base, ring->hop,
                                             ring->transmit);
    options_base = descriptor_base + 0x10;
    flags = AS_USB4_RING_FLAG_ENABLE;
    if (ring->raw_mode)
        flags |= AS_USB4_RING_FLAG_RAW;

    write64(ops, context, descriptor_base, ring->descriptors_dma);
    if (ring->transmit) {
        ops->write32(context, descriptor_base + 12, ring->size);
        ops->write32(context, options_base + 4,
                     ring->tx_shared_buffer_allocation);
    } else {
        uint32_t sof_eof = ((uint32_t)ring->sof_mask << 16) | ring->eof_mask;
        ops->write32(context, descriptor_base + 12, ring->size);
        ops->write32(context, options_base + 4, sof_eof);
        ops->write32(context,
                     ring->pdf_base + (uint64_t)ring->hop *
                                          AS_USB4_NHI_RING_STRIDE,
                     sof_eof);
    }
    ops->write32(context, options_base, flags);
    ops->write32(context, ring->nhi_base + AS_USB4_NHI_IRQ_ENABLE,
                 ops->read32(context,
                             ring->nhi_base + AS_USB4_NHI_IRQ_ENABLE) |
                     irq_bit);
    ring->running = 1;
    return AS_USB4_OK;
}

int
as_usb4_ring_stop(const struct as_usb4_ops *ops, void *context,
                  struct as_usb4_ring *ring)
{
    uint64_t descriptor_base;
    uint64_t options_base;
    uint32_t irq_bit;

    if (ops == NULL || ops->read32 == NULL || ops->write32 == NULL ||
        ops->delay_us == NULL ||
        ring == NULL || !ring->running)
        return AS_USB4_ERR_ARGUMENT;

    descriptor_base = as_usb4_nhi_ring_desc(ring->nhi_base, ring->hop,
                                             ring->transmit);
    options_base = descriptor_base + 0x10;
    irq_bit = as_usb4_nhi_ring_irq_bit(ring->hop, ring->ring_count,
                                       ring->transmit);
    ops->write32(context, ring->nhi_base + AS_USB4_NHI_IRQ_ENABLE,
                 ops->read32(context,
                             ring->nhi_base + AS_USB4_NHI_IRQ_ENABLE) &
                     ~irq_bit);
    ops->write32(context, options_base,
                 ops->read32(context, options_base) & ~AS_USB4_RING_FLAG_ENABLE);
    {
        uint64_t status = descriptor_base + (ring->transmit ? 0x1cu : 0x18u);
        uint32_t elapsed;
        for (elapsed = 0; elapsed <= AS_USB4_RING_STOP_TIMEOUT_US;
             elapsed += AS_USB4_RING_STOP_POLL_US) {
            if (ops->read32(context, status) & 1u)
                break;
            if (elapsed == AS_USB4_RING_STOP_TIMEOUT_US)
                return AS_USB4_ERR_NHI;
            ops->delay_us(context, AS_USB4_RING_STOP_POLL_US);
        }
    }
    write64(ops, context, descriptor_base, 0);
    ops->write32(context, descriptor_base + 8, 0);
    ops->write32(context, descriptor_base + 12, 0);
    ring->running = 0;
    ring->head = 0;
    ring->tail = 0;
    return AS_USB4_OK;
}

int
as_usb4_ring_post(const struct as_usb4_ops *ops, void *context,
                  struct as_usb4_ring *ring,
                  struct as_usb4_ring_descriptor *descriptors,
                  uint64_t iova, uint16_t length,
                  uint8_t eof, uint8_t sof)
{
    struct as_usb4_ring_descriptor *descriptor;
    uint16_t next;
    uint64_t descriptor_base;

    if (ops == NULL || ops->write32 == NULL || ring == NULL ||
        descriptors == NULL || !ring->running || iova == 0 ||
        length > 0xfff)
        return AS_USB4_ERR_ARGUMENT;
    next = (uint16_t)((ring->head + 1u) % ring->size);
    if (next == ring->tail)
        return AS_USB4_ERR_NHI;

    descriptor = &descriptors[ring->head];
    descriptor->iova = iova;
    descriptor->time = 0;
    descriptor->metadata = as_usb4_ring_metadata(
        ring->transmit ? length : 0,
        ring->transmit ? eof : 0,
        ring->transmit ? sof : 0,
        AS_USB4_DESC_INTERRUPT);
    if (ops->dma_write_barrier != NULL)
        ops->dma_write_barrier(context);

    ring->head = next;
    descriptor_base = as_usb4_nhi_ring_desc(ring->nhi_base, ring->hop,
                                             ring->transmit);
    ops->write32(context, descriptor_base + 8,
                 ring->transmit ? (uint32_t)ring->head << 16 : ring->head);
    return AS_USB4_OK;
}

int
as_usb4_ring_poll(const struct as_usb4_ops *ops, void *context,
                  struct as_usb4_ring *ring,
                  struct as_usb4_ring_descriptor *descriptors,
                  struct as_usb4_frame_result *result)
{
    struct as_usb4_ring_descriptor *descriptor;
    uint32_t metadata;

    if (ops == NULL || ring == NULL || descriptors == NULL || result == NULL ||
        !ring->running)
        return AS_USB4_ERR_ARGUMENT;
    if (ring->head == ring->tail)
        return AS_USB4_ERR_NHI;
    descriptor = &descriptors[ring->tail];
    metadata = descriptor->metadata;
    if (!((metadata >> 20) & AS_USB4_DESC_COMPLETED))
        return AS_USB4_ERR_NHI;
    if (ops->dma_read_barrier != NULL)
        ops->dma_read_barrier(context);
    metadata = descriptor->metadata;

    result->iova = descriptor->iova;
    result->length = metadata & 0xfff;
    result->eof = (metadata >> 12) & 0xf;
    result->sof = (metadata >> 16) & 0xf;
    result->flags = (metadata >> 20) & 0xfff;
    descriptor->metadata = 0;
    ring->tail = (uint16_t)((ring->tail + 1u) % ring->size);
    return AS_USB4_OK;
}

void
as_usb4_stop(struct as_usb4_controller *controller)
{
    if (controller == NULL || controller->ops == NULL)
        return;

    if (controller->nhi_attempted && controller->ops->stop_nhi != NULL)
        controller->ops->stop_nhi(controller->context);
    controller->nhi_attempted = 0;
    controller->nhi_started = 0;

    if (controller->dma_protection_attempted &&
        controller->ops->dma_protection_stop != NULL)
        controller->ops->dma_protection_stop(controller->context);
    controller->dma_protection_attempted = 0;
    controller->dma_protection_active = 0;

    if (controller->rtkit_attempted && controller->ops->rtkit_shutdown != NULL)
        controller->ops->rtkit_shutdown(controller->context);
    controller->rtkit_attempted = 0;
    controller->rtkit_started = 0;
    if (controller->phy_state_active && controller->ops->set_phy_state != NULL)
        controller->ops->set_phy_state(controller->context, 0);
    controller->phy_state_active = 0;
    if (controller->hw_state_active && controller->ops->set_hw_state != NULL)
        controller->ops->set_hw_state(controller->context, 0);
    controller->hw_state_active = 0;
    if (controller->power_state && controller->ops->set_power_state != NULL)
        controller->ops->set_power_state(controller->context, 0,
                                          controller->power_domain_mask);
    controller->power_state = 0;
    controller->power_domain_mask = 0;
    controller->cable_info = 0;
}

int
as_usb4_start(struct as_usb4_controller *controller, uint32_t cable_info)
{
    const struct as_usb4_ops *ops;
    int result;

    if (controller == NULL || controller->ops == NULL || cable_info == 0)
        return AS_USB4_ERR_ARGUMENT;
    if (controller->resources.nhi_base == 0 ||
        controller->resources.pdf_base == 0 ||
        controller->resources.rc_base == 0 ||
        controller->resources.hbw_base == 0 ||
        controller->resources.lbw_base == 0 ||
        controller->resources.pcie_adapter_base == 0)
        return AS_USB4_ERR_ARGUMENT;
    ops = controller->ops;
    if (ops->read32 == NULL || ops->write32 == NULL || ops->delay_us == NULL ||
        ops->set_power_state == NULL || ops->set_hw_state == NULL ||
        ops->set_phy_state == NULL ||
        ops->rtkit_boot == NULL || ops->rtkit_shutdown == NULL ||
        ops->apply_tunables == NULL ||
        ops->dma_protection_start == NULL ||
        ops->dma_protection_stop == NULL || ops->start_nhi == NULL ||
        ops->stop_nhi == NULL ||
        controller->resources.power_domain_count < AS_USB4_REQUIRED_POWER_DOMAINS)
        return AS_USB4_ERR_ARGUMENT;
    if (controller->power_state || controller->cable_info ||
        controller->rtkit_attempted || controller->rtkit_started ||
        controller->dma_protection_attempted ||
        controller->dma_protection_active || controller->nhi_attempted ||
        controller->nhi_started)
        return AS_USB4_ERR_ARGUMENT;

    controller->power_state = AS_USB4_POWER_STATE_INITIAL;
    controller->power_domain_mask = AS_USB4_POWER_DOMAINS_INITIAL;
    if (ops->set_power_state(controller->context,
                             AS_USB4_POWER_STATE_INITIAL,
                             AS_USB4_POWER_DOMAINS_INITIAL) != 0) {
        result = AS_USB4_ERR_POWER;
        goto fail;
    }
    controller->hw_state_active = 1;
    if (ops->set_hw_state(controller->context, AS_USB4_HW_STATE_ON) != 0) {
        result = AS_USB4_ERR_POWER;
        goto fail;
    }
    controller->phy_state_active = 1;
    if (ops->set_phy_state(controller->context, AS_USB4_PHY_STATE_ON) != 0) {
        result = AS_USB4_ERR_POWER;
        goto fail;
    }

    ops->write32(controller->context,
                 controller->resources.rc_base + AS_USB4_ACIO_M3_CTRL,
                 AS_USB4_ACIO_M3_CTRL_LSTX);
    controller->rtkit_attempted = 1;
    if (ops->rtkit_boot(controller->context) != 0) {
        result = AS_USB4_ERR_RTKIT;
        goto fail;
    }
    controller->rtkit_started = 1;

    result = poll_mask(controller,
                       controller->resources.rc_base + AS_USB4_ACIO_M3_STAT,
                       AS_USB4_ACIO_M3_STAT_STATE_MASK,
                       AS_USB4_ACIO_M3_STAT_READY,
                       AS_USB4_M3_POLL_US, AS_USB4_M3_TIMEOUT_US);
    if (result != AS_USB4_OK) {
        result = AS_USB4_ERR_M3_READY;
        goto fail;
    }

    if (ops->apply_tunables(controller->context,
                            &controller->resources) != 0) {
        result = AS_USB4_ERR_TUNABLES;
        goto fail;
    }
    controller->power_state = AS_USB4_POWER_STATE_RUNNING;
    controller->power_domain_mask = AS_USB4_POWER_DOMAINS_RUNNING;
    if (ops->set_power_state(controller->context,
                             AS_USB4_POWER_STATE_RUNNING,
                             AS_USB4_POWER_DOMAINS_RUNNING) != 0) {
        result = AS_USB4_ERR_POWER;
        goto fail;
    }
    controller->dma_protection_attempted = 1;
    if (ops->dma_protection_start(controller->context) != 0) {
        result = AS_USB4_ERR_NHI;
        goto fail;
    }
    controller->dma_protection_active = 1;
    controller->nhi_attempted = 1;
    if (ops->start_nhi(controller->context, controller->resources.nhi_base,
                       cable_info) != 0) {
        result = AS_USB4_ERR_NHI;
        goto fail;
    }
    controller->nhi_started = 1;
    controller->cable_info = cable_info;
    return AS_USB4_OK;

fail:
    as_usb4_stop(controller);
    return result;
}

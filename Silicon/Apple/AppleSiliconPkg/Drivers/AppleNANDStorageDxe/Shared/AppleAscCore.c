/* SPDX-License-Identifier: MIT */
#include "AppleAscCore.h"

#include <stddef.h>

static void service(struct ntasi_asc_transport *transport)
{
    if (transport->ops.service != NULL)
        transport->ops.service(transport->opaque);
}

const struct ntasi_asc_hw ntasi_asc_hw_v4 = {
    .a2i_control = NTASI_ASC_MBOX_A2I_CONTROL,
    .i2a_control = NTASI_ASC_MBOX_I2A_CONTROL,
    .a2i_send0 = NTASI_ASC_MBOX_A2I_SEND0,
    .a2i_send1 = NTASI_ASC_MBOX_A2I_SEND1,
    .i2a_recv0 = NTASI_ASC_MBOX_I2A_RECV0,
    .i2a_recv1 = NTASI_ASC_MBOX_I2A_RECV1,
};

const struct ntasi_asc_hw ntasi_asc_hw_t8015 = {
    .a2i_control = NTASI_T8015_MBOX_A2I_CONTROL,
    .i2a_control = NTASI_T8015_MBOX_I2A_CONTROL,
    .a2i_send0 = NTASI_ASC_MBOX_A2I_SEND0,
    .a2i_send1 = NTASI_ASC_MBOX_A2I_SEND1,
    .i2a_recv0 = NTASI_ASC_MBOX_I2A_RECV0,
    .i2a_recv1 = NTASI_ASC_MBOX_I2A_RECV1,
};

int ntasi_asc_init(struct ntasi_asc_transport *transport,
                   const struct ntasi_asc_ops *ops,
                   void *opaque,
                   uint32_t poll_limit)
{
    return ntasi_asc_init_variant(transport, ops, &ntasi_asc_hw_v4,
                                  opaque, poll_limit);
}

int ntasi_asc_init_variant(struct ntasi_asc_transport *transport,
                           const struct ntasi_asc_ops *ops,
                           const struct ntasi_asc_hw *hw,
                           void *opaque,
                           uint32_t poll_limit)
{
    if (transport == NULL || ops == NULL || hw == NULL ||
        ops->cpu_read32 == NULL || ops->cpu_write32 == NULL ||
        ops->mailbox_read32 == NULL || ops->mailbox_read64 == NULL ||
        ops->mailbox_write64 == NULL || poll_limit == 0)
        return NTASI_ASC_ERR_ARGUMENT;

    *transport = (struct ntasi_asc_transport){
        .ops = *ops,
        .hw = hw,
        .opaque = opaque,
        .poll_limit = poll_limit,
    };
    return NTASI_ASC_OK;
}

void ntasi_asc_cpu_start(struct ntasi_asc_transport *transport)
{
    uint32_t value = transport->ops.cpu_read32(transport->opaque,
                                                NTASI_ASC_CPU_CONTROL);
    transport->ops.cpu_write32(transport->opaque, NTASI_ASC_CPU_CONTROL,
                               value | NTASI_ASC_CPU_CONTROL_START);
}

void ntasi_asc_cpu_stop(struct ntasi_asc_transport *transport)
{
    uint32_t value = transport->ops.cpu_read32(transport->opaque,
                                                NTASI_ASC_CPU_CONTROL);
    transport->ops.cpu_write32(transport->opaque, NTASI_ASC_CPU_CONTROL,
                               value & ~NTASI_ASC_CPU_CONTROL_START);
}

bool ntasi_asc_cpu_running(struct ntasi_asc_transport *transport)
{
    return (transport->ops.cpu_read32(transport->opaque,
                                      NTASI_ASC_CPU_CONTROL) &
            NTASI_ASC_CPU_CONTROL_START) != 0;
}

bool ntasi_asc_can_send(struct ntasi_asc_transport *transport)
{
    return (transport->ops.mailbox_read32(transport->opaque,
                                          transport->hw->a2i_control) &
            NTASI_ASC_MBOX_CONTROL_FULL) == 0;
}

bool ntasi_asc_can_receive(struct ntasi_asc_transport *transport)
{
    return (transport->ops.mailbox_read32(transport->opaque,
                                          transport->hw->i2a_control) &
            NTASI_ASC_MBOX_CONTROL_EMPTY) == 0;
}

int ntasi_asc_send(struct ntasi_asc_transport *transport,
                   const struct ntasi_asc_message *message)
{
    uint32_t attempt;

    if (transport == NULL || message == NULL)
        return NTASI_ASC_ERR_ARGUMENT;
    for (attempt = 0; attempt < transport->poll_limit; ++attempt) {
        service(transport);
        if (ntasi_asc_can_send(transport)) {
            if (transport->ops.dma_write_barrier != NULL)
                transport->ops.dma_write_barrier(transport->opaque);
            transport->ops.mailbox_write64(transport->opaque,
                                           transport->hw->a2i_send0,
                                           message->payload);
            transport->ops.mailbox_write64(transport->opaque,
                                           transport->hw->a2i_send1,
                                           message->endpoint);
            return NTASI_ASC_OK;
        }
    }
    return NTASI_ASC_ERR_SEND_TIMEOUT;
}

int ntasi_asc_receive(struct ntasi_asc_transport *transport,
                      struct ntasi_asc_message *message)
{
    if (transport == NULL || message == NULL)
        return NTASI_ASC_ERR_ARGUMENT;
    service(transport);
    if (!ntasi_asc_can_receive(transport))
        return NTASI_ASC_NO_MESSAGE;

    message->payload = transport->ops.mailbox_read64(
        transport->opaque, transport->hw->i2a_recv0);
    message->endpoint = (uint32_t)transport->ops.mailbox_read64(
        transport->opaque, transport->hw->i2a_recv1);
    if (transport->ops.dma_read_barrier != NULL)
        transport->ops.dma_read_barrier(transport->opaque);
    return NTASI_ASC_OK;
}

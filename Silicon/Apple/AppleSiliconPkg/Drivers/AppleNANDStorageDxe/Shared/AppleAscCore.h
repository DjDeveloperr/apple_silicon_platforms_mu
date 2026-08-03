/* SPDX-License-Identifier: MIT */
#ifndef NTASI_APPLE_ASC_CORE_H
#define NTASI_APPLE_ASC_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* CPU-control offsets are relative to the coprocessor register base. */
#define NTASI_ASC_CPU_CONTROL 0x0044u
#define NTASI_ASC_CPU_CONTROL_START (1u << 4)

/* Mailbox offsets are relative to the separately discovered mailbox base. */
#define NTASI_ASC_MBOX_A2I_CONTROL 0x110u
#define NTASI_ASC_MBOX_I2A_CONTROL 0x114u
#define NTASI_ASC_MBOX_A2I_SEND0   0x800u
#define NTASI_ASC_MBOX_A2I_SEND1   0x808u
#define NTASI_ASC_MBOX_I2A_RECV0   0x830u
#define NTASI_ASC_MBOX_I2A_RECV1   0x838u
#define NTASI_T8015_MBOX_A2I_CONTROL 0x108u
#define NTASI_T8015_MBOX_I2A_CONTROL 0x10cu

#define NTASI_ASC_MBOX_CONTROL_FULL  (1u << 16)
#define NTASI_ASC_MBOX_CONTROL_EMPTY (1u << 17)

enum ntasi_asc_result {
    NTASI_ASC_OK = 0,
    NTASI_ASC_NO_MESSAGE = 1,
    NTASI_ASC_ERR_ARGUMENT = -1,
    NTASI_ASC_ERR_SEND_TIMEOUT = -2,
};

struct ntasi_asc_message {
    uint64_t payload;
    uint32_t endpoint;
};

struct ntasi_asc_hw {
    uint32_t a2i_control;
    uint32_t i2a_control;
    uint32_t a2i_send0;
    uint32_t a2i_send1;
    uint32_t i2a_recv0;
    uint32_t i2a_recv1;
};

extern const struct ntasi_asc_hw ntasi_asc_hw_v4;
extern const struct ntasi_asc_hw ntasi_asc_hw_t8015;

struct ntasi_asc_ops {
    uint32_t (*cpu_read32)(void *opaque, uint32_t offset);
    void (*cpu_write32)(void *opaque, uint32_t offset, uint32_t value);
    uint32_t (*mailbox_read32)(void *opaque, uint32_t offset);
    uint64_t (*mailbox_read64)(void *opaque, uint32_t offset);
    void (*mailbox_write64)(void *opaque, uint32_t offset, uint64_t value);
    void (*dma_read_barrier)(void *opaque);
    void (*dma_write_barrier)(void *opaque);
    void (*service)(void *opaque);
};

struct ntasi_asc_transport {
    struct ntasi_asc_ops ops;
    const struct ntasi_asc_hw *hw;
    void *opaque;
    uint32_t poll_limit;
};

int ntasi_asc_init(struct ntasi_asc_transport *transport,
                   const struct ntasi_asc_ops *ops,
                   void *opaque,
                   uint32_t poll_limit);

int ntasi_asc_init_variant(struct ntasi_asc_transport *transport,
                           const struct ntasi_asc_ops *ops,
                           const struct ntasi_asc_hw *hw,
                           void *opaque,
                           uint32_t poll_limit);

void ntasi_asc_cpu_start(struct ntasi_asc_transport *transport);
void ntasi_asc_cpu_start_exclusive(struct ntasi_asc_transport *transport);
void ntasi_asc_cpu_stop(struct ntasi_asc_transport *transport);
bool ntasi_asc_cpu_running(struct ntasi_asc_transport *transport);
bool ntasi_asc_can_send(struct ntasi_asc_transport *transport);
bool ntasi_asc_can_receive(struct ntasi_asc_transport *transport);

/* Polls for A2I space, then publishes payload followed by endpoint. */
int ntasi_asc_send(struct ntasi_asc_transport *transport,
                   const struct ntasi_asc_message *message);

/* Non-blocking receive. Returns NTASI_ASC_NO_MESSAGE when I2A is empty. */
int ntasi_asc_receive(struct ntasi_asc_transport *transport,
                      struct ntasi_asc_message *message);

#endif

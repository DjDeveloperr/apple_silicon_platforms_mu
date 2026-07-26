/* SPDX-License-Identifier: MIT */
#ifndef NTASI_APPLE_NVME_CONTROLLER_CORE_H
#define NTASI_APPLE_NVME_CONTROLLER_CORE_H

#include "AppleNvmeCore.h"

enum ntasi_ans_controller_result {
    NTASI_ANS_CONTROLLER_OK = 0,
    NTASI_ANS_CONTROLLER_ERR_ARGUMENT = -1,
    NTASI_ANS_CONTROLLER_ERR_ALIGNMENT = -2,
    NTASI_ANS_CONTROLLER_ERR_BOOT_TIMEOUT = -3,
    NTASI_ANS_CONTROLLER_ERR_DISABLE_TIMEOUT = -4,
    NTASI_ANS_CONTROLLER_ERR_ENABLE_TIMEOUT = -5,
    NTASI_ANS_CONTROLLER_ERR_COMMAND_TIMEOUT = -6,
    NTASI_ANS_CONTROLLER_ERR_COMPLETION_TAG = -7,
    NTASI_ANS_CONTROLLER_ERR_COMPLETION_STATUS = -8,
    NTASI_ANS_CONTROLLER_ERR_TCB_INVALIDATE = -9,
    NTASI_ANS_CONTROLLER_ERR_SHUTDOWN_TIMEOUT = -10,
    NTASI_ANS_CONTROLLER_ERR_NOT_STARTED = -11,
};

struct ntasi_ans_controller_ops {
    uint32_t (*read32)(void *opaque, uint32_t offset);
    void (*write32)(void *opaque, uint32_t offset, uint32_t value);
    void (*dma_read_barrier)(void *opaque);
    void (*dma_write_barrier)(void *opaque);
    /* Services ASC/RTKit traffic while the controller is polled. */
    void (*service)(void *opaque);
};

struct ntasi_ans_queue_memory {
    void *commands;
    struct ntasi_ans_cqe *completions;
    struct ntasi_ans_tcb *tcbs;
    uint64_t commands_dma;
    uint64_t completions_dma;
    uint64_t tcbs_dma;
};

struct ntasi_ans_controller_queue {
    struct ntasi_ans_queue_memory memory;
    struct ntasi_ans_cq_state completion;
    bool admin;
    uint32_t depth;
    uint32_t command_stride;
    uint32_t submission_tail;
};

struct ntasi_ans_controller {
    struct ntasi_ans_controller_ops ops;
    void *opaque;
    uint32_t slots;
    uint32_t poll_limit;
    const struct ntasi_ans_hw *hw;
    struct ntasi_ans_controller_queue admin;
    struct ntasi_ans_controller_queue io;
    uint64_t last_result;
    uint16_t last_completion_status;
    uint16_t last_completion_tag;
    bool enabled;
    bool io_queues_created;
};

/*
 * Starts the ANS NVMe block after ASC/RTKit has been booted by the caller.
 * Waits for ANS boot status, configures the selected legacy or linear/NVMMU
 * contract, disables the controller, programs admin queues and CC entry
 * sizes, enables it, then creates the single I/O CQ/SQ.
 */
int ntasi_ans_controller_start(
    struct ntasi_ans_controller *controller,
    const struct ntasi_ans_controller_ops *ops,
    void *opaque,
    uint32_t slots,
    uint32_t poll_limit,
    const struct ntasi_ans_queue_memory *admin,
    const struct ntasi_ans_queue_memory *io);

int ntasi_ans_controller_start_variant(
    struct ntasi_ans_controller *controller,
    const struct ntasi_ans_controller_ops *ops,
    const struct ntasi_ans_hw *hw,
    void *opaque,
    uint32_t slots,
    uint32_t poll_limit,
    const struct ntasi_ans_queue_memory *admin,
    const struct ntasi_ans_queue_memory *io);

/* Serialized tag-0 command path used by m1n1 and the first firmware driver. */
int ntasi_ans_controller_execute(
    struct ntasi_ans_controller *controller,
    struct ntasi_ans_controller_queue *queue,
    const struct ntasi_ans_sqe *command,
    enum ntasi_ans_dma_direction direction,
    uint64_t *result);

/* Deletes I/O queues, performs normal shutdown, and disables the controller. */
int ntasi_ans_controller_stop(struct ntasi_ans_controller *controller);

#endif

/* SPDX-License-Identifier: MIT */
#include "AppleNvmeControllerCore.h"

#include <string.h>

static void service(struct ntasi_ans_controller *controller)
{
    if (controller->ops.service != NULL)
        controller->ops.service(controller->opaque);
}

static void dma_read_barrier(struct ntasi_ans_controller *controller)
{
    if (controller->ops.dma_read_barrier != NULL)
        controller->ops.dma_read_barrier(controller->opaque);
}

static void dma_write_barrier(struct ntasi_ans_controller *controller)
{
    if (controller->ops.dma_write_barrier != NULL)
        controller->ops.dma_write_barrier(controller->opaque);
}

static uint32_t read32(struct ntasi_ans_controller *controller, uint32_t offset)
{
    return controller->ops.read32(controller->opaque, offset);
}

static void write32(struct ntasi_ans_controller *controller, uint32_t offset,
                    uint32_t value)
{
    controller->ops.write32(controller->opaque, offset, value);
}

static void write64_lo_hi(struct ntasi_ans_controller *controller,
                          uint32_t offset, uint64_t value)
{
    write32(controller, offset, (uint32_t)value);
    write32(controller, offset + 4u, (uint32_t)(value >> 32));
}

static bool poll_mask(struct ntasi_ans_controller *controller, uint32_t offset,
                      uint32_t mask, uint32_t expected)
{
    uint32_t attempt;

    for (attempt = 0; attempt < controller->poll_limit; ++attempt) {
        service(controller);
        if ((read32(controller, offset) & mask) == expected)
            return true;
    }
    return false;
}

static bool queue_valid(const struct ntasi_ans_queue_memory *queue,
                        bool needs_tcbs)
{
    const uint64_t align_mask = NTASI_ANS_QUEUE_ALIGN - 1u;

    return queue != NULL && queue->commands != NULL &&
           queue->completions != NULL && (!needs_tcbs || queue->tcbs != NULL) &&
           (queue->commands_dma & align_mask) == 0 &&
           (queue->completions_dma & align_mask) == 0 &&
           (!needs_tcbs || (queue->tcbs_dma & align_mask) == 0);
}

static void queue_init(struct ntasi_ans_controller_queue *queue,
                       const struct ntasi_ans_queue_memory *memory,
                       const struct ntasi_ans_hw *hw,
                       uint32_t depth, uint32_t tcb_slots, bool admin)
{
    queue->memory = *memory;
    queue->completion = NTASI_ANS_CQ_STATE_INIT;
    queue->admin = admin;
    queue->depth = depth;
    queue->command_stride = admin ? NTASI_ANS_SQE_SIZE : hw->io_command_stride;
    queue->submission_tail = 0;
    memset(queue->memory.commands, 0,
           ntasi_ans_command_bytes(hw, admin, depth));
    memset(queue->memory.completions, 0, ntasi_ans_cq_bytes(depth));
    if (hw->submission_mode == NTASI_ANS_SUBMISSION_LINEAR_NVMMU)
        memset(queue->memory.tcbs, 0, ntasi_ans_tcb_bytes(tcb_slots));
}

int ntasi_ans_controller_execute(
    struct ntasi_ans_controller *controller,
    struct ntasi_ans_controller_queue *queue,
    const struct ntasi_ans_sqe *command,
    enum ntasi_ans_dma_direction direction,
    uint64_t *result)
{
    struct ntasi_ans_cqe completion;
    struct ntasi_ans_sqe *slot;
    uint8_t *command_base;
    uint32_t attempt;
    uint32_t tcb_status;
    uint8_t tag = 0;

    if (controller == NULL || queue == NULL || command == NULL ||
        controller->ops.read32 == NULL || controller->ops.write32 == NULL ||
        controller->slots == 0 || controller->poll_limit == 0 ||
        (direction != NTASI_ANS_DMA_FROM_DEVICE &&
         direction != NTASI_ANS_DMA_TO_DEVICE))
        return NTASI_ANS_CONTROLLER_ERR_ARGUMENT;
    if (!controller->enabled)
        return NTASI_ANS_CONTROLLER_ERR_NOT_STARTED;

    command_base = queue->memory.commands;
    if (controller->hw->submission_mode ==
        NTASI_ANS_SUBMISSION_LINEAR_NVMMU) {
        slot = (struct ntasi_ans_sqe *)(command_base +
                                        (size_t)tag * queue->command_stride);
    } else {
        slot = (struct ntasi_ans_sqe *)(command_base +
            (size_t)queue->submission_tail * queue->command_stride);
    }
    *slot = *command;
    slot->tag = tag;
    slot->rsvd = 0;
    if (controller->hw->submission_mode ==
        NTASI_ANS_SUBMISSION_LINEAR_NVMMU)
        ntasi_ans_tcb_fill(&queue->memory.tcbs[tag], slot, direction);
    dma_write_barrier(controller);

    service(controller);
    if (controller->hw->submission_mode ==
        NTASI_ANS_SUBMISSION_LINEAR_NVMMU) {
        write32(controller, ntasi_ans_sq_db_off(queue->admin), tag);
    } else {
        queue->submission_tail++;
        if (queue->submission_tail == queue->depth)
            queue->submission_tail = 0;
        write32(controller, ntasi_ans_conventional_sq_db_off(queue->admin),
                queue->submission_tail);
    }
    service(controller);

    for (attempt = 0; attempt < controller->poll_limit; ++attempt) {
        service(controller);
        dma_read_barrier(controller);
        memcpy(&completion,
               &queue->memory.completions[queue->completion.head],
               sizeof(completion));
        if (!ntasi_ans_cqe_ready(completion.status,
                                  queue->completion.phase))
            continue;

        controller->last_result = completion.result;
        controller->last_completion_status = completion.status;
        controller->last_completion_tag = completion.tag;
        tcb_status = 0;
        if (controller->hw->submission_mode ==
            NTASI_ANS_SUBMISSION_LINEAR_NVMMU) {
            write32(controller, NTASI_ANS_REG_NVMMU_TCB_INVAL,
                    completion.tag);
            tcb_status = read32(controller, NTASI_ANS_REG_NVMMU_TCB_STAT);
        }
        ntasi_ans_cq_advance(&queue->completion, queue->depth);
        write32(controller, ntasi_ans_cq_db_off(queue->admin),
                queue->completion.head);

        if (tcb_status != 0)
            return NTASI_ANS_CONTROLLER_ERR_TCB_INVALIDATE;
        if (completion.tag != tag)
            return NTASI_ANS_CONTROLLER_ERR_COMPLETION_TAG;
        if (ntasi_ans_cqe_code(completion.status) != 0)
            return NTASI_ANS_CONTROLLER_ERR_COMPLETION_STATUS;
        if (result != NULL)
            *result = completion.result;
        return NTASI_ANS_CONTROLLER_OK;
    }
    return NTASI_ANS_CONTROLLER_ERR_COMMAND_TIMEOUT;
}

int ntasi_ans_controller_start(
    struct ntasi_ans_controller *controller,
    const struct ntasi_ans_controller_ops *ops,
    void *opaque,
    uint32_t slots,
    uint32_t poll_limit,
    const struct ntasi_ans_queue_memory *admin,
    const struct ntasi_ans_queue_memory *io)
{
    return ntasi_ans_controller_start_variant(
        controller, ops, &ntasi_ans_hw_t8103, opaque, slots, poll_limit,
        admin, io);
}

int ntasi_ans_controller_start_variant(
    struct ntasi_ans_controller *controller,
    const struct ntasi_ans_controller_ops *ops,
    const struct ntasi_ans_hw *hw,
    void *opaque,
    uint32_t slots,
    uint32_t poll_limit,
    const struct ntasi_ans_queue_memory *admin,
    const struct ntasi_ans_queue_memory *io)
{
    struct ntasi_ans_sqe command;
    uint32_t value;
    int status;

    bool linear;

    if (controller == NULL || ops == NULL || hw == NULL ||
        ops->read32 == NULL ||
        ops->write32 == NULL || slots == 0 ||
        slots > hw->max_queue_depth || hw->admin_queue_depth == 0 ||
        hw->admin_queue_depth > slots || hw->io_command_stride <
            NTASI_ANS_SQE_SIZE || poll_limit == 0)
        return NTASI_ANS_CONTROLLER_ERR_ARGUMENT;
    linear = hw->submission_mode == NTASI_ANS_SUBMISSION_LINEAR_NVMMU;
    if (!queue_valid(admin, linear) || !queue_valid(io, linear))
        return NTASI_ANS_CONTROLLER_ERR_ALIGNMENT;

    memset(controller, 0, sizeof(*controller));
    controller->ops = *ops;
    controller->opaque = opaque;
    controller->slots = slots;
    controller->poll_limit = poll_limit;
    controller->hw = hw;
    queue_init(&controller->admin, admin, hw, hw->admin_queue_depth,
               slots, true);
    queue_init(&controller->io, io, hw, slots, slots, false);

    if (!poll_mask(controller, NTASI_ANS_REG_BOOT_STATUS, UINT32_MAX,
                   NTASI_ANS_BOOT_STATUS_OK))
        return NTASI_ANS_CONTROLLER_ERR_BOOT_TIMEOUT;

    if (linear) {
        value = read32(controller, NTASI_ANS_REG_LINEAR_SQ_CTRL);
        write32(controller, NTASI_ANS_REG_LINEAR_SQ_CTRL,
                value | NTASI_ANS_LINEAR_SQ_EN);
        value = read32(controller, NTASI_ANS_REG_UNKNOWN_CTRL);
        write32(controller, NTASI_ANS_REG_UNKNOWN_CTRL,
                value & ~NTASI_ANS_UNKCTRL_PRP_NULL_CHECK);
        write32(controller, NTASI_ANS_REG_MAX_PEND_CMDS,
                ntasi_ans_max_pend_cmds(slots));
        write32(controller, NTASI_ANS_REG_NVMMU_NUM,
                ntasi_ans_nvmmu_num(slots));
        write64_lo_hi(controller, NTASI_ANS_REG_NVMMU_ASQ_BASE,
                      admin->tcbs_dma);
        write64_lo_hi(controller, NTASI_ANS_REG_NVMMU_IOSQ_BASE,
                      io->tcbs_dma);
    }

    value = read32(controller, NTASI_ANS_REG_CC);
    write32(controller, NTASI_ANS_REG_CC, value & ~NTASI_ANS_CC_EN);
    if (!poll_mask(controller, NTASI_ANS_REG_CSTS, NTASI_ANS_CSTS_RDY, 0))
        return NTASI_ANS_CONTROLLER_ERR_DISABLE_TIMEOUT;

    write64_lo_hi(controller, NTASI_ANS_REG_ASQ, admin->commands_dma);
    write64_lo_hi(controller, NTASI_ANS_REG_ACQ, admin->completions_dma);
    write32(controller, NTASI_ANS_REG_AQA,
            ntasi_ans_aqa(hw->admin_queue_depth));

    write32(controller, NTASI_ANS_REG_CC,
            ntasi_ans_cc_config() | NTASI_ANS_CC_EN);
    if (!poll_mask(controller, NTASI_ANS_REG_CSTS, NTASI_ANS_CSTS_RDY,
                   NTASI_ANS_CSTS_RDY))
        return NTASI_ANS_CONTROLLER_ERR_ENABLE_TIMEOUT;
    controller->enabled = true;

    ntasi_ans_cmd_create_iocq(&command, 1, slots, io->completions_dma);
    status = ntasi_ans_controller_execute(controller, &controller->admin,
                                          &command, NTASI_ANS_DMA_TO_DEVICE,
                                          NULL);
    if (status != NTASI_ANS_CONTROLLER_OK)
        return status;
    ntasi_ans_cmd_create_iosq(&command, 1, 1, slots, io->commands_dma);
    status = ntasi_ans_controller_execute(controller, &controller->admin,
                                          &command, NTASI_ANS_DMA_TO_DEVICE,
                                          NULL);
    if (status != NTASI_ANS_CONTROLLER_OK)
        return status;
    controller->io_queues_created = true;
    return NTASI_ANS_CONTROLLER_OK;
}

int ntasi_ans_controller_stop(struct ntasi_ans_controller *controller)
{
    struct ntasi_ans_sqe command;
    uint32_t value;
    int status;

    if (controller == NULL)
        return NTASI_ANS_CONTROLLER_ERR_ARGUMENT;
    if (!controller->enabled)
        return NTASI_ANS_CONTROLLER_ERR_NOT_STARTED;

    if (controller->io_queues_created) {
        ntasi_ans_cmd_delete_q(&command, NTASI_ANS_ADMIN_CMD_DELETE_SQ, 1);
        status = ntasi_ans_controller_execute(controller, &controller->admin,
                                              &command, NTASI_ANS_DMA_FROM_DEVICE,
                                              NULL);
        if (status != NTASI_ANS_CONTROLLER_OK)
            return status;
        ntasi_ans_cmd_delete_q(&command, NTASI_ANS_ADMIN_CMD_DELETE_CQ, 1);
        status = ntasi_ans_controller_execute(controller, &controller->admin,
                                              &command, NTASI_ANS_DMA_FROM_DEVICE,
                                              NULL);
        if (status != NTASI_ANS_CONTROLLER_OK)
            return status;
        controller->io_queues_created = false;
    }

    value = read32(controller, NTASI_ANS_REG_CC);
    value &= ~NTASI_ANS_CC_SHN_MASK;
    value |= NTASI_ANS_CC_SHN_NORMAL << NTASI_ANS_CC_SHN_SHIFT;
    write32(controller, NTASI_ANS_REG_CC, value);
    if (!poll_mask(controller, NTASI_ANS_REG_CSTS,
                   NTASI_ANS_CSTS_SHST_MASK,
                   NTASI_ANS_CSTS_SHST_DONE << NTASI_ANS_CSTS_SHST_SHIFT))
        return NTASI_ANS_CONTROLLER_ERR_SHUTDOWN_TIMEOUT;

    value = read32(controller, NTASI_ANS_REG_CC);
    write32(controller, NTASI_ANS_REG_CC, value & ~NTASI_ANS_CC_EN);
    if (!poll_mask(controller, NTASI_ANS_REG_CSTS, NTASI_ANS_CSTS_RDY, 0))
        return NTASI_ANS_CONTROLLER_ERR_DISABLE_TIMEOUT;
    controller->enabled = false;
    return NTASI_ANS_CONTROLLER_OK;
}

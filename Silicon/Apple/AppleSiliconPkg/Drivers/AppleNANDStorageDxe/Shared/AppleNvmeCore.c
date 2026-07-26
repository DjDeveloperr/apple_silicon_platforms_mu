/* SPDX-License-Identifier: MIT */
#include "AppleNvmeCore.h"

const struct ntasi_ans_hw ntasi_ans_hw_t8015 = {
    .submission_mode = NTASI_ANS_SUBMISSION_CONVENTIONAL,
    .max_queue_depth = 16,
    .admin_queue_depth = 16,
    .io_command_stride = 128,
};

const struct ntasi_ans_hw ntasi_ans_hw_t8103 = {
    .submission_mode = NTASI_ANS_SUBMISSION_LINEAR_NVMMU,
    .max_queue_depth = 64,
    .admin_queue_depth = 2,
    .io_command_stride = NTASI_ANS_SQE_SIZE,
};

uint32_t ntasi_ans_aqa(uint32_t slots)
{
    /* nvme.c:366: ((NVME_QUEUE_SIZE - 1) << 16) | (NVME_QUEUE_SIZE - 1) */
    return ((slots - 1u) << 16) | (slots - 1u);
}

uint32_t ntasi_ans_max_pend_cmds(uint32_t slots)
{
    /* Linux apple.c:1157-1160: register takes the actual depth, not depth-1. */
    return (slots << 16) | slots;
}

uint32_t ntasi_ans_nvmmu_num(uint32_t slots)
{
    /* nvme.c:355: write32(nvme_base + NVMMU_NUM, NVME_QUEUE_SIZE - 1) */
    return slots - 1u;
}

uint32_t ntasi_ans_cc_config(void)
{
    /* Matches nvme_enable_ctrl(): standard 64-byte SQEs and 16-byte CQEs. */
    return NTASI_ANS_CC_IOSQES_64 | NTASI_ANS_CC_IOCQES_16;
}

uint32_t ntasi_ans_sq_db_off(bool admin)
{
    /* nvme.c:234-237 */
    return admin ? NTASI_ANS_REG_DB_LINEAR_ASQ : NTASI_ANS_REG_DB_LINEAR_IOSQ;
}

uint32_t ntasi_ans_conventional_sq_db_off(bool admin)
{
    return admin ? NTASI_ANS_REG_DB_ASQ : NTASI_ANS_REG_DB_IOSQ;
}

uint32_t ntasi_ans_cq_db_off(bool admin)
{
    /* nvme.c:270-273 */
    return admin ? NTASI_ANS_REG_DB_ACQ : NTASI_ANS_REG_DB_IOCQ;
}

size_t ntasi_ans_sq_bytes(uint32_t slots)
{
    return (size_t)slots * NTASI_ANS_SQE_SIZE;
}

size_t ntasi_ans_cq_bytes(uint32_t slots)
{
    return (size_t)slots * NTASI_ANS_CQE_SIZE;
}

size_t ntasi_ans_tcb_bytes(uint32_t slots)
{
    return (size_t)slots * NTASI_ANS_TCB_SIZE;
}

size_t ntasi_ans_command_bytes(const struct ntasi_ans_hw *hw,
                               bool admin, uint32_t slots)
{
    size_t stride;

    if (hw == NULL)
        return 0;
    stride = admin ? NTASI_ANS_SQE_SIZE : hw->io_command_stride;
    return (size_t)slots * stride;
}

void ntasi_ans_tcb_fill(struct ntasi_ans_tcb *tcb,
                        const struct ntasi_ans_sqe *sqe,
                        enum ntasi_ans_dma_direction direction)
{
    /* Linux apple.c:826-837. */
    *tcb = (struct ntasi_ans_tcb){0};
    tcb->opcode = sqe->opcode;
    tcb->dma_flags = (uint8_t)direction;
    tcb->command_id = sqe->tag;
    tcb->length = (uint16_t)sqe->cdw12;
    tcb->prp1 = sqe->prp1;
    tcb->prp2 = sqe->prp2;
}

void ntasi_ans_cmd_flush(struct ntasi_ans_sqe *c, uint32_t nsid)
{
    /* nvme.c:522-525 */
    *c = (struct ntasi_ans_sqe){0};
    c->opcode = NTASI_ANS_CMD_FLUSH;
    c->nsid = nsid;
}

void ntasi_ans_cmd_rw(struct ntasi_ans_sqe *c, uint8_t opcode, uint32_t nsid, uint64_t lba,
                      uint32_t cdw12, uint64_t prp1, uint64_t prp2)
{
    /* nvme.c:541-549 (READ path; see header for the WRITE generalization note) */
    *c = (struct ntasi_ans_sqe){0};
    c->opcode = opcode;
    c->nsid = nsid;
    c->prp1 = prp1;
    c->prp2 = prp2;
    c->cdw10 = (uint32_t)(lba & 0xffffffffu);
    c->cdw11 = (uint32_t)(lba >> 32);
    c->cdw12 = cdw12;
}

void ntasi_ans_cmd_create_iocq(struct ntasi_ans_sqe *c, uint16_t qid, uint32_t slots,
                               uint64_t prp1)
{
    /* nvme.c:373-384 */
    *c = (struct ntasi_ans_sqe){0};
    c->opcode = NTASI_ANS_ADMIN_CMD_CREATE_CQ;
    c->prp1 = prp1;
    c->cdw10 = (uint32_t)qid | ((slots - 1u) << 16);
    c->cdw11 = NTASI_ANS_QUEUE_CONTIGUOUS;
}

void ntasi_ans_cmd_create_iosq(struct ntasi_ans_sqe *c, uint16_t qid, uint16_t cqid,
                               uint32_t slots, uint64_t prp1)
{
    /* nvme.c:386-396 */
    *c = (struct ntasi_ans_sqe){0};
    c->opcode = NTASI_ANS_ADMIN_CMD_CREATE_SQ;
    c->prp1 = prp1;
    c->cdw10 = (uint32_t)qid | ((slots - 1u) << 16);
    c->cdw11 = NTASI_ANS_QUEUE_CONTIGUOUS | ((uint32_t)cqid << 16);
}

void ntasi_ans_cmd_delete_q(struct ntasi_ans_sqe *c, uint8_t opcode, uint16_t qid)
{
    /* nvme.c:482-494 */
    *c = (struct ntasi_ans_sqe){0};
    c->opcode = opcode;
    c->cdw10 = qid;
}

bool ntasi_ans_cqe_ready(uint16_t cqe_status, uint8_t phase)
{
    /* nvme.c:248: if ((cqe.status & 1) != q->cq_phase) continue; */
    return (uint8_t)(cqe_status & 1u) == phase;
}

uint16_t ntasi_ans_cqe_code(uint16_t cqe_status)
{
    /* nvme.c:282-284: cqe.status >>= 1 */
    return (uint16_t)(cqe_status >> 1);
}

void ntasi_ans_cq_advance(struct ntasi_ans_cq_state *st, uint32_t slots)
{
    /* nvme.c:264-268 */
    st->head += 1u;
    if ((uint32_t)st->head == slots) {
        st->head = 0;
        st->phase ^= 1u;
    }
}

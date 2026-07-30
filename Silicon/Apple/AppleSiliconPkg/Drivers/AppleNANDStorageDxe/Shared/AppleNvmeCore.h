/* SPDX-License-Identifier: MIT */
#ifndef NTASI_APPLE_NVME_H
#define NTASI_APPLE_NVME_H

/*
 * Apple ANS2/NVMe storage controller contract, host-testable seam.
 *
 * Ported from the MIT-licensed m1n1 driver and the upstream/downstream Asahi
 * Linux apple-nvme driver. ANS is a coprocessor (ASC mailbox + RTKit
 * firmware) fronting the internal boot NVMe controller. T8015-class ANS uses
 * conventional NVMe SQ tail doorbells (with 128-byte I/O SQ entries), while
 * T8103 and every currently supported ANS2/ANS3 compatible use linear SQs
 * plus a proprietary NVMMU with one 128-byte TCB per command tag. Both use
 * Apple's field-modified 64-byte SQEs / 16-byte CQEs (u8 tag instead of u16
 * CID; u64 result instead of DW0/DW1).
 *
 * This header models only the parts of the contract that are pure data:
 * struct layouts, register offsets, and register-value/state-machine
 * arithmetic. No MMIO, no RTKit/ASC transport, no timing, no ADT parsing —
 * those belong to the runtime driver layer. See
 * docs/nvme-ans-contract.md for the full spec this seam implements, and
 * that document's "Open questions" (also mirrored in this seam's README)
 * for what is verified-but-not-yet-tested on real T6050 hardware.
 *
 * Every constant here traces to a specific nvme.c line; see the .c file and
 * README for citations. Deliberately standalone: does not include or depend
 * on apple-sart-core, apple-rtkit-core, or any other sibling seam.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Sizes (nvme.c:16, 119-121, 138-146, 538)                            */
/* ------------------------------------------------------------------ */

#define NTASI_ANS_QUEUE_SLOTS 64u /* NVME_QUEUE_SIZE, nvme.c:16 */
#define NTASI_ANS_SQE_SIZE    64u /* sizeof(struct nvme_command), nvme.c:119 */
#define NTASI_ANS_CQE_SIZE    16u /* sizeof(struct nvme_completion), nvme.c:120 */
#define NTASI_ANS_TCB_SIZE    128u /* sizeof(struct apple_nvmmu_tcb), nvme.c:121 */
#define NTASI_ANS_QUEUE_ALIGN 16384u /* memalign(SZ_16K, ...), nvme.c:138-146 */
#define NTASI_ANS_DATA_ALIGN  4096u /* NVMe page size, nvme.c:538 */

/* ------------------------------------------------------------------ */
/* Register map (offsets relative to the ANS MMIO base, nvme.c:18-56) */
/* ------------------------------------------------------------------ */

/* Standard NVMe registers used (spec-compatible). */
#define NTASI_ANS_REG_CC      0x14u /* nvme.c:18 */
#define NTASI_ANS_REG_CSTS    0x1cu /* nvme.c:25 */
#define NTASI_ANS_REG_AQA     0x24u /* nvme.c:32, written nvme.c:366 */
#define NTASI_ANS_REG_ASQ     0x28u /* nvme.c:33, written nvme.c:364 */
#define NTASI_ANS_REG_ACQ     0x30u /* nvme.c:34, written nvme.c:365 */
#define NTASI_ANS_REG_DB_ASQ  0x1000u
#define NTASI_ANS_REG_DB_ACQ  0x1004u /* nvme.c:36, written nvme.c:271 */
#define NTASI_ANS_REG_DB_IOSQ 0x1008u
#define NTASI_ANS_REG_DB_IOCQ 0x100cu /* nvme.c:37, written nvme.c:273 */

/* CC fields (nvme.c:18-23). */
#define NTASI_ANS_CC_EN         (1u << 0)
#define NTASI_ANS_CC_SHN_SHIFT  14u
#define NTASI_ANS_CC_SHN_MASK   (0x3u << NTASI_ANS_CC_SHN_SHIFT) /* GENMASK(15,14) */
#define NTASI_ANS_CC_IOSQES_SHIFT 16u
#define NTASI_ANS_CC_IOSQES_MASK  (0xfu << NTASI_ANS_CC_IOSQES_SHIFT)
#define NTASI_ANS_CC_IOCQES_SHIFT 20u
#define NTASI_ANS_CC_IOCQES_MASK  (0xfu << NTASI_ANS_CC_IOCQES_SHIFT)
#define NTASI_ANS_CC_IOSQES_64    (6u << NTASI_ANS_CC_IOSQES_SHIFT)
#define NTASI_ANS_CC_IOCQES_16    (4u << NTASI_ANS_CC_IOCQES_SHIFT)
#define NTASI_ANS_CC_SHN_NONE   0u
#define NTASI_ANS_CC_SHN_NORMAL 1u
#define NTASI_ANS_CC_SHN_ABRUPT 2u

/* CSTS fields (nvme.c:25-30). */
#define NTASI_ANS_CSTS_RDY         (1u << 0)
#define NTASI_ANS_CSTS_SHST_SHIFT  2u
#define NTASI_ANS_CSTS_SHST_MASK   (0x3u << NTASI_ANS_CSTS_SHST_SHIFT) /* GENMASK(3,2) */
#define NTASI_ANS_CSTS_SHST_NORMAL 0u
#define NTASI_ANS_CSTS_SHST_BUSY   1u
#define NTASI_ANS_CSTS_SHST_DONE   2u

/* Apple-proprietary registers. */
#define NTASI_ANS_REG_BOOT_STATUS 0x1300u /* nvme.c:39, polled nvme.c:345 */
#define NTASI_ANS_BOOT_STATUS_OK  0xde71ce55u /* nvme.c:40 */

#define NTASI_ANS_REG_MAX_PEND_CMDS 0x1210u /* nvme.c:48, written nvme.c:353-354 */

#define NTASI_ANS_REG_UNKNOWN_CTRL       0x24008u /* nvme.c:45, cleared nvme.c:352 */
#define NTASI_ANS_UNKCTRL_PRP_NULL_CHECK (1u << 11) /* nvme.c:46 */

#define NTASI_ANS_REG_LINEAR_SQ_CTRL 0x24908u /* nvme.c:42, set nvme.c:351 */
#define NTASI_ANS_LINEAR_SQ_EN       (1u << 0) /* nvme.c:43 */

#define NTASI_ANS_REG_DB_LINEAR_ASQ  0x2490cu /* nvme.c:49, written nvme.c:235 */
#define NTASI_ANS_REG_DB_LINEAR_IOSQ 0x24910u /* nvme.c:50, written nvme.c:237 */

#define NTASI_ANS_REG_NVMMU_NUM       0x28100u /* nvme.c:52, written nvme.c:355 */
#define NTASI_ANS_REG_NVMMU_ASQ_BASE  0x28108u /* nvme.c:53, written nvme.c:356 */
#define NTASI_ANS_REG_NVMMU_IOSQ_BASE 0x28110u /* nvme.c:54, written nvme.c:357 */
#define NTASI_ANS_REG_NVMMU_TCB_INVAL 0x28118u /* nvme.c:55, written nvme.c:259 */
#define NTASI_ANS_REG_NVMMU_TCB_STAT 0x29120u /* nvme.c:56, read nvme.c:260 -- corrected
                                                * 2026-07-30: this was 0x28120u, sourced from
                                                * Linux apple.c:61 instead of m1n1's own nvme.c.
                                                * m1n1's nvme.c -- proven working on this exact
                                                * hardware the same night, RTKit boot + SART v3 +
                                                * controller init + real block reads all
                                                * succeeded -- uses 0x29120u for this register.
                                                * A wrong offset here does not hang: it makes
                                                * ntasi_ans_controller_execute() read whatever
                                                * unrelated register happens to live at 0x28120,
                                                * and treat a nonzero result as
                                                * NTASI_ANS_CONTROLLER_ERR_TCB_INVALIDATE on the
                                                * very first admin command (queue creation during
                                                * controller start), which is a clean, bounded
                                                * failure -- but it is still wrong and must not
                                                * ship. */

/* Admin command opcodes (nvme.c:58-62). */
#define NTASI_ANS_ADMIN_CMD_DELETE_SQ 0x00u
#define NTASI_ANS_ADMIN_CMD_CREATE_SQ 0x01u
#define NTASI_ANS_ADMIN_CMD_DELETE_CQ 0x04u
#define NTASI_ANS_ADMIN_CMD_CREATE_CQ 0x05u
#define NTASI_ANS_QUEUE_CONTIGUOUS    (1u << 0) /* NVME_QUEUE_CONTIGUOUS, nvme.c:62 */

/* IO command opcodes (nvme.c:64-66). */
#define NTASI_ANS_CMD_FLUSH 0x00u
#define NTASI_ANS_CMD_WRITE 0x01u
#define NTASI_ANS_CMD_READ  0x02u

/* NVMMU TCB DMA direction flags (Linux apple.c:106-107, 833-837). */
#define NTASI_ANS_TCB_DMA_FROM_DEVICE (1u << 0)
#define NTASI_ANS_TCB_DMA_TO_DEVICE   (1u << 1)

enum ntasi_ans_dma_direction {
    NTASI_ANS_DMA_FROM_DEVICE = NTASI_ANS_TCB_DMA_FROM_DEVICE,
    NTASI_ANS_DMA_TO_DEVICE = NTASI_ANS_TCB_DMA_TO_DEVICE,
};

enum ntasi_ans_submission_mode {
    NTASI_ANS_SUBMISSION_CONVENTIONAL = 0,
    NTASI_ANS_SUBMISSION_LINEAR_NVMMU = 1,
};

struct ntasi_ans_hw {
    enum ntasi_ans_submission_mode submission_mode;
    uint32_t max_queue_depth;
    uint32_t admin_queue_depth;
    uint32_t io_command_stride;
};

extern const struct ntasi_ans_hw ntasi_ans_hw_t8015;
extern const struct ntasi_ans_hw ntasi_ans_hw_t8103;

/* ------------------------------------------------------------------ */
/* Queue entry layouts (nvme.c:68-106, 119-121)                        */
/* ------------------------------------------------------------------ */

/*
 * PACKING PORTABILITY -- read before moving any struct across this line.
 *
 * ntasi_ans_sqe/ntasi_ans_cqe/ntasi_ans_tcb below are ANS2 wire-ABI layouts
 * (spec-adjacent NVMe SQE/CQE plus Apple's proprietary NVMMU TCB) and must be
 * byte-exact. GCC/Clang express that with `__attribute__((packed))`; MSVC's
 * cl.exe cannot parse that syntax at all, which matters because a Windows
 * ANS/NVMe kernel driver built with cl.exe would include this header (see
 * src/agx-initdata-core/agx_initdata.h for the precedent: this exact
 * incompatibility blocked all 14 `gpu/agxkmd` translation units, 1530 error
 * lines from one root cause, until that seam adopted the same fix applied
 * here).
 *
 * So: the attribute becomes NTASI_ANS_PACKED (empty under MSVC), and MSVC
 * instead gets a `#pragma pack(push, 1)` region covering the same structs.
 *
 * WHY A REGION AND NOT THE WHOLE FILE: this header also contains
 * `struct ntasi_ans_cq_state` (below, CQ head/phase consumer state), which is
 * deliberately NOT packed -- it is host-side bookkeeping, not a wire struct.
 * It sits well outside this region (after the register-value helpers,
 * command builders, etc.), so a whole-file pragma is not needed and is not
 * used; only ntasi_ans_sqe/ntasi_ans_cqe/ntasi_ans_tcb (verified below, by
 * `grep -n "^struct \|attribute__((packed))\|^};" apple_nvme.h`, to be the
 * only three PACKED structs in this file and to be textually contiguous, with
 * no unpacked struct interleaved between them) are inside the pushed region.
 *
 * This is verified, not asserted: every struct in the region carries
 * `_Static_assert(sizeof(...) == N)` and per-field `offsetof` checks below,
 * so if MSVC's packing ever disagreed with the GCC/Clang layout the build
 * would fail rather than emit a driver with silently wrong ANS2 wire structs.
 */
#if defined(_MSC_VER)
#  define NTASI_ANS_PACKED
#  pragma pack(push, 1)
#else
#  define NTASI_ANS_PACKED __attribute__((packed))
#endif

/*
 * Submission queue entry -- 64 bytes. Identical to a spec NVMe SQE except
 * the 16-bit Command Identifier (spec bytes 2-3) is split into a u8 tag
 * (byte 2) + u8 rsvd (byte 3): "normal NVMe has tag as u16" (nvme.c:72).
 */
struct ntasi_ans_sqe {
    uint8_t opcode;   /* 0x00 */
    uint8_t flags;    /* 0x01 */
    uint8_t tag;      /* 0x02 -- u8, not u16 (nvme.c:71-72) */
    uint8_t rsvd;     /* 0x03 */
    uint32_t nsid;    /* 0x04 */
    uint32_t cdw2;    /* 0x08 */
    uint32_t cdw3;    /* 0x0c */
    uint64_t metadata; /* 0x10 */
    uint64_t prp1;    /* 0x18 */
    uint64_t prp2;    /* 0x20 */
    uint32_t cdw10;   /* 0x28 */
    uint32_t cdw11;   /* 0x2c */
    uint32_t cdw12;   /* 0x30 */
    uint32_t cdw13;   /* 0x34 */
    uint32_t cdw14;   /* 0x38 */
    uint32_t cdw15;   /* 0x3c */
} NTASI_ANS_PACKED;

_Static_assert(offsetof(struct ntasi_ans_sqe, tag) == 0x02,
               "sqe tag offset (u8, not spec u16 CID), nvme.c:71");
_Static_assert(offsetof(struct ntasi_ans_sqe, nsid) == 0x04, "sqe nsid offset");
_Static_assert(offsetof(struct ntasi_ans_sqe, metadata) == 0x10, "sqe metadata offset");
_Static_assert(offsetof(struct ntasi_ans_sqe, prp1) == 0x18, "sqe prp1 offset");
_Static_assert(offsetof(struct ntasi_ans_sqe, prp2) == 0x20, "sqe prp2 offset");
_Static_assert(offsetof(struct ntasi_ans_sqe, cdw10) == 0x28, "sqe cdw10 offset");
_Static_assert(offsetof(struct ntasi_ans_sqe, cdw11) == 0x2c, "sqe cdw11 offset");
_Static_assert(offsetof(struct ntasi_ans_sqe, cdw12) == 0x30, "sqe cdw12 offset");
_Static_assert(sizeof(struct ntasi_ans_sqe) == 64, "sqe must be 64 bytes, nvme.c:119");

/*
 * Completion queue entry -- 16 bytes. Spec's u32 DW0 + u32 DW1 become one
 * u64 result; spec's sq_head/sq_id become a reserved u32 (nvme.c:87-92).
 */
struct ntasi_ans_cqe {
    uint64_t result; /* 0x00 -- spec: u32 DW0 + u32 DW1 */
    uint32_t rsvd;   /* 0x08 -- spec: sq_head (u16) + sq_id (u16) */
    uint16_t tag;    /* 0x0c -- spec: command identifier */
    uint16_t status; /* 0x0e -- bit0 phase, bits[15:1] status code */
} NTASI_ANS_PACKED;

_Static_assert(offsetof(struct ntasi_ans_cqe, rsvd) == 0x08, "cqe rsvd offset");
_Static_assert(offsetof(struct ntasi_ans_cqe, tag) == 0x0c, "cqe tag offset");
_Static_assert(offsetof(struct ntasi_ans_cqe, status) == 0x0e, "cqe status offset");
_Static_assert(sizeof(struct ntasi_ans_cqe) == 16, "cqe must be 16 bytes, nvme.c:120");

/*
 * NVMMU TCB (translation control block) -- 128 bytes, one per SQ slot,
 * indexed by tag. Authorizes DMA for that slot (nvme.c:94-106).
 */
struct ntasi_ans_tcb {
    uint8_t opcode;       /* 0x00 -- copy of SQE opcode */
    uint8_t dma_flags;    /* 0x01 -- FROM_DEVICE or TO_DEVICE */
    uint8_t command_id;   /* 0x02 -- == tag */
    uint8_t _unk0;        /* 0x03 -- 0 */
    uint16_t length;      /* 0x04 -- low 16 bits of SQE cdw12 */
    uint8_t _unk1[18];    /* 0x06 -- 0 */
    uint64_t prp1;        /* 0x18 -- copy of SQE prp1 */
    uint64_t prp2;        /* 0x20 -- copy of SQE prp2 */
    uint8_t _unk2[16];    /* 0x28 -- 0 */
    uint8_t aes_iv[8];    /* 0x38 -- 0, inline-crypto related, unused */
    uint8_t _aes_unk[64]; /* 0x40 -- 0 */
} NTASI_ANS_PACKED;

_Static_assert(offsetof(struct ntasi_ans_tcb, dma_flags) == 0x01, "tcb dma_flags offset");
_Static_assert(offsetof(struct ntasi_ans_tcb, command_id) == 0x02, "tcb command_id offset");
_Static_assert(offsetof(struct ntasi_ans_tcb, length) == 0x04, "tcb length offset");
_Static_assert(offsetof(struct ntasi_ans_tcb, _unk1) == 0x06, "tcb unknown field offset");
_Static_assert(offsetof(struct ntasi_ans_tcb, prp1) == 0x18, "tcb prp1 offset");
_Static_assert(offsetof(struct ntasi_ans_tcb, prp2) == 0x20, "tcb prp2 offset");
_Static_assert(offsetof(struct ntasi_ans_tcb, _unk2) == 0x28, "tcb unknown field offset");
_Static_assert(offsetof(struct ntasi_ans_tcb, aes_iv) == 0x38, "tcb aes_iv offset");
_Static_assert(offsetof(struct ntasi_ans_tcb, _aes_unk) == 0x40, "tcb _aes_unk offset");
_Static_assert(sizeof(struct ntasi_ans_tcb) == 128, "tcb must be 128 bytes, nvme.c:121");

/* NTASI_ANS_PACK_POP -- end of the byte-exact ANS2 wire-ABI region opened
 * above. ntasi_ans_cq_state (below, host-side consumer bookkeeping) is NOT
 * packed and must stay outside the pragma region. */
#if defined(_MSC_VER)
#  pragma pack(pop)
#endif

/* ------------------------------------------------------------------ */
/* Register-value helpers (nvme.c:353-355, 366)                        */
/* ------------------------------------------------------------------ */

/* ((slots-1)<<16)|(slots-1); AQA, nvme.c:366. aqa(64) == 0x003F003F. */
uint32_t ntasi_ans_aqa(uint32_t slots);

/* Actual depth in each half; Linux apple.c programs (slots << 16) | slots. */
uint32_t ntasi_ans_max_pend_cmds(uint32_t slots);

/* slots-1; NVMMU_NUM, nvme.c:355. nvmmu_num(64) == 0x3F. */
uint32_t ntasi_ans_nvmmu_num(uint32_t slots);

/* NVM CSS, round-robin arbitration, 4 KiB MPS, 64 B SQE, and 16 B CQE. */
uint32_t ntasi_ans_cc_config(void);

/* ------------------------------------------------------------------ */
/* Doorbell offset selection (nvme.c:234-237, 270-273)                 */
/* ------------------------------------------------------------------ */

/* Linear-SQ tag doorbell: admin -> 0x2490c, IO -> 0x24910. */
uint32_t ntasi_ans_sq_db_off(bool admin);

/* Standard SQ tail doorbell for T8015's conventional queue mode. */
uint32_t ntasi_ans_conventional_sq_db_off(bool admin);

/* Spec-shaped CQ head doorbell: admin -> 0x1004, IO -> 0x100c. */
uint32_t ntasi_ans_cq_db_off(bool admin);

/* ------------------------------------------------------------------ */
/* Queue memory sizing (nvme.c:138-146)                                */
/* ------------------------------------------------------------------ */

size_t ntasi_ans_sq_bytes(uint32_t slots);  /* slots * 64  */
size_t ntasi_ans_cq_bytes(uint32_t slots);  /* slots * 16  */
size_t ntasi_ans_tcb_bytes(uint32_t slots); /* slots * 128 */
size_t ntasi_ans_command_bytes(const struct ntasi_ans_hw *hw,
                               bool admin, uint32_t slots);

/* ------------------------------------------------------------------ */
/* TCB packing from a command (nvme.c:222-228)                         */
/* ------------------------------------------------------------------ */

/*
 * Fill *tcb from *sqe as Linux's apple_nvme_submit_cmd() does: zero the TCB,
 * then opcode/command_id(=sqe->tag)/length(low 16 bits of sqe->cdw12)/PRPs,
 * and set the explicit DMA direction flag.
 * Caller must have set sqe->tag before calling (mirrors nvme.c setting
 * queue_cmd->tag = tag before the TCB is derived from queue_cmd).
 */
void ntasi_ans_tcb_fill(struct ntasi_ans_tcb *tcb,
                        const struct ntasi_ans_sqe *sqe,
                        enum ntasi_ans_dma_direction direction);

/* ------------------------------------------------------------------ */
/* Command builders (nvme.c:373-396, 485-494, 522-525, 541-549)        */
/* ------------------------------------------------------------------ */

/* NVME_CMD_FLUSH, nvme.c:522-525. Zeroes *c first. */
void ntasi_ans_cmd_flush(struct ntasi_ans_sqe *c, uint32_t nsid);

/*
 * Read/write command, modeled on nvme_read() (nvme.c:529-550): nsid, prp1,
 * cdw10 = lba & 0xffffffff, cdw11 = lba >> 32, cdw12 = block count, prp2 as
 * given (nvme.c only ever passes prp2 = 0; a second PRP page is unused for
 * the single 4 KiB block m1n1 issues). opcode is caller-supplied
 * (1 = NTASI_ANS_CMD_WRITE, 2 = NTASI_ANS_CMD_READ) so this builder covers
 * both; m1n1 itself only implements the READ path (there is no nvme_write()
 * in nvme.c) -- the WRITE field layout is a documented generalization by
 * symmetry with spec-standard NVMe RW command fields, not independently
 * verified in nvme.c. Zeroes *c first.
 */
void ntasi_ans_cmd_rw(struct ntasi_ans_sqe *c, uint8_t opcode, uint32_t nsid, uint64_t lba,
                      uint32_t cdw12, uint64_t prp1, uint64_t prp2);

/* CREATE I/O COMPLETION QUEUE, nvme.c:373-384. Zeroes *c first. */
void ntasi_ans_cmd_create_iocq(struct ntasi_ans_sqe *c, uint16_t qid, uint32_t slots,
                               uint64_t prp1);

/* CREATE I/O SUBMISSION QUEUE, nvme.c:386-396. Zeroes *c first. */
void ntasi_ans_cmd_create_iosq(struct ntasi_ans_sqe *c, uint16_t qid, uint16_t cqid,
                               uint32_t slots, uint64_t prp1);

/*
 * DELETE I/O SUBMISSION/COMPLETION QUEUE (nvme.c:482-494). opcode is
 * NTASI_ANS_ADMIN_CMD_DELETE_SQ (0x00) or _DELETE_CQ (0x04). Zeroes *c
 * first.
 */
void ntasi_ans_cmd_delete_q(struct ntasi_ans_sqe *c, uint8_t opcode, uint16_t qid);

/* ------------------------------------------------------------------ */
/* CQ head/phase consumer state machine (nvme.c:153-154, 242-268, 282)  */
/* ------------------------------------------------------------------ */

struct ntasi_ans_cq_state {
    uint8_t head;
    uint8_t phase;
};

/* Initial state after queue allocation: head 0, phase 1 (nvme.c:153-154). */
#define NTASI_ANS_CQ_STATE_INIT ((struct ntasi_ans_cq_state){.head = 0, .phase = 1})

/* A CQE is fresh (not yet consumed) when (status & 1) == phase, nvme.c:248. */
bool ntasi_ans_cqe_ready(uint16_t cqe_status, uint8_t phase);

/* Status code = status >> 1; 0 is success (nvme.c:282-284). */
uint16_t ntasi_ans_cqe_code(uint16_t cqe_status);

/*
 * Advance the consumer: head += 1; when head reaches `slots`, wrap to 0 and
 * flip phase (nvme.c:264-268).
 */
void ntasi_ans_cq_advance(struct ntasi_ans_cq_state *st, uint32_t slots);

#endif /* NTASI_APPLE_NVME_H */

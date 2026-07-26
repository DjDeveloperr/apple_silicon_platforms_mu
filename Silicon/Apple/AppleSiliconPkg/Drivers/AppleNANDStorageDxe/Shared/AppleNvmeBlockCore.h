/* SPDX-License-Identifier: MIT */
#ifndef NTASI_APPLE_NVME_BLOCK_CORE_H
#define NTASI_APPLE_NVME_BLOCK_CORE_H

#include "AppleNvmeCore.h"

#define NTASI_ANS_IDENTIFY_DATA_SIZE 4096u
#define NTASI_ANS_ADMIN_CMD_IDENTIFY 0x06u
#define NTASI_ANS_IDENTIFY_CNS_NAMESPACE 0x00u

enum ntasi_ans_block_result {
    NTASI_ANS_BLOCK_OK = 0,
    NTASI_ANS_BLOCK_ERR_ARGUMENT = -100,
    NTASI_ANS_BLOCK_ERR_ALIGNMENT = -101,
    NTASI_ANS_BLOCK_ERR_IDENTIFY = -102,
    NTASI_ANS_BLOCK_ERR_FORMAT = -103,
    NTASI_ANS_BLOCK_ERR_RANGE = -104,
    NTASI_ANS_BLOCK_ERR_BUFFER = -105,
    NTASI_ANS_BLOCK_ERR_NOT_IDENTIFIED = -106,
};

struct ntasi_ans_namespace {
    uint32_t nsid;
    uint64_t block_count;
    uint32_t block_size;
    uint8_t lba_format;
};

typedef int (*ntasi_ans_block_execute_fn)(
    void *opaque,
    bool admin,
    const struct ntasi_ans_sqe *command,
    enum ntasi_ans_dma_direction direction,
    uint64_t *result);

/*
 * Firmware-friendly block device. The DMA buffer is a reusable bounce buffer:
 * EFI callers do not need to supply aligned or physically contiguous memory.
 * The first implementation intentionally submits one logical block at a time,
 * matching the simplest proven ANS path while retaining spec-correct NLB.
 */
struct ntasi_ans_block_device {
    ntasi_ans_block_execute_fn execute;
    void *opaque;
    uint8_t *dma_buffer;
    uint64_t dma_address;
    size_t dma_size;
    struct ntasi_ans_namespace media;
    bool identified;
};

void ntasi_ans_cmd_identify_namespace(struct ntasi_ans_sqe *command,
                                      uint32_t nsid,
                                      uint64_t prp1);

int ntasi_ans_parse_identify_namespace(const void *identify_data,
                                       size_t identify_size,
                                       uint32_t nsid,
                                       struct ntasi_ans_namespace *media);

int ntasi_ans_block_device_init(struct ntasi_ans_block_device *device,
                                ntasi_ans_block_execute_fn execute,
                                void *opaque,
                                void *dma_buffer,
                                uint64_t dma_address,
                                size_t dma_size);

int ntasi_ans_block_identify(struct ntasi_ans_block_device *device,
                             uint32_t nsid);

int ntasi_ans_block_read(struct ntasi_ans_block_device *device,
                         uint64_t lba,
                         uint64_t block_count,
                         void *buffer,
                         size_t buffer_size);

int ntasi_ans_block_write(struct ntasi_ans_block_device *device,
                          uint64_t lba,
                          uint64_t block_count,
                          const void *buffer,
                          size_t buffer_size);

int ntasi_ans_block_flush(struct ntasi_ans_block_device *device);

#endif

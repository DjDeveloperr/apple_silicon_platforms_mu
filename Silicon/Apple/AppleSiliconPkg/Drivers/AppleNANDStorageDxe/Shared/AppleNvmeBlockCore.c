/* SPDX-License-Identifier: MIT */
#include "AppleNvmeBlockCore.h"

#include <limits.h>
#include <string.h>

#define NTASI_ANS_ID_NS_NSZE_OFFSET 0x00u
#define NTASI_ANS_ID_NS_FLBAS_OFFSET 0x1au
#define NTASI_ANS_ID_NS_LBAF_OFFSET 0x80u
#define NTASI_ANS_ID_NS_LBAF_SIZE 4u
#define NTASI_ANS_ID_NS_LBAF_COUNT 16u

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | (uint16_t)bytes[1] << 8);
}

static uint64_t read_le64(const uint8_t *bytes)
{
    uint64_t value = 0;
    unsigned int index;

    for (index = 0; index < 8; ++index)
        value |= (uint64_t)bytes[index] << (index * 8u);
    return value;
}

void ntasi_ans_cmd_identify_namespace(struct ntasi_ans_sqe *command,
                                      uint32_t nsid,
                                      uint64_t prp1)
{
    *command = (struct ntasi_ans_sqe){0};
    command->opcode = NTASI_ANS_ADMIN_CMD_IDENTIFY;
    command->nsid = nsid;
    command->prp1 = prp1;
    command->cdw10 = NTASI_ANS_IDENTIFY_CNS_NAMESPACE;
}

int ntasi_ans_parse_identify_namespace(const void *identify_data,
                                       size_t identify_size,
                                       uint32_t nsid,
                                       struct ntasi_ans_namespace *media)
{
    const uint8_t *bytes = identify_data;
    const uint8_t *lbaf;
    uint64_t block_count;
    uint16_t metadata_size;
    uint8_t lba_format;
    uint8_t block_shift;

    if (bytes == NULL || media == NULL || nsid == 0 ||
        identify_size < NTASI_ANS_IDENTIFY_DATA_SIZE)
        return NTASI_ANS_BLOCK_ERR_ARGUMENT;

    block_count = read_le64(bytes + NTASI_ANS_ID_NS_NSZE_OFFSET);
    lba_format = (uint8_t)(bytes[NTASI_ANS_ID_NS_FLBAS_OFFSET] & 0x0fu);
    if (lba_format >= NTASI_ANS_ID_NS_LBAF_COUNT)
        return NTASI_ANS_BLOCK_ERR_IDENTIFY;
    lbaf = bytes + NTASI_ANS_ID_NS_LBAF_OFFSET +
           (size_t)lba_format * NTASI_ANS_ID_NS_LBAF_SIZE;
    metadata_size = read_le16(lbaf);
    block_shift = lbaf[2];

    /* EFI Block I/O cannot describe per-LBA metadata in the data buffer. */
    if (block_count == 0 || metadata_size != 0 ||
        block_shift < 9 || block_shift >= 32)
        return NTASI_ANS_BLOCK_ERR_FORMAT;

    *media = (struct ntasi_ans_namespace){
        .nsid = nsid,
        .block_count = block_count,
        .block_size = UINT32_C(1) << block_shift,
        .lba_format = lba_format,
    };
    return NTASI_ANS_BLOCK_OK;
}

int ntasi_ans_block_device_init(struct ntasi_ans_block_device *device,
                                ntasi_ans_block_execute_fn execute,
                                void *opaque,
                                void *dma_buffer,
                                uint64_t dma_address,
                                size_t dma_size)
{
    if (device == NULL || execute == NULL || dma_buffer == NULL ||
        dma_size < NTASI_ANS_IDENTIFY_DATA_SIZE)
        return NTASI_ANS_BLOCK_ERR_ARGUMENT;
    if ((dma_address & (NTASI_ANS_DATA_ALIGN - 1u)) != 0)
        return NTASI_ANS_BLOCK_ERR_ALIGNMENT;

    *device = (struct ntasi_ans_block_device){
        .execute = execute,
        .opaque = opaque,
        .dma_buffer = dma_buffer,
        .dma_address = dma_address,
        .dma_size = dma_size,
    };
    return NTASI_ANS_BLOCK_OK;
}

int ntasi_ans_block_identify(struct ntasi_ans_block_device *device,
                             uint32_t nsid)
{
    struct ntasi_ans_namespace media;
    struct ntasi_ans_sqe command;
    int status;

    if (device == NULL || device->execute == NULL || nsid == 0)
        return NTASI_ANS_BLOCK_ERR_ARGUMENT;

    device->identified = false;
    memset(device->dma_buffer, 0, NTASI_ANS_IDENTIFY_DATA_SIZE);
    ntasi_ans_cmd_identify_namespace(&command, nsid, device->dma_address);
    status = device->execute(device->opaque, true, &command,
                             NTASI_ANS_DMA_FROM_DEVICE, NULL);
    if (status != 0)
        return status;
    status = ntasi_ans_parse_identify_namespace(device->dma_buffer,
                                                NTASI_ANS_IDENTIFY_DATA_SIZE,
                                                nsid, &media);
    if (status != NTASI_ANS_BLOCK_OK)
        return status;
    if (media.block_size > device->dma_size)
        return NTASI_ANS_BLOCK_ERR_BUFFER;

    device->media = media;
    device->identified = true;
    return NTASI_ANS_BLOCK_OK;
}

int ntasi_ans_block_read(struct ntasi_ans_block_device *device,
                         uint64_t lba,
                         uint64_t block_count,
                         void *buffer,
                         size_t buffer_size)
{
    uint8_t *output = buffer;
    uint64_t index;
    size_t required;

    if (device == NULL || device->execute == NULL)
        return NTASI_ANS_BLOCK_ERR_ARGUMENT;
    if (!device->identified)
        return NTASI_ANS_BLOCK_ERR_NOT_IDENTIFIED;
    if (block_count == 0)
        return NTASI_ANS_BLOCK_OK;
    if (output == NULL || block_count > SIZE_MAX / device->media.block_size)
        return NTASI_ANS_BLOCK_ERR_BUFFER;
    required = (size_t)block_count * device->media.block_size;
    if (buffer_size < required)
        return NTASI_ANS_BLOCK_ERR_BUFFER;
    if (lba >= device->media.block_count ||
        block_count > device->media.block_count - lba)
        return NTASI_ANS_BLOCK_ERR_RANGE;

    for (index = 0; index < block_count; ++index) {
        struct ntasi_ans_sqe command;
        int status;

        memset(device->dma_buffer, 0, device->media.block_size);
        /* NVMe NLB is zero-based: cdw12 == 0 transfers one logical block. */
        ntasi_ans_cmd_rw(&command, NTASI_ANS_CMD_READ, device->media.nsid,
                         lba + index, 0, device->dma_address, 0);
        status = device->execute(device->opaque, false, &command,
                                 NTASI_ANS_DMA_FROM_DEVICE, NULL);
        if (status != 0)
            return status;
        memcpy(output + (size_t)index * device->media.block_size,
               device->dma_buffer, device->media.block_size);
    }
    return NTASI_ANS_BLOCK_OK;
}

int ntasi_ans_block_write(struct ntasi_ans_block_device *device,
                          uint64_t lba,
                          uint64_t block_count,
                          const void *buffer,
                          size_t buffer_size)
{
    const uint8_t *input = buffer;
    uint64_t index;
    size_t required;

    if (device == NULL || device->execute == NULL)
        return NTASI_ANS_BLOCK_ERR_ARGUMENT;
    if (!device->identified)
        return NTASI_ANS_BLOCK_ERR_NOT_IDENTIFIED;
    if (block_count == 0)
        return NTASI_ANS_BLOCK_OK;
    if (input == NULL || block_count > SIZE_MAX / device->media.block_size)
        return NTASI_ANS_BLOCK_ERR_BUFFER;
    required = (size_t)block_count * device->media.block_size;
    if (buffer_size < required)
        return NTASI_ANS_BLOCK_ERR_BUFFER;
    if (lba >= device->media.block_count ||
        block_count > device->media.block_count - lba)
        return NTASI_ANS_BLOCK_ERR_RANGE;

    for (index = 0; index < block_count; ++index) {
        struct ntasi_ans_sqe command;
        int status;

        memcpy(device->dma_buffer,
               input + (size_t)index * device->media.block_size,
               device->media.block_size);
        ntasi_ans_cmd_rw(&command, NTASI_ANS_CMD_WRITE, device->media.nsid,
                         lba + index, 0, device->dma_address, 0);
        status = device->execute(device->opaque, false, &command,
                                 NTASI_ANS_DMA_TO_DEVICE, NULL);
        if (status != 0)
            return status;
    }
    return NTASI_ANS_BLOCK_OK;
}

int ntasi_ans_block_flush(struct ntasi_ans_block_device *device)
{
    struct ntasi_ans_sqe command;

    if (device == NULL || device->execute == NULL)
        return NTASI_ANS_BLOCK_ERR_ARGUMENT;
    if (!device->identified)
        return NTASI_ANS_BLOCK_ERR_NOT_IDENTIFIED;
    ntasi_ans_cmd_flush(&command, device->media.nsid);
    return device->execute(device->opaque, false, &command,
                           NTASI_ANS_DMA_FROM_DEVICE, NULL);
}

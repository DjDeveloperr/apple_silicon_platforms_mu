/* SPDX-License-Identifier: MIT */
#include "AppleSartRuntimeCore.h"

#include <stddef.h>

static uint16_t entry_bit(unsigned int entry)
{
    return (uint16_t)(UINT16_C(1) << entry);
}

static struct ntasi_sart_entry_values read_entry(
    struct ntasi_sart_runtime *runtime, unsigned int entry)
{
    struct ntasi_sart_entry_values values = {
        .config = runtime->ops.read32(
            runtime->opaque, NTASI_SART_CONFIG_OFFSET(entry)),
        .paddr = runtime->ops.read32(
            runtime->opaque, NTASI_SART_PADDR_OFFSET(entry)),
    };

    if (runtime->params->gen == NTASI_SART_GEN_V3)
        values.size = runtime->ops.read32(
            runtime->opaque, NTASI_SART_SIZE_OFFSET(entry));
    return values;
}

static void barrier(struct ntasi_sart_runtime *runtime)
{
    if (runtime->ops.write_barrier != NULL)
        runtime->ops.write_barrier(runtime->opaque);
}

/* Publish address/size first and flags last so no partial region is active. */
static void program_entry(struct ntasi_sart_runtime *runtime,
                          const struct ntasi_sart_entry_regs *regs)
{
    runtime->ops.write32(runtime->opaque, regs->paddr.offset,
                         regs->paddr.value);
    if (regs->num_writes == 3)
        runtime->ops.write32(runtime->opaque, regs->size.offset,
                             regs->size.value);
    barrier(runtime);
    runtime->ops.write32(runtime->opaque, regs->config.offset,
                         regs->config.value);
    barrier(runtime);
}

static void clear_entry(struct ntasi_sart_runtime *runtime,
                        unsigned int entry)
{
    /* Revoke permission first; stale address/size values are then harmless. */
    runtime->ops.write32(runtime->opaque, NTASI_SART_CONFIG_OFFSET(entry), 0);
    barrier(runtime);
    runtime->ops.write32(runtime->opaque, NTASI_SART_PADDR_OFFSET(entry), 0);
    if (runtime->params->gen == NTASI_SART_GEN_V3)
        runtime->ops.write32(runtime->opaque, NTASI_SART_SIZE_OFFSET(entry), 0);
    barrier(runtime);
}

int ntasi_sart_runtime_init(struct ntasi_sart_runtime *runtime,
                            const struct ntasi_sart_params *params,
                            const struct ntasi_sart_runtime_ops *ops,
                            void *opaque)
{
    unsigned int entry;

    if (runtime == NULL || params == NULL || ops == NULL ||
        ops->read32 == NULL || ops->write32 == NULL)
        return NTASI_SART_RUNTIME_ERR_ARGUMENT;
    *runtime = (struct ntasi_sart_runtime){
        .params = params,
        .ops = *ops,
        .opaque = opaque,
    };
    for (entry = 0; entry < NTASI_SART_MAX_ENTRIES; ++entry) {
        struct ntasi_sart_entry_values values = read_entry(runtime, entry);
        uint64_t paddr;
        uint64_t size;
        uint8_t flags;

        if (ntasi_sart_entry_decode(params, &values, &flags, &paddr,
                                    &size) != NTASI_SART_OK)
            return NTASI_SART_RUNTIME_ERR_ARGUMENT;
        if (flags != 0)
            runtime->protected_entries |= entry_bit(entry);
    }
    return NTASI_SART_RUNTIME_OK;
}

int ntasi_sart_runtime_add(struct ntasi_sart_runtime *runtime,
                           uint64_t paddr, uint64_t size,
                           unsigned int *entry_out)
{
    struct ntasi_sart_entry_regs regs;
    unsigned int entry;
    int status;

    if (runtime == NULL || runtime->params == NULL || size == 0)
        return NTASI_SART_RUNTIME_ERR_ARGUMENT;
    for (entry = 0; entry < NTASI_SART_MAX_ENTRIES; ++entry) {
        uint16_t bit = entry_bit(entry);

        if ((runtime->protected_entries & bit) != 0 ||
            (runtime->used_entries & bit) != 0)
            continue;
        status = ntasi_sart_entry_encode(runtime->params, entry,
                                         runtime->params->flags_allow,
                                         paddr, size, &regs);
        if (status != NTASI_SART_OK)
            return status;
        runtime->used_entries |= bit;
        program_entry(runtime, &regs);
        if (entry_out != NULL)
            *entry_out = entry;
        return NTASI_SART_RUNTIME_OK;
    }
    return NTASI_SART_RUNTIME_ERR_NO_SPACE;
}

int ntasi_sart_runtime_remove(struct ntasi_sart_runtime *runtime,
                              uint64_t paddr, uint64_t size)
{
    unsigned int entry;

    if (runtime == NULL || runtime->params == NULL || size == 0)
        return NTASI_SART_RUNTIME_ERR_ARGUMENT;
    for (entry = 0; entry < NTASI_SART_MAX_ENTRIES; ++entry) {
        struct ntasi_sart_entry_values values;
        uint64_t entry_paddr;
        uint64_t entry_size;
        uint8_t flags;
        uint16_t bit = entry_bit(entry);

        if ((runtime->used_entries & bit) == 0)
            continue;
        values = read_entry(runtime, entry);
        if (ntasi_sart_entry_decode(runtime->params, &values, &flags,
                                    &entry_paddr, &entry_size) !=
            NTASI_SART_OK)
            continue;
        if (entry_paddr != paddr || entry_size != size)
            continue;
        clear_entry(runtime, entry);
        runtime->used_entries &= (uint16_t)~bit;
        return NTASI_SART_RUNTIME_OK;
    }
    return NTASI_SART_RUNTIME_ERR_NOT_FOUND;
}

void ntasi_sart_runtime_clear_owned(struct ntasi_sart_runtime *runtime)
{
    unsigned int entry;

    if (runtime == NULL || runtime->params == NULL)
        return;
    for (entry = 0; entry < NTASI_SART_MAX_ENTRIES; ++entry) {
        uint16_t bit = entry_bit(entry);

        if ((runtime->used_entries & bit) == 0)
            continue;
        clear_entry(runtime, entry);
    }
    runtime->used_entries = 0;
}

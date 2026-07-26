/* SPDX-License-Identifier: MIT */
#include "AppleSartCore.h"

#include <stddef.h>

/*
 * Field masks mirror m1n1's APPLE_SARTn_CONFIG_FLAGS / APPLE_SARTn_CONFIG_SIZE
 * (sart.c). SARTv3's CONFIG register holds the raw flags byte with no
 * bit-packing and has a dedicated SIZE register, so its config_flags_mask
 * and config_size_mask are 0 (unused) here.
 */
_Static_assert(NTASI_SART_GENMASK(28, 24) == 0x1f000000u, "SARTv0 CONFIG.FLAGS mask");
_Static_assert(NTASI_SART_GENMASK(18, 0) == 0x0007ffffu, "SARTv0 CONFIG.SIZE mask / SIZE_MAX");
_Static_assert(NTASI_SART_GENMASK(31, 24) == 0xff000000u, "SARTv2 CONFIG.FLAGS mask");
_Static_assert(NTASI_SART_GENMASK(23, 0) == 0x00ffffffu, "SARTv2 CONFIG.SIZE mask / SIZE_MAX");
_Static_assert(NTASI_SART_GENMASK(29, 0) == 0x3fffffffu, "SARTv3 SIZE_MAX");

const struct ntasi_sart_params ntasi_sart_params_v0 = {
    .gen = NTASI_SART_GEN_V0,
    .config_flags_mask = (uint32_t)NTASI_SART_GENMASK(28, 24),
    .config_size_mask = (uint32_t)NTASI_SART_GENMASK(18, 0),
    .size_max = (uint32_t)NTASI_SART_GENMASK(18, 0), /* 0x7ffff */
    .flags_allow = 0xf,
};

const struct ntasi_sart_params ntasi_sart_params_v2 = {
    .gen = NTASI_SART_GEN_V2,
    .config_flags_mask = (uint32_t)NTASI_SART_GENMASK(31, 24),
    .config_size_mask = (uint32_t)NTASI_SART_GENMASK(23, 0),
    .size_max = (uint32_t)NTASI_SART_GENMASK(23, 0), /* 0xffffff */
    .flags_allow = 0xff,
};

const struct ntasi_sart_params ntasi_sart_params_v3 = {
    .gen = NTASI_SART_GEN_V3,
    .config_flags_mask = 0, /* CONFIG holds the raw flags byte, not a bitfield */
    .config_size_mask = 0,  /* SIZE is a separate register, not packed into CONFIG */
    .size_max = (uint32_t)NTASI_SART_GENMASK(29, 0), /* 0x3fffffff */
    .flags_allow = 0xff,
};

uint32_t ntasi_sart_field_shift(uint32_t mask)
{
    uint32_t shift = 0;

    if (mask == 0)
        return 0;
    while ((mask & 1u) == 0) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

bool ntasi_sart_flags_allowed(const struct ntasi_sart_params *params, uint8_t flags)
{
    if (params == NULL)
        return false;
    return (flags & (uint8_t)~params->flags_allow) == 0;
}

int ntasi_sart_reg_config(const struct ntasi_sart_params *params, unsigned index,
                          uint32_t *offset)
{
    if (params == NULL || offset == NULL)
        return NTASI_SART_ERR_ARGUMENT;
    if (index >= NTASI_SART_MAX_ENTRIES)
        return NTASI_SART_ERR_INDEX;

    *offset = NTASI_SART_CONFIG_OFFSET(index);
    return NTASI_SART_OK;
}

int ntasi_sart_reg_paddr(const struct ntasi_sart_params *params, unsigned index,
                         uint32_t *offset)
{
    if (params == NULL || offset == NULL)
        return NTASI_SART_ERR_ARGUMENT;
    if (index >= NTASI_SART_MAX_ENTRIES)
        return NTASI_SART_ERR_INDEX;

    *offset = NTASI_SART_PADDR_OFFSET(index);
    return NTASI_SART_OK;
}

int ntasi_sart_reg_size(const struct ntasi_sart_params *params, unsigned index,
                        uint32_t *offset)
{
    if (params == NULL || offset == NULL)
        return NTASI_SART_ERR_ARGUMENT;
    if (index >= NTASI_SART_MAX_ENTRIES)
        return NTASI_SART_ERR_INDEX;
    if (params->gen != NTASI_SART_GEN_V3)
        return NTASI_SART_ERR_ARGUMENT; /* no SIZE register on v0/v2 */

    *offset = NTASI_SART_SIZE_OFFSET(index);
    return NTASI_SART_OK;
}

int ntasi_sart_entry_encode(const struct ntasi_sart_params *params, unsigned index, uint8_t flags,
                            uint64_t paddr, uint64_t size, struct ntasi_sart_entry_regs *regs)
{
    if (params == NULL || regs == NULL)
        return NTASI_SART_ERR_ARGUMENT;
    if (index >= NTASI_SART_MAX_ENTRIES)
        return NTASI_SART_ERR_INDEX;
    if (!ntasi_sart_flags_allowed(params, flags))
        return NTASI_SART_ERR_FLAGS;
    if ((paddr & (NTASI_SART_PAGE_SIZE - 1)) != 0)
        return NTASI_SART_ERR_ALIGN;
    if ((size & (NTASI_SART_PAGE_SIZE - 1)) != 0)
        return NTASI_SART_ERR_ALIGN;

    uint64_t paddr_pages = paddr >> NTASI_SART_PAGE_SHIFT;
    uint64_t size_pages = size >> NTASI_SART_PAGE_SHIFT;

    if (size_pages > params->size_max)
        return NTASI_SART_ERR_RANGE;
    /* Every generation's PADDR register is 32 bits; reject rather than
     * silently truncate a physical page number too large to fit it. */
    if (paddr_pages > UINT32_MAX)
        return NTASI_SART_ERR_RANGE;

    struct ntasi_sart_entry_regs out;
    out.config.offset = NTASI_SART_CONFIG_OFFSET(index);
    out.paddr.offset = NTASI_SART_PADDR_OFFSET(index);
    out.paddr.value = (uint32_t)paddr_pages;

    if (params->gen == NTASI_SART_GEN_V3) {
        out.config.value = (uint32_t)flags;
        out.size.offset = NTASI_SART_SIZE_OFFSET(index);
        out.size.value = (uint32_t)size_pages;
        out.num_writes = 3;
    } else {
        uint32_t flags_shift = ntasi_sart_field_shift(params->config_flags_mask);
        uint32_t size_shift = ntasi_sart_field_shift(params->config_size_mask);

        out.config.value =
            (((uint32_t)flags << flags_shift) & params->config_flags_mask) |
            (((uint32_t)size_pages << size_shift) & params->config_size_mask);
        out.size.offset = 0;
        out.size.value = 0;
        out.num_writes = 2;
    }

    *regs = out;
    return NTASI_SART_OK;
}

int ntasi_sart_entry_decode(const struct ntasi_sart_params *params,
                            const struct ntasi_sart_entry_values *regs, uint8_t *flags,
                            uint64_t *paddr, uint64_t *size)
{
    if (params == NULL || regs == NULL || flags == NULL || paddr == NULL || size == NULL)
        return NTASI_SART_ERR_ARGUMENT;

    if (params->gen == NTASI_SART_GEN_V3) {
        *flags = (uint8_t)(regs->config & 0xffu);
        *size = (uint64_t)regs->size << NTASI_SART_PAGE_SHIFT;
    } else {
        uint32_t flags_shift = ntasi_sart_field_shift(params->config_flags_mask);
        uint32_t size_shift = ntasi_sart_field_shift(params->config_size_mask);

        *flags = (uint8_t)((regs->config & params->config_flags_mask) >> flags_shift);
        *size = (uint64_t)((regs->config & params->config_size_mask) >> size_shift)
                << NTASI_SART_PAGE_SHIFT;
    }
    *paddr = (uint64_t)regs->paddr << NTASI_SART_PAGE_SHIFT;

    return NTASI_SART_OK;
}

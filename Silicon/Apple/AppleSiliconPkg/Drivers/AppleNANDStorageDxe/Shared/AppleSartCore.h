/* SPDX-License-Identifier: MIT */
#ifndef NTASI_APPLE_SART_H
#define NTASI_APPLE_SART_H

/*
 * Apple SART (DMA address-filter) register contract, host-testable seam.
 *
 * Ported from the MIT-licensed m1n1 driver (repos/m1n1/src/sart.c). SART sits
 * in front of ANS/NVMe's RTKit coprocessor and gates which physical address
 * ranges it may DMA into or out of: up to 16 independent (flags, physical
 * address, size) allow-region entries, page granularity 4 KiB (shift 12).
 *
 * Three hardware generations, each with 16 entries:
 *   - SARTv0: a single CONFIG register per entry packs FLAGS (bits[28:24])
 *     and SIZE>>12 (bits[18:0]); PADDR is a separate register (paddr>>12).
 *   - SARTv2: same CONFIG/PADDR split as v0, but with wider FLAGS
 *     (bits[31:24]) and SIZE (bits[23:0]) fields, hence a larger SIZE_MAX.
 *   - SARTv3: CONFIG holds the raw flags byte directly (no bit-packing);
 *     PADDR and SIZE are each their own register (paddr>>12 / size>>12),
 *     with a much wider 30-bit SIZE field.
 *
 * CONFIG/PADDR live at the same byte offsets on all three generations
 * (0x00 + 4*idx and 0x40 + 4*idx respectively); only SARTv3 has a SIZE
 * register, at 0x80 + 4*idx.
 *
 * Which generation a given SoC's SART instance is (v0 / v2 / v3) is an ADT
 * fact -- m1n1 reads the `sart-version` property (or infers v0 from the
 * `sart,t8015` compatible string) -- NOT decided here. Callers pass the
 * generation's `ntasi_sart_params` in; this seam only pins the register
 * offset math, field masks, and the encode/decode contract.
 */

#include <stdbool.h>
#include <stdint.h>

#define NTASI_SART_MAX_ENTRIES 16u
#define NTASI_SART_PAGE_SHIFT  12u
#define NTASI_SART_PAGE_SIZE   (1u << NTASI_SART_PAGE_SHIFT) /* 4 KiB */

/* GENMASK(h,l): contiguous bit field, bits l..h inclusive. Matches m1n1's
 * types.h GENMASK, and apple-dart-core's NTASI_DART_GENMASK. */
#define NTASI_SART_GENMASK(h, l) \
    ((((uint64_t)~0ull) - (((uint64_t)1 << (l)) - 1)) & \
     (((uint64_t)~0ull) >> (63 - (h))))

/* Register byte offsets from the SART base, given a 0..15 entry index.
 * Identical formulas on all three generations -- only SARTv3 populates
 * the SIZE register; v0/v2 fold size into CONFIG instead. */
#define NTASI_SART_CONFIG_OFFSET(idx) (0x00u + 4u * (uint32_t)(idx))
#define NTASI_SART_PADDR_OFFSET(idx)  (0x40u + 4u * (uint32_t)(idx))
#define NTASI_SART_SIZE_OFFSET(idx)   (0x80u + 4u * (uint32_t)(idx)) /* v3 only */

enum ntasi_sart_status {
    NTASI_SART_OK = 0,
    NTASI_SART_ERR_ARGUMENT = -1, /* NULL pointer, or register N/A for this generation */
    NTASI_SART_ERR_ALIGN = -2,    /* paddr or size not 4 KiB aligned */
    NTASI_SART_ERR_RANGE = -3,    /* (size >> 12) exceeds this generation's SIZE_MAX */
    NTASI_SART_ERR_INDEX = -4,    /* index >= NTASI_SART_MAX_ENTRIES */
    NTASI_SART_ERR_FLAGS = -5,    /* flags value has bits outside FLAGS_ALLOW */
};

enum ntasi_sart_gen {
    NTASI_SART_GEN_V0 = 0,
    NTASI_SART_GEN_V2 = 2,
    NTASI_SART_GEN_V3 = 3,
};

struct ntasi_sart_params {
    enum ntasi_sart_gen gen;
    uint32_t config_flags_mask; /* v0/v2 CONFIG.FLAGS field; 0 on v3 (raw byte, not packed) */
    uint32_t config_size_mask;  /* v0/v2 CONFIG.SIZE field; 0 on v3 (SIZE is its own register) */
    uint32_t size_max;          /* max legal value of (size >> 12) for this generation */
    uint8_t flags_allow;        /* mask of flag bits the hardware accepts */
};

/* Canonical parameter sets from m1n1 (APPLE_SARTn_* in sart.c). */
extern const struct ntasi_sart_params ntasi_sart_params_v0;
extern const struct ntasi_sart_params ntasi_sart_params_v2;
extern const struct ntasi_sart_params ntasi_sart_params_v3;

/* Lowest set bit of a contiguous mask = the FIELD_PREP/FIELD_GET shift.
 * Returns 0 for a zero mask (SARTv3's unused config_flags_mask/config_size_mask). */
uint32_t ntasi_sart_field_shift(uint32_t mask);

/* True iff `flags` has no bits outside this generation's FLAGS_ALLOW mask. */
bool ntasi_sart_flags_allowed(const struct ntasi_sart_params *params, uint8_t flags);

/*
 * Register offset accessors (bytes from the SART base), bounds-checked on
 * `index < NTASI_SART_MAX_ENTRIES`. ntasi_sart_reg_size() only succeeds for
 * SARTv3 -- v0/v2 have no dedicated SIZE register (size lives in CONFIG).
 */
int ntasi_sart_reg_config(const struct ntasi_sart_params *params, unsigned index,
                          uint32_t *offset);
int ntasi_sart_reg_paddr(const struct ntasi_sart_params *params, unsigned index,
                         uint32_t *offset);
int ntasi_sart_reg_size(const struct ntasi_sart_params *params, unsigned index,
                        uint32_t *offset);

/* One MMIO write: `value` belongs at byte offset `offset` from the SART base. */
struct ntasi_sart_reg_write {
    uint32_t offset;
    uint32_t value;
};

/*
 * The register writes needed to program one SART entry. v0/v2 use
 * `config` + `paddr` (num_writes == 2); SARTv3 also uses `size`
 * (num_writes == 3, CONFIG carries the raw flags byte).
 */
struct ntasi_sart_entry_regs {
    struct ntasi_sart_reg_write config;
    struct ntasi_sart_reg_write paddr;
    struct ntasi_sart_reg_write size; /* meaningful only when num_writes == 3 */
    unsigned num_writes;
};

/* Raw register values already read back from hardware for one entry (SIZE is
 * ignored unless the generation is SARTv3). */
struct ntasi_sart_entry_values {
    uint32_t config;
    uint32_t paddr;
    uint32_t size;
};

/*
 * Encode one allow-region entry for `index`. Validates, rather than silently
 * truncating: index < NTASI_SART_MAX_ENTRIES, paddr and size 4 KiB aligned,
 * (size >> 12) <= this generation's SIZE_MAX, (paddr >> 12) fits a 32-bit
 * register, and flags within FLAGS_ALLOW. On success fills *regs with the
 * register offset/value pairs to write (CONFIG+PADDR for v0/v2; also SIZE
 * for v3) and returns NTASI_SART_OK; on failure *regs is left untouched.
 */
int ntasi_sart_entry_encode(const struct ntasi_sart_params *params, unsigned index, uint8_t flags,
                            uint64_t paddr, uint64_t size, struct ntasi_sart_entry_regs *regs);

/* Recover (flags, paddr, size) from raw register values already read back
 * from hardware for one entry: paddr = raw_paddr << 12, size = raw_size << 12. */
int ntasi_sart_entry_decode(const struct ntasi_sart_params *params,
                            const struct ntasi_sart_entry_values *regs, uint8_t *flags,
                            uint64_t *paddr, uint64_t *size);

#endif

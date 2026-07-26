/* SPDX-License-Identifier: MIT */
#ifndef NTASI_APPLE_SART_RUNTIME_CORE_H
#define NTASI_APPLE_SART_RUNTIME_CORE_H

#include "AppleSartCore.h"

enum ntasi_sart_runtime_result {
    NTASI_SART_RUNTIME_OK = 0,
    NTASI_SART_RUNTIME_ERR_ARGUMENT = -20,
    NTASI_SART_RUNTIME_ERR_NO_SPACE = -21,
    NTASI_SART_RUNTIME_ERR_NOT_FOUND = -22,
};

struct ntasi_sart_runtime_ops {
    uint32_t (*read32)(void *opaque, uint32_t offset);
    void (*write32)(void *opaque, uint32_t offset, uint32_t value);
    void (*write_barrier)(void *opaque);
};

struct ntasi_sart_runtime {
    const struct ntasi_sart_params *params;
    struct ntasi_sart_runtime_ops ops;
    void *opaque;
    uint16_t protected_entries;
    uint16_t used_entries;
};

/*
 * Scans firmware-programmed entries and permanently protects each nonzero
 * one. Callers must serialize add/remove operations around this small core.
 */
int ntasi_sart_runtime_init(struct ntasi_sart_runtime *runtime,
                            const struct ntasi_sart_params *params,
                            const struct ntasi_sart_runtime_ops *ops,
                            void *opaque);

int ntasi_sart_runtime_add(struct ntasi_sart_runtime *runtime,
                           uint64_t paddr, uint64_t size,
                           unsigned int *entry);

int ntasi_sart_runtime_remove(struct ntasi_sart_runtime *runtime,
                              uint64_t paddr, uint64_t size);

/* Clears only entries owned by this runtime; firmware entries survive. */
void ntasi_sart_runtime_clear_owned(struct ntasi_sart_runtime *runtime);

#endif

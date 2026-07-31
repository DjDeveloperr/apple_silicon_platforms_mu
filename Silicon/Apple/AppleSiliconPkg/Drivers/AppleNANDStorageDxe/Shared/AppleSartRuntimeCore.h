/* SPDX-License-Identifier: MIT */
#ifndef NTASI_APPLE_SART_RUNTIME_CORE_H
#define NTASI_APPLE_SART_RUNTIME_CORE_H

#include "AppleSartCore.h"

enum ntasi_sart_runtime_result {
    NTASI_SART_RUNTIME_OK = 0,
    NTASI_SART_RUNTIME_ERR_ARGUMENT = -20,
    NTASI_SART_RUNTIME_ERR_NO_SPACE = -21,
    NTASI_SART_RUNTIME_ERR_NOT_FOUND = -22,
    /* An entry still read back armed after being cleared. */
    NTASI_SART_RUNTIME_ERR_NOT_CLEARED = -23,
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

/*
 * Closes the SART window completely: clears EVERY entry, including the ones
 * iBoot programmed before Mu ran, and VERIFIES each one by reading it back.
 *
 * WHY THIS EXISTS SEPARATELY FROM clear_owned().
 *
 * SART is an ALLOW list. An armed entry PERMITS the ANS coprocessor to DMA
 * into that physical range. iBoot's entries cover iBoot's own ANS buffers --
 * memory that Windows reclaims as conventional RAM the moment it takes over.
 * Leaving them armed across ExitBootServices is a standing grant over pages
 * the OS will hand to arbitrary drivers. clear_owned() cannot close them,
 * because it only knows about entries this runtime added.
 *
 * ONLY SAFE ONCE THE COPROCESSOR IS CONFIRMED HALTED. Revoking a grant that a
 * live IOP is DMAing through converts a benign handoff into a DMA fault of
 * unknown blast radius, which is strictly worse than the open window. The
 * caller owns that precondition; this function does not check it and cannot.
 *
 * The readback is the point. A write that the hardware did not take would
 * otherwise be indistinguishable from a closed window, and "we closed SART"
 * would become an assumption in the log rather than a measurement.
 *
 * @param still_armed  Optional. Receives the number of entries that still read
 *                     back with a nonzero flags byte after being cleared, i.e.
 *                     0 when the window is provably shut. Also set on the
 *                     argument-error path (to 0), so a caller never reads an
 *                     uninitialised count.
 *
 * Returns NTASI_SART_RUNTIME_OK when every entry read back clear.
 */
int ntasi_sart_runtime_close_all(struct ntasi_sart_runtime *runtime,
                                 unsigned int *still_armed);

/*
 * Read back one live SART entry, decoded. Pure observation: touches no state
 * and writes no register, so it is safe to call at any point including inside
 * an ExitBootServices callback.
 *
 * Exists so a driver can DUMP the filter's true hardware state rather than its
 * own bookkeeping. On 2026-07-30 an ANS-correlated USB3 bugcheck made "did Mu
 * leave a SART entry in a state that could reject DMA?" a question that had to
 * be answered from hardware, not from a used_entries bitmap that could itself
 * be wrong.
 *
 * `entry` must be < NTASI_SART_MAX_ENTRIES.
 */
int ntasi_sart_runtime_read(struct ntasi_sart_runtime *runtime,
                            unsigned int entry, uint8_t *flags,
                            uint64_t *paddr, uint64_t *size);

#endif

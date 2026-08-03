/* SPDX-License-Identifier: MIT */
#ifndef NTASI_APPLE_RTKIT_RUNTIME_CORE_H
#define NTASI_APPLE_RTKIT_RUNTIME_CORE_H

#include "AppleAscCore.h"
#include "AppleRtkitCore.h"

#include <stddef.h>

#define NTASI_RTKIT_SYSTEM_ENDPOINT_LIMIT 0x20u

/*
 * Shared-buffer granularity. m1n1's rtkit_alloc_buffer()/rtkit_map()
 * (src/rtkit.c) round every RTKit buffer up to 16 KiB and allocate it
 * 16 KiB-aligned (memalign(SZ_16K, sz)) before handing it to
 * sart_add_allowed_region(). SART itself only requires 4 KiB granularity,
 * but 16 KiB is the CPU page size on this silicon: a 4 KiB-granular grant
 * lets the coprocessor DMA into a CPU page the AP also owns, which is
 * exactly the kind of shared-page hazard that is unreproducible after the
 * fact. Match m1n1.
 */
#define NTASI_RTKIT_SHARED_ALIGN 16384u

enum ntasi_rtkit_power_state {
    NTASI_RTKIT_POWER_OFF = 0x00,
    NTASI_RTKIT_POWER_SLEEP = 0x01,
    NTASI_RTKIT_POWER_QUIESCED = 0x10,
    NTASI_RTKIT_POWER_ON = 0x20,
    NTASI_RTKIT_POWER_INIT = 0x220,
};

/*
 * RTKit ownership at entry determines which operations are legal. COLD
 * releases a reset/stopped core with a plain RUN write and waits for HELLO
 * without transmitting first. WAKE never writes CPU_CONTROL and sends INIT
 * to the already-running coprocessor. M1N1 preserves the historical combined
 * behaviour for existing callers.
 */
enum ntasi_rtkit_boot_mode {
    NTASI_RTKIT_BOOT_MODE_M1N1 = 0,
    NTASI_RTKIT_BOOT_MODE_COLD = 1,
    NTASI_RTKIT_BOOT_MODE_WAKE = 2,
};

enum ntasi_rtkit_runtime_result {
    NTASI_RTKIT_RUNTIME_OK = 0,
    NTASI_RTKIT_RUNTIME_NO_MESSAGE = 1,
    NTASI_RTKIT_RUNTIME_APP_MESSAGE = 2,
    /*
     * A well-formed system-endpoint message this codec does not model.
     * m1n1's rtkit_recv() prints "unknown management message"/"unknown
     * syslog message"/"message to unknown system endpoint" and CONTINUES --
     * it only fails when a handler it does own fails. Returning a negative
     * status for every unmodelled message is what turned a benign
     * shutdown-time message into "RTKit handoff failed" on J414s hardware
     * on 2026-07-30. Positive, so `status < 0` checks treat it as
     * non-fatal, and distinct from OK so callers can log it.
     */
    NTASI_RTKIT_RUNTIME_UNHANDLED = 3,
    NTASI_RTKIT_RUNTIME_ERR_ARGUMENT = -20,
    NTASI_RTKIT_RUNTIME_ERR_TRANSPORT = -21,
    NTASI_RTKIT_RUNTIME_ERR_TIMEOUT = -22,
    NTASI_RTKIT_RUNTIME_ERR_PROTOCOL = -23,
    NTASI_RTKIT_RUNTIME_ERR_VERSION = -24,
    NTASI_RTKIT_RUNTIME_ERR_BUFFER = -25,
    NTASI_RTKIT_RUNTIME_ERR_CRASHED = -26,
};

struct ntasi_rtkit_shared_buffer {
    void *cpu_address;
    uint64_t device_address;
    size_t size;
    /*
     * True when the coprocessor supplied the address itself (a non-zero
     * IOVA in its buffer request) instead of asking the AP to allocate one.
     * m1n1 calls these "pre-allocated" buffers: it adopts the address,
     * sends NO reply, and -- via rtkit_free_buffer()'s is_heap() guard --
     * never unmaps or frees them. This driver must do the same: a
     * pre-allocated buffer lives in the IOP's own carveout, so revoking its
     * SART grant or handing its pages back to the allocator would let the
     * coprocessor DMA into memory somebody else now owns.
     */
    bool iop_owned;
};

struct ntasi_rtkit_runtime_ops {
    int (*allocate_shared)(void *opaque, uint8_t endpoint, size_t size,
                           struct ntasi_rtkit_shared_buffer *buffer);
    void (*release_shared)(void *opaque, uint8_t endpoint,
                           struct ntasi_rtkit_shared_buffer *buffer);
    void (*crashed)(void *opaque,
                    const struct ntasi_rtkit_shared_buffer *crashlog);
};

struct ntasi_rtkit_runtime {
    struct ntasi_asc_transport *asc;
    struct ntasi_rtkit_runtime_ops ops;
    void *opaque;
    uint32_t poll_limit;
    enum ntasi_rtkit_power_state iop_power;
    enum ntasi_rtkit_power_state ap_power;
    uint32_t system_endpoints;
    struct ntasi_rtkit_shared_buffer crashlog;
    struct ntasi_rtkit_shared_buffer syslog;
    struct ntasi_rtkit_shared_buffer ioreport;
    struct ntasi_rtkit_shared_buffer oslog;
    bool booted;
    bool crashed;
    enum ntasi_rtkit_boot_mode boot_mode;
};

int ntasi_rtkit_runtime_init(struct ntasi_rtkit_runtime *runtime,
                             struct ntasi_asc_transport *asc,
                             const struct ntasi_rtkit_runtime_ops *ops,
                             void *opaque,
                             uint32_t poll_limit);

int ntasi_rtkit_runtime_boot(struct ntasi_rtkit_runtime *runtime);

/* Handles at most one inbound message. Application messages are returned. */
int ntasi_rtkit_runtime_service(struct ntasi_rtkit_runtime *runtime,
                                struct ntasi_asc_message *application_message);

int ntasi_rtkit_runtime_sleep(struct ntasi_rtkit_runtime *runtime);

/*
 * Fail-safe wrapper around ntasi_rtkit_runtime_sleep() for the handoff path.
 *
 * m1n1's rtkit_sleep() calls asc_cpu_stop() only on the success path, so an
 * IOP that dies mid-quiesce is left with its run bit set -- still able to
 * DMA into the AP's shared buffers. That is survivable in m1n1 (it follows
 * up with pmgr_reset), but not in firmware that is about to hand the machine
 * to an OS which will reuse memory. This always drives the run bit low and
 * reports whether the coprocessor is confirmed halted, so the caller can
 * decide whether revoking SART grants is safe.
 *
 * *stopped is always written. The return value is the sleep handshake's own
 * status (0 on a clean quiesce, negative otherwise) so the caller can log
 * honestly instead of reporting success it did not get.
 */
int ntasi_rtkit_runtime_handoff(struct ntasi_rtkit_runtime *runtime,
                                bool *stopped);

/*
 * Releases only the buffers this driver allocated. IOP-owned (pre-allocated)
 * buffers are skipped -- see ntasi_rtkit_shared_buffer::iop_owned.
 */
void ntasi_rtkit_runtime_release_buffers(struct ntasi_rtkit_runtime *runtime);

#endif

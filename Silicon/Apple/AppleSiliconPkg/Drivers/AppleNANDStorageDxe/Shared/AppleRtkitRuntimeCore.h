/* SPDX-License-Identifier: MIT */
#ifndef NTASI_APPLE_RTKIT_RUNTIME_CORE_H
#define NTASI_APPLE_RTKIT_RUNTIME_CORE_H

#include "AppleAscCore.h"
#include "AppleRtkitCore.h"

#include <stddef.h>

#define NTASI_RTKIT_SYSTEM_ENDPOINT_LIMIT 0x20u

enum ntasi_rtkit_power_state {
    NTASI_RTKIT_POWER_OFF = 0x00,
    NTASI_RTKIT_POWER_SLEEP = 0x01,
    NTASI_RTKIT_POWER_QUIESCED = 0x10,
    NTASI_RTKIT_POWER_ON = 0x20,
    NTASI_RTKIT_POWER_INIT = 0x220,
};

enum ntasi_rtkit_runtime_result {
    NTASI_RTKIT_RUNTIME_OK = 0,
    NTASI_RTKIT_RUNTIME_NO_MESSAGE = 1,
    NTASI_RTKIT_RUNTIME_APP_MESSAGE = 2,
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
void ntasi_rtkit_runtime_release_buffers(struct ntasi_rtkit_runtime *runtime);

#endif

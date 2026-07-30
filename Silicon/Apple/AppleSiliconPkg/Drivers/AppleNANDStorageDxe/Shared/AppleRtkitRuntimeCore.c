/* SPDX-License-Identifier: MIT */
#include "AppleRtkitRuntimeCore.h"

#include <limits.h>
#include <string.h>

#define MGMT_POWER_STATE_MASK UINT64_C(0xffff)
#define MGMT_IOP_POWER_STATE 6u
#define MGMT_IOP_POWER_STATE_ACK 7u
#define MGMT_START_ENDPOINT 5u
#define MGMT_START_ENDPOINT_FLAG (UINT64_C(1) << 1)
#define MGMT_START_ENDPOINT_SHIFT 32u
#define MGMT_AP_POWER_STATE 0x0bu

#define BUFFER_REQUEST 1u
#define BUFFER_REQUEST_SIZE_SHIFT 44u
#define BUFFER_REQUEST_SIZE_MASK (UINT64_C(0xff) << BUFFER_REQUEST_SIZE_SHIFT)
/*
 * CORRECTED 2026-07-30: this was a 44-bit mask. m1n1's
 * MSG_BUFFER_REQUEST_IOVA is GENMASK(41, 0) (src/rtkit.c) -- 42 bits, not
 * 44. Bits 42 and 43 are not part of the address, so reading them as one
 * made any message that happens to set them look like a request for a
 * specific pre-allocated buffer at a bogus address.
 */
#define BUFFER_REQUEST_IOVA_MASK ((UINT64_C(1) << 42) - 1u)

#define SYSLOG_INIT 8u
#define SYSLOG_LOG 5u
#define IOREPORT_ACK_A 8u
#define IOREPORT_ACK_B 0x0cu

#define OSLOG_ENDPOINT 8u
#define TRACEKIT_ENDPOINT 0x0au
#define OSLOG_TYPE_SHIFT 56u
#define OSLOG_TYPE_MASK (UINT64_C(0xff) << OSLOG_TYPE_SHIFT)
#define OSLOG_BUFFER_REQUEST 1u
#define OSLOG_SIZE_SHIFT 36u
#define OSLOG_SIZE_MASK (UINT64_C(0xfffff) << OSLOG_SIZE_SHIFT)
#define OSLOG_IOVA_MASK ((UINT64_C(1) << 36) - 1u)

static uint64_t with_type(uint8_t type)
{
    return ntasi_rtkit_with_mgmt_type(0, type);
}

static int send_message(struct ntasi_rtkit_runtime *runtime,
                        uint8_t endpoint, uint64_t payload)
{
    struct ntasi_asc_message message = {
        .payload = payload,
        .endpoint = endpoint,
    };

    return ntasi_asc_send(runtime->asc, &message) == NTASI_ASC_OK
               ? NTASI_RTKIT_RUNTIME_OK
               : NTASI_RTKIT_RUNTIME_ERR_TRANSPORT;
}

static int receive_bounded(struct ntasi_rtkit_runtime *runtime,
                           struct ntasi_asc_message *message)
{
    uint32_t attempt;

    for (attempt = 0; attempt < runtime->poll_limit; ++attempt) {
        int status = ntasi_asc_receive(runtime->asc, message);

        if (status == NTASI_ASC_OK)
            return NTASI_RTKIT_RUNTIME_OK;
        if (status != NTASI_ASC_NO_MESSAGE)
            return NTASI_RTKIT_RUNTIME_ERR_TRANSPORT;
    }
    return NTASI_RTKIT_RUNTIME_ERR_TIMEOUT;
}

static struct ntasi_rtkit_shared_buffer *buffer_for_endpoint(
    struct ntasi_rtkit_runtime *runtime, uint8_t endpoint)
{
    switch (endpoint) {
    case NTASI_RTKIT_EP_CRASHLOG:
        return &runtime->crashlog;
    case NTASI_RTKIT_EP_SYSLOG:
        return &runtime->syslog;
    case NTASI_RTKIT_EP_IOREPORT:
        return &runtime->ioreport;
    case OSLOG_ENDPOINT:
        return &runtime->oslog;
    default:
        return NULL;
    }
}

static int handle_buffer_request(struct ntasi_rtkit_runtime *runtime,
                                 const struct ntasi_asc_message *message)
{
    struct ntasi_rtkit_shared_buffer *buffer;
    uint64_t requested_address;
    uint64_t pages;
    uint64_t reply;
    size_t size;
    int status;

    buffer = buffer_for_endpoint(runtime, (uint8_t)message->endpoint);
    if (buffer == NULL)
        return NTASI_RTKIT_RUNTIME_ERR_BUFFER;
    if (message->endpoint == OSLOG_ENDPOINT) {
        uint64_t raw_size = (message->payload & OSLOG_SIZE_MASK) >>
                            OSLOG_SIZE_SHIFT;

        /* OSLOG's IOVA field is page-shifted; the mgmt-style one is not.
         * Both match m1n1 (OSLOG_IOVA GENMASK(35,0) << 12 vs
         * MSG_BUFFER_REQUEST_IOVA GENMASK(41,0)). */
        requested_address = (message->payload & OSLOG_IOVA_MASK) << 12;
        if (raw_size == 0 || raw_size > SIZE_MAX)
            return NTASI_RTKIT_RUNTIME_ERR_BUFFER;
        size = (size_t)raw_size;
        pages = (raw_size + ((UINT64_C(1) << 12) - 1u)) >> 12;
    } else {
        pages = (message->payload & BUFFER_REQUEST_SIZE_MASK) >>
                BUFFER_REQUEST_SIZE_SHIFT;
        requested_address = message->payload & BUFFER_REQUEST_IOVA_MASK;
        if (pages == 0 || pages > SIZE_MAX >> 12)
            return NTASI_RTKIT_RUNTIME_ERR_BUFFER;
        size = (size_t)pages << 12;
    }

    /*
     * A non-zero address means the coprocessor has already placed this
     * buffer itself and is telling the AP where it is -- it is NOT asking
     * for an allocation. m1n1's rtkit_handle_buffer_request() adopts the
     * address and returns before the reply block is even reached (all three
     * of its pre-allocated branches `return true` early), so no reply is
     * sent. Refusing this case is what produced
     * "AppleANS: RTKit handoff failed: -25" on J414s hardware on
     * 2026-07-30: the boot itself completed, then a pre-allocated grant
     * arrived while quiescing at ExitBootServices and this function turned
     * it into a hard error.
     *
     * Nothing is mapped or SART-granted here: the address is already
     * reachable by the IOP by construction (it chose it), exactly as in
     * m1n1's SRAM/phys-window paths. Marking it iop_owned keeps the teardown
     * path from ever unmapping or freeing memory this driver does not own.
     */
    if (requested_address != 0) {
        if (message->endpoint == NTASI_RTKIT_EP_CRASHLOG &&
            buffer->cpu_address != NULL && !buffer->iop_owned) {
            runtime->crashed = true;
            if (runtime->ops.crashed != NULL)
                runtime->ops.crashed(runtime->opaque, buffer);
            return NTASI_RTKIT_RUNTIME_ERR_CRASHED;
        }
        buffer->cpu_address = NULL;
        buffer->device_address = requested_address;
        buffer->size = size;
        buffer->iop_owned = true;
        return NTASI_RTKIT_RUNTIME_OK;
    }

    if (runtime->ops.allocate_shared == NULL)
        return NTASI_RTKIT_RUNTIME_ERR_BUFFER;

    if (buffer->iop_owned) {
        /*
         * The IOP previously handed us a fixed address for this endpoint and
         * is now asking us to allocate one. Do not silently drop the old
         * grant: that would leave the coprocessor writing to an address this
         * driver has stopped tracking.
         */
        return NTASI_RTKIT_RUNTIME_ERR_PROTOCOL;
    }

    if (buffer->cpu_address == NULL) {
        status = runtime->ops.allocate_shared(runtime->opaque,
                                               (uint8_t)message->endpoint,
                                               size, buffer);
        if (status != 0 || buffer->cpu_address == NULL ||
            buffer->size < size || buffer->device_address == 0 ||
            (buffer->device_address & ~BUFFER_REQUEST_IOVA_MASK) != 0)
            return NTASI_RTKIT_RUNTIME_ERR_BUFFER;
    } else if (message->endpoint == NTASI_RTKIT_EP_CRASHLOG) {
        /* m1n1: a second crashlog buffer request is the crash indication,
         * not a real request. */
        runtime->crashed = true;
        if (runtime->ops.crashed != NULL)
            runtime->ops.crashed(runtime->opaque, buffer);
        return NTASI_RTKIT_RUNTIME_ERR_CRASHED;
    } else if (buffer->size < size) {
        /*
         * A repeat request for a LARGER buffer than the one already granted.
         * m1n1 would allocate a second buffer and leak the first; refuse
         * instead of growing a grant the IOP may still be using, and let the
         * caller report it. A repeat request for the same-or-smaller size
         * falls through and is re-acknowledged with the existing grant,
         * which is idempotent and is the common case during quiesce.
         */
        return NTASI_RTKIT_RUNTIME_ERR_BUFFER;
    }

    if (message->endpoint == OSLOG_ENDPOINT) {
        if ((buffer->device_address & ((UINT64_C(1) << 12) - 1u)) != 0 ||
            (buffer->device_address >> 12) > OSLOG_IOVA_MASK)
            return NTASI_RTKIT_RUNTIME_ERR_BUFFER;
        reply = ((uint64_t)OSLOG_BUFFER_REQUEST << OSLOG_TYPE_SHIFT) |
                ((uint64_t)size << OSLOG_SIZE_SHIFT) |
                (buffer->device_address >> 12);
    } else {
        /* SIZE echoes the requested 4 KiB page count, not the rounded
         * allocation -- m1n1 src/rtkit.c reply construction. */
        reply = with_type(BUFFER_REQUEST) |
                (pages << BUFFER_REQUEST_SIZE_SHIFT) |
                buffer->device_address;
    }
    return send_message(runtime, (uint8_t)message->endpoint, reply);
}

int ntasi_rtkit_runtime_service(struct ntasi_rtkit_runtime *runtime,
                                struct ntasi_asc_message *application_message)
{
    struct ntasi_asc_message message;
    uint8_t type;
    int status;

    if (runtime == NULL || runtime->asc == NULL)
        return NTASI_RTKIT_RUNTIME_ERR_ARGUMENT;
    if (runtime->crashed)
        return NTASI_RTKIT_RUNTIME_ERR_CRASHED;

    status = ntasi_asc_receive(runtime->asc, &message);
    if (status == NTASI_ASC_NO_MESSAGE)
        return NTASI_RTKIT_RUNTIME_NO_MESSAGE;
    if (status != NTASI_ASC_OK)
        return NTASI_RTKIT_RUNTIME_ERR_TRANSPORT;
    if (!ntasi_rtkit_ep_valid(message.endpoint))
        return NTASI_RTKIT_RUNTIME_ERR_PROTOCOL;
    if (message.endpoint >= NTASI_RTKIT_SYSTEM_ENDPOINT_LIMIT) {
        if (application_message != NULL)
            *application_message = message;
        return NTASI_RTKIT_RUNTIME_APP_MESSAGE;
    }

    /*
     * Every arm below that used to return NTASI_RTKIT_RUNTIME_ERR_PROTOCOL
     * for an unrecognised message type now returns
     * NTASI_RTKIT_RUNTIME_UNHANDLED instead. m1n1's rtkit_recv()
     * (src/rtkit.c) prints "unknown management message"/"unknown syslog
     * message"/"unknown ioreport message"/"unknown oslog message"/"message
     * to unknown system endpoint" and keeps going; it fails the receive only
     * when a handler it actually owns fails. Copying that tolerance is the
     * difference between a coprocessor that finishes quiescing and one whose
     * handoff is aborted by a single message this codec never modelled.
     */
    type = ntasi_rtkit_mgmt_type(message.payload);
    switch (message.endpoint) {
    case NTASI_RTKIT_EP_MGMT:
        if (type == MGMT_IOP_POWER_STATE_ACK) {
            runtime->iop_power = (enum ntasi_rtkit_power_state)
                (message.payload & MGMT_POWER_STATE_MASK);
            return NTASI_RTKIT_RUNTIME_OK;
        }
        if (type == MGMT_AP_POWER_STATE) {
            runtime->ap_power = (enum ntasi_rtkit_power_state)
                (message.payload & MGMT_POWER_STATE_MASK);
            return NTASI_RTKIT_RUNTIME_OK;
        }
        return NTASI_RTKIT_RUNTIME_UNHANDLED;
    case NTASI_RTKIT_EP_CRASHLOG:
    case NTASI_RTKIT_EP_SYSLOG:
    case NTASI_RTKIT_EP_IOREPORT:
        if (type == BUFFER_REQUEST)
            return handle_buffer_request(runtime, &message);
        if (message.endpoint == NTASI_RTKIT_EP_SYSLOG && type == SYSLOG_INIT)
            return NTASI_RTKIT_RUNTIME_OK;
        /* m1n1 always echoes MSG_SYSLOG_LOG back; an unacked syslog entry
         * stalls the IOP. Same for the two undocumented-but-must-be-ACKed
         * ioreport types. */
        if (message.endpoint == NTASI_RTKIT_EP_SYSLOG && type == SYSLOG_LOG)
            return send_message(runtime, (uint8_t)message.endpoint,
                                message.payload);
        if (message.endpoint == NTASI_RTKIT_EP_IOREPORT &&
            (type == IOREPORT_ACK_A || type == IOREPORT_ACK_B))
            return send_message(runtime, (uint8_t)message.endpoint,
                                message.payload);
        return NTASI_RTKIT_RUNTIME_UNHANDLED;
    case NTASI_RTKIT_EP_DEBUG:
        return NTASI_RTKIT_RUNTIME_UNHANDLED;
    case NTASI_RTKIT_EP_OSLOG:
        if (((message.payload & OSLOG_TYPE_MASK) >> OSLOG_TYPE_SHIFT) ==
            OSLOG_BUFFER_REQUEST)
            return handle_buffer_request(runtime, &message);
        return NTASI_RTKIT_RUNTIME_UNHANDLED;
    case TRACEKIT_ENDPOINT:
        return NTASI_RTKIT_RUNTIME_UNHANDLED;
    default:
        return NTASI_RTKIT_RUNTIME_UNHANDLED;
    }
}

int ntasi_rtkit_runtime_init(struct ntasi_rtkit_runtime *runtime,
                             struct ntasi_asc_transport *asc,
                             const struct ntasi_rtkit_runtime_ops *ops,
                             void *opaque,
                             uint32_t poll_limit)
{
    if (runtime == NULL || asc == NULL || ops == NULL || poll_limit == 0)
        return NTASI_RTKIT_RUNTIME_ERR_ARGUMENT;
    *runtime = (struct ntasi_rtkit_runtime){
        .asc = asc,
        .ops = *ops,
        .opaque = opaque,
        .poll_limit = poll_limit,
        .iop_power = NTASI_RTKIT_POWER_OFF,
        .ap_power = NTASI_RTKIT_POWER_OFF,
    };
    return NTASI_RTKIT_RUNTIME_OK;
}

static int start_endpoint(struct ntasi_rtkit_runtime *runtime,
                          uint8_t endpoint)
{
    uint64_t payload = with_type(MGMT_START_ENDPOINT) |
                       MGMT_START_ENDPOINT_FLAG |
                       (uint64_t)endpoint << MGMT_START_ENDPOINT_SHIFT;

    return send_message(runtime, NTASI_RTKIT_EP_MGMT, payload);
}

/*
 * Both power waits pump the system-endpoint dispatcher, exactly like m1n1's
 * rtkit_wait_for_power()/rtkit_switch_power_state() loops. Anything
 * non-negative is progress and the loop continues:
 *   - NTASI_RTKIT_RUNTIME_APP_MESSAGE: m1n1 prints "unexpected message to
 *     non-system endpoint ... during shutdown" and `continue`s. ANS has no
 *     application endpoints in this driver, so dropping one is correct and
 *     must not abort a power transition.
 *   - NTASI_RTKIT_RUNTIME_UNHANDLED: an unmodelled system message; m1n1
 *     likewise logs and continues.
 * Only a negative status (transport error, protocol violation, crash) aborts.
 */
static int wait_for_power(struct ntasi_rtkit_runtime *runtime,
                          const enum ntasi_rtkit_power_state *observed,
                          enum ntasi_rtkit_power_state target)
{
    uint32_t attempt;

    for (attempt = 0; attempt < runtime->poll_limit; ++attempt) {
        int status;

        if (*observed == target)
            return NTASI_RTKIT_RUNTIME_OK;
        status = ntasi_rtkit_runtime_service(runtime, NULL);
        if (status < 0)
            return status;
    }
    return *observed == target ? NTASI_RTKIT_RUNTIME_OK
                               : NTASI_RTKIT_RUNTIME_ERR_TIMEOUT;
}

static int wait_for_iop_power(struct ntasi_rtkit_runtime *runtime,
                              enum ntasi_rtkit_power_state target)
{
    return wait_for_power(runtime, &runtime->iop_power, target);
}

static int wait_for_ap_power(struct ntasi_rtkit_runtime *runtime,
                             enum ntasi_rtkit_power_state target)
{
    return wait_for_power(runtime, &runtime->ap_power, target);
}

int ntasi_rtkit_runtime_boot(struct ntasi_rtkit_runtime *runtime)
{
    struct ntasi_asc_message message;
    uint16_t min_version;
    uint16_t max_version;
    uint16_t wanted_version;
    uint32_t epmap_count;
    bool done = false;
    int status;

    if (runtime == NULL || runtime->asc == NULL)
        return NTASI_RTKIT_RUNTIME_ERR_ARGUMENT;
    runtime->booted = false;
    runtime->crashed = false;
    runtime->iop_power = NTASI_RTKIT_POWER_OFF;
    runtime->ap_power = NTASI_RTKIT_POWER_OFF;
    runtime->system_endpoints = 0;

    ntasi_asc_cpu_start(runtime->asc);
    status = send_message(runtime, NTASI_RTKIT_EP_MGMT,
                          with_type(MGMT_IOP_POWER_STATE) |
                              NTASI_RTKIT_POWER_INIT);
    if (status != 0)
        return status;
    status = receive_bounded(runtime, &message);
    if (status != 0)
        return status;
    if (message.endpoint != NTASI_RTKIT_EP_MGMT ||
        ntasi_rtkit_mgmt_type(message.payload) != NTASI_RTKIT_MGMT_HELLO)
        return NTASI_RTKIT_RUNTIME_ERR_PROTOCOL;
    ntasi_rtkit_hello_parse(message.payload, &min_version, &max_version);
    if (!ntasi_rtkit_hello_negotiate(min_version, max_version,
                                     &wanted_version))
        return NTASI_RTKIT_RUNTIME_ERR_VERSION;
    status = send_message(runtime, NTASI_RTKIT_EP_MGMT,
                          ntasi_rtkit_hello_ack(wanted_version));
    if (status != 0)
        return status;

    for (epmap_count = 0; epmap_count < runtime->poll_limit && !done;
         ++epmap_count) {
        uint8_t base;
        uint32_t bitmap;
        unsigned int bit;
        uint64_t reply;

        status = receive_bounded(runtime, &message);
        if (status != 0)
            return status;
        if (message.endpoint != NTASI_RTKIT_EP_MGMT ||
            ntasi_rtkit_mgmt_type(message.payload) != NTASI_RTKIT_MGMT_EPMAP)
            return NTASI_RTKIT_RUNTIME_ERR_PROTOCOL;
        ntasi_rtkit_epmap_parse(message.payload, &base, &bitmap, &done);
        for (bit = 0; bit < 32; ++bit) {
            uint8_t endpoint;

            if ((bitmap & (UINT32_C(1) << bit)) == 0)
                continue;
            endpoint = ntasi_rtkit_epmap_endpoint(base, bit);
            if (endpoint < NTASI_RTKIT_SYSTEM_ENDPOINT_LIMIT)
                runtime->system_endpoints |= UINT32_C(1) << endpoint;
        }
        reply = with_type(NTASI_RTKIT_MGMT_EPMAP) |
                (uint64_t)base << NTASI_RTKIT_EPMAP_BASE_SHIFT;
        reply |= done ? NTASI_RTKIT_EPMAP_DONE : UINT64_C(1);
        status = send_message(runtime, NTASI_RTKIT_EP_MGMT, reply);
        if (status != 0)
            return status;
    }
    if (!done)
        return NTASI_RTKIT_RUNTIME_ERR_TIMEOUT;

    {
        static const uint8_t endpoints[] = {
            NTASI_RTKIT_EP_DEBUG,
            NTASI_RTKIT_EP_CRASHLOG,
            NTASI_RTKIT_EP_SYSLOG,
            NTASI_RTKIT_EP_IOREPORT,
            NTASI_RTKIT_EP_OSLOG,
            TRACEKIT_ENDPOINT,
        };
        size_t index;

        for (index = 0; index < sizeof(endpoints); ++index) {
            uint8_t endpoint = endpoints[index];

            if ((runtime->system_endpoints &
                 (UINT32_C(1) << endpoint)) == 0)
                continue;
            status = start_endpoint(runtime, endpoint);
            if (status != 0)
                return status;
        }
    }

    status = wait_for_iop_power(runtime, NTASI_RTKIT_POWER_ON);
    if (status != 0)
        return status;
    status = send_message(runtime, NTASI_RTKIT_EP_MGMT,
                          with_type(MGMT_AP_POWER_STATE) |
                              NTASI_RTKIT_POWER_ON);
    if (status != 0)
        return status;
    runtime->booted = true;
    return NTASI_RTKIT_RUNTIME_OK;
}

int ntasi_rtkit_runtime_sleep(struct ntasi_rtkit_runtime *runtime)
{
    int status;

    if (runtime == NULL || runtime->asc == NULL || !runtime->booted)
        return NTASI_RTKIT_RUNTIME_ERR_ARGUMENT;
    status = send_message(runtime, NTASI_RTKIT_EP_MGMT,
                          with_type(MGMT_AP_POWER_STATE) |
                              NTASI_RTKIT_POWER_QUIESCED);
    if (status != 0)
        return status;
    status = wait_for_ap_power(runtime, NTASI_RTKIT_POWER_QUIESCED);
    if (status != 0)
        return status;
    status = send_message(runtime, NTASI_RTKIT_EP_MGMT,
                          with_type(MGMT_IOP_POWER_STATE) |
                              NTASI_RTKIT_POWER_SLEEP);
    if (status != 0)
        return status;
    status = wait_for_iop_power(runtime, NTASI_RTKIT_POWER_SLEEP);
    if (status != 0)
        return status;
    ntasi_asc_cpu_stop(runtime->asc);
    runtime->booted = false;
    return NTASI_RTKIT_RUNTIME_OK;
}

int ntasi_rtkit_runtime_handoff(struct ntasi_rtkit_runtime *runtime,
                               bool *stopped)
{
    int status;

    if (stopped != NULL)
        *stopped = false;
    if (runtime == NULL || runtime->asc == NULL)
        return NTASI_RTKIT_RUNTIME_ERR_ARGUMENT;

    status = ntasi_rtkit_runtime_sleep(runtime);

    /*
     * Drive the run bit low unconditionally, including when the quiesce
     * handshake failed or the IOP never booted. ntasi_rtkit_runtime_sleep()
     * only does this on its success path (matching m1n1's rtkit_sleep());
     * here the machine is about to belong to an operating system, so a
     * coprocessor left running is a coprocessor that can still DMA into
     * memory that OS is about to allocate.
     */
    ntasi_asc_cpu_stop(runtime->asc);
    runtime->booted = false;
    if (stopped != NULL)
        *stopped = !ntasi_asc_cpu_running(runtime->asc);
    return status;
}

void ntasi_rtkit_runtime_release_buffers(struct ntasi_rtkit_runtime *runtime)
{
    struct ntasi_rtkit_shared_buffer *buffers[4];
    const uint8_t endpoints[] = {
        NTASI_RTKIT_EP_CRASHLOG,
        NTASI_RTKIT_EP_SYSLOG,
        NTASI_RTKIT_EP_IOREPORT,
        NTASI_RTKIT_EP_OSLOG,
    };
    size_t index;

    if (runtime == NULL || runtime->ops.release_shared == NULL)
        return;
    buffers[0] = &runtime->crashlog;
    buffers[1] = &runtime->syslog;
    buffers[2] = &runtime->ioreport;
    buffers[3] = &runtime->oslog;
    for (index = 0; index < sizeof(buffers) / sizeof(buffers[0]); ++index) {
        /*
         * iop_owned buffers live in the coprocessor's own carveout: this
         * driver never allocated them, never SART-granted them, and must
         * never unmap or free them. Mirrors m1n1's rtkit_free_buffer()
         * is_heap() guard, which exempts pool- and IOP-provided buffers from
         * both rtkit_unmap() and free().
         */
        if (buffers[index]->iop_owned) {
            *buffers[index] = (struct ntasi_rtkit_shared_buffer){0};
            continue;
        }
        if (buffers[index]->cpu_address == NULL)
            continue;
        runtime->ops.release_shared(runtime->opaque, endpoints[index],
                                    buffers[index]);
        *buffers[index] = (struct ntasi_rtkit_shared_buffer){0};
    }
}

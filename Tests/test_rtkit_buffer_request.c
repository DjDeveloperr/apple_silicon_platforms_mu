/*
 * Host-side, hardware-free regression test for the Apple RTKit runtime
 * message codec in
 * Silicon/Apple/AppleSiliconPkg/Drivers/AppleNANDStorageDxe/Shared/.
 *
 * WHAT THIS PINS. On 2026-07-30 a J414s/T6020 hardware boot brought ANS all
 * the way up ("bring-up completed all stages; namespace 1 ready: 122138133
 * blocks x 4096 bytes") and then logged, from the ExitBootServices callback,
 * immediately before Windows started:
 *
 *     AppleANS: RTKit handoff failed: -25
 *
 * -25 is NTASI_RTKIT_RUNTIME_ERR_BUFFER, reachable only from
 * handle_buffer_request(). Two independent defects in that function could
 * produce it from a message m1n1 handles without complaint:
 *
 *   1. A buffer request carrying a NON-ZERO IOVA was rejected outright. In
 *      m1n1's rtkit_handle_buffer_request() (src/rtkit.c) a non-zero address
 *      means the coprocessor has already placed the buffer and is telling the
 *      AP where it is; m1n1 adopts the address and sends NO reply (all three
 *      of its pre-allocated branches return before the reply block).
 *
 *   2. The IOVA field mask was 44 bits wide. m1n1's MSG_BUFFER_REQUEST_IOVA
 *      is GENMASK(41, 0) -- 42 bits. Bits 42 and 43 are not address bits, so
 *      any message setting one made a perfectly ordinary
 *      "please allocate this for me" request look like a request for a
 *      specific pre-allocated buffer at a bogus address.
 *
 * Separately, every unmodelled system-endpoint message used to return
 * NTASI_RTKIT_RUNTIME_ERR_PROTOCOL (-23) and abort the receive, where m1n1
 * logs "unknown ... message" and continues. Those cases are pinned here too.
 *
 * This file compiles standalone with a plain host C compiler -- no EDK2, no
 * cross toolchain, no hardware, no proxy:
 *
 *   cc -std=c99 -Wall -Wextra -o /tmp/t Tests/test_rtkit_buffer_request.c \
 *      <Shared>/AppleAscCore.c <Shared>/AppleRtkitCore.c \
 *      <Shared>/AppleRtkitRuntimeCore.c && /tmp/t
 *
 * or via Tests/test_rtkit_buffer_request.py, which does exactly that.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../Silicon/Apple/AppleSiliconPkg/Drivers/AppleNANDStorageDxe/Shared/AppleRtkitRuntimeCore.h"

static int gFailures = 0;

#define CHECK(description, condition)                                          \
  do {                                                                         \
    if (condition) {                                                           \
      printf("PASS: %s\n", (description));                                     \
    } else {                                                                   \
      printf("FAIL: %s\n", (description));                                     \
      gFailures++;                                                             \
    }                                                                          \
  } while (0)

/* ------------------------------------------------------------------ */
/* Fake ASC mailbox                                                    */
/* ------------------------------------------------------------------ */

#define FAKE_MAX_MESSAGES 16u

struct fake_message {
    uint64_t payload;
    uint32_t endpoint;
};

struct fake_asc {
    struct fake_message inbound[FAKE_MAX_MESSAGES];
    unsigned inbound_count;
    unsigned inbound_head;

    struct fake_message sent[FAKE_MAX_MESSAGES];
    unsigned sent_count;
    uint64_t pending_payload;

    uint32_t cpu_control;

    /* Bump allocator standing in for AllocateAlignedReservedPages(). */
    uint64_t next_address;
    unsigned allocate_calls;
    unsigned release_calls;
    int allocate_result;
};

static uint32_t fake_cpu_read32(void *opaque, uint32_t offset)
{
    struct fake_asc *asc = opaque;

    return offset == NTASI_ASC_CPU_CONTROL ? asc->cpu_control : 0;
}

static void fake_cpu_write32(void *opaque, uint32_t offset, uint32_t value)
{
    struct fake_asc *asc = opaque;

    if (offset == NTASI_ASC_CPU_CONTROL)
        asc->cpu_control = value;
}

static uint32_t fake_mailbox_read32(void *opaque, uint32_t offset)
{
    struct fake_asc *asc = opaque;

    if (offset == NTASI_ASC_MBOX_I2A_CONTROL) {
        return asc->inbound_head < asc->inbound_count
                   ? 0u
                   : NTASI_ASC_MBOX_CONTROL_EMPTY;
    }
    /* A2I is never full in this harness. */
    return 0u;
}

static uint64_t fake_mailbox_read64(void *opaque, uint32_t offset)
{
    struct fake_asc *asc = opaque;

    if (asc->inbound_head >= asc->inbound_count)
        return 0u;

    if (offset == NTASI_ASC_MBOX_I2A_RECV0)
        return asc->inbound[asc->inbound_head].payload;

    if (offset == NTASI_ASC_MBOX_I2A_RECV1) {
        /* ntasi_asc_receive() reads RECV0 then RECV1; consume on RECV1. */
        uint32_t endpoint = asc->inbound[asc->inbound_head].endpoint;

        asc->inbound_head++;
        return endpoint;
    }

    return 0u;
}

static void fake_mailbox_write64(void *opaque, uint32_t offset, uint64_t value)
{
    struct fake_asc *asc = opaque;

    if (offset == NTASI_ASC_MBOX_A2I_SEND0) {
        asc->pending_payload = value;
        return;
    }

    if (offset == NTASI_ASC_MBOX_A2I_SEND1) {
        if (asc->sent_count < FAKE_MAX_MESSAGES) {
            asc->sent[asc->sent_count].payload = asc->pending_payload;
            asc->sent[asc->sent_count].endpoint = (uint32_t)value;
            asc->sent_count++;
        }
    }
}

static void fake_queue(struct fake_asc *asc, uint32_t endpoint,
                       uint64_t payload)
{
    if (asc->inbound_count >= FAKE_MAX_MESSAGES)
        return;
    asc->inbound[asc->inbound_count].endpoint = endpoint;
    asc->inbound[asc->inbound_count].payload = payload;
    asc->inbound_count++;
}

static int fake_allocate_shared(void *opaque, uint8_t endpoint, size_t size,
                                struct ntasi_rtkit_shared_buffer *buffer)
{
    struct fake_asc *asc = opaque;
    size_t mapped;

    (void)endpoint;
    asc->allocate_calls++;
    if (asc->allocate_result != 0)
        return asc->allocate_result;

    /* Mirror the driver: round to NTASI_RTKIT_SHARED_ALIGN and report the
     * MAPPED size, not the requested size. */
    mapped = (size + (NTASI_RTKIT_SHARED_ALIGN - 1u)) &
             ~(size_t)(NTASI_RTKIT_SHARED_ALIGN - 1u);

    buffer->cpu_address = (void *)(uintptr_t)asc->next_address;
    buffer->device_address = asc->next_address;
    buffer->size = mapped;
    buffer->iop_owned = false;
    asc->next_address += mapped;
    return 0;
}

static void fake_release_shared(void *opaque, uint8_t endpoint,
                                struct ntasi_rtkit_shared_buffer *buffer)
{
    struct fake_asc *asc = opaque;

    (void)endpoint;
    (void)buffer;
    asc->release_calls++;
}

static void fake_crashed(void *opaque,
                         const struct ntasi_rtkit_shared_buffer *crashlog)
{
    (void)opaque;
    (void)crashlog;
}

static const struct ntasi_asc_ops kAscOps = {
    .cpu_read32 = fake_cpu_read32,
    .cpu_write32 = fake_cpu_write32,
    .mailbox_read32 = fake_mailbox_read32,
    .mailbox_read64 = fake_mailbox_read64,
    .mailbox_write64 = fake_mailbox_write64,
};

static const struct ntasi_rtkit_runtime_ops kRtkitOps = {
    .allocate_shared = fake_allocate_shared,
    .release_shared = fake_release_shared,
    .crashed = fake_crashed,
};

struct harness {
    struct fake_asc asc;
    struct ntasi_asc_transport transport;
    struct ntasi_rtkit_runtime runtime;
};

static void harness_init(struct harness *h)
{
    memset(h, 0, sizeof(*h));
    h->asc.next_address = 0x103d000000ULL;
    ntasi_asc_init(&h->transport, &kAscOps, &h->asc, 16u);
    ntasi_rtkit_runtime_init(&h->runtime, &h->transport, &kRtkitOps, &h->asc,
                             16u);
}

/* Field builders, mirroring m1n1's FIELD_PREP usage exactly. */
#define MGMT_TYPE_SHIFT 52u
#define BUFREQ_SIZE_SHIFT 44u

static uint64_t buffer_request(uint64_t pages, uint64_t iova)
{
    return ((uint64_t)1u << MGMT_TYPE_SHIFT) | (pages << BUFREQ_SIZE_SHIFT) |
           iova;
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_ApAllocatedRequest_Replies(void)
{
    struct harness h;
    int status;

    harness_init(&h);
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG, buffer_request(4, 0));

    status = ntasi_rtkit_runtime_service(&h.runtime, NULL);

    CHECK("AP-allocated syslog buffer request succeeds",
          status == NTASI_RTKIT_RUNTIME_OK);
    CHECK("AP-allocated syslog buffer request allocates exactly once",
          h.asc.allocate_calls == 1);
    CHECK("AP-allocated syslog buffer request sends exactly one reply",
          h.asc.sent_count == 1);
    CHECK("the reply goes back to the syslog endpoint",
          h.asc.sent_count == 1 &&
              h.asc.sent[0].endpoint == NTASI_RTKIT_EP_SYSLOG);
    CHECK("the reply echoes the requested 4 KiB page count, not the rounded size",
          h.asc.sent_count == 1 &&
              ((h.asc.sent[0].payload >> BUFREQ_SIZE_SHIFT) & 0xffu) == 4u);
    CHECK("the reply carries the granted device address",
          h.asc.sent_count == 1 &&
              (h.asc.sent[0].payload & ((1ULL << 42) - 1u)) ==
                  h.runtime.syslog.device_address);
    CHECK("the granted buffer is not marked IOP-owned",
          !h.runtime.syslog.iop_owned &&
              h.runtime.syslog.cpu_address != NULL);
}

static void test_PreallocatedRequest_2026_07_30_regression(void)
{
    struct harness h;
    const uint64_t kIopAddress = 0x103fef0000ULL;
    int status;

    harness_init(&h);
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG, buffer_request(4, kIopAddress));

    status = ntasi_rtkit_runtime_service(&h.runtime, NULL);

    CHECK("2026-07-30 regression: a pre-allocated (non-zero IOVA) buffer request must NOT return -25",
          status != NTASI_RTKIT_RUNTIME_ERR_BUFFER);
    CHECK("2026-07-30 regression: a pre-allocated buffer request succeeds",
          status == NTASI_RTKIT_RUNTIME_OK);
    CHECK("a pre-allocated buffer request allocates nothing",
          h.asc.allocate_calls == 0);
    CHECK("a pre-allocated buffer request sends NO reply (matches m1n1)",
          h.asc.sent_count == 0);
    CHECK("the IOP-supplied address is adopted verbatim",
          h.runtime.syslog.device_address == kIopAddress);
    CHECK("the adopted buffer is flagged IOP-owned",
          h.runtime.syslog.iop_owned);
    CHECK("the adopted buffer has no CPU allocation this driver could free",
          h.runtime.syslog.cpu_address == NULL);
    CHECK("the adopted buffer records the requested size",
          h.runtime.syslog.size == (size_t)4u << 12);
}

static void test_IovaMaskIs42Bits(void)
{
    struct harness h;
    int status;

    /* Bit 42 is NOT an address bit (m1n1: MSG_BUFFER_REQUEST_IOVA is
     * GENMASK(41,0)). With the old 44-bit mask this payload decoded as a
     * pre-allocated request at 0x40000000000 and was rejected with -25. */
    harness_init(&h);
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG,
               buffer_request(4, 0) | (1ULL << 42));
    status = ntasi_rtkit_runtime_service(&h.runtime, NULL);
    CHECK("bit 42 is outside the IOVA field: the request is still AP-allocated",
          status == NTASI_RTKIT_RUNTIME_OK && h.asc.allocate_calls == 1 &&
              h.asc.sent_count == 1 && !h.runtime.syslog.iop_owned);

    harness_init(&h);
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG,
               buffer_request(4, 0) | (1ULL << 43));
    status = ntasi_rtkit_runtime_service(&h.runtime, NULL);
    CHECK("bit 43 is outside the IOVA field: the request is still AP-allocated",
          status == NTASI_RTKIT_RUNTIME_OK && h.asc.allocate_calls == 1 &&
              h.asc.sent_count == 1 && !h.runtime.syslog.iop_owned);

    /* Bit 41 IS the top address bit and must be honoured. */
    harness_init(&h);
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG,
               buffer_request(4, 1ULL << 41));
    status = ntasi_rtkit_runtime_service(&h.runtime, NULL);
    CHECK("bit 41 IS the top IOVA bit: the request is pre-allocated",
          status == NTASI_RTKIT_RUNTIME_OK && h.asc.allocate_calls == 0 &&
              h.asc.sent_count == 0 && h.runtime.syslog.iop_owned &&
              h.runtime.syslog.device_address == (1ULL << 41));
}

static void test_RepeatRequestIsIdempotent(void)
{
    struct harness h;
    int first;
    int second;

    harness_init(&h);
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG, buffer_request(4, 0));
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG, buffer_request(4, 0));

    first = ntasi_rtkit_runtime_service(&h.runtime, NULL);
    second = ntasi_rtkit_runtime_service(&h.runtime, NULL);

    CHECK("first syslog buffer request succeeds",
          first == NTASI_RTKIT_RUNTIME_OK);
    CHECK("a repeat syslog buffer request for the same size succeeds",
          second == NTASI_RTKIT_RUNTIME_OK);
    CHECK("a repeat request does not allocate a second buffer",
          h.asc.allocate_calls == 1);
    CHECK("a repeat request re-acknowledges the existing grant",
          h.asc.sent_count == 2 &&
              h.asc.sent[0].payload == h.asc.sent[1].payload);
}

static void test_RepeatRequestForLargerBufferIsRefused(void)
{
    struct harness h;
    int status;

    harness_init(&h);
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG, buffer_request(1, 0));
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG, buffer_request(64, 0));

    (void)ntasi_rtkit_runtime_service(&h.runtime, NULL);
    status = ntasi_rtkit_runtime_service(&h.runtime, NULL);

    CHECK("growing an existing grant is refused rather than silently leaked",
          status == NTASI_RTKIT_RUNTIME_ERR_BUFFER);
    CHECK("growing an existing grant does not allocate again",
          h.asc.allocate_calls == 1);
}

static void test_SecondCrashlogRequestMeansCrashed(void)
{
    struct harness h;
    int first;
    int second;

    harness_init(&h);
    fake_queue(&h.asc, NTASI_RTKIT_EP_CRASHLOG, buffer_request(4, 0));
    fake_queue(&h.asc, NTASI_RTKIT_EP_CRASHLOG, buffer_request(4, 0));

    first = ntasi_rtkit_runtime_service(&h.runtime, NULL);
    second = ntasi_rtkit_runtime_service(&h.runtime, NULL);

    CHECK("first crashlog buffer request succeeds",
          first == NTASI_RTKIT_RUNTIME_OK);
    CHECK("a second crashlog buffer request is the crash indication",
          second == NTASI_RTKIT_RUNTIME_ERR_CRASHED);
    CHECK("the crash latches", h.runtime.crashed);
}

static void test_UnmodelledMessagesAreTolerated(void)
{
    struct harness h;
    int status;

    /* Unknown management type. m1n1: "unknown management message %x" and
     * carry on. */
    harness_init(&h);
    fake_queue(&h.asc, NTASI_RTKIT_EP_MGMT, (uint64_t)0x3fu << MGMT_TYPE_SHIFT);
    status = ntasi_rtkit_runtime_service(&h.runtime, NULL);
    CHECK("an unknown management message is non-fatal",
          status == NTASI_RTKIT_RUNTIME_UNHANDLED && status > 0);

    /* Unknown syslog type. */
    harness_init(&h);
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG,
               (uint64_t)0x2au << MGMT_TYPE_SHIFT);
    status = ntasi_rtkit_runtime_service(&h.runtime, NULL);
    CHECK("an unknown syslog message is non-fatal",
          status == NTASI_RTKIT_RUNTIME_UNHANDLED && status > 0);

    /* Unknown ioreport type. */
    harness_init(&h);
    fake_queue(&h.asc, NTASI_RTKIT_EP_IOREPORT,
               (uint64_t)0x2bu << MGMT_TYPE_SHIFT);
    status = ntasi_rtkit_runtime_service(&h.runtime, NULL);
    CHECK("an unknown ioreport message is non-fatal",
          status == NTASI_RTKIT_RUNTIME_UNHANDLED && status > 0);

    /* A system endpoint this codec does not model at all. */
    harness_init(&h);
    fake_queue(&h.asc, 0x11u, 0);
    status = ntasi_rtkit_runtime_service(&h.runtime, NULL);
    CHECK("a message to an unmodelled system endpoint is non-fatal",
          status == NTASI_RTKIT_RUNTIME_UNHANDLED && status > 0);
}

static void test_MandatoryAcks(void)
{
    struct harness h;
    const uint64_t kSyslogLog = (uint64_t)5u << MGMT_TYPE_SHIFT | 7u;
    const uint64_t kIoreportA = (uint64_t)8u << MGMT_TYPE_SHIFT;
    const uint64_t kIoreportB = (uint64_t)0x0cu << MGMT_TYPE_SHIFT;

    harness_init(&h);
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG, kSyslogLog);
    fake_queue(&h.asc, NTASI_RTKIT_EP_IOREPORT, kIoreportA);
    fake_queue(&h.asc, NTASI_RTKIT_EP_IOREPORT, kIoreportB);

    (void)ntasi_rtkit_runtime_service(&h.runtime, NULL);
    (void)ntasi_rtkit_runtime_service(&h.runtime, NULL);
    (void)ntasi_rtkit_runtime_service(&h.runtime, NULL);

    CHECK("syslog log entries and both ioreport types are echoed back verbatim",
          h.asc.sent_count == 3 && h.asc.sent[0].payload == kSyslogLog &&
              h.asc.sent[1].payload == kIoreportA &&
              h.asc.sent[2].payload == kIoreportB);
}

static void test_ReleaseSkipsIopOwnedBuffers(void)
{
    struct harness h;

    harness_init(&h);
    /* One AP-allocated buffer (syslog) and one IOP-owned (ioreport). */
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG, buffer_request(4, 0));
    fake_queue(&h.asc, NTASI_RTKIT_EP_IOREPORT,
               buffer_request(4, 0x103fef0000ULL));
    (void)ntasi_rtkit_runtime_service(&h.runtime, NULL);
    (void)ntasi_rtkit_runtime_service(&h.runtime, NULL);

    CHECK("harness set up one AP-allocated and one IOP-owned buffer",
          !h.runtime.syslog.iop_owned && h.runtime.ioreport.iop_owned);

    ntasi_rtkit_runtime_release_buffers(&h.runtime);

    CHECK("release_shared runs for the AP-allocated buffer only",
          h.asc.release_calls == 1);
    CHECK("both buffer records are cleared",
          h.runtime.syslog.cpu_address == NULL &&
              h.runtime.syslog.device_address == 0 &&
              h.runtime.ioreport.device_address == 0 &&
              !h.runtime.ioreport.iop_owned);
}

static void test_AllocationFailureStillReportsBufferError(void)
{
    struct harness h;
    int status;

    harness_init(&h);
    h.asc.allocate_result = -1;
    fake_queue(&h.asc, NTASI_RTKIT_EP_SYSLOG, buffer_request(4, 0));
    status = ntasi_rtkit_runtime_service(&h.runtime, NULL);

    CHECK("a genuine allocation failure is still reported as -25",
          status == NTASI_RTKIT_RUNTIME_ERR_BUFFER);
    CHECK("a failed allocation sends no reply", h.asc.sent_count == 0);
}

static void test_HandoffAlwaysStopsTheCoprocessor(void)
{
    struct harness h;
    bool stopped;
    int status;

    harness_init(&h);
    /* Pretend a booted runtime whose quiesce will never be acknowledged: no
     * inbound messages at all, so both power waits time out. */
    h.runtime.booted = true;
    h.asc.cpu_control = NTASI_ASC_CPU_CONTROL_START;

    status = ntasi_rtkit_runtime_handoff(&h.runtime, &stopped);

    CHECK("a quiesce with no acknowledgement is reported as a timeout",
          status == NTASI_RTKIT_RUNTIME_ERR_TIMEOUT);
    CHECK("the coprocessor run bit is cleared even when the quiesce failed",
          (h.asc.cpu_control & NTASI_ASC_CPU_CONTROL_START) == 0);
    CHECK("the handoff reports the coprocessor as stopped", stopped);
    CHECK("the runtime is no longer marked booted", !h.runtime.booted);
}

int main(void)
{
    test_ApAllocatedRequest_Replies();
    test_PreallocatedRequest_2026_07_30_regression();
    test_IovaMaskIs42Bits();
    test_RepeatRequestIsIdempotent();
    test_RepeatRequestForLargerBufferIsRefused();
    test_SecondCrashlogRequestMeansCrashed();
    test_UnmodelledMessagesAreTolerated();
    test_MandatoryAcks();
    test_ReleaseSkipsIopOwnedBuffers();
    test_AllocationFailureStillReportsBufferError();
    test_HandoffAlwaysStopsTheCoprocessor();

    if (gFailures != 0) {
        printf("\n%d assertion(s) FAILED\n", gFailures);
        return 1;
    }

    printf("\nall assertions passed\n");
    return 0;
}

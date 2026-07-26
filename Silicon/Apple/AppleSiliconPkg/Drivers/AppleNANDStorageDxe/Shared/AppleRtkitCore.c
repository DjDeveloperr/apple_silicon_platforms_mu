/* SPDX-License-Identifier: MIT */
#include "AppleRtkitCore.h"

#include <stddef.h>

uint8_t ntasi_rtkit_mgmt_type(uint64_t msg)
{
    return (uint8_t)((msg & NTASI_RTKIT_MGMT_TYPE_MASK) >> NTASI_RTKIT_MGMT_TYPE_SHIFT);
}

uint64_t ntasi_rtkit_with_mgmt_type(uint64_t base, uint8_t type)
{
    uint64_t field = ((uint64_t)type << NTASI_RTKIT_MGMT_TYPE_SHIFT) & NTASI_RTKIT_MGMT_TYPE_MASK;
    return (base & ~NTASI_RTKIT_MGMT_TYPE_MASK) | field;
}

void ntasi_rtkit_hello_parse(uint64_t msg, uint16_t *min_ver, uint16_t *max_ver)
{
    if (min_ver != NULL)
        *min_ver = (uint16_t)(msg & NTASI_RTKIT_HELLO_MINVER_MASK);
    if (max_ver != NULL)
        *max_ver = (uint16_t)((msg & NTASI_RTKIT_HELLO_MAXVER_MASK) >>
                              NTASI_RTKIT_HELLO_MAXVER_SHIFT);
}

bool ntasi_rtkit_hello_negotiate(uint16_t peer_min, uint16_t peer_max, uint16_t *want_ver)
{
    uint16_t want = peer_max < NTASI_RTKIT_MAX_VERSION ? peer_max
                                                       : (uint16_t)NTASI_RTKIT_MAX_VERSION;
    if (want_ver != NULL)
        *want_ver = want;
    /* Supported [MIN,MAX] must overlap the peer's [peer_min,peer_max]. */
    if (peer_min > NTASI_RTKIT_MAX_VERSION || peer_max < NTASI_RTKIT_MIN_VERSION)
        return false;
    return true;
}

uint64_t ntasi_rtkit_hello_ack(uint16_t want_ver)
{
    uint64_t msg = ntasi_rtkit_with_mgmt_type(0, NTASI_RTKIT_MGMT_HELLO_ACK);
    msg |= (uint64_t)want_ver & NTASI_RTKIT_HELLO_MINVER_MASK;
    msg |= ((uint64_t)want_ver << NTASI_RTKIT_HELLO_MAXVER_SHIFT) & NTASI_RTKIT_HELLO_MAXVER_MASK;
    return msg;
}

void ntasi_rtkit_epmap_parse(uint64_t msg, uint8_t *base, uint32_t *bitmap, bool *last)
{
    if (base != NULL)
        *base = (uint8_t)((msg & NTASI_RTKIT_EPMAP_BASE_MASK) >> NTASI_RTKIT_EPMAP_BASE_SHIFT);
    if (bitmap != NULL)
        *bitmap = (uint32_t)(msg & NTASI_RTKIT_EPMAP_BITMAP_MASK);
    if (last != NULL)
        *last = (msg & NTASI_RTKIT_EPMAP_DONE) != 0;
}

uint8_t ntasi_rtkit_epmap_endpoint(uint8_t base, unsigned int bit)
{
    return (uint8_t)(32u * base + bit);
}

bool ntasi_rtkit_ep_valid(uint32_t ep)
{
    return ep < NTASI_RTKIT_EP_LIMIT;
}

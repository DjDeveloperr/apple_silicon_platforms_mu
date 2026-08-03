/** @file
  SPDX-License-Identifier: GPL-2.0-only
**/

#include "AppleUsb4RouterCore.h"

static uint32_t
load_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void
store_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void
store_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

uint32_t
as_usb4_crc32c(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;

    while (length--) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int
validate(uint64_t route, const struct as_usb4_cfg_address *address)
{
    if (address == NULL || address->length == 0 ||
        address->length > AS_USB4_CFG_MAX_DWORDS || address->offset > 0x1fff ||
        address->port > 0x3f || address->space > AS_USB4_CFG_COUNTERS ||
        address->sequence > 3)
        return AS_USB4_CFG_ERR_ARGUMENT;
    /* The protocol has only 22 route bits above route_lo. */
    if (route >> 54)
        return AS_USB4_CFG_ERR_ROUTE;
    return AS_USB4_CFG_OK;
}

static uint32_t
pack_address(const struct as_usb4_cfg_address *address)
{
    return ((uint32_t)address->offset & 0x1fffu) |
           (((uint32_t)address->length & 0x3fu) << 13) |
           (((uint32_t)address->port & 0x3fu) << 19) |
           (((uint32_t)address->space & 0x3u) << 25) |
           (((uint32_t)address->sequence & 0x3u) << 27);
}

static int
encode_header(uint8_t *wire, size_t capacity, uint64_t route,
              const struct as_usb4_cfg_address *address, size_t data_words,
              size_t *wire_length)
{
    size_t payload_length = AS_USB4_CFG_HEADER_SIZE + data_words * 4;
    size_t total_length = payload_length + AS_USB4_CFG_CRC_SIZE;
    int result = validate(route, address);

    if (result != AS_USB4_CFG_OK)
        return result;
    if (wire == NULL || wire_length == NULL || capacity < total_length)
        return AS_USB4_CFG_ERR_ARGUMENT;

    store_be32(wire, (uint32_t)(route >> 32) & 0x3fffffu);
    store_be32(wire + 4, (uint32_t)route);
    store_be32(wire + 8, pack_address(address));
    *wire_length = total_length;
    return AS_USB4_CFG_OK;
}

int
as_usb4_cfg_encode_read(uint8_t *wire, size_t capacity, uint64_t route,
                        const struct as_usb4_cfg_address *address,
                        size_t *wire_length)
{
    int result = encode_header(wire, capacity, route, address, 0, wire_length);
    if (result != AS_USB4_CFG_OK)
        return result;
    store_be32(wire + AS_USB4_CFG_HEADER_SIZE,
               as_usb4_crc32c(wire, AS_USB4_CFG_HEADER_SIZE));
    return AS_USB4_CFG_OK;
}

static int
encode_write(uint8_t *wire, size_t capacity, uint64_t route,
             const struct as_usb4_cfg_address *address,
             const uint32_t *data, size_t *wire_length, int raw_host)
{
    size_t payload_length;
    int result;

    if (address == NULL || data == NULL)
        return AS_USB4_CFG_ERR_ARGUMENT;
    result = encode_header(wire, capacity, route, address, address->length,
                           wire_length);
    if (result != AS_USB4_CFG_OK)
        return result;

    for (uint32_t index = 0; index < address->length; ++index)
        (raw_host ? store_le32 : store_be32)(
            wire + AS_USB4_CFG_HEADER_SIZE + index * 4, data[index]);
    payload_length = AS_USB4_CFG_HEADER_SIZE + address->length * 4;
    store_be32(wire + payload_length, as_usb4_crc32c(wire, payload_length));
    return AS_USB4_CFG_OK;
}

int
as_usb4_cfg_encode_write(uint8_t *wire, size_t capacity, uint64_t route,
                         const struct as_usb4_cfg_address *address,
                         const uint32_t *data, size_t *wire_length)
{
    return encode_write(wire, capacity, route, address, data, wire_length, 0);
}

int
as_usb4_cfg_encode_write_raw_host(
    uint8_t *wire, size_t capacity, uint64_t route,
    const struct as_usb4_cfg_address *address, const uint32_t *host_memory,
    size_t *wire_length)
{
    /* Apple ConfigWriteCommand copies the little-endian descriptor payload
     * verbatim after swapping only its header. 0x00000080 therefore emits
     * 80 00 00 00 and addresses register bit 31. Keep this dangerous API
     * explicit; ordinary register writes use the endian-safe function above. */
    return encode_write(wire, capacity, route, address, host_memory,
                        wire_length, 1);
}

int
as_usb4_cfg_decode(uint32_t *words, size_t word_capacity,
                   const uint8_t *wire, size_t wire_length, int expect_reply,
                   uint64_t *route,
                   struct as_usb4_cfg_address *address, size_t *word_count)
{
    uint32_t address_word;
    uint32_t route_high;
    size_t payload_length;
    size_t count;

    if (words == NULL || wire == NULL || route == NULL || address == NULL ||
        word_count == NULL || wire_length < AS_USB4_CFG_HEADER_SIZE +
                                            AS_USB4_CFG_CRC_SIZE ||
        (wire_length & 3u) != 0)
        return AS_USB4_CFG_ERR_ARGUMENT;

    payload_length = wire_length - AS_USB4_CFG_CRC_SIZE;
    if (load_be32(wire + payload_length) !=
        as_usb4_crc32c(wire, payload_length))
        return AS_USB4_CFG_ERR_CRC;

    route_high = load_be32(wire);
    /* tb_cfg_header.unknown is ten bits above route_hi. Linux requires the
     * highest one (wire bit 31) on replies and zero on requests. */
    if ((route_high & 0xffc00000u) !=
        (expect_reply ? 0x80000000u : 0u))
        return AS_USB4_CFG_ERR_RESPONSE;
    *route = ((uint64_t)(route_high & 0x3fffffu) << 32) |
             load_be32(wire + 4);
    address_word = load_be32(wire + 8);
    if (address_word >> 29)
        return AS_USB4_CFG_ERR_RESPONSE;
    address->offset = address_word & 0x1fff;
    address->length = (address_word >> 13) & 0x3f;
    address->port = (address_word >> 19) & 0x3f;
    address->space = (address_word >> 25) & 0x3;
    address->sequence = (address_word >> 27) & 0x3;

    count = (payload_length - AS_USB4_CFG_HEADER_SIZE) / 4;
    /* READ replies carry address.length data words; WRITE replies and READ
     * requests carry only the 12-byte header. Packet-type-specific code owns
     * that distinction, while this codec reports the actual payload count. */
    if (count > word_capacity)
        return AS_USB4_CFG_ERR_RESPONSE;
    for (size_t index = 0; index < count; ++index)
        words[index] = load_be32(wire + AS_USB4_CFG_HEADER_SIZE + index * 4);
    *word_count = count;
    return AS_USB4_CFG_OK;
}

int
as_usb4_cfg_decode_response(
    uint32_t *words, size_t word_capacity, const uint8_t *wire,
    size_t wire_length, enum as_usb4_cfg_operation operation,
    uint64_t expected_route, const struct as_usb4_cfg_address *expected,
    size_t *word_count)
{
    struct as_usb4_cfg_address actual;
    uint64_t route;
    size_t count;
    int result;

    if (expected == NULL || word_count == NULL ||
        (operation != AS_USB4_CFG_READ && operation != AS_USB4_CFG_WRITE))
        return AS_USB4_CFG_ERR_ARGUMENT;
    result = as_usb4_cfg_decode(words, word_capacity, wire, wire_length, 1,
                                &route, &actual, &count);
    if (result != AS_USB4_CFG_OK)
        return result;
    if (route != expected_route || actual.offset != expected->offset ||
        actual.length != expected->length || actual.port != expected->port ||
        actual.space != expected->space ||
        actual.sequence != expected->sequence ||
        count != (operation == AS_USB4_CFG_READ ? expected->length : 0u))
        return AS_USB4_CFG_ERR_RESPONSE;
    *word_count = count;
    return AS_USB4_CFG_OK;
}

int
as_usb4_find_switch_vse(as_usb4_cfg_read_fn read, void *context,
                        uint64_t route, uint16_t first_capability,
                        uint8_t vendor_capability, uint16_t *offset)
{
    uint16_t visited[256];
    size_t visited_count = 0;
    uint16_t current = first_capability;

    if (read == NULL || offset == NULL)
        return AS_USB4_CFG_ERR_ARGUMENT;

    while (current != 0 && current != UINT16_MAX) {
        uint32_t header[2] = {0};
        uint8_t cap;
        uint8_t next;

        for (size_t index = 0; index < visited_count; ++index)
            if (visited[index] == current)
                return AS_USB4_CFG_ERR_CAPABILITY;
        if (visited_count == sizeof(visited) / sizeof(visited[0]))
            return AS_USB4_CFG_ERR_CAPABILITY;
        visited[visited_count++] = current;

        if (read(context, route, AS_USB4_CFG_SWITCH, current, 2, header) != 0)
            return AS_USB4_CFG_ERR_RESPONSE;
        next = header[0] & 0xff;
        cap = (header[0] >> 8) & 0xff;

        if (cap == AS_USB4_SWITCH_CAP_VSE) {
            uint8_t vsec = (header[0] >> 16) & 0xff;
            uint8_t short_length = (header[0] >> 24) & 0xff;
            if (vsec == vendor_capability) {
                *offset = current;
                return AS_USB4_CFG_OK;
            }
            if (short_length == 0)
                current = header[1] & 0xffff;
            else
                current = next;
        } else if (cap == AS_USB4_SWITCH_CAP_TMU) {
            current = next;
        } else {
            return AS_USB4_CFG_ERR_CAPABILITY;
        }
    }
    return AS_USB4_CFG_ERR_NOT_FOUND;
}

int
as_usb4_router_configure(const struct as_usb4_router_ops *ops,
                         void *context, uint64_t route,
                         int all_parents_support_usb_tunnels)
{
    uint32_t current;
    uint32_t value;

    if (ops == NULL || ops->read32 == NULL || ops->write32 == NULL ||
        ops->poll32 == NULL || route >> 54)
        return AS_USB4_CFG_ERR_ARGUMENT;
    if (ops->poll32(context, route, 0, AS_USB4_CFG_SWITCH,
                    AS_USB4_ROUTER_CS_6, AS_USB4_ROUTER_READY,
                    AS_USB4_ROUTER_READY) != 0)
        return AS_USB4_CFG_ERR_RESPONSE;
    if (ops->read32(context, route, 0, AS_USB4_CFG_SWITCH,
                    AS_USB4_ROUTER_CS_5, &current) != 0 ||
        current == UINT32_MAX)
        return AS_USB4_CFG_ERR_RESPONSE;
    value = all_parents_support_usb_tunnels
                ? AS_USB4_ROUTER_USB_TUNNELS
                : AS_USB4_ROUTER_NO_USB_TUNNELS;
    if (ops->write32(context, route, 0, AS_USB4_CFG_SWITCH,
                     AS_USB4_ROUTER_CS_5, value) != 0)
        return AS_USB4_CFG_ERR_RESPONSE;
    if (ops->write32(context, route, 0, AS_USB4_CFG_SWITCH,
                     AS_USB4_ROUTER_CS_5, value | AS_USB4_ROUTER_CV) != 0)
        return AS_USB4_CFG_ERR_RESPONSE;
    if (ops->poll32(context, route, 0, AS_USB4_CFG_SWITCH,
                    AS_USB4_ROUTER_CS_6, AS_USB4_ROUTER_CONFIG_ACK,
                    AS_USB4_ROUTER_CONFIG_ACK) != 0)
        return AS_USB4_CFG_ERR_RESPONSE;
    return AS_USB4_CFG_OK;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_websocket_internal.h"
enum { CLOSE_RESERVED = 1004, CLOSE_NO_STATUS = 1005, CLOSE_ABNORMAL = 1006,
       CLOSE_LAST_STANDARD = 1014, CLOSE_PRIVATE_FIRST = 3000, CLOSE_PRIVATE_LAST = 4999 };
static bool valid_utf8(const uint8_t *data, size_t size)
{
    size_t index = 0;
    while (index < size) {
        uint32_t value = data[index++], minimum;
        size_t following;
        if (value < 0x80) continue;
        if ((value & 0xe0) == 0xc0) { following = 1; minimum = 0x80; value &= 0x1f; }
        else if ((value & 0xf0) == 0xe0) { following = 2; minimum = 0x800; value &= 0x0f; }
        else if ((value & 0xf8) == 0xf0) { following = 3; minimum = 0x10000; value &= 0x07; }
        else return false;
        if (following > size - index) return false;
        while (following--) {
            uint8_t next = data[index++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}
enum thrift_status thrift_websocket_validate_close(const uint8_t *data, size_t size)
{
    unsigned code;
    if (!size) return THRIFT_OK;
    if (size < THRIFT_WS_CLOSE_CODE_SIZE || size > THRIFT_WS_CONTROL_LIMIT) return THRIFT_PROTOCOL;
    code = ((unsigned)data[0] << 8) | data[1];
    if (!((code >= THRIFT_WS_NORMAL_CLOSE && code <= CLOSE_LAST_STANDARD &&
           code != CLOSE_RESERVED && code != CLOSE_NO_STATUS && code != CLOSE_ABNORMAL) ||
          (code >= CLOSE_PRIVATE_FIRST && code <= CLOSE_PRIVATE_LAST))) return THRIFT_PROTOCOL;
    return valid_utf8(data + THRIFT_WS_CLOSE_CODE_SIZE, size - THRIFT_WS_CLOSE_CODE_SIZE) ?
        THRIFT_OK : THRIFT_PROTOCOL;
}

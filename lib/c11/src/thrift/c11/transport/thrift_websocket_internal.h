/* SPDX-License-Identifier: Apache-2.0 */
#ifndef THRIFT_WEBSOCKET_INTERNAL_H
#define THRIFT_WEBSOCKET_INTERNAL_H
#include "thrift_buffer_internal.h"
#include <thrift/c11/transport/thrift_transport.h>
enum {
    THRIFT_WS_NORMAL_CLOSE = 1000, THRIFT_WS_PROTOCOL_CLOSE = 1002,
    THRIFT_WS_UNSUPPORTED_CLOSE = 1003, THRIFT_WS_TOO_LARGE_CLOSE = 1009,
    THRIFT_WS_INTERNAL_CLOSE = 1011, THRIFT_WS_CLOSE_CODE_SIZE = 2,
    THRIFT_WS_CONTROL_LIMIT = 125, THRIFT_WS_COPY_SIZE = 8192,
    THRIFT_WS_FINAL = 0x80, THRIFT_WS_RESERVED = 0x70, THRIFT_WS_OPCODE = 0x0f
};
/* Monotonic clock; platform implementation selected in CMake. */
enum thrift_status thrift_websocket_now(uint64_t *milliseconds);
enum thrift_status thrift_websocket_validate_close(const uint8_t *data, size_t size);
/* Connection-owned bounded message; reset sizes before reusing for another RPC. */
struct thrift_websocket_message {
    struct thrift_buffer request, response;
    enum thrift_status error;
};
struct thrift_transport thrift_websocket_message_transport(struct thrift_websocket_message *message);
enum thrift_status thrift_websocket_message_finish(struct thrift_websocket_message *message,
    enum thrift_status status);
#endif

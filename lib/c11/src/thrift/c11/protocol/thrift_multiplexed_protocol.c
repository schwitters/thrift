/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/protocol/thrift_multiplexed_protocol.h>
#include "thrift_protocol_internal.h"
#include <limits.h>
#include <string.h>

enum thrift_status thrift_multiplexed_protocol_init(
    struct thrift_protocol *p, struct thrift_transport transport,
    enum thrift_protocol_kind kind, const char *service_name)
{
    enum thrift_status status;
    if (!service_name || !service_name[0] || strchr(service_name, ':'))
        return THRIFT_INVALID;
    status = thrift_protocol_init_kind(p, transport, kind);
    if (status == THRIFT_OK)
        p->service_name = service_name;
    return status;
}

enum thrift_status thrift_message_name(struct thrift_protocol *p, const char *name,
    enum thrift_message kind, struct thrift_bytes *bytes, bool *owned)
{
    size_t method_size = strlen(name), service_size;
    void *allocation = NULL;
    enum thrift_status status;
    *owned = false;
    bytes->data = (uint8_t *)name;
    bytes->size = method_size;
    if (!p->service_name || (kind != C11_CALL && kind != C11_ONEWAY))
        return THRIFT_OK;
    service_size = strlen(p->service_name);
    if (method_size > INT32_MAX || service_size >= INT32_MAX - method_size)
        return THRIFT_LIMIT;
    bytes->size = service_size + 1 + method_size;
    if (bytes->size >= SIZE_MAX)
        return THRIFT_LIMIT;
    status = thrift_allocate(p, bytes->size + 1, 1, &allocation);
    if (status != THRIFT_OK)
        return status;
    bytes->data = allocation;
    *owned = true;
    memcpy(bytes->data, p->service_name, service_size);
    bytes->data[service_size] = ':';
    memcpy(bytes->data + service_size + 1, name, method_size);
    return THRIFT_OK;
}

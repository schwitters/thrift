/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/server/thrift_simple_server.h>
#include "../thrift_log_internal.h"

static enum thrift_status thrift_server_serve_kind_impl(
    struct thrift_socket *listener,
    enum thrift_status (*process)(struct thrift_protocol *, void *),
    void *handler, size_t request_count, uint32_t timeout_ms,
    enum thrift_protocol_kind kind)
{
    struct thrift_socket *client = NULL;
    struct thrift_transport transport = {0};
    struct thrift_protocol protocol;
    enum thrift_status status, close_status;
    size_t i;
    if (!listener || !process || !handler || !request_count || !timeout_ms ||
        (kind != THRIFT_BINARY && kind != THRIFT_COMPACT))
        return THRIFT_INVALID;
    status = thrift_socket_accept(listener, &client);
    if (status != THRIFT_OK)
        return status;
    status = thrift_socket_timeout(client, timeout_ms);
    if (status == THRIFT_OK)
        status = thrift_socket_transport(client, &transport);
    if (status == THRIFT_OK)
        status = thrift_protocol_init_kind(&protocol, transport, kind);
    for (i = 0; status == THRIFT_OK && i < request_count; ++i)
        status = process(&protocol, handler);
    close_status = thrift_socket_close(client);
    return status == THRIFT_OK ? close_status : status;
}

enum thrift_status thrift_server_serve_kind(
    struct thrift_socket *listener,
    enum thrift_status (*process)(struct thrift_protocol *, void *),
    void *handler, size_t request_count, uint32_t timeout_ms,
    enum thrift_protocol_kind kind)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_INFO, "tcp.server.serve", request_count);
    enum thrift_status status = thrift_server_serve_kind_impl(listener, process, handler, request_count, timeout_ms, kind);
    thrift_log_end(scope, status);
    return status;
}

enum thrift_status thrift_server_serve(
    struct thrift_socket *listener,
    enum thrift_status (*process)(struct thrift_protocol *, void *),
    void *handler, size_t request_count, uint32_t timeout_ms)
{
    return thrift_server_serve_kind(listener, process, handler, request_count, timeout_ms,
                                       THRIFT_BINARY);
}

/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_tls_internal.h"
#include "../thrift_log_internal.h"
#include <limits.h>
#include <stdlib.h>
static enum thrift_status finish(struct thrift_tls_socket *socket, enum thrift_status status)
{
    if (status != THRIFT_OK && status != THRIFT_AGAIN && socket->error == THRIFT_OK)
        socket->error = status;
    return status;
}
void thrift_tls_socket_destroy(struct thrift_tls_socket *socket)
{
    if (!socket) return;
    thrift_tls_engine_destroy(&socket->engine);
    thrift_net_destroy(socket->net);
    free(socket);
}
static enum thrift_status initialize(struct thrift_tls_socket *socket, const char *name, uint32_t timeout_ms)
{
    enum thrift_status status = thrift_tls_now(&socket->deadline);
    if (status != THRIFT_OK) return status;
    socket->deadline += timeout_ms;
    socket->interest = THRIFT_TLS_READ | THRIFT_TLS_WRITE;
    return thrift_tls_engine_create(socket->context, name, &socket->engine);
}
enum thrift_status thrift_tls_connect(struct thrift_tls_context *context, const char *address,
    uint16_t port, const char *server_name, uint32_t timeout_ms, struct thrift_tls_socket **out)
{
    struct thrift_tls_socket *socket;
    enum thrift_status status;
    if (!out) return THRIFT_INVALID;
    *out = NULL;
    if (!context || thrift_tls_context_server(context) || !address || !port || !server_name ||
        !server_name[0] || !timeout_ms || timeout_ms > INT_MAX) return THRIFT_INVALID;
    socket = calloc(1, sizeof(*socket));
    if (!socket) return THRIFT_NOMEM;
    socket->context = context; socket->connecting = true;
    status = initialize(socket, server_name, timeout_ms);
    if (status == THRIFT_OK) status = thrift_net_open(address, port, false, &socket->net);
    if (status != THRIFT_OK) { thrift_tls_socket_destroy(socket); return status; }
    *out = socket;
    return THRIFT_OK;
}
enum thrift_status thrift_tls_listen(struct thrift_tls_context *context, const char *address,
    uint16_t port, struct thrift_tls_socket **out)
{
    struct thrift_tls_socket *socket;
    enum thrift_status status;
    if (!out) return THRIFT_INVALID;
    *out = NULL;
    if (!context || !thrift_tls_context_server(context)) return THRIFT_INVALID;
    socket = calloc(1, sizeof(*socket));
    if (!socket) return THRIFT_NOMEM;
    socket->context = context; socket->listener = true; socket->interest = THRIFT_TLS_READ;
    status = thrift_net_open(address, port, true, &socket->net);
    if (status != THRIFT_OK) { thrift_tls_socket_destroy(socket); return status; }
    *out = socket;
    return THRIFT_OK;
}
enum thrift_status thrift_tls_accept(struct thrift_tls_socket *listener, uint32_t timeout_ms, struct thrift_tls_socket **out)
{
    struct thrift_tls_socket *socket;
    struct thrift_net *net = NULL;
    enum thrift_status status;
    if (!out) return THRIFT_INVALID;
    *out = NULL;
    if (!listener || !listener->listener || !timeout_ms || timeout_ms > INT_MAX) return THRIFT_INVALID;
    status = thrift_net_accept(listener->net, &net);
    if (status != THRIFT_OK) return status;
    socket = calloc(1, sizeof(*socket));
    if (!socket) { thrift_net_destroy(net); return THRIFT_NOMEM; }
    socket->context = listener->context; socket->net = net;
    status = initialize(socket, NULL, timeout_ms);
    if (status != THRIFT_OK) { thrift_tls_socket_destroy(socket); return status; }
    *out = socket;
    return THRIFT_OK;
}
enum thrift_status thrift_tls_port(const struct thrift_tls_socket *socket, uint16_t *port)
{
    return socket && port ? thrift_net_port(socket->net, port) : THRIFT_INVALID;
}
unsigned thrift_tls_interest(const struct thrift_tls_socket *socket) { return socket ? socket->interest : 0; }
enum thrift_status thrift_tls_poll(struct thrift_tls_poll_entry *entries, size_t count, uint32_t timeout_ms)
{
    size_t i;
    if ((!entries && count) || timeout_ms > INT_MAX) return THRIFT_INVALID;
    for (i = 0; i < count; ++i) {
        entries[i].ready = 0;
        if (!entries[i].socket || !entries[i].events || (entries[i].events & ~(unsigned)(THRIFT_TLS_READ | THRIFT_TLS_WRITE)))
            return THRIFT_INVALID;
    }
    return thrift_net_poll(entries, count, timeout_ms);
}
static enum thrift_status flush_socket(struct thrift_tls_socket *socket)
{
    struct thrift_buffer *output;
    size_t count = 0;
    enum thrift_status status;
    if (!socket || socket->listener) return THRIFT_INVALID;
    if (socket->error != THRIFT_OK) return socket->error;
    output = &socket->engine.output;
    if (output->position == output->size) { output->position = output->size = 0; return THRIFT_OK; }
    socket->interest = THRIFT_TLS_WRITE;
    status = thrift_net_write(socket->net, output->data + output->position, output->size - output->position, &count);
    output->position += count;
    if (status != THRIFT_OK) return finish(socket, status);
    if (output->position != output->size) return THRIFT_AGAIN;
    output->position = output->size = 0;
    socket->interest = THRIFT_TLS_READ;
    return THRIFT_OK;
}
static enum thrift_status receive(struct thrift_tls_socket *socket)
{
    uint8_t bytes[THRIFT_TLS_RECORD_SIZE];
    size_t count = 0;
    enum thrift_status status;
    socket->interest = THRIFT_TLS_READ;
    status = thrift_net_read(socket->net, bytes, sizeof(bytes), &count);
    if (status == THRIFT_EOF) return THRIFT_IO; /* TLS close_notify is required. */
    return status == THRIFT_OK ? thrift_tls_engine_feed(&socket->engine, bytes, count) : status;
}
static enum thrift_status handshake_socket(struct thrift_tls_socket *socket)
{
    enum thrift_status status;
    uint64_t now;
    if (!socket || socket->listener || socket->closing) return THRIFT_INVALID;
    if (socket->error != THRIFT_OK) return socket->error;
    status = thrift_tls_now(&now);
    if (status != THRIFT_OK) return finish(socket, status);
    if (!socket->established && now >= socket->deadline) return finish(socket, THRIFT_TIMEOUT);
    if (socket->connecting) {
        socket->interest = THRIFT_TLS_WRITE;
        status = thrift_net_connected(socket->net);
        if (status != THRIFT_OK) return finish(socket, status);
        socket->connecting = false;
    }
    status = thrift_tls_flush(socket);
    if (status != THRIFT_OK) return status;
    if (socket->engine.ready) { socket->established = true; return THRIFT_OK; }
    status = thrift_tls_engine_handshake(&socket->engine);
    if (status == THRIFT_AGAIN) {
        enum thrift_status flushed = thrift_tls_flush(socket);
        if (flushed != THRIFT_OK) return flushed;
        status = receive(socket);
        if (status == THRIFT_OK) status = thrift_tls_engine_handshake(&socket->engine);
    }
    if (status != THRIFT_OK && status != THRIFT_AGAIN) return finish(socket, status);
    { enum thrift_status flushed = thrift_tls_flush(socket);
      if (flushed != THRIFT_OK) return flushed; }
    socket->interest = THRIFT_TLS_READ;
    socket->established = socket->engine.ready;
    return socket->established ? THRIFT_OK : THRIFT_AGAIN;
}
static enum thrift_status read_socket(struct thrift_tls_socket *socket, void *data, size_t size, size_t *count)
{
    enum thrift_status status;
    if (!count) return THRIFT_INVALID;
    *count = 0;
    if (!socket || socket->listener || !socket->established || (!data && size)) return THRIFT_INVALID;
    if (socket->error != THRIFT_OK) return socket->error;
    if (!size) return THRIFT_OK;
    status = thrift_tls_flush(socket);
    if (status != THRIFT_OK) return status;
    status = thrift_tls_engine_read(&socket->engine, data, size, count);
    if (status == THRIFT_AGAIN) {
        enum thrift_status flushed = thrift_tls_flush(socket);
        if (flushed != THRIFT_OK) return flushed;
        status = receive(socket);
        if (status == THRIFT_OK) status = thrift_tls_engine_read(&socket->engine, data, size, count);
    }
    socket->interest = socket->engine.output.size ? THRIFT_TLS_WRITE : THRIFT_TLS_READ;
    return finish(socket, status);
}
static enum thrift_status write_socket(struct thrift_tls_socket *socket, const void *data, size_t size, size_t *count)
{
    enum thrift_status status;
    if (!count) return THRIFT_INVALID;
    *count = 0;
    if (!socket || socket->listener || socket->closing || !socket->established || (!data && size)) return THRIFT_INVALID;
    if (socket->error != THRIFT_OK) return socket->error;
    if (!size) return THRIFT_OK;
    status = thrift_tls_flush(socket);
    if (status != THRIFT_OK) return status;
    status = thrift_tls_engine_write(&socket->engine, data, size, count);
    socket->interest = socket->engine.output.size ? THRIFT_TLS_WRITE : THRIFT_TLS_READ;
    return finish(socket, status);
}
static enum thrift_status shutdown_socket(struct thrift_tls_socket *socket)
{
    enum thrift_status status;
    if (!socket || socket->listener || !socket->established) return THRIFT_INVALID;
    if (socket->error != THRIFT_OK && socket->error != THRIFT_EOF) return socket->error;
    socket->error = THRIFT_OK;
    status = thrift_tls_flush(socket);
    if (status != THRIFT_OK) return status;
    if (!socket->closing) {
        socket->closing = true;
        status = thrift_tls_engine_shutdown(&socket->engine);
        if (status != THRIFT_OK) return finish(socket, status);
    }
    return thrift_tls_flush(socket);
}

enum thrift_status thrift_tls_handshake(struct thrift_tls_socket *socket)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_TRACE, "tls.handshake", 0);
    enum thrift_status status = handshake_socket(socket);
    thrift_log_end(scope, status);
    return status;
}

enum thrift_status thrift_tls_read(struct thrift_tls_socket *socket, void *data, size_t size, size_t *count)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_TRACE, "tls.read", 0);
    enum thrift_status status = read_socket(socket, data, size, count);
    thrift_log_end(scope, status);
    return status;
}

enum thrift_status thrift_tls_write(struct thrift_tls_socket *socket, const void *data, size_t size, size_t *count)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_TRACE, "tls.write", 0);
    enum thrift_status status = write_socket(socket, data, size, count);
    thrift_log_end(scope, status);
    return status;
}

enum thrift_status thrift_tls_flush(struct thrift_tls_socket *socket)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_TRACE, "tls.flush", 0);
    enum thrift_status status = flush_socket(socket);
    thrift_log_end(scope, status);
    return status;
}

enum thrift_status thrift_tls_shutdown(struct thrift_tls_socket *socket)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_TRACE, "tls.shutdown", 0);
    enum thrift_status status = shutdown_socket(socket);
    thrift_log_end(scope, status);
    return status;
}

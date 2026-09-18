/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_tls_server.h"
#include "../transport/thrift_tls_internal.h"
#include "../thrift_log_internal.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum connection_phase { TLS_HANDSHAKE, FRAME_HEADER, FRAME_BODY, JOB_WAIT, JOB_RUN, FRAME_REPLY };
enum { TLS_CONNECTION_LIMIT = 4096 };
struct tls_connection {
    struct thrift_tls_server *server;
    struct thrift_tls_socket *socket;
    enum connection_phase phase;
    uint64_t deadline;
    uint8_t header[THRIFT_TLS_FRAME_HEADER];
    size_t header_used, body_used;
    struct thrift_buffer request, response;
    enum thrift_status result;
};
struct thrift_tls_server {
    struct thrift_tls_server_options options;
    struct thrift_limits limits;
    struct thrift_tls_socket *listener;
    struct tls_connection *connections;
    struct thrift_tls_worker **workers;
    bool *busy;
    struct thrift_tls_poll_entry *polls;
    enum thrift_status (*process)(struct thrift_protocol *, void *);
    void *handler;
    size_t next_job;
};
static enum thrift_status request_read(void *context, void *data, size_t size)
{
    struct tls_connection *connection = context;
    return thrift_buffer_read(&connection->request, data, size);
}
static enum thrift_status response_write(void *context, const void *data, size_t size)
{
    struct tls_connection *connection = context;
    return thrift_buffer_append(&connection->response, data, size);
}
static void run_request(void *job)
{
    struct tls_connection *connection = job;
    struct thrift_tls_server *server = connection->server;
    struct thrift_transport transport = {connection, request_read, response_write, NULL};
    struct thrift_protocol protocol;
    connection->result = thrift_protocol_init_kind(&protocol, transport, server->options.kind);
    if (connection->result != THRIFT_OK) return;
    if (server->options.limits != NULL) {
        protocol.limits = server->limits;
        thrift_protocol_reset(&protocol);
    }
    connection->result = server->process(&protocol, server->handler);
    if (connection->result == THRIFT_OK && connection->request.position != connection->request.size)
        connection->result = THRIFT_INVALID;
}
static void release_connection(struct tls_connection *connection)
{
    thrift_tls_socket_destroy(connection->socket);
    connection->socket = NULL;
    /* A running worker exclusively owns both buffers until take/join. */
    if (connection->phase == JOB_RUN) return;
    thrift_buffer_clear(&connection->request);
    thrift_buffer_clear(&connection->response);
    memset(connection, 0, sizeof(*connection));
}
static void next_request(struct tls_connection *connection, uint64_t now)
{
    thrift_buffer_clear(&connection->request);
    thrift_buffer_clear(&connection->response);
    connection->request.limit = connection->server->options.max_frame_size;
    connection->response.limit = connection->server->options.max_frame_size;
    connection->header_used = connection->body_used = 0;
    connection->phase = FRAME_HEADER;
    connection->deadline = now + connection->server->options.timeout_ms;
}
static void collect_jobs(struct thrift_tls_server *server, uint64_t now)
{
    size_t index;
    for (index = 0; index < server->options.worker_count; ++index) {
        void *job = NULL;
        struct tls_connection *connection;
        uint32_t length;
        if (!thrift_tls_worker_take(server->workers[index], &job)) continue;
        server->busy[index] = false;
        connection = job;
        connection->phase = FRAME_REPLY;
        if (connection->socket == NULL || connection->result != THRIFT_OK || now >= connection->deadline) {
            release_connection(connection);
            continue;
        }
        if (connection->response.size == 0) {
            next_request(connection, now);
            continue;
        }
        length = (uint32_t)connection->response.size;
        connection->header[0] = (uint8_t)(length >> 24);
        connection->header[1] = (uint8_t)(length >> 16);
        connection->header[2] = (uint8_t)(length >> 8);
        connection->header[3] = (uint8_t)length;
        connection->header_used = connection->body_used = 0;
    }
}
static enum thrift_status receive_frame(struct tls_connection *connection)
{
    size_t count = 0;
    enum thrift_status status;
    if (connection->phase == FRAME_HEADER) {
        uint32_t length;
        status = thrift_tls_read(connection->socket, connection->header + connection->header_used,
            sizeof(connection->header) - connection->header_used, &count);
        connection->header_used += count;
        if (status != THRIFT_OK) return status;
        if (connection->header_used != sizeof(connection->header)) return THRIFT_AGAIN;
        length = ((uint32_t)connection->header[0] << 24) | ((uint32_t)connection->header[1] << 16)
            | ((uint32_t)connection->header[2] << 8) | connection->header[3];
        if (length == 0 || length > connection->request.limit) return THRIFT_LIMIT;
        connection->request.data = malloc(length);
        if (connection->request.data == NULL) return THRIFT_NOMEM;
        connection->request.capacity = connection->request.size = length;
        connection->phase = FRAME_BODY;
    }
    status = thrift_tls_read(connection->socket, connection->request.data + connection->body_used,
        connection->request.size - connection->body_used, &count);
    connection->body_used += count;
    if (status == THRIFT_OK && connection->body_used == connection->request.size)
        connection->phase = JOB_WAIT;
    return status;
}
static enum thrift_status send_frame(struct tls_connection *connection, uint64_t now)
{
    size_t count = 0;
    enum thrift_status status;
    if (connection->header_used < sizeof(connection->header)) {
        status = thrift_tls_write(connection->socket, connection->header + connection->header_used,
            sizeof(connection->header) - connection->header_used, &count);
        connection->header_used += count;
        if (status != THRIFT_OK) return status;
        if (connection->header_used < sizeof(connection->header)) return THRIFT_AGAIN;
    }
    if (connection->body_used < connection->response.size) {
        status = thrift_tls_write(connection->socket, connection->response.data + connection->body_used,
            connection->response.size - connection->body_used, &count);
        connection->body_used += count;
        if (status != THRIFT_OK) return status;
        if (connection->body_used < connection->response.size) return THRIFT_AGAIN;
    }
    status = thrift_tls_flush(connection->socket);
    if (status == THRIFT_OK) next_request(connection, now);
    return status;
}
static void service_connection(struct tls_connection *connection, uint64_t now)
{
    enum thrift_status status = THRIFT_OK;
    if (connection->socket == NULL) return;
    if (now >= connection->deadline) {
        release_connection(connection);
        return;
    }
    if (connection->phase == TLS_HANDSHAKE) {
        status = thrift_tls_handshake(connection->socket);
        if (status == THRIFT_OK) next_request(connection, now);
    }
    if (status == THRIFT_OK && (connection->phase == FRAME_HEADER || connection->phase == FRAME_BODY))
        status = receive_frame(connection);
    if (status == THRIFT_OK && connection->phase == FRAME_REPLY)
        status = send_frame(connection, now);
    if (status != THRIFT_OK && status != THRIFT_AGAIN) release_connection(connection);
}
static enum thrift_status accept_connections(struct thrift_tls_server *server, uint64_t now)
{
    size_t index;
    for (index = 0; index < server->options.max_connections; ++index) {
        struct tls_connection *connection = &server->connections[index];
        enum thrift_status status;
        if (connection->socket != NULL || connection->phase == JOB_RUN) continue;
        status = thrift_tls_accept(server->listener, server->options.timeout_ms, &connection->socket);
        if (status == THRIFT_AGAIN) return THRIFT_OK;
        if (status != THRIFT_OK) return status;
        connection->server = server;
        connection->phase = TLS_HANDSHAKE;
        connection->deadline = now + server->options.timeout_ms;
    }
    return THRIFT_OK;
}
static enum thrift_status assign_jobs(struct thrift_tls_server *server)
{
    size_t offset, worker_index = 0, start = server->next_job;
    for (offset = 0; offset < server->options.max_connections; ++offset) {
        size_t connection_index = (start + offset) % server->options.max_connections;
        struct tls_connection *connection = &server->connections[connection_index];
        enum thrift_status status;
        if (connection->socket == NULL || connection->phase != JOB_WAIT) continue;
        while (worker_index < server->options.worker_count && server->busy[worker_index]) ++worker_index;
        if (worker_index == server->options.worker_count) break;
        status = thrift_tls_worker_submit(server->workers[worker_index], connection);
        if (status != THRIFT_OK) return status;
        server->busy[worker_index] = true;
        connection->phase = JOB_RUN;
        server->next_job = (connection_index + 1) % server->options.max_connections;
    }
    return THRIFT_OK;
}
enum thrift_status thrift_tls_server_poll(struct thrift_tls_server *server, uint32_t timeout_ms)
{
    size_t index, count = 0;
    uint64_t now;
    bool vacancy = false;
    enum thrift_status status;
    if (server == NULL || timeout_ms > INT_MAX) return THRIFT_INVALID;
    status = thrift_tls_now(&now);
    if (status != THRIFT_OK) return status;
    collect_jobs(server, now);
    for (index = 0; index < server->options.max_connections; ++index) {
        struct tls_connection *connection = &server->connections[index];
        enum connection_phase phase = connection->phase;
        size_t header_used = connection->header_used, body_used = connection->body_used;
        service_connection(connection, now);
        /* Continue draining buffered TLS records before waiting for network readiness. */
        if (connection->phase != phase || connection->header_used != header_used || connection->body_used != body_used)
            timeout_ms = 0;
    }
    status = assign_jobs(server);
    if (status != THRIFT_OK) return status;
    status = accept_connections(server, now);
    if (status != THRIFT_OK) return status;
    if (timeout_ms > THRIFT_TLS_POLL_QUANTUM_MS) timeout_ms = THRIFT_TLS_POLL_QUANTUM_MS;
    for (index = 0; index < server->options.max_connections; ++index) {
        struct tls_connection *connection = &server->connections[index];
        if (connection->socket == NULL) {
            if (connection->phase != JOB_RUN) vacancy = true;
            continue;
        }
        if (connection->deadline <= now) timeout_ms = 0;
        else if (connection->deadline - now < timeout_ms) timeout_ms = (uint32_t)(connection->deadline - now);
        if (connection->phase == JOB_RUN || connection->phase == JOB_WAIT) continue;
        server->polls[count].socket = connection->socket;
        server->polls[count++].events = thrift_tls_interest(connection->socket);
    }
    if (vacancy) {
        server->polls[count].socket = server->listener;
        server->polls[count++].events = THRIFT_TLS_READ;
    }
    return thrift_tls_poll(server->polls, count, timeout_ms);
}
static enum thrift_status create_server(const struct thrift_tls_server_options *options,
    enum thrift_status (*process)(struct thrift_protocol *, void *), void *handler,
    struct thrift_tls_server **out)
{
    struct thrift_tls_server *server;
    size_t index;
    enum thrift_status status;
    if (out == NULL) return THRIFT_INVALID;
    *out = NULL;
    if (options == NULL || process == NULL || !thrift_tls_context_server(options->context)
        || options->max_connections == 0 || options->max_connections > TLS_CONNECTION_LIMIT
        || options->max_frame_size == 0 || options->max_frame_size > INT32_MAX
        || options->timeout_ms == 0 || options->timeout_ms > INT_MAX
        || options->worker_count == 0 || options->worker_count > THRIFT_TLS_WORKER_LIMIT
        || (options->kind != THRIFT_BINARY && options->kind != THRIFT_COMPACT)) return THRIFT_INVALID;
    server = calloc(1, sizeof(*server));
    if (server == NULL) return THRIFT_NOMEM;
    server->options = *options;
    if (options->limits != NULL) server->limits = *options->limits;
    server->options.limits = options->limits == NULL ? NULL : &server->limits;
    server->process = process;
    server->handler = handler;
    /* Counts above have small fixed ceilings, checked before multiplication by calloc. */
    server->connections = calloc(options->max_connections, sizeof(*server->connections));
    server->workers = calloc(options->worker_count, sizeof(*server->workers));
    server->busy = calloc(options->worker_count, sizeof(*server->busy));
    server->polls = calloc(options->max_connections + 1, sizeof(*server->polls));
    if (server->connections == NULL || server->workers == NULL || server->busy == NULL || server->polls == NULL) {
        status = THRIFT_NOMEM;
        goto fail;
    }
    status = thrift_tls_listen(options->context, options->address, options->port, &server->listener);
    if (status != THRIFT_OK) goto fail;
    for (index = 0; index < options->worker_count; ++index) {
        status = thrift_tls_worker_create(run_request, &server->workers[index]);
        if (status != THRIFT_OK) goto fail;
    }
    *out = server;
    return THRIFT_OK;
fail:
    thrift_tls_server_destroy(server);
    return status;
}
enum thrift_status thrift_tls_server_port(const struct thrift_tls_server *server, uint16_t *port)
{
    return server == NULL ? THRIFT_INVALID : thrift_tls_port(server->listener, port);
}
void thrift_tls_server_destroy(struct thrift_tls_server *server)
{
    size_t index;
    if (server == NULL) return;
    thrift_tls_socket_destroy(server->listener);
    if (server->connections != NULL)
        for (index = 0; index < server->options.max_connections; ++index)
            release_connection(&server->connections[index]);
    if (server->workers != NULL)
        for (index = 0; index < server->options.worker_count; ++index)
            thrift_tls_worker_destroy(server->workers[index]);
    if (server->connections != NULL)
        for (index = 0; index < server->options.max_connections; ++index) {
            server->connections[index].phase = FRAME_HEADER;
            release_connection(&server->connections[index]);
        }
    free(server->connections);
    free(server->workers);
    free(server->busy);
    free(server->polls);
    free(server);
}

enum thrift_status thrift_tls_server_create(const struct thrift_tls_server_options *options,
    enum thrift_status (*process)(struct thrift_protocol *, void *), void *handler,
    struct thrift_tls_server **out)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_INFO, "tls.server.create", options ? options->max_connections : 0);
    enum thrift_status status = create_server(options, process, handler, out);
    thrift_log_end(scope, status);
    return status;
}

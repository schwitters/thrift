/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/server/thrift_websocket_server.h>
#include "../transport/thrift_websocket_internal.h"
#include "../thrift_log_internal.h"
#include <civetweb.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { HTTP_BAD_REQUEST = 400, HTTP_NOT_FOUND = 404, HTTP_UPGRADE_REQUIRED = 426,
       HTTP_UNAVAILABLE = 503, HTTP_INTERNAL_ERROR = 500, OPTION_TEXT_SIZE = 16,
       PORT_OPTION_VALUE = 1, WORKER_LIMIT = 64, REJECTION_HEADER_SIZE = 128,
       WEBSOCKET_KEY_LENGTH = 24, WEBSOCKET_KEY_DATA_LENGTH = 22 };
struct thrift_websocket_server {
    struct mg_context *context;
    char *path;
    size_t max_message_size;
    uint32_t timeout_ms;
    enum thrift_protocol_kind kind;
    struct thrift_limits limits;
    bool custom_limits;
    atomic_bool ready;
    enum thrift_status (*process)(struct thrift_protocol *, void *);
    void *handler;
};
struct websocket_session {
    struct thrift_websocket_message message;
    bool fragmented;
};

enum thrift_status thrift_websocket_server_library_init(void)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "civetweb.initialize", 0);
    enum thrift_status status = !mg_check_feature(MG_FEATURES_WEBSOCKET) ? THRIFT_INVALID :
        mg_init_library(MG_FEATURES_WEBSOCKET) ? THRIFT_OK : THRIFT_IO;
    thrift_log_end(scope, status);
    return status;
}
enum thrift_status thrift_websocket_server_library_cleanup(void)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "civetweb.cleanup", 0);
    enum thrift_status status = mg_exit_library() ? THRIFT_OK : THRIFT_IO;
    thrift_log_end(scope, status);
    return status;
}
static int reject_request(struct mg_connection *connection, int code)
{
    char header[REJECTION_HEADER_SIZE];
    int size;
    /* CivetWeb 1.16's generic response-header API rejects connections already
     * classified as WebSocket, even before the upgrade. Send a plain HTTP
     * rejection here while the handshake has not yet been accepted. */
    mg_disable_connection_keep_alive(connection);
    size = snprintf(header, sizeof(header),
        "HTTP/1.1 %d Rejected\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", code);
    if (size <= 0 || (size_t)size >= sizeof(header) ||
        mg_write(connection, header, (size_t)size) != size) return HTTP_INTERNAL_ERROR;
    return code;
}
static bool equals_ascii(const char *left, const char *right)
{
    if (!left) return false;
    while (*left && *right) {
        char character = *left++;
        if (character >= 'A' && character <= 'Z') character = (char)(character + ('a' - 'A'));
        if (character != *right++) return false;
    }
    return *left == *right;
}
static bool connection_upgrade(const struct mg_request_info *info)
{
    static const char upgrade[] = "upgrade";
    int header;
    for (header = 0; header < info->num_headers; ++header) {
        const char *value = info->http_headers[header].value;
        if (!equals_ascii(info->http_headers[header].name, "connection") || !value) continue;
        while (*value) {
            size_t index = 0;
            bool match = true;
            while (*value == ' ' || *value == '\t' || *value == ',') ++value;
            while (*value && *value != ',' && *value != ' ' && *value != '\t') {
                char character = *value++;
                if (character >= 'A' && character <= 'Z') character = (char)(character + ('a' - 'A'));
                if (index >= sizeof(upgrade) - 1 || character != upgrade[index]) match = false;
                ++index;
            }
            if (match && index == sizeof(upgrade) - 1) return true;
        }
    }
    return false;
}
static bool valid_key(const char *key)
{
    size_t index;
    if (!key || strlen(key) != WEBSOCKET_KEY_LENGTH ||
        strcmp(key + WEBSOCKET_KEY_DATA_LENGTH, "==")) return false;
    for (index = 0; index < WEBSOCKET_KEY_DATA_LENGTH; ++index) {
        char c = key[index];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/')) return false;
    }
    /* A canonical base64 encoding of a 16-byte nonce has four zero padding bits. */
    return strchr("AQgw", key[WEBSOCKET_KEY_DATA_LENGTH - 1]) != NULL;
}
static int begin_request(struct mg_connection *connection)
{
    const struct mg_request_info *info = mg_get_request_info(connection);
    struct thrift_websocket_server *server;
    if (!info || !info->user_data) return reject_request(connection, HTTP_INTERNAL_ERROR);
    server = info->user_data;
    if (!atomic_load_explicit(&server->ready, memory_order_acquire))
        return reject_request(connection, HTTP_UNAVAILABLE);
    if (!info->local_uri || strcmp(info->local_uri, server->path))
        return reject_request(connection, HTTP_NOT_FOUND);
    if (!info->request_method || strcmp(info->request_method, "GET") ||
        !equals_ascii(mg_get_header(connection, "Upgrade"), "websocket") || !connection_upgrade(info) ||
        !equals_ascii(mg_get_header(connection, "Sec-WebSocket-Version"), "13"))
        return reject_request(connection, HTTP_UPGRADE_REQUIRED);
    if (!valid_key(mg_get_header(connection, "Sec-WebSocket-Key")))
        return reject_request(connection, HTTP_BAD_REQUEST);
    /* No extensions are needed for Thrift; avoid negotiating compression in
     * CivetWeb builds with experimental extension support. */
    if (mg_get_header(connection, "Sec-WebSocket-Extensions"))
        return reject_request(connection, HTTP_BAD_REQUEST);
    return 0;
}
static int quiet_log(const struct mg_connection *connection, const char *message)
{
    /* CivetWeb diagnostics may include URI/header content. Log only our statuses. */
    (void)connection; (void)message;
    return 1;
}
static int accept_connection(const struct mg_connection *connection, void *context)
{
    struct thrift_websocket_server *server = context;
    /* CMake requires the bounded-reader extension. Configure before CivetWeb
     * reads a frame header; application callbacks run too late for these checks. */
    return mg_set_websocket_limits(connection, server->max_message_size, server->timeout_ms) ? 0 : 1;
}
static void ready_connection(struct mg_connection *connection, void *context)
{
    struct thrift_websocket_server *server = context;
    struct websocket_session *session = calloc(1, sizeof(*session));
    if (session) session->message.request.limit = session->message.response.limit = server->max_message_size;
    mg_set_user_connection_data(connection, session);
}
static void closed_connection(const struct mg_connection *connection, void *context)
{
    struct websocket_session *session = mg_get_user_connection_data(connection);
    (void)context;
    if (!session) return;
    thrift_buffer_clear(&session->message.request);
    thrift_buffer_clear(&session->message.response);
    free(session);
    mg_set_user_connection_data(connection, NULL);
}
static enum thrift_status write_frame(struct mg_connection *connection, int opcode,
    const void *data, size_t size)
{
    int count = mg_websocket_write(connection, opcode, data, size);
    return count > 0 && (!size || (size_t)count == size) ? THRIFT_OK : THRIFT_IO;
}
static void close_with_status(struct mg_connection *connection, unsigned code)
{
    uint8_t payload[THRIFT_WS_CLOSE_CODE_SIZE] = {(uint8_t)(code >> 8), (uint8_t)code};
    enum thrift_status status = write_frame(connection, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, payload, sizeof(payload));
    if (status != THRIFT_OK) thrift_log_native("websocket.close", status);
}
static enum thrift_status process_message(struct mg_connection *connection,
    struct thrift_websocket_server *server, struct websocket_session *session)
{
    struct thrift_websocket_message *message = &session->message;
    struct thrift_transport transport = thrift_websocket_message_transport(message);
    struct thrift_protocol protocol;
    enum thrift_status status = thrift_protocol_init_kind(&protocol, transport, server->kind);
    if (status == THRIFT_OK) {
        if (server->custom_limits) {
            protocol.limits = server->limits;
            thrift_protocol_reset(&protocol);
        }
        status = server->process(&protocol, server->handler);
    }
    status = thrift_websocket_message_finish(message, status);
    if (status == THRIFT_OK && message->response.size)
        status = write_frame(connection, MG_WEBSOCKET_OPCODE_BINARY, message->response.data, message->response.size);
    message->request.size = message->request.position = message->response.size = 0;
    return status;
}
static enum thrift_status handle_frame(struct mg_connection *connection, int bits,
    char *data, size_t size, struct thrift_websocket_server *server, bool *close)
{
    struct websocket_session *session = mg_get_user_connection_data(connection);
    int opcode = bits & THRIFT_WS_OPCODE;
    bool final = (bits & THRIFT_WS_FINAL) != 0;
    enum thrift_status status;
    if (!session) return THRIFT_NOMEM;
    if (bits & THRIFT_WS_RESERVED) return THRIFT_PROTOCOL;
    if (opcode >= MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) {
        if (!final || size > THRIFT_WS_CONTROL_LIMIT) return THRIFT_PROTOCOL;
        if (opcode == MG_WEBSOCKET_OPCODE_PING)
            return write_frame(connection, MG_WEBSOCKET_OPCODE_PONG, data, size);
        if (opcode == MG_WEBSOCKET_OPCODE_PONG) return THRIFT_OK;
        if (opcode != MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) return THRIFT_PROTOCOL;
        status = thrift_websocket_validate_close((const uint8_t *)data, size);
        if (status != THRIFT_OK) return status;
        *close = true;
        return write_frame(connection, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, data, size);
    }
    if (opcode == MG_WEBSOCKET_OPCODE_BINARY) {
        if (session->fragmented) return THRIFT_PROTOCOL;
        session->fragmented = !final;
    } else if (opcode == MG_WEBSOCKET_OPCODE_CONTINUATION) {
        if (!session->fragmented) return THRIFT_PROTOCOL;
        session->fragmented = !final;
    } else {
        return THRIFT_PROTOCOL;
    }
    status = thrift_buffer_append(&session->message.request, data, size);
    if (status != THRIFT_OK || !final) return status;
    if (!session->message.request.size) return THRIFT_OK;
    return process_message(connection, server, session);
}
static int data_received(struct mg_connection *connection, int bits, char *data, size_t size, void *context)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "websocket.server.frame", size);
    bool close = false;
    enum thrift_status status = handle_frame(connection, bits, data, size, context, &close);
    if (status != THRIFT_OK) {
        close_with_status(connection, status == THRIFT_LIMIT ? THRIFT_WS_TOO_LARGE_CLOSE :
            status == THRIFT_NOMEM || status == THRIFT_IO ? THRIFT_WS_INTERNAL_CLOSE :
            (bits & THRIFT_WS_OPCODE) == MG_WEBSOCKET_OPCODE_TEXT ? THRIFT_WS_UNSUPPORTED_CLOSE : THRIFT_WS_PROTOCOL_CLOSE);
        close = true;
    }
    thrift_log_end(scope, status);
    return close ? 0 : 1;
}
static enum thrift_status start_server(const struct thrift_websocket_server_options *options,
    enum thrift_status (*process)(struct thrift_protocol *, void *), void *handler,
    struct thrift_websocket_server **result)
{
    struct thrift_websocket_server *server;
    struct mg_callbacks callbacks = {0};
    char timeout[OPTION_TEXT_SIZE], workers[OPTION_TEXT_SIZE];
    const char *settings[] = {"listening_ports", NULL, "num_threads", workers,
        "request_timeout_ms", timeout, "websocket_timeout_ms", timeout,
        "enable_websocket_ping_pong", "no", NULL};
    size_t path_size;
    int count;
    if (!result) return THRIFT_INVALID;
    *result = NULL;
    if (!options || !process || !options->path || options->path[0] != '/' ||
        !options->listening_port || !options->listening_port[0] ||
        strpbrk(options->listening_port, "sSrR,") || !options->max_message_size ||
        options->max_message_size > INT_MAX || !options->timeout_ms || options->timeout_ms > INT_MAX ||
        !options->worker_count || options->worker_count > WORKER_LIMIT ||
        (options->kind != THRIFT_BINARY && options->kind != THRIFT_COMPACT) ||
        !mg_check_feature(MG_FEATURES_WEBSOCKET)) return THRIFT_INVALID;
    path_size = strlen(options->path);
    if (path_size == SIZE_MAX) return THRIFT_LIMIT;
    server = calloc(1, sizeof(*server));
    if (!server) return THRIFT_NOMEM;
    atomic_init(&server->ready, false);
    server->path = malloc(path_size + 1);
    if (!server->path) { free(server); return THRIFT_NOMEM; }
    memcpy(server->path, options->path, path_size + 1);
    server->max_message_size = options->max_message_size;
    server->timeout_ms = options->timeout_ms;
    server->kind = options->kind;
    server->process = process;
    server->handler = handler;
    if (options->limits) { server->limits = *options->limits; server->custom_limits = true; }
    count = snprintf(timeout, sizeof(timeout), "%lu", (unsigned long)options->timeout_ms);
    if (count <= 0 || (size_t)count >= sizeof(timeout)) goto invalid;
    count = snprintf(workers, sizeof(workers), "%lu", (unsigned long)options->worker_count);
    if (count <= 0 || (size_t)count >= sizeof(workers)) goto invalid;
    callbacks.begin_request = begin_request;
    callbacks.log_message = quiet_log;
    settings[PORT_OPTION_VALUE] = options->listening_port;
    server->context = mg_start(&callbacks, server, settings);
    if (!server->context) { thrift_websocket_server_stop(server); return THRIFT_IO; }
    mg_set_websocket_handler(server->context, server->path, accept_connection, ready_connection,
        data_received, closed_connection, server);
    atomic_store_explicit(&server->ready, true, memory_order_release);
    *result = server;
    return THRIFT_OK;
invalid:
    thrift_websocket_server_stop(server);
    return THRIFT_INVALID;
}
enum thrift_status thrift_websocket_server_start(const struct thrift_websocket_server_options *options,
    enum thrift_status (*process)(struct thrift_protocol *, void *), void *handler,
    struct thrift_websocket_server **result)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_INFO, "websocket.server.start", 0);
    enum thrift_status status = start_server(options, process, handler, result);
    thrift_log_end(scope, status);
    return status;
}
enum thrift_status thrift_websocket_server_port(const struct thrift_websocket_server *server, uint16_t *port)
{
    struct mg_server_port entry;
    if (!server || !server->context || !port) return THRIFT_INVALID;
    if (mg_get_server_ports(server->context, 1, &entry) != 1 || entry.port <= 0 || entry.port > UINT16_MAX)
        return THRIFT_IO;
    *port = (uint16_t)entry.port;
    return THRIFT_OK;
}
void thrift_websocket_server_stop(struct thrift_websocket_server *server)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_INFO, "websocket.server.stop", 0);
    if (server) {
        if (server->context) mg_stop(server->context);
        free(server->path);
        free(server);
    }
    thrift_log_end(scope, THRIFT_OK);
}

/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/server/thrift_http_server.h>
#include "../thrift_log_internal.h"
#include <thrift/c11/transport/thrift_http_server_transport.h>
#include <civetweb.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HTTP_OK = 200, HTTP_BAD_REQUEST = 400, HTTP_NOT_FOUND = 404,
       HTTP_METHOD_NOT_ALLOWED = 405, HTTP_PAYLOAD_TOO_LARGE = 413,
       HTTP_INTERNAL_ERROR = 500, TIMEOUT_TEXT_SIZE = 16,
       LISTENING_PORT_OPTION_VALUE = 1, HTTP_WORKER_COUNT = 1, WORKER_TEXT_SIZE = 12 };
struct thrift_http_server {
    struct mg_context *context;
    char *path;
    size_t max_body_size;
    enum thrift_protocol_kind kind;
    struct thrift_limits limits;
    bool custom_limits;
    enum thrift_status (*process)(struct thrift_protocol *, void *);
    void *handler;
};

static enum thrift_status thrift_http_server_library_init_impl(void)
{
    /* A zero feature mask also returns zero on success in CivetWeb. Request
     * compiled-in non-TLS features so initialization failures stay detectable. */
    unsigned features = mg_check_feature(MG_FEATURES_ALL) & ~(unsigned)MG_FEATURES_TLS;
    if (!features)
        return THRIFT_INVALID;
    return mg_init_library(features) ? THRIFT_OK : THRIFT_IO;
}

enum thrift_status thrift_http_server_library_init(void)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "civetweb.initialize", 0);
    enum thrift_status status = thrift_http_server_library_init_impl();
    thrift_log_end(scope, status);
    return status;
}

static enum thrift_status thrift_http_server_library_cleanup_impl(void)
{
    return mg_exit_library() ? THRIFT_OK : THRIFT_IO;
}

enum thrift_status thrift_http_server_library_cleanup(void)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "civetweb.cleanup", 0);
    enum thrift_status status = thrift_http_server_library_cleanup_impl();
    thrift_log_end(scope, status);
    return status;
}

static int send_error(struct mg_connection *connection, int code)
{
    if (mg_send_http_error(connection, code, "%s", "Thrift HTTP request rejected") < 0)
        return HTTP_INTERNAL_ERROR;
    return code;
}

/* begin_request intercepts every URI, so unmatched paths never reach CivetWeb's
 * filesystem or built-in request handlers. The context is ready before start. */
static int handle_request_impl(struct mg_connection *connection)
{
    const struct mg_request_info *info = mg_get_request_info(connection);
    struct thrift_http_server *server;
    struct thrift_http_server_transport *http = NULL;
    struct thrift_transport transport = {0};
    struct thrift_protocol protocol;
    enum thrift_status status;
    if (!info || !info->user_data)
        return send_error(connection, HTTP_INTERNAL_ERROR);
    server = info->user_data;
    if (!info->local_uri || strcmp(info->local_uri, server->path))
        return send_error(connection, HTTP_NOT_FOUND);
    if (!info->request_method || strcmp(info->request_method, "POST"))
        return send_error(connection, HTTP_METHOD_NOT_ALLOWED);
    status = thrift_http_server_transport_create(connection, server->max_body_size,
                                                     &http, &transport);
    if (status != THRIFT_OK)
        return send_error(connection, status == THRIFT_LIMIT ? HTTP_PAYLOAD_TOO_LARGE :
                          status == THRIFT_NOMEM ? HTTP_INTERNAL_ERROR : HTTP_BAD_REQUEST);
    status = thrift_protocol_init_kind(&protocol, transport, server->kind);
    if (status == THRIFT_OK) {
        if (server->custom_limits) {
            protocol.limits = server->limits;
            thrift_protocol_reset(&protocol);
        }
        status = server->process(&protocol, server->handler);
    }
    status = thrift_http_server_transport_finish(http, status);
    thrift_http_server_transport_destroy(http);
    if (status == THRIFT_OK)
        return HTTP_OK;
    return status == THRIFT_LIMIT ? HTTP_PAYLOAD_TOO_LARGE :
        status == THRIFT_NOMEM || status == THRIFT_IO ? HTTP_INTERNAL_ERROR : HTTP_BAD_REQUEST;
}

static int handle_request(struct mg_connection *connection)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "http.server.request", 0);
    int code = handle_request_impl(connection);
    if (code != HTTP_OK)
        thrift_log_native("http", code);
    thrift_log_end(scope, code == HTTP_OK ? THRIFT_OK : THRIFT_REMOTE);
    return code;
}

static enum thrift_status thrift_http_server_start_impl(
    const struct thrift_http_server_options *options,
    enum thrift_status (*process)(struct thrift_protocol *, void *),
    void *handler, struct thrift_http_server **result)
{
    struct thrift_http_server *server;
    struct mg_callbacks callbacks = {0};
    char timeout[TIMEOUT_TEXT_SIZE];
    char workers[WORKER_TEXT_SIZE];
    const char *settings[] = {"listening_ports", NULL, "num_threads", workers,
        "request_timeout_ms", timeout, "enable_keep_alive", "yes", NULL};
    size_t path_size;
    int count;
    if (!result)
        return THRIFT_INVALID;
    *result = NULL;
    if (!options || !process || !options->listening_port || !options->listening_port[0] ||
        !options->path || options->path[0] != '/' || !options->max_body_size ||
        options->max_body_size > LLONG_MAX || !options->timeout_ms || options->timeout_ms > INT_MAX ||
        (options->kind != THRIFT_BINARY && options->kind != THRIFT_COMPACT) ||
        strpbrk(options->listening_port, "sSrR,"))
        return THRIFT_INVALID;
    path_size = strlen(options->path);
    if (path_size == SIZE_MAX)
        return THRIFT_LIMIT;
    server = calloc(1, sizeof(*server));
    if (!server)
        return THRIFT_NOMEM;
    server->path = malloc(path_size + 1);
    if (!server->path) {
        free(server);
        return THRIFT_NOMEM;
    }
    memcpy(server->path, options->path, path_size + 1);
    server->max_body_size = options->max_body_size;
    server->kind = options->kind;
    server->process = process;
    server->handler = handler;
    if (options->limits) {
        server->limits = *options->limits;
        server->custom_limits = true;
    }
    count = snprintf(timeout, sizeof(timeout), "%lu", (unsigned long)options->timeout_ms);
    if (count < 0 || (size_t)count >= sizeof(timeout)) {
        thrift_http_server_stop(server);
        return THRIFT_INVALID;
    }
    count = snprintf(workers, sizeof(workers), "%u", (unsigned)HTTP_WORKER_COUNT);
    if (count < 0 || (size_t)count >= sizeof(workers)) {
        thrift_http_server_stop(server);
        return THRIFT_INVALID;
    }
    callbacks.begin_request = handle_request;
    settings[LISTENING_PORT_OPTION_VALUE] = options->listening_port;
    server->context = mg_start(&callbacks, server, settings);
    if (!server->context) {
        thrift_http_server_stop(server);
        return THRIFT_IO;
    }
    *result = server;
    return THRIFT_OK;
}

enum thrift_status thrift_http_server_start(
    const struct thrift_http_server_options *options,
    enum thrift_status (*process)(struct thrift_protocol *, void *),
    void *handler, struct thrift_http_server **result)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_INFO, "http.server.start", options ? options->max_body_size : 0);
    enum thrift_status status = thrift_http_server_start_impl(options, process, handler, result);
    thrift_log_end(scope, status);
    return status;
}

enum thrift_status thrift_http_server_port(const struct thrift_http_server *server,
                                                  uint16_t *port)
{
    struct mg_server_port entry;
    if (!server || !server->context || !port)
        return THRIFT_INVALID;
    if (mg_get_server_ports(server->context, 1, &entry) != 1 || entry.port <= 0 || entry.port > UINT16_MAX)
        return THRIFT_IO;
    *port = (uint16_t)entry.port;
    return THRIFT_OK;
}

static void thrift_http_server_stop_impl(struct thrift_http_server *server)
{
    if (!server)
        return;
    if (server->context)
        mg_stop(server->context);
    free(server->path);
    free(server);
}

void thrift_http_server_stop(struct thrift_http_server *server)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_INFO, "http.server.stop", 0);
    thrift_http_server_stop_impl(server);
    thrift_log_end(scope, THRIFT_OK);
}

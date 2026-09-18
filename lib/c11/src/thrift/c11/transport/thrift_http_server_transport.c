/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/transport/thrift_http_server_transport.h>
#include "thrift_buffer_internal.h"
#include <civetweb.h>
#include <limits.h>
#include <stdlib.h>

enum { HTTP_BAD_REQUEST = 400, HTTP_PAYLOAD_TOO_LARGE = 413,
       HTTP_INTERNAL_ERROR = 500, HTTP_COPY_SIZE = 8192 };
struct thrift_http_server_transport {
    struct mg_connection *connection;
    struct thrift_buffer request, response;
    enum thrift_status error;
    bool finished;
};

static enum thrift_status server_read(void *context, void *data, size_t size)
{
    struct thrift_http_server_transport *transport = context;
    if (!transport || transport->finished)
        return THRIFT_INVALID;
    if (transport->error == THRIFT_OK)
        transport->error = thrift_buffer_read(&transport->request, data, size);
    return transport->error;
}

static enum thrift_status server_write(void *context, const void *data, size_t size)
{
    struct thrift_http_server_transport *transport = context;
    if (!transport || transport->finished)
        return THRIFT_INVALID;
    if (transport->error == THRIFT_OK)
        transport->error = thrift_buffer_append(&transport->response, data, size);
    return transport->error;
}

static enum thrift_status server_flush(void *context)
{
    struct thrift_http_server_transport *transport = context;
    if (!transport || transport->finished)
        return THRIFT_INVALID;
    return transport->error;
}

static enum thrift_status receive_request(struct thrift_http_server_transport *transport)
{
    const struct mg_request_info *info = mg_get_request_info(transport->connection);
    uint8_t bytes[HTTP_COPY_SIZE];
    enum thrift_status status;
    int count;
    if (!info)
        return THRIFT_INVALID;
    if (info->content_length >= 0 && (unsigned long long)info->content_length > transport->request.limit)
        return THRIFT_LIMIT;
    for (;;) {
        count = mg_read(transport->connection, bytes, sizeof(bytes));
        if (count < 0)
            return THRIFT_IO;
        if (!count)
            break;
        status = thrift_buffer_append(&transport->request, bytes, (size_t)count);
        if (status != THRIFT_OK)
            return status;
    }
    if (info->content_length >= 0 && (unsigned long long)info->content_length != transport->request.size)
        return THRIFT_EOF;
    return THRIFT_OK;
}

enum thrift_status thrift_http_server_transport_create(struct mg_connection *conn,
    size_t max_body_size, struct thrift_http_server_transport **result,
    struct thrift_transport *transport)
{
    struct thrift_http_server_transport *http;
    enum thrift_status status;
    if (!result || !transport)
        return THRIFT_INVALID;
    *result = NULL;
    if (!conn || !max_body_size || max_body_size > LLONG_MAX)
        return THRIFT_INVALID;
    http = calloc(1, sizeof(*http));
    if (!http)
        return THRIFT_NOMEM;
    http->connection = conn;
    http->request.limit = http->response.limit = max_body_size;
    status = receive_request(http);
    if (status != THRIFT_OK) {
        thrift_http_server_transport_destroy(http);
        return status;
    }
    transport->context = http;
    transport->read = server_read;
    transport->write = server_write;
    transport->flush = server_flush;
    *result = http;
    return THRIFT_OK;
}

enum thrift_status thrift_http_server_transport_finish(
    struct thrift_http_server_transport *transport, enum thrift_status process_status)
{
    size_t position = 0;
    if (!transport || transport->finished)
        return THRIFT_INVALID;
    transport->finished = true;
    if (process_status == THRIFT_OK)
        process_status = transport->error;
    if (process_status == THRIFT_OK && transport->request.position != transport->request.size)
        process_status = THRIFT_PROTOCOL;
    if (process_status != THRIFT_OK) {
        int code = process_status == THRIFT_LIMIT ? HTTP_PAYLOAD_TOO_LARGE :
            process_status == THRIFT_NOMEM ? HTTP_INTERNAL_ERROR : HTTP_BAD_REQUEST;
        if (mg_send_http_error(transport->connection, code, "%s", "Thrift request failed") < 0)
            return THRIFT_IO;
        return process_status;
    }
    if (mg_send_http_ok(transport->connection, "application/x-thrift",
                        (long long)transport->response.size) < 0)
        return THRIFT_IO;
    while (position < transport->response.size) {
        size_t size = transport->response.size - position;
        int count;
        if (size > INT_MAX)
            size = INT_MAX;
        count = mg_write(transport->connection, transport->response.data + position, size);
        if (count <= 0 || (size_t)count > size)
            return THRIFT_IO;
        position += (size_t)count;
    }
    return THRIFT_OK;
}

void thrift_http_server_transport_destroy(struct thrift_http_server_transport *transport)
{
    if (!transport)
        return;
    thrift_buffer_clear(&transport->request);
    thrift_buffer_clear(&transport->response);
    free(transport);
}

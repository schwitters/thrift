/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/transport/thrift_websocket_client.h>
#include "thrift_websocket_internal.h"
#include "thrift_curl_internal.h"
#include "../thrift_log_internal.h"
#include <curl/curl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { WS_UPGRADE_STATUS = 101, WS_CONNECT_ONLY = 2 };
struct thrift_websocket_client {
    CURL *curl;
    CURLM *waiter;
    struct thrift_buffer request, response;
    uint32_t timeout_ms;
    enum thrift_status error;
    bool response_loaded, peer_closed, closing;
};

static enum thrift_status curl_status(CURLcode code)
{
    if (code == CURLE_OK) return THRIFT_OK;
    thrift_log_native("curl", code);
    if (code == CURLE_OPERATION_TIMEDOUT) return THRIFT_TIMEOUT;
    if (code == CURLE_OUT_OF_MEMORY) return THRIFT_NOMEM;
    if (code == CURLE_GOT_NOTHING) return THRIFT_EOF;
    return THRIFT_IO;
}
enum thrift_status thrift_websocket_client_library_init(void)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "curl.initialize", 0);
    enum thrift_status status = curl_status(curl_global_init(CURL_GLOBAL_DEFAULT));
    thrift_log_end(scope, status);
    return status;
}
void thrift_websocket_client_library_cleanup(void)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "curl.cleanup", 0);
    curl_global_cleanup();
    thrift_log_end(scope, THRIFT_OK);
}
static enum thrift_status deadline_start(struct thrift_websocket_client *client, uint64_t *deadline)
{
    enum thrift_status status = thrift_websocket_now(deadline);
    if (status == THRIFT_OK) *deadline += client->timeout_ms;
    return status;
}
static enum thrift_status remaining_time(uint64_t deadline, int *remaining)
{
    uint64_t now;
    enum thrift_status status = thrift_websocket_now(&now);
    if (status != THRIFT_OK) return status;
    if (now >= deadline) return THRIFT_TIMEOUT;
    *remaining = (int)(deadline - now);
    return THRIFT_OK;
}
static enum thrift_status wait_socket(struct thrift_websocket_client *client, bool writing,
    uint64_t deadline)
{
    struct curl_waitfd descriptor;
    int remaining, ready;
    CURLcode code;
    CURLMcode multi_code;
    enum thrift_status status = remaining_time(deadline, &remaining);
    if (status != THRIFT_OK) return status;
    code = curl_easy_getinfo(client->curl, CURLINFO_ACTIVESOCKET, &descriptor.fd);
    if (code != CURLE_OK) return curl_status(code);
    descriptor.events = writing ? CURL_WAIT_POLLOUT : CURL_WAIT_POLLIN;
    descriptor.revents = 0;
    multi_code = curl_multi_poll(client->waiter, &descriptor, 1, remaining, &ready);
    if (multi_code != CURLM_OK) {
        thrift_log_native("curl.multi", multi_code);
        return THRIFT_IO;
    }
    return remaining_time(deadline, &remaining);
}
static enum thrift_status send_frame(struct thrift_websocket_client *client,
    const uint8_t *data, size_t size, unsigned flags, uint64_t deadline)
{
    size_t position = 0;
    do {
        size_t sent = 0;
        int remaining;
        enum thrift_status status = remaining_time(deadline, &remaining);
        CURLcode code;
        if (status != THRIFT_OK) return status;
        code = curl_ws_send(client->curl, size ? data + position : (const uint8_t *)"",
            size - position, &sent, 0, flags);
        if (sent > size - position) return THRIFT_IO;
        position += sent;
        if (code != CURLE_OK && code != CURLE_AGAIN) return curl_status(code);
        if (code == CURLE_AGAIN || (position < size && !sent)) {
            status = wait_socket(client, true, deadline);
            if (status != THRIFT_OK) return status;
        } else if (position == size) {
            return THRIFT_OK;
        }
    } while (true);
}
static enum thrift_status receive_message(struct thrift_websocket_client *client, uint64_t deadline)
{
    uint8_t bytes[THRIFT_WS_COPY_SIZE], control[THRIFT_WS_CONTROL_LIMIT];
    size_t control_size = 0;
    client->response.size = client->response.position = 0;
    for (;;) {
        const struct curl_ws_frame *meta;
        size_t count = 0;
        int remaining, flags;
        CURLcode code;
        enum thrift_status status = remaining_time(deadline, &remaining);
        if (status != THRIFT_OK) return status;
        code = curl_ws_recv(client->curl, bytes, sizeof(bytes), &count, &meta);
        if (code == CURLE_AGAIN) {
            status = wait_socket(client, false, deadline);
            if (status != THRIFT_OK) return status;
            continue;
        }
        if (code != CURLE_OK) return curl_status(code);
        if (!meta || meta->offset < 0 || meta->bytesleft < 0) return THRIFT_PROTOCOL;
        flags = meta->flags;
        if (flags & (CURLWS_CLOSE | CURLWS_PING | CURLWS_PONG)) {
            if (flags & CURLWS_CONT) return THRIFT_PROTOCOL;
            if (!meta->offset) control_size = 0;
            if ((uint64_t)meta->offset != control_size || count > sizeof(control) - control_size ||
                (uint64_t)meta->bytesleft > sizeof(control) - control_size - count)
                return THRIFT_PROTOCOL;
            memcpy(control + control_size, bytes, count);
            control_size += count;
            if (meta->bytesleft) continue;
            if (flags & CURLWS_PING) {
                status = send_frame(client, control, control_size, CURLWS_PONG, deadline);
                if (status != THRIFT_OK) return status;
            } else if (flags & CURLWS_CLOSE) {
                status = thrift_websocket_validate_close(control, control_size);
                if (status != THRIFT_OK) return status;
                client->peer_closed = true;
                if (!client->closing) {
                    status = send_frame(client, control, control_size, CURLWS_CLOSE, deadline);
                    if (status != THRIFT_OK) return status;
                }
                return THRIFT_EOF;
            }
            continue;
        }
        if (!(flags & CURLWS_BINARY) || (flags & CURLWS_TEXT)) return THRIFT_PROTOCOL;
        if (count > client->response.limit - client->response.size ||
            (uint64_t)meta->bytesleft > client->response.limit - client->response.size - count)
            return THRIFT_LIMIT;
        status = thrift_buffer_append(&client->response, bytes, count);
        if (status != THRIFT_OK) return status;
        if (!meta->bytesleft && !(flags & CURLWS_CONT) && client->response.size)
            return THRIFT_OK;
    }
}
static enum thrift_status client_read(void *context, void *data, size_t size)
{
    struct thrift_websocket_client *client = context;
    enum thrift_status status;
    uint64_t deadline;
    if (!client || (!data && size)) return THRIFT_INVALID;
    if (client->error != THRIFT_OK) return client->error;
    if (client->closing || client->peer_closed) return THRIFT_EOF;
    if (!size) return THRIFT_OK;
    if (!client->response_loaded) {
        struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "websocket.client.receive", 0);
        status = deadline_start(client, &deadline);
        if (status == THRIFT_OK) status = receive_message(client, deadline);
        thrift_log_end(scope, status);
        if (status != THRIFT_OK) return client->error = status;
        client->response_loaded = true;
    }
    return client->error = thrift_buffer_read(&client->response, data, size);
}
static enum thrift_status client_write(void *context, const void *data, size_t size)
{
    struct thrift_websocket_client *client = context;
    if (!client) return THRIFT_INVALID;
    if (client->closing || client->peer_closed) return THRIFT_EOF;
    if (client->error == THRIFT_OK)
        client->error = thrift_buffer_append(&client->request, data, size);
    return client->error;
}
static enum thrift_status flush_impl(struct thrift_websocket_client *client)
{
    uint64_t deadline;
    enum thrift_status status;
    if (!client) return THRIFT_INVALID;
    if (client->error != THRIFT_OK) return client->error;
    if (client->closing || client->peer_closed) return THRIFT_EOF;
    if (client->response_loaded && client->response.position != client->response.size)
        return client->error = THRIFT_PROTOCOL;
    if (!client->request.size) return THRIFT_OK;
    status = deadline_start(client, &deadline);
    if (status == THRIFT_OK)
        status = send_frame(client, client->request.data, client->request.size, CURLWS_BINARY, deadline);
    if (status != THRIFT_OK) return client->error = status;
    client->request.size = 0;
    client->response_loaded = false;
    return THRIFT_OK;
}
static enum thrift_status client_flush(void *context)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "websocket.client.send", 0);
    enum thrift_status status = flush_impl(context);
    thrift_log_end(scope, status);
    return status;
}
static size_t discard_upgrade_body(char *data, size_t size, size_t count, void *context)
{
    (void)data; (void)context;
    return size && count > SIZE_MAX / size ? 0 : size * count;
}
static enum thrift_status connect_client(struct thrift_websocket_client *client,
    const struct thrift_websocket_client_options *options)
{
    CURLcode code;
    long http_status = 0;
    if ((code = curl_easy_setopt(client->curl, CURLOPT_URL, options->url)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_PROTOCOLS_STR, "ws,wss")) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_CONNECT_ONLY, (long)WS_CONNECT_ONLY)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_WS_OPTIONS, (long)CURLWS_NOAUTOPONG)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_NOSIGNAL, 1L)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_TIMEOUT_MS, (long)options->timeout_ms)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_CONNECTTIMEOUT_MS, (long)options->timeout_ms)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, discard_upgrade_body)) != CURLE_OK)
        return curl_status(code);
    if (options->ca_file && (code = curl_easy_setopt(client->curl, CURLOPT_CAINFO, options->ca_file)) != CURLE_OK)
        return curl_status(code);
    if (options->bearer_token &&
        ((code = curl_easy_setopt(client->curl, CURLOPT_XOAUTH2_BEARER, options->bearer_token)) != CURLE_OK ||
         (code = curl_easy_setopt(client->curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BEARER)) != CURLE_OK))
        return curl_status(code);
    code = curl_easy_perform(client->curl);
    if (curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE, &http_status) != CURLE_OK)
        return THRIFT_IO;
    if (http_status && http_status != WS_UPGRADE_STATUS) {
        thrift_log_native("http", http_status);
        return THRIFT_REMOTE;
    }
    return curl_status(code);
}
static enum thrift_status create_client(const struct thrift_websocket_client_options *options,
    struct thrift_websocket_client **result, struct thrift_transport *transport)
{
    struct thrift_websocket_client *client;
    enum thrift_status status;
    if (!result || !transport) return THRIFT_INVALID;
    *result = NULL;
    if (!options || !options->url ||
        (strncmp(options->url, "ws://", sizeof("ws://") - 1) &&
         strncmp(options->url, "wss://", sizeof("wss://") - 1)) ||
        !options->timeout_ms || options->timeout_ms > INT_MAX ||
        !options->max_message_size || options->max_message_size > INT_MAX)
        return THRIFT_INVALID;
    status = thrift_curl_validate_bearer_token(options->bearer_token);
    if (status != THRIFT_OK) return status;
    client = calloc(1, sizeof(*client));
    if (!client) return THRIFT_NOMEM;
    client->timeout_ms = options->timeout_ms;
    client->request.limit = client->response.limit = options->max_message_size;
    client->curl = curl_easy_init();
    client->waiter = curl_multi_init();
    status = client->curl && client->waiter ? connect_client(client, options) : THRIFT_NOMEM;
    if (status != THRIFT_OK) {
        thrift_websocket_client_destroy(client);
        return status;
    }
    transport->context = client;
    transport->read = client_read;
    transport->write = client_write;
    transport->flush = client_flush;
    *result = client;
    return THRIFT_OK;
}
enum thrift_status thrift_websocket_client_create(const struct thrift_websocket_client_options *options,
    struct thrift_websocket_client **result, struct thrift_transport *transport)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_INFO, "websocket.client.connect", 0);
    enum thrift_status status = create_client(options, result, transport);
    thrift_log_end(scope, status);
    return status;
}
static enum thrift_status close_client(struct thrift_websocket_client *client)
{
    const uint8_t normal_close[] = {THRIFT_WS_NORMAL_CLOSE >> 8, THRIFT_WS_NORMAL_CLOSE & 0xff};
    uint64_t deadline;
    enum thrift_status status;
    if (!client) return THRIFT_INVALID;
    if (client->peer_closed) return THRIFT_OK;
    if (client->error != THRIFT_OK) return client->error;
    status = deadline_start(client, &deadline);
    if (status == THRIFT_OK)
        status = send_frame(client, normal_close, sizeof(normal_close), CURLWS_CLOSE, deadline);
    client->closing = true;
    while (status == THRIFT_OK) status = receive_message(client, deadline);
    if (status == THRIFT_EOF && client->peer_closed) return THRIFT_OK;
    return client->error = status;
}
enum thrift_status thrift_websocket_client_close(struct thrift_websocket_client *client)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "websocket.client.close", 0);
    enum thrift_status status = close_client(client);
    thrift_log_end(scope, status);
    return status;
}
void thrift_websocket_client_destroy(struct thrift_websocket_client *client)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "websocket.client.destroy", 0);
    enum thrift_status status = THRIFT_OK;
    if (client) {
        CURLMcode code = CURLM_OK;
        curl_easy_cleanup(client->curl);
        if (client->waiter) code = curl_multi_cleanup(client->waiter);
        if (code != CURLM_OK) {
            thrift_log_native("curl.multi", code);
            status = THRIFT_IO;
        }
        thrift_buffer_clear(&client->request);
        thrift_buffer_clear(&client->response);
        free(client);
    }
    thrift_log_end(scope, status);
}

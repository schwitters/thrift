/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_tls_internal.h"
#include <limits.h>
#include <stdlib.h>
struct thrift_tls_client {
    struct thrift_tls_socket *socket;
    struct thrift_buffer request, response;
    uint32_t timeout_ms;
    enum thrift_status error;
    bool loaded;
};
static enum thrift_status wait_io(struct thrift_tls_client *client, uint64_t deadline)
{
    uint64_t now;
    struct thrift_tls_poll_entry entry = {client->socket, thrift_tls_interest(client->socket), 0};
    enum thrift_status status = thrift_tls_now(&now);
    uint32_t wait;
    if (status != THRIFT_OK) return status;
    if (now >= deadline) return THRIFT_TIMEOUT;
    wait = deadline - now > THRIFT_TLS_POLL_QUANTUM_MS ? THRIFT_TLS_POLL_QUANTUM_MS : (uint32_t)(deadline - now);
    return thrift_tls_poll(&entry, 1, wait);
}
static enum thrift_status transfer(struct thrift_tls_client *client, void *data, size_t size, bool read, uint64_t deadline)
{
    size_t position = 0;
    while (position < size) {
        size_t count = 0;
        uint64_t now;
        enum thrift_status status = thrift_tls_now(&now);
        if (status != THRIFT_OK) return status;
        if (now >= deadline) return THRIFT_TIMEOUT;
        status = read ? thrift_tls_read(client->socket, (uint8_t *)data + position, size - position, &count) :
            thrift_tls_write(client->socket, (uint8_t *)data + position, size - position, &count);
        position += count;
        if (status == THRIFT_AGAIN) status = wait_io(client, deadline);
        if (status != THRIFT_OK) return status;
    }
    return THRIFT_OK;
}
static enum thrift_status write_request(void *context, const void *data, size_t size)
{
    struct thrift_tls_client *client = context;
    if (client->error == THRIFT_OK) client->error = thrift_buffer_append(&client->request, data, size);
    return client->error;
}
static enum thrift_status flush_request(void *context)
{
    struct thrift_tls_client *client = context;
    uint8_t header[THRIFT_TLS_FRAME_HEADER];
    uint64_t deadline;
    size_t i;
    enum thrift_status status;
    if (client->error != THRIFT_OK) return client->error;
    if (client->loaded && client->response.position != client->response.size) return client->error = THRIFT_PROTOCOL;
    if (!client->request.size) return THRIFT_OK;
    status = thrift_tls_now(&deadline);
    if (status != THRIFT_OK) return client->error = status;
    deadline += client->timeout_ms;
    do {
        status = thrift_tls_handshake(client->socket);
        if (status == THRIFT_AGAIN) {
            status = wait_io(client, deadline);
            if (status == THRIFT_OK) status = THRIFT_AGAIN;
        }
    } while (status == THRIFT_AGAIN);
    for (i = 0; i < sizeof(header); ++i) header[sizeof(header) - i - 1] = (uint8_t)(client->request.size >> (i * 8));
    if (status == THRIFT_OK) status = transfer(client, header, sizeof(header), false, deadline);
    if (status == THRIFT_OK) status = transfer(client, client->request.data, client->request.size, false, deadline);
    while (status == THRIFT_OK) {
        status = thrift_tls_flush(client->socket);
        if (status != THRIFT_AGAIN) break;
        status = wait_io(client, deadline);
    }
    if (status == THRIFT_OK) {
        client->request.size = 0; client->loaded = false;
        client->response.position = client->response.size = 0;
    }
    return client->error = status;
}
static enum thrift_status read_response(void *context, void *data, size_t size)
{
    struct thrift_tls_client *client = context;
    enum thrift_status status;
    if (client->error != THRIFT_OK) return client->error;
    if (!size) return THRIFT_OK;
    if (!client->loaded) {
        uint8_t header[THRIFT_TLS_FRAME_HEADER];
        uint64_t deadline;
        size_t length = 0, i;
        void *allocation;
        status = thrift_tls_now(&deadline);
        if (status != THRIFT_OK) return client->error = status;
        deadline += client->timeout_ms;
        status = transfer(client, header, sizeof(header), true, deadline);
        if (status != THRIFT_OK) return client->error = status;
        for (i = 0; i < sizeof(header); ++i) length = (length << 8) | header[i];
        if (!length || length > client->response.limit) return client->error = THRIFT_LIMIT;
        allocation = realloc(client->response.data, length);
        if (!allocation) return client->error = THRIFT_NOMEM;
        client->response.data = allocation; client->response.capacity = length;
        status = transfer(client, allocation, length, true, deadline);
        if (status != THRIFT_OK) return client->error = status;
        client->response.size = length; client->loaded = true;
    }
    return client->error = thrift_buffer_read(&client->response, data, size);
}
enum thrift_status thrift_tls_client_create(struct thrift_tls_socket *socket, size_t max_frame_size,
    uint32_t timeout_ms, struct thrift_tls_client **out, struct thrift_transport *transport)
{
    struct thrift_tls_client *client;
    if (!out) return THRIFT_INVALID;
    *out = NULL;
    if (!transport) return THRIFT_INVALID;
    if (!socket || socket->listener || !max_frame_size || max_frame_size > INT32_MAX || !timeout_ms || timeout_ms > INT_MAX)
        return THRIFT_INVALID;
    client = calloc(1, sizeof(*client));
    if (!client) return THRIFT_NOMEM;
    client->socket = socket; client->timeout_ms = timeout_ms;
    client->request.limit = client->response.limit = max_frame_size;
    transport->context = client; transport->read = read_response;
    transport->write = write_request; transport->flush = flush_request;
    *out = client;
    return THRIFT_OK;
}
void thrift_tls_client_destroy(struct thrift_tls_client *client)
{
    if (client) { thrift_buffer_clear(&client->request); thrift_buffer_clear(&client->response); free(client); }
}

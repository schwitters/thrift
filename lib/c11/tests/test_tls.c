/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/server/thrift_tls_server.h>
#include <thrift/c11/transport/thrift_memory_buffer.h>
#include "../src/thrift/c11/transport/thrift_tls_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return EXIT_FAILURE; \
} } while (0)
enum { TEST_TIMEOUT_MS = 2000, TEST_FRAME_LIMIT = 256 * 1024, TEST_CONNECTIONS = 32,
       TEST_WORKERS = 2, TEST_RUN_MS = 10000, TEST_POLL_MS = 2, TEST_WIRE_CAPACITY = 64 };
struct sample { int32_t value; bool present; };
static const struct thrift_field fields[] = {
    {1, offsetof(struct sample, value), offsetof(struct sample, present), true, false, &thrift_type_i32}
};
static const struct thrift_record record = {sizeof(struct sample), 1, fields, false, NULL};
static enum thrift_status process(struct thrift_protocol *protocol, void *context)
{
    struct sample sample = {0};
    enum thrift_status status = thrift_record_read(protocol, &record, &sample);
    (void)context;
    if (status == THRIFT_OK) status = thrift_record_write(protocol, &record, &sample);
    return status;
}
static int server_mode(const char *certificate, const char *key, enum thrift_protocol_kind kind)
{
    struct thrift_tls_options credentials = {true, certificate, key, NULL, NULL, THRIFT_TEST_MACHINE_KEY};
    struct thrift_tls_context *context = NULL;
    struct thrift_tls_server *server = NULL;
    struct thrift_tls_server_options options = {0};
    uint16_t port;
    uint64_t now, end;
    CHECK(thrift_tls_context_create(&credentials, &context) == THRIFT_OK);
    options.context = context; options.max_connections = TEST_CONNECTIONS;
    options.max_frame_size = TEST_FRAME_LIMIT; options.timeout_ms = TEST_TIMEOUT_MS;
    options.worker_count = TEST_WORKERS; options.kind = kind;
    CHECK(thrift_tls_server_create(&options, process, NULL, &server) == THRIFT_OK);
    CHECK(thrift_tls_server_port(server, &port) == THRIFT_OK);
    CHECK(printf("%u\n", (unsigned)port) > 0 && fflush(stdout) == 0);
    CHECK(thrift_tls_now(&now) == THRIFT_OK);
    end = now + TEST_RUN_MS;
    do {
        CHECK(thrift_tls_server_poll(server, TEST_POLL_MS) == THRIFT_OK);
        CHECK(thrift_tls_now(&now) == THRIFT_OK);
    } while (now < end);
    thrift_tls_server_destroy(server);
    thrift_tls_context_destroy(context);
    return EXIT_SUCCESS;
}
static int client_mode(const char *ca, const char *port_text, const char *name, bool expect_success)
{
    struct thrift_tls_options options = {false, NULL, NULL, NULL, ca, false};
    struct thrift_tls_context *context = NULL;
    struct thrift_tls_socket *socket = NULL;
    struct thrift_tls_client *client = NULL;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    struct sample input = {INT32_MIN, true}, output = {0};
    enum thrift_status status;
    char *end;
    unsigned long port = strtoul(port_text, &end, 10);
    CHECK(*port_text && !*end && port > 0 && port <= UINT16_MAX);
    CHECK(thrift_tls_context_create(&options, &context) == THRIFT_OK);
    CHECK(thrift_tls_connect(context, "127.0.0.1", (uint16_t)port, name, TEST_TIMEOUT_MS, &socket) == THRIFT_OK);
    CHECK(thrift_tls_client_create(socket, TEST_FRAME_LIMIT, TEST_TIMEOUT_MS, &client, &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init(&protocol, transport) == THRIFT_OK);
    CHECK(thrift_record_write(&protocol, &record, &input) == THRIFT_OK);
    status = transport.flush(transport.context);
    if (status == THRIFT_OK) status = thrift_record_read(&protocol, &record, &output);
    if (expect_success) CHECK(status == THRIFT_OK && output.present && output.value == input.value);
    else CHECK(status != THRIFT_OK && status != THRIFT_AGAIN);
    thrift_tls_client_destroy(client);
    thrift_tls_socket_destroy(socket);
    thrift_tls_context_destroy(context);
    return EXIT_SUCCESS;
}
static int pair_mode(const char *certificate, const char *key, const char *ca)
{
    struct thrift_tls_options server_options = {true, certificate, key, NULL, NULL, THRIFT_TEST_MACHINE_KEY};
    struct thrift_tls_options client_options = {false, NULL, NULL, NULL, ca, false};
    struct thrift_tls_context *server_context = NULL, *client_context = NULL;
    struct thrift_tls_socket *listener = NULL, *server = NULL, *client = NULL;
    enum thrift_status server_status = THRIFT_AGAIN, client_status = THRIFT_AGAIN;
    uint16_t port;
    uint64_t now, deadline;
    size_t count;
    uint8_t value = 0;
    CHECK(thrift_tls_context_create(&server_options, &server_context) == THRIFT_OK);
    CHECK(thrift_tls_context_create(&client_options, &client_context) == THRIFT_OK);
    CHECK(thrift_tls_listen(server_context, NULL, 0, &listener) == THRIFT_OK);
    CHECK(thrift_tls_port(listener, &port) == THRIFT_OK);
    CHECK(thrift_tls_accept(listener, TEST_TIMEOUT_MS, &server) == THRIFT_AGAIN && server == NULL);
    CHECK(thrift_tls_connect(client_context, "127.0.0.1", port, "localhost", TEST_TIMEOUT_MS, &client) == THRIFT_OK);
    CHECK(thrift_tls_now(&now) == THRIFT_OK);
    deadline = now + TEST_TIMEOUT_MS;
    while (server_status != THRIFT_OK || client_status != THRIFT_OK) {
        if (!server) {
            enum thrift_status accepted = thrift_tls_accept(listener, TEST_TIMEOUT_MS, &server);
            CHECK(accepted == THRIFT_OK || accepted == THRIFT_AGAIN);
        }
        if (client_status != THRIFT_OK) client_status = thrift_tls_handshake(client);
        if (server && server_status != THRIFT_OK) server_status = thrift_tls_handshake(server);
        CHECK(client_status == THRIFT_OK || client_status == THRIFT_AGAIN);
        CHECK(server_status == THRIFT_OK || server_status == THRIFT_AGAIN);
        CHECK(thrift_tls_now(&now) == THRIFT_OK && now < deadline);
    }
    CHECK(thrift_tls_read(server, &value, sizeof(value), &count) == THRIFT_AGAIN && count == 0);
    CHECK(thrift_tls_write(client, "x", 1, &count) == THRIFT_OK && count == 1);
    do { client_status = thrift_tls_flush(client); } while (client_status == THRIFT_AGAIN);
    CHECK(client_status == THRIFT_OK);
    do {
        server_status = thrift_tls_read(server, &value, sizeof(value), &count);
        CHECK(thrift_tls_now(&now) == THRIFT_OK && now < deadline);
    } while (server_status == THRIFT_AGAIN);
    CHECK(server_status == THRIFT_OK && count == 1 && value == 'x');
    do { client_status = thrift_tls_shutdown(client); } while (client_status == THRIFT_AGAIN);
    CHECK(client_status == THRIFT_OK);
    do {
        server_status = thrift_tls_read(server, &value, sizeof(value), &count);
        CHECK(thrift_tls_now(&now) == THRIFT_OK && now < deadline);
    } while (server_status == THRIFT_AGAIN);
    CHECK(server_status == THRIFT_EOF);
    thrift_tls_socket_destroy(client); thrift_tls_socket_destroy(server); thrift_tls_socket_destroy(listener);
    thrift_tls_context_destroy(client_context); thrift_tls_context_destroy(server_context);
    return EXIT_SUCCESS;
}
int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("TLS test peer: pair CERT KEY CA | server CERT KEY binary|compact | client CA PORT NAME ok|fail");
        return EXIT_SUCCESS;
    }
    if (argc == 5 && strcmp(argv[1], "pair") == 0)
        return pair_mode(argv[2], strcmp(argv[3], "-") == 0 ? NULL : argv[3], argv[4]);
    if (argc == 5 && strcmp(argv[1], "server") == 0)
        return server_mode(argv[2], strcmp(argv[3], "-") == 0 ? NULL : argv[3],
            strcmp(argv[4], "compact") == 0 ? THRIFT_COMPACT : THRIFT_BINARY);
    if (argc == 6 && strcmp(argv[1], "client") == 0)
        return client_mode(argv[2], argv[3], argv[4], strcmp(argv[5], "ok") == 0);
    return EXIT_FAILURE;
}

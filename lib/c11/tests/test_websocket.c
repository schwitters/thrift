/* SPDX-License-Identifier: Apache-2.0 */
#include "fixture_types.h"
#include <thrift/c11/server/thrift_websocket_server.h>
#include <thrift/c11/transport/thrift_websocket_client.h>
#include <thrift/c11/protocol/thrift_multiplexed_protocol.h>
#include <thrift/c11/processor/thrift_multiplexed_processor.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    result = EXIT_FAILURE; goto cleanup; } } while (0)
enum { MESSAGE_LIMIT = 131072, TIMEOUT_MS = 2000, PROBE_TIMEOUT_MS = 300,
       URL_CAPACITY = 128, WORKER_COUNT = 4, RPC_ITERATIONS = 3,
       MODE_ARGUMENT = 1, URL_ARGUMENT = 2, KIND_ARGUMENT = 3, MUX_ARGUMENT = 4,
       SERVER_ARGUMENT_COUNT = 4, CLIENT_ARGUMENT_COUNT = 5, PROBE_ARGUMENT_COUNT = 3,
       TLS_PROBE_ARGUMENT_COUNT = 4, CA_ARGUMENT = 3 };
static const char bearer_token[] = "test-websocket-token";
struct handler_context {
    struct test_echo_handler handler;
    atomic_int_least32_t notification;
    bool multiplexed;
};
static enum thrift_status inherited(void *user_context,
    const struct test_base_inherited_args *args,
    struct test_base_inherited_result *result)
{
    struct handler_context *handler = user_context;
    result->f_success = args->f_value + atomic_load(&handler->notification);
    result->has_success = true;
    return THRIFT_OK;
}
static enum thrift_status notify(void *user_context,
    const struct test_echo_notify_args *args,
    struct test_echo_notify_result *result)
{
    struct handler_context *handler = user_context;
    (void)result;
    atomic_store(&handler->notification, args->f_value);
    return THRIFT_OK;
}
static enum thrift_status process(struct thrift_protocol *protocol, void *context)
{
    struct handler_context *handler = context;
    if (handler->multiplexed) {
        struct thrift_service service = test_echo_service(&handler->handler);
        return thrift_multiplexed_process(protocol, &service, 1, NULL);
    }
    return test_echo_process(protocol, &handler->handler);
}
static void init_handler(struct handler_context *handler, bool multiplexed)
{
    atomic_init(&handler->notification, 0);
    handler->handler.user_context = handler;
    handler->handler.f_inherited = inherited;
    handler->handler.f_notify = notify;
    handler->multiplexed = multiplexed;
}
static int call_peer(const char *url, enum thrift_protocol_kind kind, bool multiplexed, bool graceful)
{
    struct thrift_websocket_client_options options = {url, MESSAGE_LIMIT, TIMEOUT_MS, NULL, bearer_token};
    struct thrift_websocket_client *client = NULL;
    struct thrift_transport transport = {0};
    struct thrift_protocol protocol;
    struct test_base_inherited_args args = {0};
    struct test_base_inherited_result reply = {0};
    struct test_echo_notify_args notification = {0};
    int result = EXIT_SUCCESS, iteration;
    CHECK(thrift_websocket_client_create(&options, &client, &transport) == THRIFT_OK);
    CHECK((multiplexed ? thrift_multiplexed_protocol_init(&protocol, transport, kind, "Echo") :
        thrift_protocol_init_kind(&protocol, transport, kind)) == THRIFT_OK);
    for (iteration = 0; iteration < RPC_ITERATIONS; ++iteration) {
        notification.f_value = 7;
        CHECK(test_echo_notify_call(&protocol, iteration, &notification, NULL) == THRIFT_OK);
        args.f_value = 35;
        CHECK(test_echo_inherited_call(&protocol, iteration, &args, &reply) == THRIFT_OK);
        CHECK(reply.has_success && reply.f_success == 42);
    }
    if (graceful) {
        CHECK(thrift_websocket_client_close(client) == THRIFT_OK);
        CHECK(thrift_websocket_client_close(client) == THRIFT_OK);
        CHECK(transport.write(transport.context, "x", 1) == THRIFT_EOF);
    }
cleanup:
    test_base_inherited_result_clear(&reply);
    thrift_websocket_client_destroy(client);
    return result;
}
static int run_server(enum thrift_protocol_kind kind, bool multiplexed, bool peer)
{
    struct handler_context handler = {0};
    struct thrift_websocket_server_options options = {
        "127.0.0.1:0", "/rpc", MESSAGE_LIMIT, TIMEOUT_MS, WORKER_COUNT, kind, NULL};
    struct thrift_websocket_server *server = NULL;
    uint16_t port;
    char url[URL_CAPACITY];
    int result = EXIT_SUCCESS, count;
    init_handler(&handler, multiplexed);
    CHECK(thrift_websocket_server_start(&options, process, &handler, &server) == THRIFT_OK);
    CHECK(thrift_websocket_server_port(server, &port) == THRIFT_OK);
    if (peer) {
        CHECK(printf("%u\n", (unsigned)port) > 0 && fflush(stdout) == 0);
        CHECK(getchar() == '\n');
    } else {
        count = snprintf(url, sizeof(url), "ws://127.0.0.1:%u/rpc", (unsigned)port);
        CHECK(count > 0 && (size_t)count < sizeof(url));
        CHECK(call_peer(url, kind, multiplexed, true) == EXIT_SUCCESS);
    }
cleanup:
    thrift_websocket_server_stop(server);
    return result;
}
static int probe(const char *url, bool large, const char *ca_file)
{
    struct thrift_websocket_client_options options = {url, MESSAGE_LIMIT, PROBE_TIMEOUT_MS, NULL, bearer_token};
    struct thrift_websocket_client *client = NULL;
    struct thrift_transport transport = {0};
    enum thrift_status status;
    char *payload = NULL;
    char reply[sizeof("ok") - 1];
    int result = EXIT_SUCCESS;
    options.ca_file = ca_file;
    if (ca_file) options.timeout_ms = TIMEOUT_MS;
    status = thrift_websocket_client_create(&options, &client, &transport);
    if (status == THRIFT_OK) {
        size_t size = large ? MESSAGE_LIMIT : sizeof("abc") - 1;
        payload = malloc(size);
        CHECK(payload);
        if (large) memset(payload, 'x', size);
        else memcpy(payload, "abc", size);
        status = transport.write(transport.context, payload, size);
        if (status == THRIFT_OK) status = transport.flush(transport.context);
        if (status == THRIFT_OK) status = transport.read(transport.context, reply, sizeof(reply));
        if (status == THRIFT_OK) {
            CHECK(!memcmp(reply, "ok", sizeof(reply)));
            status = thrift_websocket_client_close(client);
        }
    }
    CHECK(printf("%d\n", (int)status) > 0);
cleanup:
    free(payload);
    thrift_websocket_client_destroy(client);
    return result;
}
static int validation(void)
{
    struct thrift_websocket_client_options client_options = {"file:///invalid", MESSAGE_LIMIT, TIMEOUT_MS, NULL, NULL};
    struct thrift_websocket_server_options server_options = {"127.0.0.1:0", "/rpc", MESSAGE_LIMIT, TIMEOUT_MS, WORKER_COUNT, THRIFT_BINARY, NULL};
    struct thrift_websocket_client *client = NULL;
    struct thrift_websocket_server *server = NULL;
    struct thrift_transport transport = {0};
    int result = EXIT_SUCCESS;
    CHECK(thrift_websocket_client_create(&client_options, &client, &transport) == THRIFT_INVALID && !client);
    client_options.url = "ws://127.0.0.1:1/rpc";
    client_options.bearer_token = "bad\r\nInjected: yes";
    CHECK(thrift_websocket_client_create(&client_options, &client, &transport) == THRIFT_INVALID && !client);
    client_options.bearer_token = NULL;
    client_options.timeout_ms = 0;
    CHECK(thrift_websocket_client_create(&client_options, &client, &transport) == THRIFT_INVALID && !client);
    server_options.worker_count = 0;
    CHECK(thrift_websocket_server_start(&server_options, process, NULL, &server) == THRIFT_INVALID && !server);
    CHECK(thrift_websocket_client_close(NULL) == THRIFT_INVALID);
cleanup:
    thrift_websocket_client_destroy(client);
    thrift_websocket_server_stop(server);
    return result;
}
int main(int argc, char **argv)
{
    int result = EXIT_SUCCESS;
    bool client_library = false, server_library = false;
    CHECK(thrift_websocket_client_library_init() == THRIFT_OK);
    client_library = true;
    CHECK(thrift_websocket_server_library_init() == THRIFT_OK);
    server_library = true;
    if (argc == SERVER_ARGUMENT_COUNT && !strcmp(argv[MODE_ARGUMENT], "--server")) {
        CHECK(run_server(!strcmp(argv[URL_ARGUMENT], "compact") ? THRIFT_COMPACT : THRIFT_BINARY,
            !strcmp(argv[KIND_ARGUMENT], "mux"), true) == EXIT_SUCCESS);
    } else if (argc == CLIENT_ARGUMENT_COUNT &&
        (!strcmp(argv[MODE_ARGUMENT], "--client") || !strcmp(argv[MODE_ARGUMENT], "--cpp-client"))) {
        CHECK(call_peer(argv[URL_ARGUMENT], !strcmp(argv[KIND_ARGUMENT], "compact") ? THRIFT_COMPACT : THRIFT_BINARY,
            !strcmp(argv[MUX_ARGUMENT], "mux"), strcmp(argv[MODE_ARGUMENT], "--cpp-client") != 0) == EXIT_SUCCESS);
    } else if ((argc == PROBE_ARGUMENT_COUNT || argc == TLS_PROBE_ARGUMENT_COUNT) &&
        (!strcmp(argv[MODE_ARGUMENT], "--probe") || !strcmp(argv[MODE_ARGUMENT], "--large"))) {
        CHECK(probe(argv[URL_ARGUMENT], !strcmp(argv[MODE_ARGUMENT], "--large"),
            argc == TLS_PROBE_ARGUMENT_COUNT ? argv[CA_ARGUMENT] : NULL) == EXIT_SUCCESS);
    } else {
        CHECK(argc == 1);
        CHECK(validation() == EXIT_SUCCESS);
        CHECK(run_server(THRIFT_BINARY, false, false) == EXIT_SUCCESS);
        CHECK(run_server(THRIFT_COMPACT, false, false) == EXIT_SUCCESS);
        CHECK(run_server(THRIFT_BINARY, true, false) == EXIT_SUCCESS);
        CHECK(run_server(THRIFT_COMPACT, true, false) == EXIT_SUCCESS);
    }
cleanup:
    if (server_library && thrift_websocket_server_library_cleanup() != THRIFT_OK) result = EXIT_FAILURE;
    if (client_library) thrift_websocket_client_library_cleanup();
    return result;
}

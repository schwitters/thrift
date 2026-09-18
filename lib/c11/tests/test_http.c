/* SPDX-License-Identifier: Apache-2.0 */
#include "fixture_types.h"
#include <thrift/c11/server/thrift_http_server.h>
#include <thrift/c11/transport/thrift_http_client.h>
#include <thrift/c11/protocol/thrift_multiplexed_protocol.h>
#include <thrift/c11/processor/thrift_multiplexed_processor.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return EXIT_FAILURE; } } while (0)
enum { BODY_LIMIT = 65536, TIMEOUT_MS = 2000, URL_SIZE = 128,
       HTTP_OK = 200, HTTP_BAD_REQUEST = 400, HTTP_NOT_FOUND = 404,
       HTTP_METHOD_NOT_ALLOWED = 405, HTTP_PAYLOAD_TOO_LARGE = 413,
       PROBE_BODY_LIMIT = 32, PROBE_TIMEOUT_MS = 200, RPC_ITERATIONS = 3,
       MODE_ARGUMENT = 1, VALUE_ARGUMENT = 2, EXTRA_ARGUMENT = 3, MUX_ARGUMENT = 4,
       PROBE_ARGUMENT_COUNT = 3, SERVER_ARGUMENT_COUNT = 4, SEQUENCE_ARGUMENT_COUNT = 5 };
static const char probe_request[] = "abc";
static const char probe_reply[] = "ok";
static const char invalid_request[] = "bad";
struct handler_context {
    struct test_echo_handler handler;
    bool multiplexed;
    int32_t notification;
};
static enum thrift_status inherited(void *user_context,
    const struct test_base_inherited_args *args,
    struct test_base_inherited_result *result)
{
    struct handler_context *handler = user_context;
    result->f_success = args->f_value + handler->notification;
    result->has_success = true;
    return THRIFT_OK;
}
static enum thrift_status notify(void *user_context,
    const struct test_echo_notify_args *args,
    struct test_echo_notify_result *result)
{
    struct handler_context *handler = user_context;
    (void)result;
    handler->notification = args->f_value;
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
static size_t discard(char *data, size_t size, size_t count, void *context)
{
    (void)data; (void)context;
    return size * count;
}
static int raw_request(const char *url, const char *method, const char *body,
                       long body_size, bool chunked, long expected)
{
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    long status;
    CHECK(curl);
    if (chunked) {
        headers = curl_slist_append(NULL, "Transfer-Encoding: chunked");
        CHECK(headers);
    }
    CHECK(curl_easy_setopt(curl, CURLOPT_URL, url) == CURLE_OK);
    CHECK(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK);
    CHECK(curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) == CURLE_OK);
    CHECK(curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body_size) == CURLE_OK);
    CHECK(curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method) == CURLE_OK);
    CHECK(curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)TIMEOUT_MS) == CURLE_OK);
    CHECK(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard) == CURLE_OK);
    CHECK(curl_easy_perform(curl) == CURLE_OK);
    CHECK(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK);
    CHECK(status == expected);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return EXIT_SUCCESS;
}
static int run(enum thrift_protocol_kind kind, bool multiplexed)
{
    struct handler_context handler = {0};
    struct thrift_http_server_options server_options = {"127.0.0.1:0", "/rpc", BODY_LIMIT, TIMEOUT_MS, kind, NULL};
    struct thrift_http_client_options client_options = {NULL, BODY_LIMIT, TIMEOUT_MS, NULL};
    struct thrift_http_server *server = NULL;
    struct thrift_http_client *client = NULL;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    struct test_base_inherited_args args = {0};
    struct test_base_inherited_result result = {0};
    struct test_echo_notify_args notification = {0};
    struct test_echo_ping_args ping = {0};
    struct test_echo_ping_result pong = {0};
    uint16_t port;
    char url[URL_SIZE];
    int count, iteration;
    handler.handler.user_context = &handler;
    handler.handler.f_inherited = inherited;
    handler.handler.f_notify = notify;
    handler.multiplexed = multiplexed;
    CHECK(thrift_http_server_start(&server_options, process, &handler, &server) == THRIFT_OK);
    CHECK(thrift_http_server_port(server, &port) == THRIFT_OK);
    count = snprintf(url, sizeof(url), "http://127.0.0.1:%u/rpc", (unsigned)port);
    CHECK(count > 0 && (size_t)count < sizeof(url));
    client_options.url = url;
    CHECK(thrift_http_client_create(&client_options, &client, &transport) == THRIFT_OK);
    CHECK(thrift_multiplexed_protocol_init(&protocol, transport, kind,
          multiplexed ? "Echo" : NULL) == (multiplexed ? THRIFT_OK : THRIFT_INVALID));
    if (!multiplexed)
        CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
    for (iteration = 0; iteration < RPC_ITERATIONS; ++iteration) {
        args.f_value = 42;
        CHECK(test_echo_inherited_call(&protocol, iteration, &args, &result) == THRIFT_OK);
        CHECK(result.has_success && result.f_success == 42 + (iteration ? 7 : 0));
        notification.f_value = 7;
        CHECK(test_echo_notify_call(&protocol, iteration, &notification, NULL) == THRIFT_OK);
        CHECK(test_echo_ping_call(&protocol, iteration, &ping, &pong) == THRIFT_REMOTE);
    }
    thrift_http_client_destroy(client);
    /* HTTP failures are visible through the transport status model. */
    CHECK(thrift_http_client_create(&client_options, &client, &transport) == THRIFT_OK);
    CHECK(transport.write(transport.context, invalid_request, sizeof(invalid_request) - 1) == THRIFT_OK);
    CHECK(transport.flush(transport.context) == THRIFT_REMOTE);
    thrift_http_client_destroy(client);
    CHECK(raw_request(url, "GET", "", 0, false, HTTP_METHOD_NOT_ALLOWED) == EXIT_SUCCESS);
    CHECK(raw_request(url, "POST", invalid_request, sizeof(invalid_request) - 1, true, HTTP_BAD_REQUEST) == EXIT_SUCCESS);
    {
        char *large = calloc(BODY_LIMIT + 1, 1);
        CHECK(large);
        CHECK(raw_request(url, "POST", large, BODY_LIMIT + 1, false, HTTP_PAYLOAD_TOO_LARGE) == EXIT_SUCCESS);
        CHECK(raw_request(url, "POST", large, BODY_LIMIT + 1, true, HTTP_PAYLOAD_TOO_LARGE) == EXIT_SUCCESS);
        free(large);
    }
    count = snprintf(url, sizeof(url), "http://127.0.0.1:%u/", (unsigned)port);
    CHECK(count > 0 && (size_t)count < sizeof(url));
    CHECK(raw_request(url, "GET", "", 0, false, HTTP_NOT_FOUND) == EXIT_SUCCESS);
    thrift_http_server_stop(server);
    return EXIT_SUCCESS;
}
static int serve_peer(const char *kind_name, const char *mux_name)
{
    struct handler_context handler = {0};
    struct thrift_http_server *server = NULL;
    struct thrift_http_server_options options = {"127.0.0.1:0", "/rpc", BODY_LIMIT,
        TIMEOUT_MS, THRIFT_BINARY, NULL};
    uint16_t port;
    options.kind = strcmp(kind_name, "compact") ? THRIFT_BINARY : THRIFT_COMPACT;
    handler.multiplexed = !strcmp(mux_name, "mux");
    handler.handler.user_context = &handler;
    handler.handler.f_inherited = inherited;
    handler.handler.f_notify = notify;
    CHECK(thrift_http_server_library_init() == THRIFT_OK);
    CHECK(thrift_http_server_start(&options, process, &handler, &server) == THRIFT_OK);
    CHECK(thrift_http_server_port(server, &port) == THRIFT_OK);
    CHECK(printf("%u\n", (unsigned)port) > 0 && fflush(stdout) == 0);
    CHECK(getchar() == '\n');
    thrift_http_server_stop(server);
    CHECK(thrift_http_server_library_cleanup() == THRIFT_OK);
    return EXIT_SUCCESS;
}

static int probe_peer(const char *url)
{
    struct thrift_http_client_options options = {url, PROBE_BODY_LIMIT, PROBE_TIMEOUT_MS, NULL};
    struct thrift_http_client *client = NULL;
    struct thrift_transport transport;
    enum thrift_status status;
    char reply[sizeof(probe_reply) - 1];
    CHECK(thrift_http_client_library_init() == THRIFT_OK);
    CHECK(thrift_http_client_create(&options, &client, &transport) == THRIFT_OK);
    CHECK(transport.write(transport.context, probe_request, sizeof(probe_request) - 1) == THRIFT_OK);
    status = transport.flush(transport.context);
    if (status == THRIFT_OK) {
        CHECK(transport.read(transport.context, reply, sizeof(reply)) == THRIFT_OK);
        CHECK(!memcmp(reply, probe_reply, sizeof(reply)));
    }
    CHECK(printf("%d\n", (int)status) > 0);
    thrift_http_client_destroy(client);
    thrift_http_client_library_cleanup();
    return EXIT_SUCCESS;
}

/* OneWayHTTPTest.cpp: multiple oneway calls must not poison the next reply. */
static int sequence_peer(const char *url, const char *kind_name, const char *mux_name)
{
    struct thrift_http_client_options options = {url, BODY_LIMIT, TIMEOUT_MS, NULL};
    struct thrift_http_client *client = NULL;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    struct test_base_inherited_args args = {0};
    struct test_base_inherited_result result = {0};
    struct test_echo_notify_args notification = {0};
    enum thrift_protocol_kind kind = !strcmp(kind_name, "compact") ?
        THRIFT_COMPACT : THRIFT_BINARY;
    int iteration;
    CHECK(thrift_http_client_library_init() == THRIFT_OK);
    CHECK(thrift_http_client_create(&options, &client, &transport) == THRIFT_OK);
    if (!strcmp(mux_name, "mux"))
        CHECK(thrift_multiplexed_protocol_init(&protocol, transport, kind, "Echo") == THRIFT_OK);
    else
        CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
    args.f_value = 35;
    CHECK(test_echo_inherited_call(&protocol, INT32_MIN, &args, &result) == THRIFT_OK);
    CHECK(result.has_success && result.f_success == 35);
    for (iteration = 0; iteration < RPC_ITERATIONS; ++iteration) {
        notification.f_value = 7;
        CHECK(test_echo_notify_call(&protocol, 0, &notification, NULL) == THRIFT_OK);
        notification.f_value = 8;
        CHECK(test_echo_notify_call(&protocol, 65536, &notification, NULL) == THRIFT_OK);
        CHECK(test_echo_inherited_call(&protocol, INT32_MAX, &args, &result) == THRIFT_OK);
        CHECK(result.has_success && result.f_success == 43);
    }
    thrift_http_client_destroy(client);
    thrift_http_client_library_cleanup();
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    if (argc == SEQUENCE_ARGUMENT_COUNT && !strcmp(argv[MODE_ARGUMENT], "--sequence"))
        return sequence_peer(argv[VALUE_ARGUMENT], argv[EXTRA_ARGUMENT], argv[MUX_ARGUMENT]);
    if (argc == SERVER_ARGUMENT_COUNT && !strcmp(argv[MODE_ARGUMENT], "--server"))
        return serve_peer(argv[VALUE_ARGUMENT], argv[EXTRA_ARGUMENT]);
    if (argc == PROBE_ARGUMENT_COUNT && !strcmp(argv[MODE_ARGUMENT], "--probe"))
        return probe_peer(argv[VALUE_ARGUMENT]);
    CHECK(argc == 1);
    CHECK(thrift_http_client_library_init() == THRIFT_OK);
    CHECK(thrift_http_server_library_init() == THRIFT_OK);
    CHECK(run(THRIFT_BINARY, false) == EXIT_SUCCESS);
    CHECK(run(THRIFT_COMPACT, false) == EXIT_SUCCESS);
    CHECK(run(THRIFT_BINARY, true) == EXIT_SUCCESS);
    CHECK(run(THRIFT_COMPACT, true) == EXIT_SUCCESS);
    CHECK(thrift_http_server_library_cleanup() == THRIFT_OK);
    thrift_http_client_library_cleanup();
    return EXIT_SUCCESS;
}

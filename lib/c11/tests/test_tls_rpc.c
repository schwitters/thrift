/* SPDX-License-Identifier: Apache-2.0 */
#include "fixture_types.h"
#include <thrift/c11/server/thrift_tls_server.h>
#include <thrift/c11/protocol/thrift_multiplexed_protocol.h>
#include <thrift/c11/processor/thrift_multiplexed_processor.h>
#include "../src/thrift/c11/transport/thrift_tls_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return EXIT_FAILURE; \
} } while (0)
enum { RPC_DEADLINE_MS = 5000, FRAME_LIMIT = 65536, CONNECTION_LIMIT = 8, WORKER_COUNT = 2,
       POLL_MS = 2, PING_SEQUENCE = 1, NOTIFY_SEQUENCE = 2, LAST_SEQUENCE = 3, NOTIFY_VALUE = 42 };
struct rpc_job {
    struct thrift_tls_context *context;
    uint16_t port;
    enum thrift_protocol_kind kind;
    bool multiplexed;
    enum thrift_status status;
};
static enum thrift_status ping(void *user_context, const struct test_echo_ping_args *args,
    struct test_echo_ping_result *result)
{
    (void)user_context; (void)args; (void)result;
    return THRIFT_OK;
}
static enum thrift_status notify(void *user_context, const struct test_echo_notify_args *args,
    struct test_echo_notify_result *result)
{
    (void)user_context; (void)result;
    return args->f_value == NOTIFY_VALUE ? THRIFT_OK : THRIFT_INVALID;
}
static enum thrift_status process(struct thrift_protocol *protocol, void *context)
{
    struct test_echo_handler handler = {0};
    struct thrift_service service;
    const bool *multiplexed = context;
    handler.f_ping = ping; handler.f_notify = notify;
    if (!*multiplexed) return test_echo_process(protocol, &handler);
    service = test_echo_service(&handler);
    service.name = "echo";
    return thrift_multiplexed_process(protocol, &service, 1, NULL);
}
static void call_rpc(void *context)
{
    struct rpc_job *job = context;
    struct thrift_tls_socket *socket = NULL;
    struct thrift_tls_client *client = NULL;
    struct thrift_transport transport = {0};
    struct thrift_protocol protocol;
    struct test_echo_ping_args args = {0};
    struct test_echo_ping_result result = {0};
    struct test_echo_notify_args notification = {NOTIFY_VALUE, true};
    struct test_echo_notify_result ignored = {0};
    job->status = thrift_tls_connect(job->context, "127.0.0.1", job->port, "localhost", RPC_DEADLINE_MS, &socket);
    if (job->status == THRIFT_OK)
        job->status = thrift_tls_client_create(socket, FRAME_LIMIT, RPC_DEADLINE_MS, &client, &transport);
    if (job->status == THRIFT_OK)
        job->status = job->multiplexed ? thrift_multiplexed_protocol_init(&protocol, transport, job->kind, "echo") :
            thrift_protocol_init_kind(&protocol, transport, job->kind);
    if (job->status == THRIFT_OK)
        job->status = test_echo_ping_call(&protocol, PING_SEQUENCE, &args, &result);
    if (job->status == THRIFT_OK)
        job->status = test_echo_notify_call(&protocol, NOTIFY_SEQUENCE, &notification, &ignored);
    if (job->status == THRIFT_OK)
        job->status = test_echo_ping_call(&protocol, LAST_SEQUENCE, &args, &result);
    test_echo_ping_result_clear(&result);
    test_echo_notify_result_clear(&ignored);
    thrift_tls_client_destroy(client);
    thrift_tls_socket_destroy(socket);
}
static int run_case(struct thrift_tls_context *server_context, struct thrift_tls_context *client_context,
    enum thrift_protocol_kind kind, bool multiplexed)
{
    struct thrift_tls_server_options options = {0};
    struct thrift_tls_server *server = NULL;
    struct thrift_tls_worker *client_worker = NULL;
    struct rpc_job job = {0};
    void *completed = NULL;
    uint64_t now, deadline;
    options.context = server_context; options.max_connections = CONNECTION_LIMIT;
    options.max_frame_size = FRAME_LIMIT; options.timeout_ms = RPC_DEADLINE_MS;
    options.worker_count = WORKER_COUNT; options.kind = kind;
    CHECK(thrift_tls_server_create(&options, process, &multiplexed, &server) == THRIFT_OK);
    CHECK(thrift_tls_server_port(server, &job.port) == THRIFT_OK);
    job.context = client_context; job.kind = kind; job.multiplexed = multiplexed;
    CHECK(thrift_tls_worker_create(call_rpc, &client_worker) == THRIFT_OK);
    CHECK(thrift_tls_worker_submit(client_worker, &job) == THRIFT_OK);
    CHECK(thrift_tls_now(&now) == THRIFT_OK);
    deadline = now + RPC_DEADLINE_MS;
    while (!thrift_tls_worker_take(client_worker, &completed)) {
        CHECK(thrift_tls_server_poll(server, POLL_MS) == THRIFT_OK);
        CHECK(thrift_tls_now(&now) == THRIFT_OK && now < deadline);
    }
    CHECK(completed == &job && job.status == THRIFT_OK);
    thrift_tls_worker_destroy(client_worker);
    thrift_tls_server_destroy(server);
    return EXIT_SUCCESS;
}
int main(int argc, char **argv)
{
    struct thrift_tls_options server_options = {0}, client_options = {0};
    struct thrift_tls_context *server_context = NULL, *client_context = NULL;
    CHECK(argc == 4);
    server_options.server = true; server_options.machine_key = THRIFT_TEST_MACHINE_KEY; server_options.certificate_file = argv[1];
    server_options.private_key_file = strcmp(argv[2], "-") == 0 ? NULL : argv[2];
    client_options.ca_file = argv[3];
    CHECK(thrift_tls_context_create(&server_options, &server_context) == THRIFT_OK);
    CHECK(thrift_tls_context_create(&client_options, &client_context) == THRIFT_OK);
    CHECK(run_case(server_context, client_context, THRIFT_BINARY, false) == EXIT_SUCCESS);
    CHECK(run_case(server_context, client_context, THRIFT_COMPACT, false) == EXIT_SUCCESS);
    CHECK(run_case(server_context, client_context, THRIFT_BINARY, true) == EXIT_SUCCESS);
    CHECK(run_case(server_context, client_context, THRIFT_COMPACT, true) == EXIT_SUCCESS);
    thrift_tls_context_destroy(client_context); thrift_tls_context_destroy(server_context);
    return EXIT_SUCCESS;
}

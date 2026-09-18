/* SPDX-License-Identifier: Apache-2.0 */
#include "fixture_types.h"
#include <thrift/c11/processor/thrift_multiplexed_processor.h>
#include <thrift/c11/protocol/thrift_multiplexed_protocol.h>
#include <thrift/c11/server/thrift_simple_server.h>
#include <thrift/c11/transport/thrift_unix_socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MODE_ARGUMENT = 1, PATH_ARGUMENT = 2, PROTOCOL_ARGUMENT = 3,
       SERVICE_ARGUMENT = 4, MIN_ARGUMENT_COUNT = 4, MAX_ARGUMENT_COUNT = 5,
       RPC_TIMEOUT_MS = 3000, REQUEST_COUNT = 1, REQUEST_SEQUENCE = 7,
       REQUEST_VALUE = 42 };

struct server_context {
    struct test_base_handler handler;
    const char *service_name;
};

static enum thrift_status inherited(void *user_context,
    const struct test_base_inherited_args *args,
    struct test_base_inherited_result *result)
{
    (void)user_context;
    result->f_success = args->f_value;
    result->has_success = true;
    return THRIFT_OK;
}

static enum thrift_status process(struct thrift_protocol *protocol, void *context)
{
    struct server_context *server = context;
    if (server->service_name) {
        struct thrift_service service = test_base_service(&server->handler);
        service.name = server->service_name;
        return thrift_multiplexed_process(protocol, &service, 1, NULL);
    }
    return test_base_process(protocol, &server->handler);
}

int main(int argc, char **argv)
{
    struct thrift_socket *socket_value = NULL;
    struct thrift_transport transport = {0};
    struct thrift_protocol protocol;
    struct server_context server = {{NULL, inherited}, NULL};
    struct test_base_inherited_args args = {0};
    struct test_base_inherited_result result = {0};
    enum thrift_protocol_kind kind;
    enum thrift_status status, close_status;
    bool serve;
    if (argc < MIN_ARGUMENT_COUNT || argc > MAX_ARGUMENT_COUNT)
        return EXIT_FAILURE;
    serve = !strcmp(argv[MODE_ARGUMENT], "--server");
    if (!serve && strcmp(argv[MODE_ARGUMENT], "--client"))
        return EXIT_FAILURE;
    if (!strcmp(argv[PROTOCOL_ARGUMENT], "binary"))
        kind = THRIFT_BINARY;
    else if (!strcmp(argv[PROTOCOL_ARGUMENT], "compact"))
        kind = THRIFT_COMPACT;
    else
        return EXIT_FAILURE;
    if (argc == MAX_ARGUMENT_COUNT)
        server.service_name = argv[SERVICE_ARGUMENT];
    if (serve) {
        status = thrift_socket_listen_unix(argv[PATH_ARGUMENT], &socket_value);
        if (status == THRIFT_OK) {
            if (puts("READY") == EOF || fflush(stdout) != 0)
                status = THRIFT_IO;
            else
                status = thrift_server_serve_kind(socket_value, process, &server,
                    REQUEST_COUNT, RPC_TIMEOUT_MS, kind);
        }
    } else {
        status = thrift_socket_connect_unix(argv[PATH_ARGUMENT], &socket_value);
        if (status == THRIFT_OK)
            status = thrift_socket_timeout(socket_value, RPC_TIMEOUT_MS);
        if (status == THRIFT_OK)
            status = thrift_socket_transport(socket_value, &transport);
        if (status == THRIFT_OK) {
            if (server.service_name)
                status = thrift_multiplexed_protocol_init(&protocol, transport, kind, server.service_name);
            else
                status = thrift_protocol_init_kind(&protocol, transport, kind);
        }
        args.f_value = REQUEST_VALUE;
        if (status == THRIFT_OK)
            status = test_base_inherited_call(&protocol, REQUEST_SEQUENCE, &args, &result);
        if (status == THRIFT_OK && (!result.has_success || result.f_success != REQUEST_VALUE))
            status = THRIFT_PROTOCOL;
        if (status == THRIFT_OK && puts("RESULT 42") == EOF)
            status = THRIFT_IO;
    }
    test_base_inherited_result_clear(&result);
    close_status = thrift_socket_close(socket_value);
    if (status == THRIFT_OK)
        status = close_status;
    return status == THRIFT_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

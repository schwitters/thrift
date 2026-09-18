/* SPDX-License-Identifier: Apache-2.0 */
#include "calculator_client.h"
#include "calculator_server.h"
#include "calculator_types.h"
#include <thrift/c11/protocol/thrift_multiplexed_protocol.h>
#include <thrift/c11/processor/thrift_multiplexed_processor.h>
#include <thrift/c11/server/thrift_simple_server.h>
#include <thrift/c11/thrift_log.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RPC_TIMEOUT_MS = 5000, DEFAULT_PORT = 9090,
       CLIENT_LEFT = 20, CLIENT_RIGHT = 22,
       MODE_ARGUMENT = 1, PORT_ARGUMENT = 2, PROTOCOL_ARGUMENT = 3,
       SERVICE_ARGUMENT = 4, MIN_ARGUMENT_COUNT = 2, MAX_ARGUMENT_COUNT = 5,
       DECIMAL_BASE = 10 };
static const char log_level_option[] = "--log-level=";

static void report_error(enum thrift_status status, const char *detail)
{
    const struct thrift_log_event event = {THRIFT_LOG_ERROR, THRIFT_LOG_END,
        "calculator", status, 0, NULL, 0, detail};
    thrift_log_write(&event);
}

/* Calculator.add is implemented in the generated, application-owned
 * calculator_server.c skeleton (see example_calculator_server_handle_add
 * there); regenerating with `--gen c11:server_stubs` preserves that file. */
struct server_context {
    struct example_calculator_handler handler;
    const char *service_name;
};

static enum thrift_status process(struct thrift_protocol *protocol, void *context)
{
    struct server_context *server = context;
    if (server->service_name) {
        struct thrift_service service = example_calculator_service(&server->handler);
        service.name = server->service_name;
        return thrift_multiplexed_process(protocol, &service, 1, NULL);
    }
    return example_calculator_server_process(protocol, &server->handler);
}

static enum thrift_status run_server(uint16_t port, enum thrift_protocol_kind kind, const char *service_name)
{
    struct thrift_socket *listener = NULL;
    struct server_context server = {.service_name = service_name};
    enum thrift_status status, close_status;
    status = example_calculator_server_init(&server.handler, NULL);
    if (status != THRIFT_OK)
        return status;
    status = thrift_socket_listen(NULL, port, &listener);
    if (status != THRIFT_OK)
        return status;
    status = thrift_socket_port(listener, &port);
    if (status == THRIFT_OK) {
        if (printf("Listening on %u\n", (unsigned)port) < 0 || fflush(stdout) != 0)
            status = THRIFT_IO;
        else
            status = thrift_server_serve_kind(listener, process, &server, 1, RPC_TIMEOUT_MS, kind);
    }
    close_status = thrift_socket_close(listener);
    return status == THRIFT_OK ? close_status : status;
}

static enum thrift_status run_client(uint16_t port, enum thrift_protocol_kind kind, const char *service_name)
{
    struct thrift_socket *socket_value = NULL;
    struct thrift_transport transport = {0};
    struct thrift_protocol protocol;
    /* The generated client wraps the raw _call(): it owns sequence-ID bookkeeping
     * (starting at 1, wrapping past INT32_MAX) instead of a hardcoded constant.
     * add() has no optional argument and a scalar return, so --gen c11:flat_calls
     * also generated example_calculator_client_add() below: one C parameter per
     * IDL argument and one output pointer per success/declared exception, instead
     * of building an args/result record by hand. Methods that do not qualify
     * (optional arguments, or a string/list/set/map return) keep using
     * example_calculator_client_add_call() with the args/result records. */
    struct example_calculator_client client = {0};
    int64_t sum = 0;
    struct example_calculation_error *error = NULL;
    enum thrift_status status, close_status;
    status = thrift_socket_connect("127.0.0.1", port, &socket_value);
    if (status != THRIFT_OK)
        return status;
    status = thrift_socket_timeout(socket_value, RPC_TIMEOUT_MS);
    if (status == THRIFT_OK)
        status = thrift_socket_transport(socket_value, &transport);
    if (status == THRIFT_OK) {
        if (service_name)
            status = thrift_multiplexed_protocol_init(&protocol, transport, kind, service_name);
        else
            status = thrift_protocol_init_kind(&protocol, transport, kind);
    }
    if (status == THRIFT_OK)
        status = example_calculator_client_init(&client, &protocol);
    if (status == THRIFT_OK)
        status = example_calculator_client_add(&client, CLIENT_LEFT, CLIENT_RIGHT, &sum, &error);
    if (status == THRIFT_OK) {
        if (error) {
            status = THRIFT_REMOTE;
            report_error(status, "Calculator returned a declared exception instead of a result.");
        }
        else if (printf("Result: %" PRId64 "\n", sum) < 0)
            status = THRIFT_IO;
    }
    if (error) {
        example_calculation_error_clear(error);
        free(error);
    }
    close_status = thrift_socket_close(socket_value);
    return status == THRIFT_OK ? close_status : status;
}

int main(int argc, char **argv)
{
    uint16_t port = DEFAULT_PORT;
    enum thrift_protocol_kind kind = THRIFT_BINARY;
    const char *service_name = NULL;
    enum thrift_status status;
    const char *log_level = NULL;
    int index, positional_count = 1;
    for (index = 1; index < argc; ++index) {
        if (!strncmp(argv[index], log_level_option, sizeof(log_level_option) - 1)) {
            if (log_level) {
                report_error(THRIFT_INVALID, "Duplicate --log-level option.");
                return EXIT_FAILURE;
            }
            log_level = argv[index] + sizeof(log_level_option) - 1;
        } else {
            argv[positional_count++] = argv[index];
        }
    }
    argc = positional_count;
    status = thrift_log_configure_level(log_level);
    if (status != THRIFT_OK) {
        report_error(status, "Invalid log level; expected trace, debug, info, warning, error or fatal.");
        return EXIT_FAILURE;
    }
    if (argc == MIN_ARGUMENT_COUNT && strcmp(argv[MODE_ARGUMENT], "--help") == 0) {
        if (puts("Usage: thrift_c11_calculator --server|--client [port [binary|compact [service]]] [--log-level=LEVEL]\n"
                 "The server handles one request. Port 0 selects a free server port.") == EOF)
            return EXIT_FAILURE;
        return EXIT_SUCCESS;
    }
    if (argc < MIN_ARGUMENT_COUNT || argc > MAX_ARGUMENT_COUNT || (strcmp(argv[MODE_ARGUMENT], "--server") != 0 && strcmp(argv[MODE_ARGUMENT], "--client") != 0)) {
        report_error(THRIFT_INVALID, "Invalid arguments; use --help.");
        return EXIT_FAILURE;
    }
    if (argc > PORT_ARGUMENT) {
        char *end = NULL;
        unsigned long parsed;
        int saved_error;
        const char *cursor;
        if (!argv[PORT_ARGUMENT][0]) {
            report_error(THRIFT_INVALID, "Invalid port.");
            return EXIT_FAILURE;
        }
        for (cursor = argv[PORT_ARGUMENT]; *cursor; ++cursor) {
            if (*cursor < '0' || *cursor > '9') {
                report_error(THRIFT_INVALID, "Invalid port; expected a decimal value in 0..65535.");
                return EXIT_FAILURE;
            }
        }
        errno = 0;
        parsed = strtoul(argv[PORT_ARGUMENT], &end, DECIMAL_BASE);
        saved_error = errno;
        if (saved_error || *end || parsed > UINT16_MAX) {
            report_error(THRIFT_INVALID, "Invalid port; expected a decimal value in 0..65535.");
            return EXIT_FAILURE;
        }
        port = (uint16_t)parsed;
    }
    if (argc > PROTOCOL_ARGUMENT) {
        if (strcmp(argv[PROTOCOL_ARGUMENT], "compact") == 0)
            kind = THRIFT_COMPACT;
        else if (strcmp(argv[PROTOCOL_ARGUMENT], "binary") != 0) {
            report_error(THRIFT_INVALID, "Invalid protocol; expected binary or compact.");
            return EXIT_FAILURE;
        }
    }
    if (argc > SERVICE_ARGUMENT) {
        service_name = argv[SERVICE_ARGUMENT];
        if (!service_name[0] || strchr(service_name, ':')) {
            report_error(THRIFT_INVALID, "Invalid service name.");
            return EXIT_FAILURE;
        }
    }
    status = strcmp(argv[MODE_ARGUMENT], "--server") == 0 ? run_server(port, kind, service_name)
                                              : run_client(port, kind, service_name);
    if (status != THRIFT_OK) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

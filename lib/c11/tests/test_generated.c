/* SPDX-License-Identifier: Apache-2.0 */
#include "fixture_server.h"
#include "fixture_client.h"
#include <thrift/c11/thrift_log.h>
#include <thrift/c11/protocol/thrift_multiplexed_protocol.h>
#include <thrift/c11/processor/thrift_multiplexed_processor.h>
#include <thrift/c11/transport/thrift_memory_buffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return EXIT_FAILURE; \
} } while (0)

enum { BUFFER_SIZE = 8192, REGISTERED_SERVICE_COUNT = 2,
       COMPACT_SEQUENCE_OFFSET = 2, ECHO_BINARY_SEQUENCE_OFFSET = 15 };

static int check_roundtrip(enum thrift_protocol_kind kind)
{
    uint8_t buffer[BUFFER_SIZE], second[BUFFER_SIZE];
    struct thrift_memory memory, comparison;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    struct test_packet *input = NULL, output = {0};
    size_t size;
    CHECK(test_const_sample_get(&input) == THRIFT_OK);
    CHECK(input->f_small == INT8_MIN && input->f_large == INT64_MIN);
    CHECK(input->f_short_value == INT16_MIN && input->f_number == INT32_MIN);
    CHECK(input->f_defaults.size == 2 && input->has_defaults);
    CHECK(thrift_memory_init(&memory, buffer, sizeof(buffer), 0, &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
    CHECK(test_packet_write(&protocol, input) == THRIFT_OK);
    CHECK(test_packet_read(&protocol, &output) == THRIFT_OK);
    CHECK(output.f_number == INT32_MIN && output.f_large == INT64_MIN);
    CHECK(output.f_ratio == -1.5 && output.f_values.size == 3);
    CHECK(((int32_t *)output.f_values.data)[1] == -2);
    CHECK(output.f_node && output.f_node->f_value == 42);
    CHECK(output.has_choice && output.f_choice->has_text && output.f_choice->f_text.size == 8);
    CHECK(output.f_external && output.f_external->f_value == 99);
    CHECK(output.f_identifier.data[0] == 0 && output.f_identifier.data[15] == 0xff);
    CHECK(((struct thrift_list *)output.f_index.values)[0].size == 2);
    CHECK(output.f_values.capacity == output.f_values.size);
    CHECK(thrift_memory_init(&comparison, second, sizeof(second), 0, &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
    CHECK(test_packet_write(&protocol, &output) == THRIFT_OK);
    CHECK(memory.size == comparison.size && memcmp(buffer, second, memory.size) == 0);
    /* Every truncation must fail without modifying the initialized destination. */
    for (size = 0; size < memory.size; ++size) {
        enum thrift_status status;
        struct thrift_memory truncated;
        CHECK(thrift_memory_init(&truncated, buffer, sizeof(buffer), size, &transport) == THRIFT_OK);
        CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
        status = test_packet_read(&protocol, &output);
        CHECK(status != THRIFT_OK);
        CHECK(output.f_node->f_value == 42 && output.f_text.size == 5);
    }
    /* Mutations exercise invalid tags, signed lengths, counts and nesting. */
    for (size = 0; size < memory.size; ++size) {
        struct thrift_memory mutated;
        enum thrift_status status;
        buffer[size] ^= 0x80;
        CHECK(thrift_memory_init(&mutated, buffer, sizeof(buffer), memory.size, &transport) == THRIFT_OK);
        CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
        protocol.limits.max_container = 32;
        protocol.limits.max_allocation = BUFFER_SIZE;
        thrift_protocol_reset(&protocol);
        status = test_packet_read(&protocol, &output);
        CHECK(status == THRIFT_OK || status == THRIFT_LIMIT || status == THRIFT_EOF ||
              status == THRIFT_PROTOCOL || status == THRIFT_REQUIRED);
        test_packet_clear(&output);
        buffer[size] ^= 0x80;
    }
    test_packet_clear(input);
    free(input);
    test_packet_clear(&output);
    test_packet_clear(&output);
    return EXIT_SUCCESS;
}

static int check_limits_and_defaults(enum thrift_protocol_kind kind)
{
    uint8_t buffer[BUFFER_SIZE];
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    struct test_packet *input = NULL, output = {0};
    struct test_default_choice choice = {0}, decoded = {0};
    struct thrift_map nested = {0};
    int32_t answer = 0;
    CHECK(test_const_answer_get(&answer) == THRIFT_OK && answer == 42);
    CHECK(test_const_nested_get(&nested) == THRIFT_OK);
    CHECK(nested.size == 1);
    CHECK(((struct thrift_list *)nested.values)[0].size == 2);
    /* Constants can be safely replaced without leaking the previous value. */
    CHECK(test_const_nested_get(&nested) == THRIFT_OK);
    test_const_nested_clear(&nested);
    CHECK(!nested.keys && !nested.values && !nested.size);
    CHECK(test_const_sample_get(&input) == THRIFT_OK);
    CHECK(thrift_memory_init(&memory, buffer, sizeof(buffer), 0, &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
    CHECK(test_packet_write(&protocol, input) == THRIFT_OK);
    protocol.limits.max_allocation = sizeof(output);
    thrift_protocol_reset(&protocol);
    CHECK(test_packet_read(&protocol, &output) == THRIFT_LIMIT);
    memory.position = 0;
    protocol.limits.max_allocation = BUFFER_SIZE;
    protocol.limits.max_depth = 1;
    thrift_protocol_reset(&protocol);
    CHECK(test_packet_read(&protocol, &output) == THRIFT_LIMIT);
    memory.position = 0;
    protocol.limits.max_depth = 64;
    protocol.limits.max_container = 1;
    thrift_protocol_reset(&protocol);
    CHECK(test_packet_read(&protocol, &output) == THRIFT_LIMIT);
    test_packet_clear(input);
    free(input);
    test_packet_clear(&output);
    CHECK(test_default_choice_init(&decoded) == THRIFT_OK);
    CHECK(decoded.has_number && decoded.f_number == 9);
    choice.has_text = true;
    CHECK(thrift_bytes_set(&choice.f_text, "union", 5) == THRIFT_OK);
    CHECK(thrift_memory_init(&memory, buffer, sizeof(buffer), 0, &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
    CHECK(test_default_choice_write(&protocol, &choice) == THRIFT_OK);
    CHECK(test_default_choice_read(&protocol, &decoded) == THRIFT_OK);
    CHECK(decoded.has_text && !decoded.has_number && decoded.f_text.size == 5);
    choice.has_number = true;
    CHECK(test_default_choice_write(&protocol, &choice) == THRIFT_INVALID);
    test_default_choice_clear(&choice);
    test_default_choice_clear(&decoded);
    return EXIT_SUCCESS;
}

struct loopback {
    struct thrift_memory request, response;
    struct thrift_transport request_io, response_io;
    struct test_echo_handler handler;
    int notifications;
    bool corrupt_sequence;
    enum thrift_protocol_kind kind;
    bool multiplexed;
    bool allow_default;
};

static enum thrift_status echo(void *user_context, const struct test_echo_echo_args *args,
                                  struct test_echo_echo_result *result)
{
    (void)user_context;
    if (!args->f_packet->f_enabled) {
        result->f_problem = calloc(1, sizeof(*result->f_problem));
        if (!result->f_problem)
            return THRIFT_NOMEM;
        result->has_problem = true;
        return thrift_bytes_set(&result->f_problem->f_reason, "disabled", 8);
    }
    result->has_success = true;
    return test_const_sample_get(&result->f_success);
}
static enum thrift_status inherited(void *user_context, const struct test_base_inherited_args *args,
                                       struct test_base_inherited_result *result)
{
    (void)user_context;
    result->f_success = args->f_value;
    result->has_success = true;
    return THRIFT_OK;
}
static enum thrift_status ping(void *user_context, const struct test_echo_ping_args *args,
                                  struct test_echo_ping_result *result)
{
    (void)user_context; (void)args; (void)result;
    return THRIFT_OK;
}
static enum thrift_status notify(void *user_context, const struct test_echo_notify_args *args,
                                    struct test_echo_notify_result *result)
{
    struct loopback *loopback = user_context;
    (void)result;
    loopback->notifications = args->f_value;
    return THRIFT_OK;
}
static enum thrift_status server_read(void *context, void *data, size_t size)
{
    struct loopback *loopback = context;
    return loopback->request_io.read(&loopback->request, data, size);
}
static enum thrift_status server_write(void *context, const void *data, size_t size)
{
    struct loopback *loopback = context;
    return loopback->response_io.write(&loopback->response, data, size);
}
static enum thrift_status client_read(void *context, void *data, size_t size)
{
    struct loopback *loopback = context;
    return loopback->response_io.read(&loopback->response, data, size);
}
static enum thrift_status client_write(void *context, const void *data, size_t size)
{
    struct loopback *loopback = context;
    return loopback->request_io.write(&loopback->request, data, size);
}
static enum thrift_status client_flush(void *context)
{
    struct loopback *loopback = context;
    struct thrift_transport transport = {loopback, server_read, server_write, NULL};
    struct thrift_protocol protocol;
    enum thrift_status status;
    loopback->response.size = 0;
    loopback->response.position = 0;
    status = thrift_protocol_init_kind(&protocol, transport, loopback->kind);
    if (status == THRIFT_OK) {
        if (loopback->multiplexed) {
            struct thrift_service services[REGISTERED_SERVICE_COUNT];
            services[0] = test_echo_service(&loopback->handler);
            services[1] = services[0];
            services[1].name = "Alternate";
            status = thrift_multiplexed_process(&protocol, services, REGISTERED_SERVICE_COUNT,
                                                     loopback->allow_default ? "Echo" : NULL);
        } else {
            status = test_echo_server_process(&protocol, &loopback->handler);
        }
    }
    loopback->request.size = 0;
    loopback->request.position = 0;
    if (loopback->corrupt_sequence && loopback->response.size > ECHO_BINARY_SEQUENCE_OFFSET)
        loopback->response.data[loopback->kind == THRIFT_COMPACT ? COMPACT_SEQUENCE_OFFSET : ECHO_BINARY_SEQUENCE_OFFSET] ^= 1; /* echo: version + name length + name + sequence */
    return status;
}

struct rpc_diagnostics {
    bool missing_callback;
    bool not_implemented;
};

static enum thrift_status capture_rpc_error(void *user_context, const struct thrift_log_event *event)
{
    struct rpc_diagnostics *diagnostics = user_context;
    if (strcmp(event->operation, "rpc.server.handler") == 0) {
        if (event->status == THRIFT_MISSING_CALLBACK && event->detail && strcmp(event->detail, "echo") == 0)
            diagnostics->missing_callback = true;
        if (event->status == THRIFT_NOT_IMPLEMENTED)
            diagnostics->not_implemented = true;
    }
    return THRIFT_OK;
}

static int check_rpc(enum thrift_protocol_kind kind, bool multiplexed)
{
    uint8_t request[BUFFER_SIZE], response[BUFFER_SIZE];
    struct loopback loopback = {0};
    struct thrift_transport transport = {&loopback, client_read, client_write, client_flush};
    struct thrift_protocol protocol;
    struct test_echo_client client = {0};
    struct rpc_diagnostics diagnostics = {0};
    struct test_echo_echo_args args = {0};
    struct test_echo_echo_result result = {0};
    struct test_echo_notify_args notification = {0};
    struct test_echo_ping_args ping_args = {0};
    struct test_echo_ping_result ping_result = {0};
    struct test_base_inherited_args base_args = {0};
    struct test_base_inherited_result base_result = {0};
    CHECK(thrift_memory_init(&loopback.request, request, sizeof(request), 0, &loopback.request_io) == THRIFT_OK);
    CHECK(thrift_memory_init(&loopback.response, response, sizeof(response), 0, &loopback.response_io) == THRIFT_OK);
    CHECK(thrift_log_configure(THRIFT_LOG_ERROR, capture_rpc_error, &diagnostics) == THRIFT_OK);
    loopback.kind = kind;
    loopback.multiplexed = multiplexed;
    loopback.allow_default = true;
    CHECK(test_echo_server_init(NULL, &loopback) == THRIFT_NULL_HANDLER);
    CHECK(test_echo_server_init(&loopback.handler, &loopback) == THRIFT_OK);
    CHECK(loopback.handler.user_context == &loopback);
    CHECK(test_echo_server_process(NULL, &loopback.handler) == THRIFT_NULL_PROTOCOL);
    CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
    if (multiplexed)
        CHECK(thrift_multiplexed_protocol_init(&protocol, transport, kind, "Echo") == THRIFT_OK);
    CHECK(test_echo_client_init(NULL, &protocol) == THRIFT_NULL_CLIENT);
    CHECK(test_echo_client_init(&client, &protocol) == THRIFT_OK);
    CHECK(test_echo_client_init(&client, NULL) == THRIFT_NULL_PROTOCOL);
    CHECK(client.protocol == &protocol && client.next_sequence == 1);
    CHECK(test_echo_client_ping_call(NULL, &ping_args, &ping_result) == THRIFT_NULL_CLIENT);
    {
        struct thrift_protocol uninitialized = {0};
        struct test_echo_client empty = {0};
        size_t request_size = loopback.request.size;
        CHECK(test_echo_client_init(&empty, &uninitialized) == THRIFT_PROTOCOL_UNINITIALIZED);
        CHECK(empty.protocol == NULL && empty.next_sequence == 0);
        CHECK(test_echo_client_ping_call(&empty, &ping_args, &ping_result) == THRIFT_NULL_PROTOCOL);
        empty.protocol = &uninitialized;
        CHECK(test_echo_client_ping_call(&empty, &ping_args, &ping_result) == THRIFT_PROTOCOL_UNINITIALIZED);
        CHECK(test_echo_server_process(&uninitialized, &loopback.handler) == THRIFT_PROTOCOL_UNINITIALIZED);
        CHECK(test_echo_server_process(&protocol, NULL) == THRIFT_NULL_HANDLER);
        client.next_sequence = 0;
        CHECK(test_echo_client_ping_call(&client, &ping_args, &ping_result) == THRIFT_INVALID_SEQUENCE);
        CHECK(client.next_sequence == 0 && loopback.request.size == request_size);
        client.next_sequence = 1;
        CHECK(test_echo_ping_call(NULL, 1, &ping_args, &ping_result) == THRIFT_NULL_PROTOCOL);
        CHECK(test_echo_ping_call(&uninitialized, 1, &ping_args, &ping_result) == THRIFT_PROTOCOL_UNINITIALIZED);
        CHECK(test_echo_ping_call(&protocol, 1, NULL, &ping_result) == THRIFT_NULL_ARGS);
        CHECK(test_echo_ping_call(&protocol, 1, &ping_args, NULL) == THRIFT_NULL_RESULT);
        CHECK(thrift_client_call(&protocol, NULL, 1, &ping_args, &ping_result) == THRIFT_NULL_METHOD);
        CHECK(test_echo_process(&protocol, NULL) == THRIFT_NULL_HANDLER);
        CHECK(thrift_process(&protocol, NULL, 1, &loopback.handler) == THRIFT_NULL_METHOD);
        CHECK(loopback.handler.f_ping(NULL, NULL, &ping_result) == THRIFT_NULL_ARGS);
        CHECK(loopback.handler.f_ping(NULL, &ping_args, NULL) == THRIFT_NULL_RESULT);
        CHECK(loopback.handler.f_ping(NULL, &ping_args, &ping_result) == THRIFT_NOT_IMPLEMENTED);
    }
    CHECK(test_echo_client_ping_call(&client, NULL, &ping_result) == THRIFT_NULL_ARGS);
    CHECK(test_echo_client_ping_call(&client, &ping_args, NULL) == THRIFT_NULL_RESULT);
    CHECK(client.next_sequence == 1);
    CHECK(test_echo_client_ping_call(&client, &ping_args, &ping_result) == THRIFT_REMOTE);
    CHECK(client.next_sequence == 2);
    CHECK(diagnostics.not_implemented && thrift_log_last_status() == THRIFT_OK);
    CHECK(test_echo_client_inherited_call(&client, &base_args, &base_result) == THRIFT_REMOTE);
    CHECK(loopback.handler.f_notify(loopback.handler.user_context, &notification, NULL) == THRIFT_NOT_IMPLEMENTED);
    loopback.handler.f_echo = echo;
    loopback.handler.f_ping = ping;
    loopback.handler.f_notify = notify;
    loopback.handler.f_inherited = inherited;
    if (multiplexed) {
        struct thrift_service duplicate[2];
        duplicate[0] = test_echo_service(&loopback.handler);
        duplicate[1] = duplicate[0];
        CHECK(thrift_multiplexed_process(&protocol, duplicate, 2, NULL) == THRIFT_INVALID);
        CHECK(thrift_multiplexed_process(&protocol, duplicate, 1, "Missing") == THRIFT_INVALID);
    }
    CHECK(test_const_sample_get(&args.f_packet) == THRIFT_OK);
    CHECK(test_echo_echo_call(&protocol, -1, &args, &result) == THRIFT_OK);
    CHECK(test_echo_client_echo_call(&client, &args, &result) == THRIFT_OK);
    CHECK(result.has_success && result.f_success->f_node->f_value == 42);
    args.f_packet->f_enabled = false;
    CHECK(test_echo_echo_call(&protocol, INT32_MIN, &args, &result) == THRIFT_OK);
    CHECK(test_echo_client_echo_call(&client, &args, &result) == THRIFT_OK);
    CHECK(result.has_problem && !result.has_success && result.f_problem->f_reason.size == 8);
    client.next_sequence = INT32_MAX; /* Exercise wraparound without billions of calls. */
    CHECK(test_echo_client_ping_call(&client, &ping_args, &ping_result) == THRIFT_OK);
    CHECK(client.next_sequence == 1);
    CHECK(test_echo_client_ping_call(&client, &ping_args, &ping_result) == THRIFT_OK);
    CHECK(client.next_sequence == 2);
    notification.f_value = 123;
    CHECK(test_echo_client_notify_call(&client, &notification, NULL) == THRIFT_OK);
    CHECK(loopback.notifications == 123 && loopback.response.size == 0);
    base_args.f_value = 456;
    CHECK(test_echo_client_inherited_call(&client, &base_args, &base_result) == THRIFT_OK);
    CHECK(base_result.has_success && base_result.f_success == 456);
    loopback.handler.f_echo = NULL;
    CHECK(test_echo_echo_call(&protocol, 5, &args, &result) == THRIFT_REMOTE);
    CHECK(diagnostics.missing_callback && thrift_log_last_status() == THRIFT_OK);
    loopback.handler.f_echo = echo;
    if (multiplexed) {
        protocol.service_name = "Alternate";
        CHECK(test_echo_inherited_call(&protocol, 5, &base_args, &base_result) == THRIFT_OK);
        protocol.service_name = "Missing";
        CHECK(test_echo_echo_call(&protocol, 5, &args, &result) == THRIFT_REMOTE);
        CHECK(test_echo_notify_call(&protocol, 5, &notification, NULL) == THRIFT_OK);
        CHECK(loopback.response.size == 0);
        protocol.service_name = NULL;
        CHECK(test_echo_inherited_call(&protocol, 5, &base_args, &base_result) == THRIFT_OK);
        loopback.allow_default = false;
        CHECK(test_echo_inherited_call(&protocol, 5, &base_args, &base_result) == THRIFT_REMOTE);
        protocol.service_name = "Echo";
    }
    loopback.corrupt_sequence = true;
    CHECK(test_echo_client_echo_call(&client, &args, &result) == THRIFT_PROTOCOL);
    test_echo_echo_args_clear(&args);
    test_echo_echo_result_clear(&result);
    CHECK(thrift_log_configure(THRIFT_LOG_INFO, NULL, NULL) == THRIFT_OK);
    return EXIT_SUCCESS;
}

static int check_enums(enum thrift_protocol_kind kind)
{
    uint8_t buffer[BUFFER_SIZE];
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    struct test_enum_values input = {0}, output = {0};
    enum test_color unknown = (enum test_color)-17;
    enum test_enum_boundary boundaries[] = {
        TEST_ENUM_BOUNDARY_MINIMUM, TEST_ENUM_BOUNDARY_MAXIMUM, TEST_ENUM_BOUNDARY_NEGATIVE
    };
    size_t i;
    _Static_assert(sizeof(enum test_color) == sizeof(int32_t), "color enum width");
    _Static_assert(sizeof(enum test_enum_boundary) == sizeof(int32_t), "boundary enum width");
    CHECK(test_enum_values_set_color(&input, unknown) == THRIFT_OK);
    CHECK(thrift_list_append(&input.f_colors, &unknown, sizeof(unknown), NULL) == THRIFT_OK);
    input.has_colors = true;
    for (i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        CHECK(test_enum_values_set_boundary(&input, boundaries[i]) == THRIFT_OK);
        CHECK(thrift_memory_init(&memory, buffer, sizeof(buffer), 0, &transport) == THRIFT_OK);
        CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
        CHECK(test_enum_values_write(&protocol, &input) == THRIFT_OK);
        CHECK(test_enum_values_read(&protocol, &output) == THRIFT_OK);
        CHECK(output.f_color == unknown && output.f_boundary == boundaries[i]);
        CHECK(output.f_colors.size == 1 && ((enum test_color *)output.f_colors.data)[0] == unknown);
        test_enum_values_clear(&output);
    }
    test_enum_values_clear(&input);
    return EXIT_SUCCESS;
}

static int check_generated_errors(void)
{
    struct test_packet *packet = NULL;
    struct test_packet existing = {0};
    struct test_node node = {0};
    struct test_owned_choice choice = {0};
    struct thrift_protocol protocol = {0};
    struct thrift_list list = {0};
    struct thrift_map map = {0};
    size_t budget = 0;
    int32_t element = 1;
    CHECK(test_packet_create(NULL, &budget) == THRIFT_NULL_VALUE);
    CHECK(test_packet_create(&packet, &budget) == THRIFT_LIMIT && packet == NULL);
    budget = sizeof(*packet);
    CHECK(test_packet_create(&packet, &budget) == THRIFT_LIMIT && packet == NULL);
    packet = &existing;
    budget = SIZE_MAX;
    CHECK(test_packet_create(&packet, &budget) == THRIFT_OUTPUT_NOT_EMPTY);
    CHECK(packet == &existing && budget == SIZE_MAX);
    packet = NULL;
    CHECK(test_packet_create(&packet, &budget) == THRIFT_OK);
    CHECK(packet->f_defaults.size == 2 && budget < SIZE_MAX - sizeof(*packet));
    CHECK(test_packet_init(NULL) == THRIFT_NULL_VALUE);
    CHECK(test_packet_read(NULL, packet) == THRIFT_NULL_PROTOCOL);
    CHECK(test_packet_write(&protocol, packet) == THRIFT_PROTOCOL_UNINITIALIZED);
    CHECK(test_node_set_next(&node, &node) == THRIFT_INVALID_OWNERSHIP);
    CHECK(!node.has_next && node.f_next == NULL);
    CHECK(test_packet_set_values(packet, NULL) == THRIFT_NULL_FIELD);
    list.size = 2;
    list.capacity = 1;
    CHECK(test_packet_set_values(packet, &list) == THRIFT_INVALID_CAPACITY);
    list.capacity = list.size;
    CHECK(test_packet_set_values(packet, &list) == THRIFT_NULL_DATA);
    CHECK(list.size == 2 && !packet->has_values);
    memset(&list, 0, sizeof(list));
    CHECK(thrift_list_append(&list, &element, sizeof(element), NULL) == THRIFT_OK);
    CHECK(test_packet_set_values(packet, &list) == THRIFT_OK);
    list = packet->f_values;
    CHECK(test_packet_set_values(packet, &list) == THRIFT_INVALID_OWNERSHIP);
    CHECK(list.data == packet->f_values.data && packet->f_values.size == 1);
    CHECK(test_owned_choice_set_node(&choice, test_node_new()) == THRIFT_OK);
    map.size = 1;
    CHECK(test_owned_choice_set_index(&choice, &map) == THRIFT_NULL_MAP_KEYS);
    map.keys = &element;
    CHECK(test_owned_choice_set_index(&choice, &map) == THRIFT_NULL_MAP_VALUES);
    map.values = &element;
    CHECK(test_owned_choice_set_index(&choice, &map) == THRIFT_INVALID_OWNERSHIP);
    CHECK(choice.has_node && !choice.has_index && map.size == 1);
    test_owned_choice_clear(&choice);
    test_packet_clear(packet);
    free(packet);
    return EXIT_SUCCESS;
}

static int check_setters(void)
{
    struct test_packet packet = {0};
    struct test_choice choice = {0};
    struct test_default_choice *defaults = test_default_choice_new();
    struct test_owned_choice owned = {0};
    struct test_echo_echo_result result = {0};
    struct test_node *node = test_node_new();
    struct test_problem *problem = test_problem_new();
    struct thrift_list names = {0};
    struct thrift_map index = {0};
    struct thrift_bytes text = {0};
    struct thrift_uuid uuid = {{0}};
    CHECK(defaults && node && problem);
    CHECK(defaults->has_number && defaults->f_number == 9);
    CHECK(test_default_choice_case(defaults) == TEST_DEFAULT_CHOICE_CASE_FIELD_NUMBER);
    CHECK(test_default_choice_set_text(defaults, "selected", 8) == THRIFT_OK);
    CHECK(!defaults->has_number && defaults->has_text);
    test_default_choice_clear(defaults);
    CHECK(test_default_choice_case(defaults) == TEST_DEFAULT_CHOICE_CASE_NONE);
    free(defaults);
    CHECK(test_packet_set_number(&packet, 42) == THRIFT_OK);
    CHECK(packet.has_number && packet.f_number == 42);
    CHECK(test_packet_set_number(NULL, 42) == THRIFT_NULL_VALUE);
    CHECK(test_packet_set_large(&packet, INT64_MAX) == THRIFT_OK);
    CHECK(test_packet_set_color(&packet, TEST_COLOR_RED) == THRIFT_OK);
    CHECK(test_packet_set_identifier(&packet, uuid) == THRIFT_OK);
    CHECK(test_packet_set_text(&packet, "literal", 7) == THRIFT_OK);
    CHECK(test_packet_set_text(&packet, packet.f_text.data + 1, 6) == THRIFT_OK);
    CHECK(packet.has_text && packet.f_text.size == 6 && memcmp(packet.f_text.data, "iteral", 6) == 0);
    CHECK(test_packet_set_text(&packet, NULL, 1) == THRIFT_NULL_DATA);
    CHECK(packet.f_text.size == 6);
    CHECK(test_packet_set_payload(&packet, "a\0b", 3) == THRIFT_OK);
    CHECK(packet.f_payload.size == 3 && packet.f_payload.data[1] == 0);
    CHECK(test_node_set_value(node, 9) == THRIFT_OK);
    CHECK(test_packet_set_node(&packet, node) == THRIFT_OK);
    CHECK(test_packet_set_node(&packet, node) == THRIFT_OK);
    CHECK(test_packet_set_node(&packet, NULL) == THRIFT_NULL_FIELD);
    CHECK(packet.has_node && packet.f_node == node);
    CHECK(test_packet_set_node(&packet, test_node_new()) == THRIFT_OK);
    CHECK(thrift_bytes_set_cstr(&text, "entry") == THRIFT_OK);
    CHECK(thrift_list_append(&names, &text, sizeof(text), NULL) == THRIFT_OK);
    memset(&text, 0, sizeof(text));
    CHECK(test_packet_set_names(&packet, &names) == THRIFT_OK);
    CHECK(names.data == NULL && names.size == 0 && packet.has_names);
    CHECK(test_packet_set_names(&packet, &packet.f_names) == THRIFT_OK);
    CHECK(packet.f_names.size == 1);
    CHECK(test_choice_case(&choice) == TEST_CHOICE_CASE_NONE);
    CHECK(test_choice_case(NULL) == TEST_CHOICE_CASE_INVALID);
    CHECK(test_choice_set_number(&choice, 17) == THRIFT_OK);
    CHECK(test_choice_set_text(&choice, "text", 4) == THRIFT_OK);
    CHECK(test_choice_case(&choice) == TEST_CHOICE_CASE_FIELD_TEXT && !choice.has_number);
    CHECK(test_choice_set_text(&choice, choice.f_text.data + 1, 3) == THRIFT_OK);
    CHECK(test_choice_set_text(&choice, "x", SIZE_MAX) == THRIFT_LIMIT);
    CHECK(choice.has_text && choice.f_text.size == 3);
    CHECK(test_choice_set_text(&choice, NULL, 1) == THRIFT_NULL_DATA);
    CHECK(choice.has_text && choice.f_text.size == 3);
    CHECK(test_choice_set_number(&choice, 23) == THRIFT_OK);
    CHECK(!choice.has_text && choice.f_text.data == NULL && choice.f_number == 23);
    choice.has_text = true; /* Manual access remains possible; diagnostics stay defensive. */
    CHECK(test_choice_case(&choice) == TEST_CHOICE_CASE_INVALID);
    CHECK(test_choice_set_text(&choice, NULL, 0) == THRIFT_OK);
    CHECK(test_choice_case(&choice) == TEST_CHOICE_CASE_FIELD_TEXT);
    CHECK(test_owned_choice_set_names(&owned, &packet.f_names) == THRIFT_OK);
    CHECK(test_owned_choice_set_names(&owned, &owned.f_names) == THRIFT_OK);
    CHECK(owned.has_names && owned.f_names.size == 1);
    CHECK(test_owned_choice_set_node(&owned, test_node_new()) == THRIFT_OK);
    CHECK(!owned.has_names && owned.f_names.data == NULL);
    CHECK(test_owned_choice_set_node(&owned, owned.f_node) == THRIFT_OK);
    CHECK(test_owned_choice_set_index(&owned, &index) == THRIFT_OK);
    CHECK(!owned.has_node && owned.f_node == NULL && owned.has_index);
    CHECK(test_problem_set_reason(problem, "failed", 6) == THRIFT_OK);
    CHECK(test_echo_echo_result_set_problem(&result, problem) == THRIFT_OK);
    CHECK(test_echo_echo_result_case(&result) == TEST_ECHO_ECHO_RESULT_CASE_FIELD_PROBLEM);
    CHECK(test_echo_echo_result_set_success(&result, test_packet_new()) == THRIFT_OK);
    CHECK(result.has_success && !result.has_problem && result.f_problem == NULL);
    CHECK(result.f_success->f_defaults.size == 2 && result.f_success->f_defaults.capacity == 2);
    test_echo_echo_result_clear(&result);
    test_owned_choice_clear(&owned);
    test_choice_clear(&choice);
    test_packet_clear(&packet);
    return EXIT_SUCCESS;
}

int main(void)
{
    enum thrift_protocol_kind kinds[] = {THRIFT_BINARY, THRIFT_COMPACT};
    size_t i;
    if (check_generated_errors()) return EXIT_FAILURE;
    if (check_setters()) return EXIT_FAILURE;
    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i)
        if (check_enums(kinds[i]) || check_roundtrip(kinds[i]) || check_limits_and_defaults(kinds[i]) ||
            check_rpc(kinds[i], false) || check_rpc(kinds[i], true))
            return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

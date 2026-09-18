/* SPDX-License-Identifier: Apache-2.0 */
#include "language_types.h"
#include "metadata_types.h"
#include <thrift/c11/transport/thrift_memory_buffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* All owned objects converge on cleanup, including failed assertions. */
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    result = EXIT_FAILURE; goto cleanup; \
} } while (0)

enum { WIRE_CAPACITY = 8192, ARGUMENT_COUNT = 3, SCENARIO_ARGUMENT = 1,
       PROTOCOL_ARGUMENT = 2, LEAF_METHOD_COUNT = 5, FETCH_METHOD_INDEX = 2,
       NOTIFY_METHOD_INDEX = 4, FETCH_RESULT_FIELD_COUNT = 3 };

static enum thrift_status roundtrip(enum thrift_protocol_kind kind,
    const struct thrift_record *record, const void *input, void *output)
{
    uint8_t bytes[WIRE_CAPACITY];
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    enum thrift_status status = thrift_memory_init(&memory, bytes, sizeof(bytes), 0, &transport);
    if (status == THRIFT_OK)
        status = thrift_protocol_init_kind(&protocol, transport, kind);
    if (status == THRIFT_OK)
        status = thrift_record_write(&protocol, record, input);
    if (status == THRIFT_OK)
        status = thrift_record_read(&protocol, record, output);
    if (status == THRIFT_OK && memory.position != memory.size)
        status = THRIFT_PROTOCOL;
    return status;
}

static int constants(enum thrift_protocol_kind kind)
{
    int result = EXIT_SUCCESS;
    bool boolean = false;
    int8_t byte = 0;
    int16_t short_value = 0;
    int32_t number = 0;
    int64_t large = 0;
    double fraction = 0;
    enum fallback_space_state enum_value = FALLBACK_SPACE_STATE_FIRST;
    struct thrift_bytes text = {0};
    struct thrift_uuid identifier = {{0}};
    struct thrift_map index = {0}, empty_map = {0};
    struct thrift_list members = {0}, empty_list = {0}, empty_set = {0};
    struct fallback_space_item *item = NULL, decoded = {0};
    const char expected_text[] = "line\n\"quote\"\\tail";
    const uint8_t expected_uuid[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    const int16_t expected_members[] = {1, -2, 3};
    CHECK(conform_const_yes_get(&boolean) == THRIFT_OK && boolean);
    CHECK(conform_const_no_get(&boolean) == THRIFT_OK && !boolean);
    CHECK(conform_const_byte_min_get(&byte) == THRIFT_OK && byte == INT8_MIN);
    CHECK(conform_const_byte_max_get(&byte) == THRIFT_OK && byte == INT8_MAX);
    CHECK(conform_const_short_min_get(&short_value) == THRIFT_OK && short_value == INT16_MIN);
    CHECK(conform_const_short_max_get(&short_value) == THRIFT_OK && short_value == INT16_MAX);
    CHECK(conform_const_int_minimum_get(&number) == THRIFT_OK && number == INT32_MIN);
    CHECK(conform_const_int_maximum_get(&number) == THRIFT_OK && number == INT32_MAX);
    CHECK(conform_const_long_min_get(&large) == THRIFT_OK && large == INT64_MIN);
    CHECK(conform_const_long_max_get(&large) == THRIFT_OK && large == INT64_MAX);
    CHECK(conform_const_hex_get(&number) == THRIFT_OK && number == 42);
    CHECK(conform_const_negative_hex_get(&number) == THRIFT_OK && number == -42);
    CHECK(conform_const_fraction_get(&fraction) == THRIFT_OK && fraction == -0.00125);
    CHECK(conform_const_integer_double_get(&fraction) == THRIFT_OK && fraction == 42.0);
    CHECK(conform_const_referenced_get(&number) == THRIFT_OK && number == 41);
    CHECK(conform_const_enum_value_get(&enum_value) == THRIFT_OK && enum_value == FALLBACK_SPACE_STATE_NEXT);
    CHECK(conform_const_escaped_get(&text) == THRIFT_OK);
    CHECK(text.size == sizeof(expected_text) - 1 && !memcmp(text.data, expected_text, text.size));
    CHECK(conform_const_octets_get(&text) == THRIFT_OK);
    CHECK(text.size == sizeof("abc") - 1 && !memcmp(text.data, "abc", text.size));
    CHECK(conform_const_id_get(&identifier) == THRIFT_OK);
    CHECK(!memcmp(identifier.data, expected_uuid, sizeof(expected_uuid)));
    CHECK(conform_const_item_get(&item) == THRIFT_OK && item->f_value == 41);
    CHECK(roundtrip(kind, &fallback_space_item_record, item, &decoded) == THRIFT_OK);
    CHECK(decoded.f_value == 41 && decoded.has_value);
    CHECK(conform_const_index_get(&index) == THRIFT_OK && index.size == 1);
    CHECK(((int32_t *)index.keys)[0] == FALLBACK_SPACE_STATE_SECOND);
    CHECK(((struct thrift_list *)index.values)[0].size == 2);
    CHECK(((struct fallback_space_item **)((struct thrift_list *)index.values)[0].data)[1]->f_value == 9);
    CHECK(conform_const_members_get(&members) == THRIFT_OK);
    CHECK(members.size == sizeof(expected_members) / sizeof(expected_members[0]));
    CHECK(!memcmp(members.data, expected_members, sizeof(expected_members)));
    CHECK(conform_const_empty_map_get(&empty_map) == THRIFT_OK && !empty_map.size);
    CHECK(conform_const_empty_list_get(&empty_list) == THRIFT_OK && !empty_list.size);
    CHECK(conform_const_empty_set_get(&empty_set) == THRIFT_OK && !empty_set.size);
cleanup:
    conform_const_escaped_clear(&text);
    conform_const_item_clear(&item);
    fallback_space_item_clear(&decoded);
    conform_const_index_clear(&index);
    conform_const_members_clear(&members);
    conform_const_empty_map_clear(&empty_map);
    conform_const_empty_list_clear(&empty_list);
    conform_const_empty_set_clear(&empty_set);
    return result;
}

static int defaults(enum thrift_protocol_kind kind)
{
    int result = EXIT_SUCCESS;
    struct conform_defaults input = {0}, output = {0};
    struct conform_defaults_peer peer = {0};
    uint8_t bytes[WIRE_CAPACITY];
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    struct conform_selection choice = {0}, decoded = {0};
    CHECK(conform_defaults_init(&input) == THRIFT_OK);
    CHECK(input.f_mandatory == 41 && input.f_ordinary == 17 && input.f_small == INT8_MIN);
    CHECK(input.has_optional_text && input.has_item && input.has_index);
    CHECK(input.f_item && input.f_item->f_value == 41 && input.f_index.size == 1);
    CHECK(input.f_identifier.data[THRIFT_UUID_SIZE - 1] == 0xff);
    CHECK(thrift_memory_init(&memory, bytes, sizeof(bytes), 0, &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
    CHECK(conform_defaults_write(&protocol, &input) == THRIFT_OK);
    CHECK(conform_defaults_peer_read(&protocol, &peer) == THRIFT_OK);
    CHECK(peer.has_optional_text && peer.f_optional_text.size == sizeof("default") - 1);
    CHECK(memcmp(peer.f_optional_text.data, "default", sizeof("default") - 1) == 0);
    CHECK(peer.has_small && peer.f_small == INT8_MIN);
    CHECK(peer.has_item && peer.f_item && peer.f_item->f_value == 41);
    CHECK(peer.has_index && peer.f_index.size == 1 && memory.position == memory.size);
    CHECK(roundtrip(kind, &conform_defaults_record, &input, &output) == THRIFT_OK);
    CHECK(output.has_mandatory && output.has_ordinary && output.has_optional_text);
    CHECK(output.f_optional_text.size == sizeof("default") - 1);
    CHECK(output.f_item != input.f_item && output.f_index.values != input.f_index.values);
    input.has_item = true;
    input.f_item->f_value = 99;
    input.has_index = true;
    CHECK(roundtrip(kind, &conform_defaults_record, &input, &output) == THRIFT_OK);
    CHECK(output.has_item && output.f_item->f_value == 99 && output.has_index);
    CHECK(conform_selection_init(&choice) == THRIFT_OK);
    CHECK(choice.has_number && choice.f_number == 23 && !choice.has_text);
    CHECK(roundtrip(kind, &conform_selection_record, &choice, &decoded) == THRIFT_OK);
    CHECK(decoded.has_number && decoded.f_number == 23);
    choice.has_number = false;
    choice.has_text = true;
    CHECK(thrift_bytes_set(&choice.f_text, "selected", sizeof("selected") - 1) == THRIFT_OK);
    CHECK(roundtrip(kind, &conform_selection_record, &choice, &decoded) == THRIFT_OK);
    CHECK(decoded.has_text && !decoded.has_number && decoded.f_text.size == sizeof("selected") - 1);
    choice.has_number = true;
    CHECK(roundtrip(kind, &conform_selection_record, &choice, &decoded) == THRIFT_INVALID);
cleanup:
    conform_defaults_clear(&input);
    conform_defaults_clear(&output);
    conform_defaults_peer_clear(&peer);
    conform_selection_clear(&choice);
    conform_selection_clear(&decoded);
    return result;
}

static int presence(enum thrift_protocol_kind kind)
{
    int result = EXIT_SUCCESS;
    uint8_t bytes[WIRE_CAPACITY];
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    struct conform_empty empty = {0};
    struct conform_defaults required = {0};
    struct conform_ids input = {0}, output = {0};
    struct conform_keywords keywords = {0}, read_keywords = {0};
    CHECK(conform_ids_record.fields[0].id == -1);
    CHECK(conform_ids_record.fields[1].id == -2);
    CHECK(conform_ids_record.fields[2].id == INT16_MAX);
    input.f_implicit_first = 123;
    input.f_implicit_second = -456;
    input.has_highest = true;
    input.f_highest = INT64_MIN;
    CHECK(roundtrip(kind, &conform_ids_record, &input, &output) == THRIFT_OK);
    CHECK(output.f_implicit_first == 123 && output.f_implicit_second == -456);
    CHECK(output.has_highest && output.f_highest == INT64_MIN);
    keywords.f_int = 7;
    keywords.f_bool = true;
    CHECK(roundtrip(kind, &conform_keywords_record, &keywords, &read_keywords) == THRIFT_OK);
    CHECK(read_keywords.f_int == 7 && read_keywords.f_bool);
    CHECK(thrift_memory_init(&memory, bytes, sizeof(bytes), 0, &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
    CHECK(conform_empty_write(&protocol, &empty) == THRIFT_OK);
    CHECK(conform_defaults_init(&required) == THRIFT_OK);
    CHECK(conform_defaults_read(&protocol, &required) == THRIFT_REQUIRED);
    CHECK(required.f_mandatory == 41 && required.f_item && required.f_item->f_value == 41);
cleanup:
    conform_defaults_clear(&required);
    conform_ids_clear(&output);
    conform_keywords_clear(&keywords);
    conform_keywords_clear(&read_keywords);
    return result;
}

static int recursion(enum thrift_protocol_kind kind)
{
    int result = EXIT_SUCCESS;
    struct conform_left input = {0}, output = {0};
    struct conform_tree tree = {0}, read_tree = {0};
    struct conform_tree **children;
    input.f_right = calloc(1, sizeof(*input.f_right));
    CHECK(input.f_right);
    input.has_right = true;
    input.f_right->f_left = calloc(1, sizeof(*input.f_right->f_left));
    CHECK(input.f_right->f_left);
    input.f_right->has_left = true;
    CHECK(roundtrip(kind, &conform_left_record, &input, &output) == THRIFT_OK);
    CHECK(output.has_right && output.f_right->has_left && !output.f_right->f_left->has_right);
    CHECK(output.f_right != input.f_right);
    tree.f_children.data = calloc(1, sizeof(*children));
    CHECK(tree.f_children.data);
    tree.f_children.size = 1;
    children = tree.f_children.data;
    children[0] = calloc(1, sizeof(*children[0]));
    CHECK(children[0]);
    children[0]->f_value = 73;
    CHECK(roundtrip(kind, &conform_tree_record, &tree, &read_tree) == THRIFT_OK);
    CHECK(read_tree.f_children.size == 1);
    CHECK(((struct conform_tree **)read_tree.f_children.data)[0]->f_value == 73);
cleanup:
    conform_left_clear(&input);
    conform_left_clear(&output);
    conform_tree_clear(&tree);
    conform_tree_clear(&read_tree);
    return result;
}

struct rpc_context {
    struct thrift_memory request, response;
    struct thrift_transport request_io, response_io;
    struct thrift_service service;
    enum thrift_protocol_kind kind;
    int8_t notified;
};
static enum thrift_status client_read(void *context, void *data, size_t size)
{
    struct rpc_context *rpc = context;
    return rpc->response_io.read(&rpc->response, data, size);
}
static enum thrift_status client_write(void *context, const void *data, size_t size)
{
    struct rpc_context *rpc = context;
    return rpc->request_io.write(&rpc->request, data, size);
}
static enum thrift_status server_read(void *context, void *data, size_t size)
{
    struct rpc_context *rpc = context;
    return rpc->request_io.read(&rpc->request, data, size);
}
static enum thrift_status server_write(void *context, const void *data, size_t size)
{
    struct rpc_context *rpc = context;
    return rpc->response_io.write(&rpc->response, data, size);
}
static enum thrift_status client_flush(void *context)
{
    struct rpc_context *rpc = context;
    struct thrift_transport transport = {rpc, server_read, server_write, NULL};
    struct thrift_protocol protocol;
    enum thrift_status status;
    rpc->response.size = rpc->response.position = 0;
    status = thrift_protocol_init_kind(&protocol, transport, rpc->kind);
    if (status == THRIFT_OK)
        status = thrift_process(&protocol, rpc->service.methods, rpc->service.method_count, rpc->service.handler);
    rpc->request.size = rpc->request.position = 0;
    return status;
}
static enum thrift_status inherited(void *user_context,
    const struct fallback_space_base_inherited_args *args,
    struct fallback_space_base_inherited_result *result)
{
    (void)user_context;
    result->has_success = true;
    result->f_success = args->f_value;
    return THRIFT_OK;
}
static enum thrift_status may_fail(void *user_context,
    const struct conform_leaf_may_fail_args *args,
    struct conform_leaf_may_fail_result *result)
{
    (void)user_context;
    (void)args;
    result->f_other = calloc(1, sizeof(*result->f_other));
    if (!result->f_other)
        return THRIFT_NOMEM;
    result->has_other = true;
    result->f_other->f_code = 67;
    return THRIFT_OK;
}
static enum thrift_status notify(void *user_context,
    const struct conform_leaf_notify_args *args,
    struct conform_leaf_notify_result *result)
{
    struct rpc_context *rpc = user_context;
    (void)result;
    rpc->notified = args->f_value;
    return THRIFT_OK;
}
static int services(enum thrift_protocol_kind kind)
{
    int result = EXIT_SUCCESS;
    uint8_t request[WIRE_CAPACITY], response[WIRE_CAPACITY];
    struct rpc_context rpc = {0};
    struct conform_leaf_handler handler = {0};
    struct conform_empty_service_handler empty_handler = {0};
    struct thrift_service empty = conform_empty_service_service(&empty_handler);
    struct thrift_transport transport = {&rpc, client_read, client_write, client_flush};
    struct thrift_protocol protocol;
    struct fallback_space_base_inherited_args args = {0};
    struct fallback_space_base_inherited_result reply = {0};
    struct conform_leaf_may_fail_args failure_args = {0};
    struct conform_leaf_may_fail_result failure = {0};
    struct conform_leaf_notify_args notification = {0};
    handler.user_context = &rpc;
    handler.f_inherited = inherited;
    handler.f_may_fail = may_fail;
    handler.f_notify = notify;
    rpc.service = conform_leaf_service(&handler);
    rpc.kind = kind;
    CHECK(empty.method_count == 0 && !empty.methods);
    CHECK(rpc.service.method_count == LEAF_METHOD_COUNT);
    CHECK(!strcmp(rpc.service.methods[FETCH_METHOD_INDEX].name, "fetch"));
    CHECK(rpc.service.methods[FETCH_METHOD_INDEX].result->field_count == FETCH_RESULT_FIELD_COUNT);
    CHECK(rpc.service.methods[FETCH_METHOD_INDEX].returns_value);
    CHECK(rpc.service.methods[NOTIFY_METHOD_INDEX].oneway);
    CHECK(thrift_memory_init(&rpc.request, request, sizeof(request), 0, &rpc.request_io) == THRIFT_OK);
    CHECK(thrift_memory_init(&rpc.response, response, sizeof(response), 0, &rpc.response_io) == THRIFT_OK);
    CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
    args.f_value = -123;
    CHECK(conform_leaf_inherited_call(&protocol, INT32_MIN, &args, &reply) == THRIFT_OK);
    CHECK(reply.has_success && reply.f_success == -123);
    CHECK(conform_leaf_may_fail_call(&protocol, 0, &failure_args, &failure) == THRIFT_OK);
    CHECK(failure.has_other && failure.f_other && failure.f_other->f_code == 67);
    notification.f_value = INT8_MIN;
    CHECK(conform_leaf_notify_call(&protocol, INT32_MAX, &notification, NULL) == THRIFT_OK);
    CHECK(rpc.notified == INT8_MIN && rpc.response.size == 0);
cleanup:
    fallback_space_base_inherited_result_clear(&reply);
    conform_leaf_may_fail_result_clear(&failure);
    return result;
}

static int metadata(enum thrift_protocol_kind kind)
{
    int result = EXIT_SUCCESS;
    struct metadata_extended_attribute attribute = {0}, decoded = {0};
    struct metadata_annotated_record input = {0}, output = {0};
    struct metadata_annotated_service_handler handler = {0};
    struct thrift_service service = metadata_annotated_service_service(&handler);
    CHECK(metadata_annotated_record_init(&input) == THRIFT_OK);
    CHECK(input.f_value == 7 && input.has_value);
    CHECK(service.method_count == 1 && service.methods[0].oneway);
    CHECK(roundtrip(kind, &metadata_annotated_record_record, &input, &output) == THRIFT_OK);
    CHECK(output.f_value == 7 && output.has_value && !output.has_reference);
    attribute.f_extended_type = METADATA_EXTENDED_ATTRIBUTE_TYPE_CALCULATED;
    attribute.has_extended_type = true;
    CHECK(roundtrip(kind, &metadata_extended_attribute_record,
        &attribute, &decoded) == THRIFT_OK);
    CHECK(decoded.has_extended_type && decoded.f_extended_type == attribute.f_extended_type);
    CHECK(metadata_extended_attribute_type_descriptor.record ==
        &metadata_extended_attribute_record);
cleanup:
    metadata_extended_attribute_clear(&attribute);
    metadata_extended_attribute_clear(&decoded);
    metadata_annotated_record_clear(&input);
    metadata_annotated_record_clear(&output);
    return result;
}

int main(int argc, char **argv)
{
    const struct { const char *name; int (*run)(enum thrift_protocol_kind); } cases[] = {
        {"constants", constants}, {"defaults", defaults}, {"presence", presence},
        {"recursion", recursion}, {"services", services}, {"metadata", metadata}
    };
    enum thrift_protocol_kind kind;
    size_t index;
    if (argc != ARGUMENT_COUNT)
        return EXIT_FAILURE;
    if (!strcmp(argv[PROTOCOL_ARGUMENT], "binary"))
        kind = THRIFT_BINARY;
    else if (!strcmp(argv[PROTOCOL_ARGUMENT], "compact"))
        kind = THRIFT_COMPACT;
    else
        return EXIT_FAILURE;
    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
        if (!strcmp(argv[SCENARIO_ARGUMENT], cases[index].name))
            return cases[index].run(kind);
    return EXIT_FAILURE;
}

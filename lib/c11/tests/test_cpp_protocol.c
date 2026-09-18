/* SPDX-License-Identifier: Apache-2.0 */
/* Adapted from lib/cpp/test/AllProtocolTests.tcc, OptionalRequiredTest.cpp
 * and ThrifttReadCheckTests.cpp. See cpp_test_adaptations.md. */
#include <thrift/c11/processor/thrift_processor.h>
#include <thrift/c11/transport/thrift_memory_buffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return EXIT_FAILURE; } } while (0)
enum { WIRE_CAPACITY = 16384, ELEMENT_COUNT = 256, SCALAR_FIELD_ID = 15,
       COMPACT_INLINE_COUNT_LIMIT = 15, VARINT_BYTE_BOUNDARY = 128,
       HOSTILE_BYTE_BUDGET = 1024, SCENARIO_ARGUMENT = 1,
       PROTOCOL_ARGUMENT = 2, ARGUMENT_COUNT = 3, I64_SIGN_BIT = 63,
       FIELD_OPTIONAL = 0, FIELD_DEFAULT = 1, FIELD_REQUIRED = 2, REQUIREMENT_COUNT = 3,
       RPC_NORMAL = 0, RPC_ONEWAY = 1, RPC_EXCEPTION = 2, RPC_MODE_COUNT = 3 };
union payload {
    bool boolean;
    int8_t byte;
    int16_t short_value;
    int32_t integer;
    int64_t long_value;
    double real;
    struct thrift_bytes bytes;
    struct thrift_uuid uuid;
    struct thrift_list list;
    struct thrift_map map;
};
struct field_value { union payload value; bool present; };

static int scalar(enum thrift_protocol_kind kind, const struct thrift_type *type,
                  const void *value, int16_t id)
{
    struct thrift_field field = {id, offsetof(struct field_value, value),
        offsetof(struct field_value, present), true, false, type};
    struct thrift_record record = {sizeof(struct field_value), 1, &field, false, NULL};
    struct field_value input = {0}, output = {0};
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    uint8_t wire[WIRE_CAPACITY];
    memcpy(&input.value, value, type->size);
    input.present = true;
    CHECK(thrift_memory_init(&memory, wire, sizeof(wire), 0, &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
    CHECK(thrift_record_write(&protocol, &record, &input) == THRIFT_OK);
    CHECK(thrift_record_read(&protocol, &record, &output) == THRIFT_OK);
    CHECK(output.present && memory.position == memory.size);
    if (type->wire == THRIFT_STRING) {
        CHECK(output.value.bytes.size == input.value.bytes.size);
        CHECK(!input.value.bytes.size || !memcmp(input.value.bytes.data,
              output.value.bytes.data, input.value.bytes.size));
    } else {
        /* Compare double bits as well: signed zero and NaNs must survive. */
        CHECK(!memcmp(&input.value, &output.value, type->size));
    }
    thrift_record_clear(&record, &output);
    return EXIT_SUCCESS;
}

static int scalars(enum thrift_protocol_kind kind)
{
    static const int16_t shorts[] = {0, 1, -1, 7, -7, 150, -150, 15000, -15000, INT16_MIN, INT16_MAX};
    static const int32_t integers[] = {0, 1, -1, 15000, -15000, 31337, 65535, -65535,
        16777215, -16777215, INT32_MIN, INT32_MAX};
    static const int16_t ids[] = {INT16_MIN, -1, 0, 1, 15, 16, INT16_MAX};
    static const uint64_t double_bits[] = {UINT64_C(0), UINT64_C(0x8000000000000000),
        UINT64_C(0x3ff0000000000000), UINT64_C(0x405edd2f1a9fbe77),
        UINT64_C(1), UINT64_C(0x7fefffffffffffff), UINT64_C(0x7ff0000000000000),
        UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000000042)};
    struct thrift_uuid uuid = {{0x5e, 0x2a, 0xb1, 0x88, 0x17, 0x26, 0x4e, 0x75,
                                   0xa0, 0x4f, 0x1e, 0xd9, 0xa6, 0xa8, 0x9c, 0x4c}};
    uint8_t bytes[ELEMENT_COUNT];
    size_t index, id_index;
    int number, bit;
    for (number = INT8_MIN; number <= INT8_MAX; ++number) {
        int8_t value = (int8_t)number;
        CHECK(scalar(kind, &thrift_type_byte, &value, SCALAR_FIELD_ID) == EXIT_SUCCESS);
    }
    for (index = 0; index < sizeof(shorts) / sizeof(shorts[0]); ++index)
        CHECK(scalar(kind, &thrift_type_i16, &shorts[index], SCALAR_FIELD_ID) == EXIT_SUCCESS);
    for (index = 0; index < sizeof(integers) / sizeof(integers[0]); ++index)
        for (id_index = 0; id_index < sizeof(ids) / sizeof(ids[0]); ++id_index)
            CHECK(scalar(kind, &thrift_type_i32, &integers[index], ids[id_index]) == EXIT_SUCCESS);
    for (bit = 0; bit < I64_SIGN_BIT; ++bit) {
        int64_t value = INT64_C(1) << bit;
        CHECK(scalar(kind, &thrift_type_i64, &value, SCALAR_FIELD_ID) == EXIT_SUCCESS);
        value = -value;
        CHECK(scalar(kind, &thrift_type_i64, &value, SCALAR_FIELD_ID) == EXIT_SUCCESS);
    }
    {
        const int64_t minimum = INT64_MIN, maximum = INT64_MAX, zero = 0;
        CHECK(scalar(kind, &thrift_type_i64, &minimum, SCALAR_FIELD_ID) == EXIT_SUCCESS);
        CHECK(scalar(kind, &thrift_type_i64, &maximum, SCALAR_FIELD_ID) == EXIT_SUCCESS);
        CHECK(scalar(kind, &thrift_type_i64, &zero, SCALAR_FIELD_ID) == EXIT_SUCCESS);
    }
    for (index = 0; index < sizeof(double_bits) / sizeof(double_bits[0]); ++index) {
        double value;
        memcpy(&value, &double_bits[index], sizeof(value));
        CHECK(scalar(kind, &thrift_type_double, &value, SCALAR_FIELD_ID) == EXIT_SUCCESS);
    }
    for (index = 0; index < sizeof(bytes); ++index)
        bytes[index] = (uint8_t)index;
    for (index = 0; index <= sizeof(bytes); ++index) {
        struct thrift_bytes value = {index, bytes};
        CHECK(scalar(kind, &thrift_type_string, &value, SCALAR_FIELD_ID) == EXIT_SUCCESS);
    }
    for (number = 0; number < 2; ++number) {
        bool value = number != 0;
        CHECK(scalar(kind, &thrift_type_bool, &value, SCALAR_FIELD_ID) == EXIT_SUCCESS);
    }
    CHECK(scalar(kind, &thrift_type_uuid, &uuid, SCALAR_FIELD_ID) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int container(enum thrift_protocol_kind kind, enum thrift_wire wire_type, size_t count)
{
    struct thrift_type type = {wire_type,
        wire_type == THRIFT_MAP ? sizeof(struct thrift_map) : sizeof(struct thrift_list),
        NULL, &thrift_type_i32, &thrift_type_uuid};
    struct thrift_field field = {1, offsetof(struct field_value, value),
        offsetof(struct field_value, present), true, false, &type};
    struct thrift_record record = {sizeof(struct field_value), 1, &field, false, NULL};
    struct thrift_record unknown = {sizeof(struct field_value), 0, NULL, false, NULL};
    struct field_value input = {0}, output = {0};
    struct thrift_uuid values[ELEMENT_COUNT];
    int32_t keys[ELEMENT_COUNT];
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    uint8_t wire[WIRE_CAPACITY];
    size_t index, encoded_size;
    for (index = 0; index < count; ++index) {
        keys[index] = (int32_t)index;
        memset(values[index].data, (int)(index & 255), sizeof(values[index].data));
    }
    if (wire_type == THRIFT_MAP) {
        input.value.map.size = count;
        input.value.map.keys = keys;
        input.value.map.values = values;
    } else {
        input.value.list.size = count;
        input.value.list.data = values;
    }
    input.present = true;
    CHECK(thrift_memory_init(&memory, wire, sizeof(wire), 0, &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
    CHECK(thrift_record_write(&protocol, &record, &input) == THRIFT_OK);
    encoded_size = memory.size;
    protocol.limits.max_bytes = encoded_size;
    thrift_protocol_reset(&protocol);
    CHECK(thrift_record_read(&protocol, &record, &output) == THRIFT_OK);
    CHECK(output.present && memory.position == encoded_size);
    if (wire_type == THRIFT_MAP) {
        CHECK(output.value.map.size == count);
        CHECK(!count || !memcmp(output.value.map.keys, keys, count * sizeof(keys[0])));
        CHECK(!count || !memcmp(output.value.map.values, values, count * sizeof(values[0])));
    } else {
        CHECK(output.value.list.size == count);
        CHECK(!count || !memcmp(output.value.list.data, values, count * sizeof(values[0])));
    }
    thrift_record_clear(&record, &output);
    /* The same message must also be skippable by an older empty schema. */
    memory.position = 0;
    thrift_protocol_reset(&protocol);
    CHECK(thrift_record_read(&protocol, &unknown, &output) == THRIFT_OK);
    CHECK(memory.position == encoded_size);
    memory.position = 0;
    protocol.limits.max_bytes = encoded_size - 1;
    thrift_protocol_reset(&protocol);
    CHECK(thrift_record_read(&protocol, &record, &output) == THRIFT_LIMIT);
    CHECK(!output.present && !output.value.list.data);
    memory.size = memory.position = 0;
    thrift_protocol_reset(&protocol);
    CHECK(thrift_record_write(&protocol, &record, &input) == THRIFT_LIMIT);
    memory.size = memory.position = 0;
    protocol.limits.max_bytes = encoded_size;
    thrift_protocol_reset(&protocol);
    CHECK(thrift_record_write(&protocol, &record, &input) == THRIFT_OK);
    if (count) {
        protocol.limits.max_container = count - 1;
        thrift_protocol_reset(&protocol);
        CHECK(thrift_record_read(&protocol, &record, &output) == THRIFT_LIMIT);
        CHECK(!output.present && !output.value.list.data);
        memory.position = 0;
        thrift_protocol_reset(&protocol);
        CHECK(thrift_record_read(&protocol, &unknown, &output) == THRIFT_LIMIT);
    }
    return EXIT_SUCCESS;
}

static int containers(enum thrift_protocol_kind kind)
{
    static const size_t counts[] = {0, 1, COMPACT_INLINE_COUNT_LIMIT - 1, COMPACT_INLINE_COUNT_LIMIT,
        COMPACT_INLINE_COUNT_LIMIT + 1, VARINT_BYTE_BOUNDARY - 1, VARINT_BYTE_BOUNDARY,
        ELEMENT_COUNT - 1, ELEMENT_COUNT};
    static const enum thrift_wire types[] = {THRIFT_LIST, THRIFT_SET, THRIFT_MAP};
    size_t count, type;
    for (type = 0; type < sizeof(types) / sizeof(types[0]); ++type)
        for (count = 0; count < sizeof(counts) / sizeof(counts[0]); ++count)
            CHECK(container(kind, types[type], counts[count]) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int schema(enum thrift_protocol_kind kind)
{
    struct thrift_field field = {1, offsetof(struct field_value, value),
        offsetof(struct field_value, present), false, true, &thrift_type_i32};
    struct thrift_record record = {sizeof(struct field_value), 1, &field, false, NULL};
    struct field_value input = {0}, output = {0};
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    uint8_t wire[WIRE_CAPACITY];
    bool present;
    unsigned requirement;
    for (requirement = 0; requirement < REQUIREMENT_COUNT; ++requirement) {
        for (present = false;; present = true) {
            input.value.integer = 42;
            input.present = present;
            field.required = false;
            field.optional = true;
            CHECK(thrift_memory_init(&memory, wire, sizeof(wire), 0, &transport) == THRIFT_OK);
            CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
            CHECK(thrift_record_write(&protocol, &record, &input) == THRIFT_OK);
            field.required = requirement == FIELD_REQUIRED;
            field.optional = requirement == FIELD_OPTIONAL;
            output.value.integer = 99;
            output.present = true;
            CHECK(thrift_record_read(&protocol, &record, &output) ==
                  (field.required && !present ? THRIFT_REQUIRED : THRIFT_OK));
            CHECK(output.present == (present || field.required));
            CHECK(output.value.integer == (present ? 42 : field.required ? 99 : 0));
            /* A changed wire type is skipped, but cannot satisfy required. */
            memory.position = 0;
            thrift_protocol_reset(&protocol);
            field.type = &thrift_type_i64;
            output.value.long_value = 99;
            output.present = true;
            CHECK(thrift_record_read(&protocol, &record, &output) ==
                  (field.required ? THRIFT_REQUIRED : THRIFT_OK));
            CHECK(output.value.long_value == (field.required ? 99 : 0));
            CHECK(output.present == field.required);
            field.type = &thrift_type_i32;
            if (present)
                break;
        }
    }
    return EXIT_SUCCESS;
}

static int hostile(enum thrift_protocol_kind kind)
{
    /* C++ read-check regressions: a 32-bit product formerly wrapped at 4 GiB.
     * Put containers in an unknown field to exercise the skip path as well. */
    static const uint8_t binary[] = {15, 0, 1, 8, 0x40, 0, 0, 0};
    static const uint8_t compact[] = {0x19, 0xfd, 0x80, 0x80, 0x80, 0x80, 1};
    struct thrift_type type = {THRIFT_LIST, sizeof(struct thrift_list),
        NULL, NULL, kind == THRIFT_BINARY ? &thrift_type_i32 : &thrift_type_uuid};
    struct thrift_field field = {1, offsetof(struct field_value, value),
        offsetof(struct field_value, present), false, true, &type};
    struct thrift_record record = {sizeof(struct field_value), 1, &field, false, NULL};
    struct field_value output = {0};
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    uint8_t wire[sizeof(binary)];
    size_t size = kind == THRIFT_BINARY ? sizeof(binary) : sizeof(compact);
    unsigned mode;
    memcpy(wire, kind == THRIFT_BINARY ? binary : compact, size);
    for (mode = 0; mode < 2; ++mode) {
        record.field_count = mode;
        CHECK(thrift_memory_init(&memory, wire, sizeof(wire), size, &transport) == THRIFT_OK);
        CHECK(thrift_protocol_init_kind(&protocol, transport, kind) == THRIFT_OK);
        protocol.limits.max_container = SIZE_MAX;
        protocol.limits.max_bytes = HOSTILE_BYTE_BUDGET;
        protocol.limits.max_allocation = HOSTILE_BYTE_BUDGET;
        thrift_protocol_reset(&protocol);
        CHECK(thrift_record_read(&protocol, &record, &output) == THRIFT_LIMIT);
        CHECK(!output.present && !output.value.list.data);
    }
    return EXIT_SUCCESS;
}

struct rpc_loop {
    struct thrift_memory request, response;
    struct thrift_transport request_io, response_io;
    struct thrift_method method;
    enum thrift_protocol_kind kind;
    size_t calls;
    bool fail;
};
static enum thrift_status rpc_invoke(void *context, const void *args, void *result)
{
    struct rpc_loop *loop = context;
    (void)args; (void)result;
    ++loop->calls;
    return loop->fail ? THRIFT_IO : THRIFT_OK;
}
static enum thrift_status rpc_request_read(void *context, void *data, size_t size)
{
    struct rpc_loop *loop = context;
    return loop->request_io.read(&loop->request, data, size);
}
static enum thrift_status rpc_request_write(void *context, const void *data, size_t size)
{
    struct rpc_loop *loop = context;
    return loop->request_io.write(&loop->request, data, size);
}
static enum thrift_status rpc_response_read(void *context, void *data, size_t size)
{
    struct rpc_loop *loop = context;
    return loop->response_io.read(&loop->response, data, size);
}
static enum thrift_status rpc_response_write(void *context, const void *data, size_t size)
{
    struct rpc_loop *loop = context;
    return loop->response_io.write(&loop->response, data, size);
}
static enum thrift_status rpc_flush(void *context)
{
    struct rpc_loop *loop = context;
    struct thrift_transport server_io = {loop, rpc_request_read, rpc_response_write, NULL};
    struct thrift_protocol server;
    enum thrift_status status;
    loop->response.position = loop->response.size = 0;
    status = thrift_protocol_init_kind(&server, server_io, loop->kind);
    if (status == THRIFT_OK)
        status = thrift_process(&server, &loop->method, 1, loop);
    if (status == THRIFT_OK && loop->request.position != loop->request.size)
        status = THRIFT_PROTOCOL;
    loop->request.position = loop->request.size = 0;
    return status;
}
static int messages(enum thrift_protocol_kind kind)
{
    /* Message names/sequences from AllProtocolTests::testMessage, extended
     * with signed sequence extrema and application exceptions. */
    static const char *const names[] = {"short message name", "1",
        "loooooooooooooooooooooooooooooooooong", "one way push", "Janky"};
    static const int32_t sequences[] = {0, 12345, 65536, 12, -1, INT32_MIN, INT32_MAX};
    const struct thrift_record empty = {sizeof(struct field_value), 0, NULL, false, NULL};
    struct rpc_loop loop = {0};
    struct thrift_transport client_io = {&loop, rpc_response_read, rpc_request_write, rpc_flush};
    struct thrift_protocol client;
    struct field_value args = {0}, result = {0};
    uint8_t request[WIRE_CAPACITY], response[WIRE_CAPACITY];
    size_t name, sequence, calls = 0;
    unsigned mode;
    CHECK(thrift_memory_init(&loop.request, request, sizeof(request), 0, &loop.request_io) == THRIFT_OK);
    CHECK(thrift_memory_init(&loop.response, response, sizeof(response), 0, &loop.response_io) == THRIFT_OK);
    loop.kind = kind;
    loop.method.args = loop.method.result = &empty;
    loop.method.invoke = rpc_invoke;
    CHECK(thrift_protocol_init_kind(&client, client_io, kind) == THRIFT_OK);
    for (name = 0; name < sizeof(names) / sizeof(names[0]); ++name) {
        loop.method.name = names[name];
        for (sequence = 0; sequence < sizeof(sequences) / sizeof(sequences[0]); ++sequence) {
            for (mode = 0; mode < RPC_MODE_COUNT; ++mode) {
                loop.method.oneway = mode == RPC_ONEWAY;
                loop.fail = mode == RPC_EXCEPTION;
                CHECK(thrift_client_call(&client, &loop.method, sequences[sequence], &args,
                    loop.method.oneway ? NULL : &result) == (loop.fail ? THRIFT_REMOTE : THRIFT_OK));
                CHECK(loop.calls == ++calls);
                CHECK(loop.response.position == loop.response.size);
                CHECK(loop.method.oneway ? loop.response.size == 0 : loop.response.size > 0);
            }
        }
    }
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    enum thrift_protocol_kind kind;
    CHECK(argc == ARGUMENT_COUNT);
    CHECK(!strcmp(argv[PROTOCOL_ARGUMENT], "binary") || !strcmp(argv[PROTOCOL_ARGUMENT], "compact"));
    kind = !strcmp(argv[PROTOCOL_ARGUMENT], "binary") ? THRIFT_BINARY : THRIFT_COMPACT;
    if (!strcmp(argv[SCENARIO_ARGUMENT], "messages")) return messages(kind);
    if (!strcmp(argv[SCENARIO_ARGUMENT], "scalars")) return scalars(kind);
    if (!strcmp(argv[SCENARIO_ARGUMENT], "containers")) return containers(kind);
    if (!strcmp(argv[SCENARIO_ARGUMENT], "schema")) return schema(kind);
    if (!strcmp(argv[SCENARIO_ARGUMENT], "hostile")) return hostile(kind);
    return EXIT_FAILURE;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/protocol/thrift_binary_protocol.h>
#include <thrift/c11/protocol/thrift_compact_protocol.h>
#include <thrift/c11/protocol/thrift_multiplexed_protocol.h>
#include <thrift/c11/processor/thrift_processor.h>
#include <thrift/c11/transport/thrift_memory_buffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The repository has no cpro/Unity test infrastructure. Keep checks active in
 * release builds and use CTest without adding a production dependency. */
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return EXIT_FAILURE; \
} } while (0)

struct sample { int32_t value; bool present; };
static const struct thrift_field fields[] = {
    {1, offsetof(struct sample, value), offsetof(struct sample, present), true, false, &thrift_type_i32}
};
static const struct thrift_record record = {sizeof(struct sample), 1, fields, false, NULL};

enum { BUFFER_SIZE = 128 };

static int check_wire(void)
{
    const uint8_t expected[] = {THRIFT_I32, 0, 1, 0x80, 0, 0, 0, THRIFT_STOP};
    uint8_t buffer[BUFFER_SIZE];
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    struct sample input = {INT32_MIN, true}, output = {7, false};
    CHECK(thrift_memory_init(&memory, buffer, sizeof(buffer), 0, &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init(&protocol, transport) == THRIFT_OK);
    CHECK(thrift_record_write(&protocol, &record, &input) == THRIFT_OK);
    CHECK(memory.size == sizeof(expected));
    CHECK(memcmp(buffer, expected, sizeof(expected)) == 0);
    CHECK(thrift_record_read(&protocol, &record, &output) == THRIFT_OK);
    CHECK(output.present && output.value == INT32_MIN);
    return EXIT_SUCCESS;
}

static int check_errors(void)
{
    uint8_t buffer[] = {THRIFT_I32, 0, 1, 0, 0, 0, 42, THRIFT_STOP};
    uint8_t unknown[] = {THRIFT_STRING, 0, 99, 0, 0, 0, 3, 'a', 0, 'b',
                         THRIFT_I32, 0, 1, 0, 0, 0, 42, THRIFT_STOP};
    uint8_t duplicate[] = {THRIFT_I32, 0, 1, 0, 0, 0, 1,
                           THRIFT_I32, 0, 1, 0, 0, 0, 2, THRIFT_STOP};
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    struct sample output = {7, true};
    size_t size;
    for (size = 0; size < sizeof(buffer); ++size) {
        CHECK(thrift_memory_init(&memory, buffer, sizeof(buffer), size, &transport) == THRIFT_OK);
        CHECK(thrift_protocol_init(&protocol, transport) == THRIFT_OK);
        CHECK(thrift_record_read(&protocol, &record, &output) == THRIFT_EOF);
        CHECK(output.value == 7 && output.present);
    }
    CHECK(thrift_memory_init(&memory, unknown, sizeof(unknown), sizeof(unknown), &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init(&protocol, transport) == THRIFT_OK);
    CHECK(thrift_record_read(&protocol, &record, &output) == THRIFT_OK);
    CHECK(output.value == 42);
    CHECK(thrift_memory_init(&memory, duplicate, sizeof(duplicate), sizeof(duplicate), &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init(&protocol, transport) == THRIFT_OK);
    CHECK(thrift_record_read(&protocol, &record, &output) == THRIFT_PROTOCOL);
    CHECK(output.value == 42);
    buffer[0] = THRIFT_STOP;
    CHECK(thrift_memory_init(&memory, buffer, sizeof(buffer), sizeof(buffer), &transport) == THRIFT_OK);
    CHECK(thrift_protocol_init(&protocol, transport) == THRIFT_OK);
    CHECK(thrift_record_read(&protocol, &record, &output) == THRIFT_REQUIRED);
    memory.position = 0;
    protocol.limits.max_allocation = 1;
    thrift_protocol_reset(&protocol);
    CHECK(thrift_record_read(&protocol, &record, &output) == THRIFT_LIMIT);
    CHECK(thrift_memory_init(NULL, NULL, 0, 0, &transport) == THRIFT_INVALID);
    CHECK(thrift_memory_init(&memory, NULL, 1, 0, &transport) == THRIFT_INVALID);
    CHECK(thrift_memory_init(&memory, buffer, 0, 1, &transport) == THRIFT_INVALID);
    CHECK(thrift_record_read(NULL, &record, &output) == THRIFT_INVALID);
    CHECK(thrift_record_write(&protocol, NULL, &output) == THRIFT_INVALID);
    CHECK(thrift_protocol_init(NULL, transport) == THRIFT_INVALID);
    return EXIT_SUCCESS;
}

static int check_bytes(void)
{
    const uint8_t binary[] = {'a', 0, 'b'};
    struct thrift_bytes bytes = {0};
    void *allocation = NULL;
    size_t budget = 2;
    CHECK(thrift_alloc(SIZE_MAX, 2, NULL, &allocation) == THRIFT_LIMIT && !allocation);
    CHECK(thrift_bytes_set_limited(&bytes, binary, sizeof(binary), &budget) == THRIFT_LIMIT);
    CHECK(budget == 2 && !bytes.data);
    CHECK(thrift_bytes_set(&bytes, binary, sizeof(binary)) == THRIFT_OK);
    CHECK(bytes.size == sizeof(binary) && memcmp(bytes.data, binary, sizeof(binary)) == 0);
    CHECK(thrift_bytes_set(&bytes, bytes.data + 1, 2) == THRIFT_OK);
    CHECK(bytes.size == 2 && bytes.data[0] == 0 && bytes.data[1] == 'b');
    CHECK(thrift_bytes_set(&bytes, NULL, 1) == THRIFT_INVALID);
    thrift_value_clear(&thrift_type_string, &bytes);
    CHECK(!bytes.data && bytes.size == 0);
    thrift_value_clear(&thrift_type_string, &bytes);
    return EXIT_SUCCESS;
}

static int check_compact(void)
{
    enum { VECTOR_CAPACITY = 16 };
    const struct {
        uint8_t data[VECTOR_CAPACITY];
        size_t size;
        enum thrift_status status;
        int32_t value;
    } cases[] = {
        {{0x15, 0xff, 0xff, 0xff, 0xff, 0x0f, 0}, 7, THRIFT_OK, INT32_MIN},
        {{0x22, 0x05, 0x02, 0x54, 0}, 5, THRIFT_OK, 42},
        {{0x2b, 0, 0x05, 0x02, 0x54, 0}, 6, THRIFT_OK, 42},
        {{0x05, 0xff, 0xff, 0x03, 0, 0x05, 0x02, 0x54, 0}, 9, THRIFT_OK, 42},
        {{0x15, 0xff, 0xff, 0xff, 0xff, 0x10, 0}, 7, THRIFT_PROTOCOL, 0},
        {{0x15, 0xff, 0xff, 0xff, 0xff, 0x8f, 0}, 7, THRIFT_PROTOCOL, 0},
        {{0x05, 0xfe, 0xff, 0x03, 0, 0x15, 0, 0}, 8, THRIFT_PROTOCOL, 0},
        {{0x1e, 0}, 2, THRIFT_PROTOCOL, 0},
        {{0x2b, 1, 0x10, 0}, 4, THRIFT_PROTOCOL, 0},
        {{0x15, 2, 0x05, 2, 4, 0}, 6, THRIFT_PROTOCOL, 0},
        {{0}, 1, THRIFT_REQUIRED, 0}
    };
    uint8_t buffer[BUFFER_SIZE];
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    struct sample value = {7, true};
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memcpy(buffer, cases[i].data, cases[i].size);
        CHECK(thrift_memory_init(&memory, buffer, sizeof(buffer), cases[i].size, &transport) == THRIFT_OK);
        CHECK(thrift_compact_protocol_init(&protocol, transport) == THRIFT_OK);
        value.value = 7;
        CHECK(thrift_record_read(&protocol, &record, &value) == cases[i].status);
        CHECK(value.value == (cases[i].status == THRIFT_OK ? cases[i].value : 7));
    }
    CHECK(thrift_memory_init(&memory, buffer, sizeof(buffer), 0, &transport) == THRIFT_OK);
    CHECK(thrift_compact_protocol_init(&protocol, transport) == THRIFT_OK);
    value.value = INT32_MIN;
    CHECK(thrift_record_write(&protocol, &record, &value) == THRIFT_OK);
    CHECK(memory.size == cases[0].size && memcmp(buffer, cases[0].data, memory.size) == 0);
    CHECK(thrift_multiplexed_protocol_init(&protocol, transport, THRIFT_COMPACT, "") == THRIFT_INVALID);
    CHECK(thrift_multiplexed_protocol_init(&protocol, transport, THRIFT_BINARY, "bad:name") == THRIFT_INVALID);
    CHECK(thrift_multiplexed_protocol_init(&protocol, transport, THRIFT_BINARY, NULL) == THRIFT_INVALID);
    buffer[0] = 0x82;
    buffer[1] = 0x22; /* CALL with unsupported Compact version 2. */
    CHECK(thrift_memory_init(&memory, buffer, sizeof(buffer), 2, &transport) == THRIFT_OK);
    CHECK(thrift_compact_protocol_init(&protocol, transport) == THRIFT_OK);
    CHECK(thrift_process(&protocol, NULL, 0, &value) == THRIFT_PROTOCOL);
    return EXIT_SUCCESS;
}

static int check_list_helpers(void)
{
    const struct thrift_type numbers_type = {THRIFT_LIST, sizeof(struct thrift_list), NULL, NULL, &thrift_type_i32};
    const struct thrift_type strings_type = {THRIFT_SET, sizeof(struct thrift_list), NULL, NULL, &thrift_type_string};
    struct thrift_list numbers = {0}, strings = {0}, invalid = {0};
    struct thrift_bytes text = {0};
    int32_t element = 17;
    size_t budget = sizeof(element) * 2, original_budget;
    void *previous;
    CHECK(thrift_bytes_set_cstr(&text, "owned") == THRIFT_OK);
    CHECK(thrift_bytes_set_cstr(&text, (const char *)text.data + 1) == THRIFT_OK);
    CHECK(text.size == 4 && memcmp(text.data, "wned", 4) == 0);
    CHECK(thrift_bytes_set_cstr(&text, NULL) == THRIFT_INVALID);
    CHECK(text.size == 4);
    CHECK(thrift_bytes_set_cstr(NULL, "") == THRIFT_INVALID);
    CHECK(thrift_list_reserve(&numbers, 2, sizeof(element), &budget) == THRIFT_OK);
    CHECK(numbers.size == 0 && numbers.capacity == 2 && budget == 0);
    CHECK(((int32_t *)numbers.data)[0] == 0);
    CHECK(thrift_list_append(&numbers, &element, sizeof(element), &budget) == THRIFT_OK);
    CHECK(thrift_list_append(&numbers, numbers.data, sizeof(element), &budget) == THRIFT_OK);
    previous = numbers.data;
    CHECK(thrift_list_append(&numbers, &element, sizeof(element), &budget) == THRIFT_LIMIT);
    CHECK(numbers.data == previous && numbers.size == 2 && budget == 0);
    CHECK(thrift_list_append(&numbers, numbers.data, sizeof(element), NULL) == THRIFT_OK);
    CHECK(numbers.size == 3 && ((int32_t *)numbers.data)[2] == element);
    CHECK(thrift_list_reserve(&numbers, 1, sizeof(element), &budget) == THRIFT_OK);
    CHECK(numbers.size == 3);
    CHECK(thrift_list_reserve(&numbers, SIZE_MAX, sizeof(element), NULL) == THRIFT_LIMIT);
    CHECK(thrift_list_reserve(&numbers, 2, SIZE_MAX, NULL) == THRIFT_LIMIT);
    CHECK(thrift_list_append(&numbers, NULL, sizeof(element), NULL) == THRIFT_INVALID);
    CHECK(thrift_list_reserve(NULL, 1, sizeof(element), NULL) == THRIFT_INVALID);
    CHECK(thrift_list_reserve(&numbers, 1, 0, NULL) == THRIFT_INVALID);
    invalid.size = 1;
    CHECK(thrift_list_append(&invalid, &element, sizeof(element), NULL) == THRIFT_INVALID);
    thrift_value_clear(&numbers_type, &numbers);
    CHECK(numbers.data == NULL && numbers.size == 0 && numbers.capacity == 0);
    /* A budget for one slot must not fail because geometric growth prefers more. */
    budget = sizeof(text);
    CHECK(thrift_list_append(&strings, &text, sizeof(text), &budget) == THRIFT_OK);
    memset(&text, 0, sizeof(text));
    CHECK(strings.size == 1 && strings.capacity == 1 && budget == 0);
    CHECK(((struct thrift_bytes *)strings.data)[0].size == 4);
    thrift_value_clear(&strings_type, &strings);
    /* Legacy manually allocated lists have capacity zero until the first growth. */
    numbers.data = malloc(sizeof(element));
    CHECK(numbers.data != NULL);
    numbers.size = 1;
    memcpy(numbers.data, &element, sizeof(element));
    budget = sizeof(element) * 2;
    original_budget = budget;
    CHECK(thrift_list_append(&numbers, numbers.data, sizeof(element), &budget) == THRIFT_OK);
    CHECK(numbers.size == 2 && numbers.capacity == 2 && budget == original_budget - 2 * sizeof(element));
    thrift_value_clear(&numbers_type, &numbers);
    return EXIT_SUCCESS;
}

int main(void)
{
    int code;
    for (code = THRIFT_OK; code < THRIFT_STATUS_COUNT; ++code)
        CHECK(strcmp(thrift_status_string((enum thrift_status)code), "unknown status") != 0);
    CHECK(strcmp(thrift_status_string(THRIFT_STATUS_COUNT), "unknown status") == 0);
    if (check_wire() || check_errors() || check_bytes() || check_compact() || check_list_helpers())
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

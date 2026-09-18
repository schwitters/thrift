/* SPDX-License-Identifier: Apache-2.0 */
/* Adapted from TMemoryBufferTest.cpp: test_read_write_grow, test_observe,
 * test_exceptions and test_buffer_overflow. C11 storage is fixed and borrowed. */
#include <thrift/c11/transport/thrift_memory_buffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return EXIT_FAILURE; } } while (0)
enum { CAPACITY = 65536 };
static const char observed_payload[] = "foo\0bar";
static const char changed_payload[] = "Foo\0bar";
int main(void)
{
    uint8_t *storage = malloc(CAPACITY), *input = malloc(CAPACITY), *output = malloc(CAPACITY);
    struct thrift_memory memory;
    struct thrift_transport transport;
    size_t size, index, saved;
    CHECK(storage && input && output);
    for (index = 0; index < CAPACITY; ++index)
        input[index] = (uint8_t)index;
    CHECK(thrift_memory_init(&memory, storage, CAPACITY, 0, &transport) == THRIFT_OK);
    for (size = 1; size < CAPACITY; size *= 2)
        CHECK(transport.write(transport.context, input, size) == THRIFT_OK);
    CHECK(memory.size == CAPACITY - 1);
    for (size = 1; size < CAPACITY; size *= 2) {
        CHECK(transport.read(transport.context, output, size) == THRIFT_OK);
        CHECK(!memcmp(output, input, size));
    }
    CHECK(memory.position == memory.size);
    /* Reads do not reclaim capacity; writes append, even after EOF. */
    CHECK(transport.read(transport.context, output, 1) == THRIFT_EOF);
    CHECK(transport.write(transport.context, input, 1) == THRIFT_OK);
    CHECK(transport.read(transport.context, output, 1) == THRIFT_OK);
    CHECK(output[0] == input[0]);
    saved = memory.size;
    CHECK(transport.write(transport.context, input, SIZE_MAX) == THRIFT_LIMIT);
    CHECK(memory.size == saved);
    CHECK(transport.read(transport.context, output, SIZE_MAX) == THRIFT_EOF);
    CHECK(memory.position == saved);
    CHECK(transport.write(transport.context, NULL, 0) == THRIFT_OK);
    CHECK(transport.read(transport.context, NULL, 0) == THRIFT_OK);
    CHECK(transport.write(transport.context, NULL, 1) == THRIFT_INVALID);
    CHECK(transport.read(transport.context, NULL, 1) == THRIFT_INVALID);
    /* OBSERVE semantics: the caller owns and can modify the initial bytes. */
    CHECK(thrift_memory_init(&memory, storage, CAPACITY, sizeof(observed_payload) - 1, &transport) == THRIFT_OK);
    memcpy(storage, observed_payload, sizeof(observed_payload) - 1);
    storage[0] = 'F';
    CHECK(transport.read(transport.context, output, sizeof(observed_payload)) == THRIFT_EOF);
    CHECK(memory.position == 0);
    CHECK(transport.read(transport.context, output, sizeof(observed_payload) - 1) == THRIFT_OK);
    CHECK(!memcmp(output, changed_payload, sizeof(changed_payload) - 1));
    CHECK(thrift_memory_init(&memory, NULL, 0, 0, &transport) == THRIFT_OK);
    CHECK(transport.write(transport.context, input, 1) == THRIFT_LIMIT);
    CHECK(transport.read(transport.context, output, 1) == THRIFT_EOF);
    CHECK(transport.write(transport.context, NULL, 0) == THRIFT_OK);
    CHECK(transport.read(transport.context, NULL, 0) == THRIFT_OK);
    free(storage); free(input); free(output);
    return EXIT_SUCCESS;
}

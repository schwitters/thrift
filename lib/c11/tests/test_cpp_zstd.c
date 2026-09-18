/* SPDX-License-Identifier: Apache-2.0 */
/* ZlibTest.cpp test_read_write_mix, test_write_after_flush, test_no_write
 * adapted to independent Zstandard frames and exact-transfer C11 callbacks. */
#include <thrift/c11/transport/thrift_zstd_transport.h>
#include <thrift/c11/transport/thrift_memory_buffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return EXIT_FAILURE; } } while (0)
enum { ZSTD_BLOCK_BYTES = 128 * 1024, FLUSH_INTERVAL_BYTES = 64 * 1024,
       DATA_CAPACITY = 2 * ZSTD_BLOCK_BYTES + 1, WIRE_CAPACITY = 300000,
       COMPRESSION_LEVEL = 3, SMALL_CHUNK_BOUNDARY = 128, PAGE_BYTES = 4096,
       SCENARIO_ARGUMENT = 1, ARGUMENT_COUNT = 2,
       CHUNK_WRITE_INDEX = 0, CHUNK_READ_INDEX = 1,
       FAIL_READ = 0, FAIL_WRITE = 1, FAIL_FLUSH = 2, FAILURE_MODE_COUNT = 3 };
static const uint32_t random_seed = UINT32_C(0x12345678);
static const uint32_t random_multiplier = UINT32_C(1664525);
static const uint32_t random_increment = UINT32_C(1013904223);
static const char tail_payload[] = "tail";
static const char unflushed_payload[] = "unflushed";
static int mixed(bool random_data, size_t write_chunk, size_t read_chunk)
{
    uint8_t *wire = malloc(WIRE_CAPACITY), *input = malloc(DATA_CAPACITY), *output = malloc(DATA_CAPACITY);
    struct thrift_memory memory;
    struct thrift_transport underlying, writer, reader;
    struct thrift_zstd *encoder = NULL, *decoder = NULL;
    uint32_t state = random_seed;
    size_t index, position, frames = 0;
    CHECK(wire && input && output);
    for (index = 0; index < DATA_CAPACITY; ++index) {
        state = state * random_multiplier + random_increment;
        input[index] = random_data ? (uint8_t)(state >> 24) : (uint8_t)'a';
    }
    CHECK(thrift_memory_init(&memory, wire, WIRE_CAPACITY, 0, &underlying) == THRIFT_OK);
    CHECK(thrift_zstd_create(underlying, DATA_CAPACITY, COMPRESSION_LEVEL, &encoder, &writer) == THRIFT_OK);
    CHECK(thrift_zstd_create(underlying, DATA_CAPACITY, COMPRESSION_LEVEL, &decoder, &reader) == THRIFT_OK);
    position = 0;
    while (position < DATA_CAPACITY) {
        size_t size = write_chunk < DATA_CAPACITY - position ? write_chunk : DATA_CAPACITY - position;
        CHECK(writer.write(writer.context, input + position, size) == THRIFT_OK);
        position += size;
        /* Flush several times, including across Zstandard's 128 KiB blocks. */
        if (position / FLUSH_INTERVAL_BYTES > frames) {
            CHECK(writer.flush(writer.context) == THRIFT_OK);
            frames = position / FLUSH_INTERVAL_BYTES;
        }
    }
    CHECK(writer.flush(writer.context) == THRIFT_OK);
    position = 0;
    while (position < DATA_CAPACITY) {
        size_t size = read_chunk < DATA_CAPACITY - position ? read_chunk : DATA_CAPACITY - position;
        CHECK(reader.read(reader.context, output + position, size) == THRIFT_OK);
        position += size;
    }
    CHECK(!memcmp(input, output, DATA_CAPACITY));
    /* The final flush may leave an empty frame for the next read.
     * A subsequent frame remains readable through the same decoder. */
    CHECK(writer.write(writer.context, tail_payload, sizeof(tail_payload) - 1) == THRIFT_OK);
    CHECK(writer.flush(writer.context) == THRIFT_OK);
    CHECK(reader.read(reader.context, output, sizeof(tail_payload) - 1) == THRIFT_OK);
    CHECK(!memcmp(output, tail_payload, sizeof(tail_payload) - 1));
    CHECK(memory.position == memory.size);
    thrift_zstd_destroy(encoder);
    thrift_zstd_destroy(decoder);
    free(wire); free(input); free(output);
    return EXIT_SUCCESS;
}

struct failing_transport { size_t writes, flushes; bool fail_write; };
static enum thrift_status fail_read(void *context, void *data, size_t size)
{
    (void)context; (void)data; (void)size;
    return THRIFT_TIMEOUT;
}
static enum thrift_status fail_write(void *context, const void *data, size_t size)
{
    struct failing_transport *failure = context;
    (void)data; (void)size;
    ++failure->writes;
    return failure->fail_write ? THRIFT_IO : THRIFT_OK;
}
static enum thrift_status fail_flush(void *context)
{
    struct failing_transport *failure = context;
    ++failure->flushes;
    return THRIFT_TIMEOUT;
}
static int lifecycle(void)
{
    struct failing_transport failure = {0};
    struct thrift_transport underlying = {&failure, fail_read, fail_write, fail_flush}, view;
    struct thrift_zstd *zstd = NULL;
    char byte;
    unsigned mode;
    CHECK(thrift_zstd_create(underlying, DATA_CAPACITY, COMPRESSION_LEVEL, &zstd, &view) == THRIFT_OK);
    thrift_zstd_destroy(zstd);
    CHECK(failure.writes == 0 && failure.flushes == 0);
    CHECK(thrift_zstd_create(underlying, DATA_CAPACITY, COMPRESSION_LEVEL, &zstd, &view) == THRIFT_OK);
    CHECK(view.write(view.context, unflushed_payload, sizeof(unflushed_payload) - 1) == THRIFT_OK);
    thrift_zstd_destroy(zstd);
    CHECK(failure.writes == 0 && failure.flushes == 0);
    for (mode = 0; mode < FAILURE_MODE_COUNT; ++mode) {
        enum thrift_status expected = mode == FAIL_WRITE ? THRIFT_IO : THRIFT_TIMEOUT;
        failure.writes = failure.flushes = 0;
        failure.fail_write = mode == FAIL_WRITE;
        CHECK(thrift_zstd_create(underlying, DATA_CAPACITY, COMPRESSION_LEVEL, &zstd, &view) == THRIFT_OK);
        if (mode == FAIL_READ)
            CHECK(view.read(view.context, &byte, 1) == expected);
        else {
            CHECK(view.write(view.context, "x", 1) == THRIFT_OK);
            CHECK(view.flush(view.context) == expected);
        }
        CHECK(view.flush(view.context) == expected);
        CHECK(view.write(view.context, "x", 1) == expected);
        CHECK(view.read(view.context, &byte, 1) == expected);
        CHECK(failure.writes == (mode != FAIL_READ ? 1u : 0u));
        CHECK(failure.flushes == (mode == FAIL_FLUSH ? 1u : 0u));
        thrift_zstd_destroy(zstd);
    }
    return EXIT_SUCCESS;
}
int main(int argc, char **argv)
{
    static const size_t chunks[][CHUNK_READ_INDEX + 1] = {{1, PAGE_BYTES - 3}, {PAGE_BYTES - 3, 1}, {SMALL_CHUNK_BOUNDARY - 1, SMALL_CHUNK_BOUNDARY},
        {SMALL_CHUNK_BOUNDARY, SMALL_CHUNK_BOUNDARY - 1}, {PAGE_BYTES, 4 * PAGE_BYTES + 1},
        {ZSTD_BLOCK_BYTES + 1, FLUSH_INTERVAL_BYTES + 1}, {DATA_CAPACITY, DATA_CAPACITY}};
    size_t index;
    CHECK(argc == ARGUMENT_COUNT);
    if (!strcmp(argv[SCENARIO_ARGUMENT], "lifecycle")) return lifecycle();
    CHECK(!strcmp(argv[SCENARIO_ARGUMENT], "compressible") || !strcmp(argv[SCENARIO_ARGUMENT], "random"));
    for (index = 0; index < sizeof(chunks) / sizeof(chunks[0]); ++index)
        CHECK(mixed(!strcmp(argv[SCENARIO_ARGUMENT], "random"), chunks[index][CHUNK_WRITE_INDEX], chunks[index][CHUNK_READ_INDEX]) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

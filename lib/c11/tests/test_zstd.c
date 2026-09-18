/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/transport/thrift_zstd_transport.h>
#include <thrift/c11/transport/thrift_memory_buffer.h>
#include <zstd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return EXIT_FAILURE; } } while (0)
enum { BODY_SIZE = 300000, WIRE_SIZE = 310000, COMPRESSION_LEVEL = 3,
       FAST_COMPRESSION_LEVEL = 1, SMALL_FRAME_LIMIT = 16,
       EMPTY_FRAME_LIMIT = 64, PATTERN_PERIOD = 37 };
static const char short_payload[] = "abc";

static int roundtrip(void)
{
    uint8_t *wire = malloc(WIRE_SIZE), *data = malloc(BODY_SIZE), *decoded = malloc(BODY_SIZE);
    struct thrift_memory memory;
    struct thrift_transport underlying, transport;
    struct thrift_zstd *zstd = NULL;
    size_t index, frame_size;
    CHECK(wire && data && decoded);
    for (index = 0; index < BODY_SIZE; ++index)
        data[index] = (uint8_t)(index * index + index / PATTERN_PERIOD);
    CHECK(thrift_memory_init(&memory, wire, WIRE_SIZE, 0, &underlying) == THRIFT_OK);
    CHECK(thrift_zstd_create(underlying, BODY_SIZE, COMPRESSION_LEVEL, &zstd, &transport) == THRIFT_OK);
    CHECK(transport.write(transport.context, data, BODY_SIZE) == THRIFT_OK);
    CHECK(memory.size == 0);
    CHECK(transport.flush(transport.context) == THRIFT_OK);
    frame_size = memory.size;
    /* Independent decoder verifies standard framing and the complete payload. */
    CHECK(ZSTD_decompress(decoded, BODY_SIZE, wire, frame_size) == BODY_SIZE);
    CHECK(!memcmp(decoded, data, BODY_SIZE));
    CHECK(transport.flush(transport.context) == THRIFT_OK); /* Empty frame. */
    CHECK(transport.write(transport.context, short_payload, sizeof(short_payload) - 1) == THRIFT_OK);
    CHECK(transport.flush(transport.context) == THRIFT_OK);
    CHECK(transport.read(transport.context, decoded, BODY_SIZE - 1) == THRIFT_OK);
    CHECK(transport.read(transport.context, decoded + BODY_SIZE - 1, 1) == THRIFT_OK);
    CHECK(!memcmp(decoded, data, BODY_SIZE));
    CHECK(transport.read(transport.context, decoded, sizeof(short_payload) - 1) == THRIFT_OK);
    CHECK(!memcmp(decoded, short_payload, sizeof(short_payload) - 1));
    thrift_zstd_destroy(zstd);
    /* Each truncation must fail without accepting an incomplete frame. */
    for (index = 0; index < frame_size; ++index) {
        memory.size = index;
        memory.position = 0;
        CHECK(thrift_zstd_create(underlying, BODY_SIZE, COMPRESSION_LEVEL, &zstd, &transport) == THRIFT_OK);
        CHECK(transport.read(transport.context, decoded, 1) == THRIFT_EOF);
        thrift_zstd_destroy(zstd);
    }
    memory.size = frame_size;
    memory.position = 0;
    wire[frame_size - 1] ^= 1; /* Corrupt checksum. */
    CHECK(thrift_zstd_create(underlying, BODY_SIZE, COMPRESSION_LEVEL, &zstd, &transport) == THRIFT_OK);
    CHECK(transport.read(transport.context, decoded, 1) == THRIFT_PROTOCOL);
    CHECK(transport.flush(transport.context) == THRIFT_PROTOCOL);
    thrift_zstd_destroy(zstd);
    wire[frame_size - 1] ^= 1;
    memory.position = 0;
    CHECK(thrift_zstd_create(underlying, SMALL_FRAME_LIMIT, COMPRESSION_LEVEL, &zstd, &transport) == THRIFT_OK);
    CHECK(transport.read(transport.context, decoded, 1) == THRIFT_LIMIT);
    thrift_zstd_destroy(zstd);
    /* Independent encoder, including content-size-absent frames. */
    {
        ZSTD_CCtx *compressor = ZSTD_createCCtx();
        CHECK(compressor);
        CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(compressor, ZSTD_c_contentSizeFlag, 0)));
        memory.size = ZSTD_compress2(compressor, wire, WIRE_SIZE, data, BODY_SIZE);
        CHECK(!ZSTD_isError(memory.size));
        ZSTD_freeCCtx(compressor);
        memory.position = 0;
        CHECK(thrift_zstd_create(underlying, BODY_SIZE, FAST_COMPRESSION_LEVEL, &zstd, &transport) == THRIFT_OK);
        CHECK(transport.read(transport.context, decoded, BODY_SIZE) == THRIFT_OK);
        CHECK(!memcmp(data, decoded, BODY_SIZE));
        thrift_zstd_destroy(zstd);
        memory.position = 0;
        CHECK(thrift_zstd_create(underlying, BODY_SIZE - 1, FAST_COMPRESSION_LEVEL, &zstd, &transport) == THRIFT_OK);
        CHECK(transport.read(transport.context, decoded, 1) == THRIFT_LIMIT);
        thrift_zstd_destroy(zstd);
    }
    CHECK(thrift_zstd_create(underlying, SMALL_FRAME_LIMIT, COMPRESSION_LEVEL, &zstd, &transport) == THRIFT_OK);
    CHECK(transport.write(transport.context, data, SMALL_FRAME_LIMIT + 1) == THRIFT_LIMIT);
    CHECK(transport.flush(transport.context) == THRIFT_LIMIT);
    thrift_zstd_destroy(zstd);
    memory.size = memory.position = 0;
    CHECK(thrift_zstd_create(underlying, SMALL_FRAME_LIMIT, COMPRESSION_LEVEL, &zstd, &transport) == THRIFT_OK);
    for (index = 0; index <= EMPTY_FRAME_LIMIT; ++index)
        CHECK(transport.flush(transport.context) == THRIFT_OK);
    CHECK(transport.read(transport.context, decoded, 1) == THRIFT_LIMIT);
    thrift_zstd_destroy(zstd);
    free(wire); free(data); free(decoded);
    return EXIT_SUCCESS;
}
int main(void)
{
    return roundtrip();
}

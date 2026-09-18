/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/transport/thrift_zstd_transport.h>
#include "../thrift_log_internal.h"
#include "thrift_buffer_internal.h"
#include <stdlib.h>
#include <string.h>
#include <zstd.h>
#include <zstd_errors.h>

enum { ZSTD_PREFIX_SIZE = 5, ZSTD_BLOCK_HEADER_SIZE = 3,
       ZSTD_CHECKSUM_SIZE = 4, ZSTD_BLOCK_LIMIT = 128 * 1024,
       ZSTD_COPY_SIZE = 4096, ZSTD_EMPTY_FRAME_LIMIT = 64,
       ZSTD_DESCRIPTOR_RESERVED = 1u << 3, ZSTD_DESCRIPTOR_CHECKSUM = 1u << 2,
       ZSTD_BLOCK_LAST = 1u << 0, ZSTD_BLOCK_RLE = 1, ZSTD_BLOCK_RESERVED = 3 };
struct thrift_zstd {
    struct thrift_transport underlying;
    struct thrift_buffer input, output, compressed;
    ZSTD_CCtx *compressor;
    enum thrift_status error;
};

static enum thrift_status frame_copy(struct thrift_zstd *zstd, size_t size)
{
    uint8_t bytes[ZSTD_COPY_SIZE];
    enum thrift_status status;
    if (size > zstd->compressed.limit - zstd->compressed.size)
        return THRIFT_LIMIT;
    while (size) {
        size_t count = size < sizeof(bytes) ? size : sizeof(bytes);
        status = zstd->underlying.read(zstd->underlying.context, bytes, count);
        if (status != THRIFT_OK)
            return status;
        status = thrift_buffer_append(&zstd->compressed, bytes, count);
        if (status != THRIFT_OK)
            return status;
        size -= count;
    }
    return THRIFT_OK;
}

/* Read only the bytes belonging to this frame: exact-read socket transports
 * must not wait for a following RPC. libzstd validates the collected frame. */
static enum thrift_status frame_read_impl(struct thrift_zstd *zstd)
{
    static const uint8_t magic[] = {0x28, 0xb5, 0x2f, 0xfd};
    static const size_t dictionary_sizes[] = {0, 1, 2, 4};
    static const size_t content_sizes[] = {0, 2, 4, 8};
    enum thrift_status status;
    unsigned descriptor, single, block;
    size_t extra, offset, size;
    unsigned long long content_size;
    zstd->compressed.size = 0;
    status = frame_copy(zstd, ZSTD_PREFIX_SIZE);
    if (status != THRIFT_OK)
        return status;
    if (memcmp(zstd->compressed.data, magic, sizeof(magic)))
        return THRIFT_PROTOCOL;
    descriptor = zstd->compressed.data[ZSTD_PREFIX_SIZE - 1];
    if (descriptor & ZSTD_DESCRIPTOR_RESERVED)
        return THRIFT_PROTOCOL;
    single = (descriptor >> 5) & 1;
    extra = dictionary_sizes[descriptor & 3] + content_sizes[descriptor >> 6];
    extra += single ? ((descriptor >> 6) == 0) : 1;
    status = frame_copy(zstd, extra);
    if (status != THRIFT_OK)
        return status;
    content_size = ZSTD_getFrameContentSize(zstd->compressed.data, zstd->compressed.size);
    if (content_size == ZSTD_CONTENTSIZE_ERROR)
        return THRIFT_PROTOCOL;
    if (content_size != ZSTD_CONTENTSIZE_UNKNOWN && content_size > zstd->input.limit)
        return THRIFT_LIMIT;
    if (ZSTD_getDictID_fromFrame(zstd->compressed.data, zstd->compressed.size))
        return THRIFT_PROTOCOL;
    do {
        offset = zstd->compressed.size;
        status = frame_copy(zstd, ZSTD_BLOCK_HEADER_SIZE);
        if (status != THRIFT_OK)
            return status;
        block = zstd->compressed.data[offset] |
            ((unsigned)zstd->compressed.data[offset + 1] << 8) |
            ((unsigned)zstd->compressed.data[offset + 2] << 16);
        size = block >> 3;
        if (((block >> 1) & 3) == ZSTD_BLOCK_RESERVED || size > ZSTD_BLOCK_LIMIT)
            return THRIFT_PROTOCOL;
        if (((block >> 1) & 3) == ZSTD_BLOCK_RLE)
            size = 1; /* RLE blocks contain one repeated byte. */
        status = frame_copy(zstd, size);
        if (status != THRIFT_OK)
            return status;
    } while (!(block & ZSTD_BLOCK_LAST));
    if (descriptor & ZSTD_DESCRIPTOR_CHECKSUM) {
        status = frame_copy(zstd, ZSTD_CHECKSUM_SIZE);
        if (status != THRIFT_OK)
            return status;
    }
    size = content_size == ZSTD_CONTENTSIZE_UNKNOWN ? zstd->input.limit : (size_t)content_size;
    if (size > zstd->input.capacity) {
        uint8_t *next = realloc(zstd->input.data, size);
        if (!next)
            return THRIFT_NOMEM;
        zstd->input.data = next;
        zstd->input.capacity = size;
    }
    size = ZSTD_decompress(zstd->input.data, zstd->input.capacity,
                           zstd->compressed.data, zstd->compressed.size);
    if (ZSTD_isError(size)) {
        thrift_log_native("zstd", (int64_t)ZSTD_getErrorCode(size));
        return ZSTD_getErrorCode(size) == ZSTD_error_dstSize_tooSmall ?
            THRIFT_LIMIT : THRIFT_PROTOCOL;
    }
    zstd->input.size = size;
    zstd->input.position = 0;
    return THRIFT_OK;
}

static enum thrift_status frame_read(struct thrift_zstd *zstd)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "zstd.read_frame", zstd->input.limit);
    enum thrift_status status = frame_read_impl(zstd);
    thrift_log_end(scope, status);
    return status;
}

static enum thrift_status zstd_read(void *context, void *data, size_t size)
{
    struct thrift_zstd *zstd = context;
    uint8_t *next = data;
    unsigned empty_frames = 0;
    if (!zstd || (!data && size))
        return THRIFT_INVALID;
    if (zstd->error != THRIFT_OK)
        return zstd->error;
    while (size) {
        size_t count = zstd->input.size - zstd->input.position;
        if (!count) {
            zstd->error = frame_read(zstd);
            if (zstd->error != THRIFT_OK)
                return zstd->error;
            /* Bound work even when a peer supplies endless empty frames. */
            if (!zstd->input.size && ++empty_frames > ZSTD_EMPTY_FRAME_LIMIT)
                return zstd->error = THRIFT_LIMIT;
            continue;
        }
        if (count > size)
            count = size;
        zstd->error = thrift_buffer_read(&zstd->input, next, count);
        if (zstd->error != THRIFT_OK)
            return zstd->error;
        next += count;
        size -= count;
    }
    return THRIFT_OK;
}

static enum thrift_status zstd_write(void *context, const void *data, size_t size)
{
    struct thrift_zstd *zstd = context;
    if (!zstd)
        return THRIFT_INVALID;
    if (zstd->error == THRIFT_OK)
        zstd->error = thrift_buffer_append(&zstd->output, data, size);
    return zstd->error;
}

static enum thrift_status zstd_flush_impl(void *context)
{
    struct thrift_zstd *zstd = context;
    uint8_t *bytes;
    size_t capacity, size;
    if (!zstd)
        return THRIFT_INVALID;
    if (zstd->error != THRIFT_OK)
        return zstd->error;
    capacity = ZSTD_compressBound(zstd->output.size);
    bytes = malloc(capacity);
    if (!bytes)
        return zstd->error = THRIFT_NOMEM;
    size = ZSTD_compress2(zstd->compressor, bytes, capacity,
                         zstd->output.data, zstd->output.size);
    if (ZSTD_isError(size)) {
        thrift_log_native("zstd", (int64_t)ZSTD_getErrorCode(size));
        zstd->error = THRIFT_IO;
    } else
        zstd->error = zstd->underlying.write(zstd->underlying.context, bytes, size);
    free(bytes);
    if (zstd->error == THRIFT_OK && zstd->underlying.flush)
        zstd->error = zstd->underlying.flush(zstd->underlying.context);
    if (zstd->error == THRIFT_OK)
        zstd->output.size = 0;
    return zstd->error;
}

static enum thrift_status zstd_flush(void *context)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "zstd.flush", context ? ((struct thrift_zstd *)context)->output.size : 0);
    enum thrift_status status = zstd_flush_impl(context);
    thrift_log_end(scope, status);
    return status;
}

static enum thrift_status thrift_zstd_create_impl(struct thrift_transport underlying,
    size_t max_frame_size, int compression_level, struct thrift_zstd **result,
    struct thrift_transport *transport)
{
    struct thrift_zstd *zstd;
    size_t bound;
    if (!result || !transport)
        return THRIFT_INVALID;
    *result = NULL;
    if (!underlying.read || !underlying.write || !max_frame_size ||
        compression_level < ZSTD_minCLevel() || compression_level > ZSTD_maxCLevel())
        return THRIFT_INVALID;
    bound = ZSTD_compressBound(max_frame_size);
    if (ZSTD_isError(bound) || bound < max_frame_size)
        return THRIFT_LIMIT;
    zstd = calloc(1, sizeof(*zstd));
    if (!zstd)
        return THRIFT_NOMEM;
    zstd->compressor = ZSTD_createCCtx();
    if (!zstd->compressor) {
        free(zstd);
        return THRIFT_NOMEM;
    }
    if (ZSTD_isError(ZSTD_CCtx_setParameter(zstd->compressor, ZSTD_c_compressionLevel, compression_level)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(zstd->compressor, ZSTD_c_checksumFlag, 1))) {
        thrift_zstd_destroy(zstd);
        return THRIFT_INVALID;
    }
    zstd->underlying = underlying;
    zstd->input.limit = zstd->output.limit = max_frame_size;
    zstd->compressed.limit = bound;
    transport->context = zstd;
    transport->read = zstd_read;
    transport->write = zstd_write;
    transport->flush = zstd_flush;
    *result = zstd;
    return THRIFT_OK;
}

enum thrift_status thrift_zstd_create(struct thrift_transport underlying,
    size_t max_frame_size, int compression_level, struct thrift_zstd **result,
    struct thrift_transport *transport)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "zstd.create", max_frame_size);
    enum thrift_status status = thrift_zstd_create_impl(underlying, max_frame_size, compression_level, result, transport);
    thrift_log_end(scope, status);
    return status;
}

static void thrift_zstd_destroy_impl(struct thrift_zstd *zstd)
{
    if (!zstd)
        return;
    ZSTD_freeCCtx(zstd->compressor);
    thrift_buffer_clear(&zstd->input);
    thrift_buffer_clear(&zstd->output);
    thrift_buffer_clear(&zstd->compressed);
    free(zstd);
}

void thrift_zstd_destroy(struct thrift_zstd *zstd)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "zstd.destroy", 0);
    thrift_zstd_destroy_impl(zstd);
    thrift_log_end(scope, THRIFT_OK);
}

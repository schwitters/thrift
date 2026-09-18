/* SPDX-License-Identifier: Apache-2.0 */
#include "fixture_types.h"
#include <thrift/c11/transport/thrift_memory_buffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CODEC_BUFFER_SIZE = 65536, PROTOCOL_ARGUMENT = 1, RECORD_ARGUMENT = 2,
       INPUT_ARGUMENT = 3, OUTPUT_ARGUMENT = 4, HELP_ARGUMENT_COUNT = 2, ARGUMENT_COUNT = 5 };

/* File-based adapter for independent runtime interoperability tests. Binary file
 * modes work identically on Linux and Windows without platform-specific stdio. */
static enum thrift_status convert(enum thrift_protocol_kind kind,
    const struct thrift_record *record, const char *input_path, const char *output_path)
{
    uint8_t *input = NULL, *output = NULL;
    FILE *file = NULL;
    void *value = NULL;
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    size_t size;
    enum thrift_status status = THRIFT_NOMEM;
    input = malloc(CODEC_BUFFER_SIZE);
    output = malloc(CODEC_BUFFER_SIZE);
    value = calloc(1, record->size);
    if (!input || !output || !value)
        goto cleanup;
    status = THRIFT_IO;
    file = fopen(input_path, "rb");
    if (!file)
        goto cleanup;
    size = fread(input, 1, CODEC_BUFFER_SIZE, file);
    if (ferror(file) || fgetc(file) != EOF || ferror(file))
        goto cleanup;
    if (fclose(file) != 0) {
        file = NULL;
        goto cleanup;
    }
    file = NULL;
    status = thrift_memory_init(&memory, input, CODEC_BUFFER_SIZE, size, &transport);
    if (status == THRIFT_OK)
        status = thrift_protocol_init_kind(&protocol, transport, kind);
    if (status == THRIFT_OK)
        status = thrift_record_read(&protocol, record, value);
    if (status != THRIFT_OK)
        goto cleanup;
    if (memory.position != memory.size) {
        status = THRIFT_PROTOCOL;
        goto cleanup;
    }
    status = thrift_memory_init(&memory, output, CODEC_BUFFER_SIZE, 0, &transport);
    if (status == THRIFT_OK)
        status = thrift_protocol_init_kind(&protocol, transport, kind);
    if (status == THRIFT_OK)
        status = thrift_record_write(&protocol, record, value);
    if (status != THRIFT_OK)
        goto cleanup;
    status = THRIFT_IO;
    file = fopen(output_path, "wb");
    if (!file)
        goto cleanup;
    if (fwrite(output, 1, memory.size, file) != memory.size)
        goto cleanup;
    status = THRIFT_OK;
cleanup:
    if (file && fclose(file) != 0)
        status = THRIFT_IO;
    thrift_record_clear(record, value);
    free(value);
    free(input);
    free(output);
    return status;
}

int main(int argc, char **argv)
{
    enum thrift_protocol_kind kind;
    const struct thrift_record *record;
    enum thrift_status status;
    if (argc == HELP_ARGUMENT_COUNT && strcmp(argv[PROTOCOL_ARGUMENT], "--help") == 0)
        return puts("Usage: thrift_codec binary|compact packet|edges input output") < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    if (argc != ARGUMENT_COUNT)
        return EXIT_FAILURE;
    if (strcmp(argv[PROTOCOL_ARGUMENT], "binary") == 0)
        kind = THRIFT_BINARY;
    else if (strcmp(argv[PROTOCOL_ARGUMENT], "compact") == 0)
        kind = THRIFT_COMPACT;
    else
        return EXIT_FAILURE;
    if (strcmp(argv[RECORD_ARGUMENT], "packet") == 0)
        record = &test_packet_record;
    else if (strcmp(argv[RECORD_ARGUMENT], "edges") == 0)
        record = &test_encoding_edges_record;
    else
        return EXIT_FAILURE;
    status = convert(kind, record, argv[INPUT_ARGUMENT], argv[OUTPUT_ARGUMENT]);
    if (status != THRIFT_OK) {
        fprintf(stderr, "Codec conversion failed: %s\n", thrift_status_string(status));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

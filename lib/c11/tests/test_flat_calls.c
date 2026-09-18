/* SPDX-License-Identifier: Apache-2.0 */
#include "flat_calls_client.h"
#include <thrift/c11/protocol/thrift_protocol_internal.h>
#include <thrift/c11/transport/thrift_memory_buffer.h>
#include <stdio.h>
#include <stdlib.h>

enum { BUFFER_CAPACITY = 4096, EXPECTED_NUMBER = 42, INITIAL_OUTPUT = -1 };
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    result = EXIT_FAILURE; goto cleanup; \
} } while (0)

struct channel {
    uint8_t buffer[BUFFER_CAPACITY];
    struct thrift_memory memory;
    struct thrift_transport transport;
    struct thrift_protocol protocol;
    struct flat_api_client client;
};

static enum thrift_status reply(struct channel *channel, enum thrift_protocol_kind kind,
    const char *method, const struct thrift_record *record, const void *value)
{
    enum thrift_status status = thrift_memory_init(&channel->memory, channel->buffer,
        sizeof(channel->buffer), 0, &channel->transport);
    if (status == THRIFT_OK)
        status = thrift_protocol_init_kind(&channel->protocol, channel->transport, kind);
    if (status == THRIFT_OK)
        status = thrift_message_write(&channel->protocol, method, C11_REPLY, 1);
    if (status == THRIFT_OK)
        status = thrift_record_write(&channel->protocol, record, value);
    if (status == THRIFT_OK)
        status = flat_api_client_init(&channel->client, &channel->protocol);
    return status;
}

static int check_calls(enum thrift_protocol_kind kind)
{
    int result = EXIT_SUCCESS;
    struct channel channel;
    struct flat_base_done_result done = {0};
    struct flat_api_number_result number = {0};
    struct flat_api_item_result item = {0};
    struct flat_api_names_result names = {0};
    struct flat_problem exception = {0};
    struct flat_item *output = NULL;
    struct flat_problem *error = NULL, *other = NULL;
    int32_t value = INITIAL_OUTPUT;
    size_t response_size;

    CHECK(reply(&channel, kind, "done", &flat_base_done_result_record, &done) == THRIFT_OK);
    CHECK(flat_api_client_done(&channel.client, &error) == THRIFT_OK && error == NULL);
    CHECK(channel.client.next_sequence == 2);

    number.has_success = true;
    number.f_success = EXPECTED_NUMBER;
    CHECK(reply(&channel, kind, "number", &flat_api_number_result_record, &number) == THRIFT_OK);
    response_size = channel.memory.size;
    CHECK(flat_api_client_number(&channel.client, &value, NULL) == THRIFT_NULL_VALUE);
    CHECK(channel.memory.size == response_size && channel.client.next_sequence == 1);
    error = &exception;
    CHECK(flat_api_client_number(&channel.client, &value, &error) == THRIFT_OUTPUT_NOT_EMPTY);
    CHECK(value == INITIAL_OUTPUT && error == &exception && channel.client.next_sequence == 1);
    CHECK(channel.memory.size == response_size);
    error = NULL;
    CHECK(flat_api_client_number(&channel.client, &value, &error) == THRIFT_OK);
    CHECK(value == EXPECTED_NUMBER && error == NULL);

    number.has_success = false;
    number.has_problem = true;
    number.f_problem = &exception; /* Borrowed only for serialization. */
    CHECK(reply(&channel, kind, "number", &flat_api_number_result_record, &number) == THRIFT_OK);
    CHECK(flat_api_client_number(&channel.client, &value, &error) == THRIFT_OK);
    CHECK(value == 0 && error != NULL);
    flat_problem_clear(error);
    free(error);
    error = NULL;
    number.has_problem = false;
    number.f_problem = NULL;
    value = INITIAL_OUTPUT;
    CHECK(reply(&channel, kind, "number", &flat_api_number_result_record, &number) == THRIFT_OK);
    CHECK(flat_api_client_number(&channel.client, &value, &error) == THRIFT_MISSING_RESULT);
    CHECK(value == INITIAL_OUTPUT && error == NULL);

    CHECK(flat_item_create(&item.f_success, NULL) == THRIFT_OK);
    item.has_success = true;
    CHECK(flat_item_set_number(item.f_success, EXPECTED_NUMBER) == THRIFT_OK);
    CHECK(reply(&channel, kind, "item", &flat_api_item_result_record, &item) == THRIFT_OK);
    CHECK(flat_api_client_item(&channel.client, &output, &error, &error) == THRIFT_INVALID_OWNERSHIP);
    CHECK(channel.client.next_sequence == 1 && output == NULL);
    CHECK(flat_api_client_item(&channel.client, &output, &error, &other) == THRIFT_OK);
    CHECK(output && output->f_number == EXPECTED_NUMBER && !error && !other);
    CHECK(flat_api_client_item(&channel.client, &output, &error, &other) == THRIFT_OUTPUT_NOT_EMPTY);
    CHECK(channel.client.next_sequence == 2);
    flat_item_clear(output);
    free(output);
    output = NULL;

    names.has_success = true;
    names.f_success = EXPECTED_NUMBER;
    CHECK(reply(&channel, kind, "names", &flat_api_names_result_record, &names) == THRIFT_OK);
    CHECK(flat_api_client_names(&channel.client, 1, 2, 3, 4, 5, 6, "literal", 7, 8, &value) == THRIFT_OK);
    CHECK(value == EXPECTED_NUMBER);
    CHECK(flat_api_client_notify(&channel.client, EXPECTED_NUMBER) == THRIFT_OK);
    CHECK(channel.client.next_sequence == 3);

cleanup:
    flat_item_clear(output);
    free(output);
    if (error != &exception) {
        flat_problem_clear(error);
        free(error);
    }
    flat_problem_clear(other);
    free(other);
    flat_api_item_result_clear(&item);
    return result;
}

int main(void)
{
    if (check_calls(THRIFT_BINARY) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    return check_calls(THRIFT_COMPACT);
}

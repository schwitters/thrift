/* Generated clients; do not edit. */
#include "calculator_client.h"

enum thrift_status example_calculator_client_init(struct example_calculator_client *client, struct thrift_protocol *protocol)
{
    if (!client) return THRIFT_NULL_CLIENT;
    if (!protocol) return THRIFT_NULL_PROTOCOL;
    if (!protocol->ops) return THRIFT_PROTOCOL_UNINITIALIZED;
    client->protocol = protocol;
    client->next_sequence = 1;
    return THRIFT_OK;
}

enum thrift_status example_calculator_client_add_call(struct example_calculator_client *client,
    const struct example_calculator_add_args *args, struct example_calculator_add_result *result)
{
    int32_t sequence;
    if (!client) return THRIFT_NULL_CLIENT;
    if (!client->protocol) return THRIFT_NULL_PROTOCOL;
    if (!client->protocol->ops) return THRIFT_PROTOCOL_UNINITIALIZED;
    if (!args) return THRIFT_NULL_ARGS;
    if (!result) return THRIFT_NULL_RESULT;
    if (client->next_sequence < 1) return THRIFT_INVALID_SEQUENCE;
    sequence = client->next_sequence;
    client->next_sequence = sequence == INT32_MAX ? 1 : sequence + 1;
    return example_calculator_add_call(client->protocol, sequence, args, result);
}

enum thrift_status example_calculator_client_add(struct example_calculator_client *client,
    int64_t arg_1_left,
    int64_t arg_2_right,
    int64_t *out_success,
    struct example_calculation_error **out_error)
{
    struct example_calculator_add_args args = {0};
    struct example_calculator_add_result result = {0};
    enum thrift_status status;
    if (!out_success) return THRIFT_NULL_VALUE;
    if (!out_error) return THRIFT_NULL_VALUE;
    if ((const void *)out_success == (const void *)out_error) return THRIFT_INVALID_OWNERSHIP;
    if (*out_error) return THRIFT_OUTPUT_NOT_EMPTY;
    args.f_left = arg_1_left;
    args.has_left = true;
    args.f_right = arg_2_right;
    args.has_right = true;
    status = example_calculator_client_add_call(client, &args, &result);
    if (status == THRIFT_OK) {
        *out_success = result.has_success ? result.f_success : (int64_t){0};
        *out_error = result.f_error;
        result.f_error = NULL;
    }
    example_calculator_add_result_clear(&result);
    return status;
}


/* Generated server skeleton; edit to implement your service.
 * Regeneration preserves this file. Merge IDL changes manually. */
#include "calculator_server.h"
#include <stdlib.h>

enum thrift_status example_calculator_server_add(void *user_context,
    int64_t arg_1_left,
    int64_t arg_2_right,
    int64_t *out_success,
    struct example_calculation_error **out_error)
{
    (void)user_context;
    if ((arg_2_right > 0 && arg_1_left > INT64_MAX - arg_2_right) ||
        (arg_2_right < 0 && arg_1_left < INT64_MIN - arg_2_right)) {
        const char message[] = "integer overflow";
        struct example_calculation_error *error = example_calculation_error_new();
        enum thrift_status status;
        if (!error)
            return THRIFT_NOMEM;
        status = example_calculation_error_set_message(error, message, sizeof(message) - 1);
        if (status != THRIFT_OK) {
            example_calculation_error_clear(error);
            free(error);
            return status;
        }
        *out_error = error;
        return THRIFT_OK;
    }
    *out_success = arg_1_left + arg_2_right;
    return THRIFT_OK;
}

enum thrift_status example_calculator_server_init(struct example_calculator_handler *handler, void *user_context)
{
    if (!handler) return THRIFT_NULL_HANDLER;
    *handler = (struct example_calculator_handler){0};
    handler->user_context = user_context;
    handler->f_add = example_calculator_server_add_adapter;
    return THRIFT_OK;
}

enum thrift_status example_calculator_server_process(struct thrift_protocol *protocol, void *handler_context)
{
    if (!protocol) return THRIFT_NULL_PROTOCOL;
    if (!protocol->ops) return THRIFT_PROTOCOL_UNINITIALIZED;
    if (!handler_context) return THRIFT_NULL_HANDLER;
    return example_calculator_process(protocol, handler_context);
}


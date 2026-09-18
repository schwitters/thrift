/** @file Generated server declarations. */
#ifndef CALCULATOR_SERVER_H
#define CALCULATOR_SERVER_H
#include "calculator_types.h"

/** Initialize callbacks and store borrowed application data in handler->user_context (may be NULL).
 * The handler must outlive all server requests; synchronize access to shared application data.
 * Returns THRIFT_NULL_HANDLER for a NULL handler, otherwise THRIFT_OK. */
enum thrift_status example_calculator_server_init(struct example_calculator_handler *handler, void *user_context);
/** Server-runtime adapter; handler_context points to the initialized handler, not user_context.
 * Validation returns THRIFT_NULL_PROTOCOL, THRIFT_PROTOCOL_UNINITIALIZED or THRIFT_NULL_HANDLER. */
enum thrift_status example_calculator_server_process(struct thrift_protocol *protocol, void *handler_context);

/**
 * Implement Calculator.add.
 * Flattened callback: one parameter per IDL argument, one output pointer per
 * success/declared exception, matching the --gen c11:flat_calls client call.
 * Arguments are borrowed for this call. On THRIFT_OK, populate exactly one
 * output (or none, for a void result with no exception); any other status is
 * reported to the caller as an application error. Copy borrowed data before
 * storing it in an owned output. Oneway methods have no outputs and cannot
 * send an error reply.
 */
enum thrift_status example_calculator_server_add(void *user_context,
    int64_t arg_1_left,
    int64_t arg_2_right,
    int64_t *out_success,
    struct example_calculation_error **out_error);
static inline enum thrift_status example_calculator_server_add_adapter(void *user_context, const struct example_calculator_add_args *args, struct example_calculator_add_result *result)
{
    enum thrift_status status;
    if (!args) return THRIFT_NULL_ARGS;
    if (!result) return THRIFT_NULL_RESULT;
    status = example_calculator_server_add(user_context, args->f_left, args->f_right, &result->f_success, &result->f_error);
    if (status != THRIFT_OK) return status;
    if (result->f_error) result->has_error = true;
    else result->has_success = true;
    return THRIFT_OK;
}

#endif

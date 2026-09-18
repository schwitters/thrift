/** @file Generated synchronous clients. */
#ifndef CALCULATOR_CLIENT_H
#define CALCULATOR_CLIENT_H
#include "calculator_types.h"

/**
 * 
 * Synchronous stack-allocated client. Borrows its protocol and transport.
 * Keep both alive for all calls; serialize access to a shared client/protocol.
 * The client neither connects nor closes the transport. Reconnect after transport/protocol errors.
 */
struct example_calculator_client {
    struct thrift_protocol *protocol; /**< Borrowed initialized protocol. */
    int32_t next_sequence; /**< Internal counter; do not modify. */
};
/** Bind an initialized protocol and start sequence IDs at 1.
 * Returns THRIFT_NULL_CLIENT, THRIFT_NULL_PROTOCOL or THRIFT_PROTOCOL_UNINITIALIZED; failure leaves the client unchanged. */
enum thrift_status example_calculator_client_init(struct example_calculator_client *client, struct thrift_protocol *protocol);

/**
 * 
 * Perform a synchronous RPC with an automatic sequence ID (wraps INT32_MAX to 1).
 * Args are borrowed. Initialize result to zero or a valid owned value; clear it after use.
 * A declared exception returns THRIFT_OK: inspect the result case.
 * Returns the RPC status; after I/O or protocol failure discard the connection.
 * Validation returns THRIFT_NULL_CLIENT, THRIFT_NULL_PROTOCOL, THRIFT_PROTOCOL_UNINITIALIZED,
 * THRIFT_NULL_ARGS, THRIFT_NULL_RESULT or THRIFT_INVALID_SEQUENCE for the first failed check.
 * Invalid arguments do not consume a sequence; each attempted RPC does.
 * A non-NULL result is required.
 */
enum thrift_status example_calculator_client_add_call(struct example_calculator_client *client,
    const struct example_calculator_add_args *args, struct example_calculator_add_result *result);

/**
 * Flattened synchronous call: one parameter per IDL argument instead of
 * an args record, one output pointer per success/declared exception instead
 * of a result record. Not generated for methods with an optional argument or
 * a string/list/set/map success type; use example_calculator_client_add_call for those.
 * Arguments are borrowed for the duration of the call; no ownership is taken.
 * Outputs must be non-NULL, nonoverlapping, and written only on THRIFT_OK.
 * Struct/exception output slots must initially contain NULL; otherwise returns
 * THRIFT_OUTPUT_NOT_EMPTY before I/O. Equal output addresses return THRIFT_INVALID_OWNERSHIP.
 * On THRIFT_OK, inactive outputs are zero/NULL and active owned outputs transfer
 * ownership to the caller. Clear and free owned outputs before reuse.
 * An empty void result is successful. Failure preserves every output.
 */
enum thrift_status example_calculator_client_add(struct example_calculator_client *client,
    int64_t arg_1_left,
    int64_t arg_2_right,
    int64_t *out_success,
    struct example_calculation_error **out_error);

#endif

/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/processor/thrift_processor.h>
#include "../thrift_log_internal.h"
#include "thrift_processor_internal.h"
#include <stdlib.h>
#include <string.h>

static enum thrift_status flush(struct thrift_protocol *p)
{
    return p->transport.flush ? p->transport.flush(p->transport.context) : THRIFT_OK;
}

static bool name_matches(const struct thrift_bytes *name, const char *expected)
{
    size_t size = strlen(expected);
    return name->size == size && (!size || memcmp(name->data, expected, size) == 0);
}

static enum thrift_status result_valid(const struct thrift_method *method, const void *result)
{
    size_t i, count = 0;
    for (i = 0; i < method->result->field_count; ++i) {
        const struct thrift_field *field = &method->result->fields[i];
        if (*(const bool *)((const uint8_t *)result + field->present_offset))
            ++count;
    }
    if (count > 1)
        return THRIFT_PROTOCOL;
    if (method->returns_value && !count)
        return THRIFT_MISSING_RESULT;
    return THRIFT_OK;
}

static enum thrift_status thrift_client_call_impl(struct thrift_protocol *p,
                                             const struct thrift_method *method,
                                             int32_t sequence, const void *args, void *result)
{
    struct thrift_bytes name = {0};
    enum thrift_message kind = C11_CALL;
    enum thrift_status status;
    int32_t received_sequence = 0;
    if (!p) return THRIFT_NULL_PROTOCOL;
    if (!p->ops) return THRIFT_PROTOCOL_UNINITIALIZED;
    if (!method) return THRIFT_NULL_METHOD;
    if (!args) return THRIFT_NULL_ARGS;
    if (!result && !method->oneway) return THRIFT_NULL_RESULT;
    status = thrift_message_write(p, method->name, method->oneway ? C11_ONEWAY : C11_CALL, sequence);
    if (status == THRIFT_OK)
        status = thrift_record_write(p, method->args, args);
    if (status == THRIFT_OK)
        status = flush(p);
    if (status != THRIFT_OK || method->oneway)
        return status;
    status = thrift_message_read(p, &name, &kind, &received_sequence);
    if (status != THRIFT_OK)
        goto cleanup;
    if (received_sequence != sequence || !name_matches(&name, method->name) ||
        (kind != C11_REPLY && kind != C11_EXCEPTION)) {
        status = THRIFT_PROTOCOL;
        goto cleanup;
    }
    if (kind == C11_EXCEPTION) {
        status = thrift_skip(p, THRIFT_STRUCT, 0);
        if (status == THRIFT_OK)
            status = THRIFT_REMOTE;
    } else {
        status = thrift_record_read(p, method->result, result);
        if (status == THRIFT_OK)
            status = result_valid(method, result);
    }
cleanup:
    free(name.data);
    return status;
}

enum thrift_status thrift_client_call(struct thrift_protocol *p,
                                             const struct thrift_method *method,
                                             int32_t sequence, const void *args, void *result)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "rpc.client.call", 0);
    enum thrift_status status = thrift_client_call_impl(p, method, sequence, args, result);
    thrift_log_end(scope, status);
    return status;
}

struct application_exception { int32_t code; bool present; };
static const struct thrift_field exception_fields[] = {
    {2, offsetof(struct application_exception, code), offsetof(struct application_exception, present),
     true, false, &thrift_type_i32}
};
static const struct thrift_record exception_record = {
    sizeof(struct application_exception), 1, exception_fields, false, NULL
};

static enum thrift_status exception_reply(struct thrift_protocol *p,
                                              const char *name, int32_t sequence, int32_t code)
{
    struct application_exception exception = {code, true};
    enum thrift_status status = thrift_message_write(p, name, C11_EXCEPTION, sequence);
    if (status == THRIFT_OK)
        status = thrift_record_write(p, &exception_record, &exception);
    if (status == THRIFT_OK)
        status = flush(p);
    return status;
}

enum thrift_status thrift_reject_request(struct thrift_protocol *p, const char *name,
    enum thrift_message kind, int32_t sequence, int32_t code)
{
    enum thrift_status status = thrift_skip(p, THRIFT_STRUCT, 0);
    if (status == THRIFT_OK && kind != C11_ONEWAY)
        status = exception_reply(p, name, sequence, code);
    return status;
}

enum thrift_status thrift_dispatch(struct thrift_protocol *p,
    const struct thrift_method *methods, size_t count, void *handler,
    const struct thrift_bytes *name, enum thrift_message kind, int32_t sequence)
{
    enum thrift_status status;
    const struct thrift_method *method = NULL;
    void *args = NULL, *result = NULL;
    size_t i;
    /* Names cannot contain NUL: callbacks and exception replies use C strings. */
    if (memchr(name->data, 0, name->size)) {
        status = THRIFT_PROTOCOL;
        goto cleanup;
    }
    for (i = 0; i < count; ++i) {
        if (name_matches(name, methods[i].name)) {
            method = &methods[i];
            break;
        }
    }
    if (!method || (kind != C11_CALL && kind != C11_ONEWAY) ||
        (method->oneway != (kind == C11_ONEWAY))) {
        status = thrift_reject_request(p, (const char *)name->data, kind, sequence,
                                    !method ? C11_UNKNOWN_METHOD : C11_INVALID_MESSAGE_TYPE);
        goto cleanup;
    }
    status = thrift_allocate(p, 1, method->args->size, &args);
    if (status == THRIFT_OK)
        status = thrift_allocate(p, 1, method->result->size, &result);
    if (status == THRIFT_OK)
        status = thrift_record_read(p, method->args, args);
    if (status != THRIFT_OK)
        goto cleanup;
    status = method->invoke ? method->invoke(handler, args, result) : THRIFT_MISSING_CALLBACK;
    if (status != THRIFT_OK) {
        const struct thrift_log_event event = {
            THRIFT_LOG_ERROR, THRIFT_LOG_END, "rpc.server.handler", status,
            0, NULL, 0, method->name
        };
        /* Preserve the local cause before converting it into a wire exception. */
        thrift_log_write(&event);
    }
    if (method->oneway)
        goto cleanup;
    if (status != THRIFT_OK) {
        status = exception_reply(p, method->name, sequence, C11_INTERNAL_ERROR);
        goto cleanup;
    }
    status = result_valid(method, result);
    if (status != THRIFT_OK) {
        status = exception_reply(p, method->name, sequence, C11_MISSING_RESULT);
        goto cleanup;
    }
    status = thrift_message_write(p, method->name, C11_REPLY, sequence);
    if (status == THRIFT_OK)
        status = thrift_record_write(p, method->result, result);
    if (status == THRIFT_OK)
        status = flush(p);
cleanup:
    if (method) {
        thrift_record_clear(method->args, args);
        thrift_record_clear(method->result, result);
    }
    free(args);
    free(result);
    return status;
}

static enum thrift_status thrift_process_impl(struct thrift_protocol *p,
                                         const struct thrift_method *methods,
                                         size_t count, void *handler)
{
    struct thrift_bytes name = {0};
    enum thrift_message kind = C11_CALL;
    enum thrift_status status;
    int32_t sequence = 0;
    if (!p) return THRIFT_NULL_PROTOCOL;
    if (!p->ops) return THRIFT_PROTOCOL_UNINITIALIZED;
    if (!methods && count) return THRIFT_NULL_METHOD;
    if (!handler) return THRIFT_NULL_HANDLER;
    status = thrift_message_read(p, &name, &kind, &sequence);
    if (status == THRIFT_OK)
        status = thrift_dispatch(p, methods, count, handler, &name, kind, sequence);
    free(name.data);
    return status;
}

enum thrift_status thrift_process(struct thrift_protocol *p,
                                         const struct thrift_method *methods,
                                         size_t count, void *handler)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "rpc.server.process", 0);
    enum thrift_status status = thrift_process_impl(p, methods, count, handler);
    thrift_log_end(scope, status);
    return status;
}

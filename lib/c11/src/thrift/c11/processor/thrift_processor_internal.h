/* SPDX-License-Identifier: Apache-2.0 */
#ifndef THRIFT_PROCESSOR_INTERNAL_H
#define THRIFT_PROCESSOR_INTERNAL_H
#include <thrift/c11/processor/thrift_processor.h>
#include "../protocol/thrift_protocol_internal.h"
enum { C11_UNKNOWN_METHOD = 1, C11_INVALID_MESSAGE_TYPE = 2,
       C11_MISSING_RESULT = 5, C11_INTERNAL_ERROR = 6 };
enum thrift_status thrift_dispatch(struct thrift_protocol *, const struct thrift_method *,
    size_t, void *, const struct thrift_bytes *, enum thrift_message, int32_t);
enum thrift_status thrift_reject_request(struct thrift_protocol *, const char *,
    enum thrift_message, int32_t, int32_t);
#endif

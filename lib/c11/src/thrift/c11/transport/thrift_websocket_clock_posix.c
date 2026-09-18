/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "thrift_websocket_internal.h"
#include "../thrift_log_internal.h"
#include <errno.h>
#include <time.h>
enum { MILLISECONDS_PER_SECOND = 1000, NANOSECONDS_PER_MILLISECOND = 1000000 };
enum thrift_status thrift_websocket_now(uint64_t *milliseconds)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        int saved_errno = errno;
        thrift_log_native("errno", saved_errno);
        return THRIFT_IO;
    }
    *milliseconds = (uint64_t)now.tv_sec * MILLISECONDS_PER_SECOND +
        (uint64_t)now.tv_nsec / NANOSECONDS_PER_MILLISECOND;
    return THRIFT_OK;
}

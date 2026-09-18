/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_log_internal.h"
/* Compatible with the minimum supported MSVC, which lacks _Thread_local. */
#if defined(_MSC_VER)
static __declspec(thread) struct thrift_log_thread_state thread_state;
#else
static _Thread_local struct thrift_log_thread_state thread_state;
#endif
struct thrift_log_thread_state *thrift_log_thread_state(void)
{
    return &thread_state;
}

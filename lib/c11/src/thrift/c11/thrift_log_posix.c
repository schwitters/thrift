/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_log_internal.h"
static _Thread_local struct thrift_log_thread_state thread_state;
struct thrift_log_thread_state *thrift_log_thread_state(void)
{
    return &thread_state;
}

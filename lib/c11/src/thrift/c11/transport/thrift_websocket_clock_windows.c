/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_websocket_internal.h"
#include <windows.h>
enum thrift_status thrift_websocket_now(uint64_t *milliseconds)
{
    *milliseconds = GetTickCount64();
    return THRIFT_OK;
}

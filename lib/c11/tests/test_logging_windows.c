/* SPDX-License-Identifier: Apache-2.0 */
#include "test_logging_thread.h"
#include <windows.h>
struct worker { void (*callback)(void *); void *context; };
static DWORD WINAPI run(LPVOID context)
{
    struct worker *worker = context;
    worker->callback(worker->context);
    return 0;
}
bool thrift_test_run_thread(void (*callback)(void *), void *context)
{
    struct worker worker = {callback, context};
    HANDLE thread = CreateThread(NULL, 0, run, &worker, 0, NULL);
    DWORD wait_status;
    if (!thread)
        return false;
    wait_status = WaitForSingleObject(thread, INFINITE);
    if (!CloseHandle(thread))
        return false;
    return wait_status == WAIT_OBJECT_0;
}

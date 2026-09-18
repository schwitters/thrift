/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_websocket_internal.h"
static enum thrift_status message_read(void *context, void *data, size_t size)
{
    struct thrift_websocket_message *message = context;
    if (message->error == THRIFT_OK)
        message->error = thrift_buffer_read(&message->request, data, size);
    return message->error;
}
static enum thrift_status message_write(void *context, const void *data, size_t size)
{
    struct thrift_websocket_message *message = context;
    if (message->error == THRIFT_OK)
        message->error = thrift_buffer_append(&message->response, data, size);
    return message->error;
}
static enum thrift_status message_flush(void *context)
{
    /* Commit only after the processor consumed the entire request successfully. */
    return ((struct thrift_websocket_message *)context)->error;
}
struct thrift_transport thrift_websocket_message_transport(struct thrift_websocket_message *message)
{
    struct thrift_transport transport = {message, message_read, message_write, message_flush};
    return transport;
}
enum thrift_status thrift_websocket_message_finish(struct thrift_websocket_message *message,
    enum thrift_status status)
{
    if (status == THRIFT_OK)
        status = message->error;
    if (status == THRIFT_OK && message->request.position != message->request.size)
        status = THRIFT_PROTOCOL;
    return status;
}

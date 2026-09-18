/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/processor/thrift_multiplexed_processor.h>
#include "../thrift_log_internal.h"
#include "thrift_processor_internal.h"
#include <stdlib.h>
#include <string.h>

static enum thrift_status validate_services(const struct thrift_service *services,
                                               size_t count, const char *default_service)
{
    size_t i, j;
    bool default_found = default_service == NULL;
    if (!services && count)
        return THRIFT_INVALID;
    for (i = 0; i < count; ++i) {
        const struct thrift_service *service = &services[i];
        if (!service->name || !service->name[0] || strchr(service->name, ':') ||
            !service->handler || (!service->methods && service->method_count))
            return THRIFT_INVALID;
        for (j = 0; j < i; ++j)
            if (strcmp(service->name, services[j].name) == 0)
                return THRIFT_INVALID;
        if (default_service && strcmp(service->name, default_service) == 0)
            default_found = true;
    }
    return default_found ? THRIFT_OK : THRIFT_INVALID;
}

static enum thrift_status thrift_multiplexed_process_impl(struct thrift_protocol *p,
    const struct thrift_service *services, size_t count, const char *default_service)
{
    struct thrift_bytes name = {0}, method_name;
    const struct thrift_service *service = NULL;
    enum thrift_message kind = C11_CALL;
    enum thrift_status status;
    int32_t sequence = 0;
    const uint8_t *separator;
    size_t i, service_size = 0;
    if (!p || !p->ops)
        return THRIFT_INVALID;
    status = validate_services(services, count, default_service);
    if (status != THRIFT_OK)
        return status;
    status = thrift_message_read(p, &name, &kind, &sequence);
    if (status != THRIFT_OK)
        goto cleanup;
    if (memchr(name.data, 0, name.size)) {
        status = THRIFT_PROTOCOL;
        goto cleanup;
    }
    method_name = name;
    separator = memchr(name.data, ':', name.size);
    if (separator) {
        service_size = (size_t)(separator - name.data);
        method_name.data = name.data + service_size + 1;
        method_name.size = name.size - service_size - 1;
        if (!service_size || !method_name.size || memchr(method_name.data, ':', method_name.size)) {
            status = thrift_reject_request(p, (const char *)method_name.data, kind, sequence, C11_UNKNOWN_METHOD);
            goto cleanup;
        }
    }
    for (i = 0; i < count; ++i) {
        if ((separator && strlen(services[i].name) == service_size &&
             memcmp(name.data, services[i].name, service_size) == 0) ||
            (!separator && default_service && strcmp(default_service, services[i].name) == 0)) {
            service = &services[i];
            break;
        }
    }
    if (!service)
        status = thrift_reject_request(p, (const char *)method_name.data, kind, sequence, C11_UNKNOWN_METHOD);
    else
        status = thrift_dispatch(p, service->methods, service->method_count, service->handler,
                              &method_name, kind, sequence);
cleanup:
    free(name.data);
    return status;
}

enum thrift_status thrift_multiplexed_process(struct thrift_protocol *p,
    const struct thrift_service *services, size_t count, const char *default_service)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "rpc.server.multiplexed", count);
    enum thrift_status status = thrift_multiplexed_process_impl(p, services, count, default_service);
    thrift_log_end(scope, status);
    return status;
}

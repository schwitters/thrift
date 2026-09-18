/* SPDX-License-Identifier: Apache-2.0 */
#include "cpro_integra_common_client.h"
#include <thrift/c11/protocol/thrift_multiplexed_protocol.h>
#include <thrift/c11/transport/thrift_http_client.h>
#include <thrift/c11/thrift_log.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DEFAULT_TIMEOUT_MS = 5000, BODY_LIMIT = 1048576, SERVICE_NAME_CAPACITY = 128,
       FIRST_ARGUMENT = 1, DECIMAL_BASE = 10 };
struct client_config {
    const char *url;
    const char *token;
    const char *ca_file;
    const char *timeout;
    const char *log_level;
    uint32_t timeout_ms;
};

static int usage(void)
{
    return puts("Usage: thrift_c11_cadin_client [options]\n"
        "Read StatusSvc.getApiVersion using Compact/Multiplexed HTTP(S).\n"
        "  --url URL             THRIFT_CADIN_URL (required; include /rpc)\n"
        "  --bearer-token TOKEN  THRIFT_CADIN_BEARER_TOKEN (required)\n"
        "  --ca-file PATH        THRIFT_CADIN_CA_FILE (optional CA bundle)\n"
        "  --timeout-ms MS       THRIFT_CADIN_TIMEOUT_MS (default 5000)\n"
        "  --log-level LEVEL     THRIFT_CADIN_LOG_LEVEL (default info)\n"
        "  --help                Show this help without accessing the network\n"
        "CLI overrides environment. Prefer the environment for tokens to avoid\n"
        "exposing them in process arguments. Use HTTPS for remote credentials.") < 0 ?
        EXIT_FAILURE : EXIT_SUCCESS;
}

static enum thrift_status parse_config(int argc, char **argv, struct client_config *config)
{
    int index;
    config->url = getenv("THRIFT_CADIN_URL");
    config->token = getenv("THRIFT_CADIN_BEARER_TOKEN");
    config->ca_file = getenv("THRIFT_CADIN_CA_FILE");
    config->timeout = getenv("THRIFT_CADIN_TIMEOUT_MS");
    config->log_level = getenv("THRIFT_CADIN_LOG_LEVEL");
    config->timeout_ms = DEFAULT_TIMEOUT_MS;
    for (index = FIRST_ARGUMENT; index < argc; ++index) {
        const char *option = argv[index];
        const char *value;
        if (++index == argc)
            return THRIFT_INVALID;
        value = argv[index];
        if (!strcmp(option, "--url")) config->url = value;
        else if (!strcmp(option, "--bearer-token")) config->token = value;
        else if (!strcmp(option, "--ca-file")) config->ca_file = value;
        else if (!strcmp(option, "--timeout-ms")) config->timeout = value;
        else if (!strcmp(option, "--log-level")) config->log_level = value;
        else return THRIFT_INVALID;
    }
    if (!config->url || !config->url[0] || !config->token || !config->token[0] ||
        (config->ca_file && !config->ca_file[0]))
        return THRIFT_INVALID;
    if (config->timeout) {
        unsigned long value;
        char *end;
        const char *digit;
        int saved_errno;
        if (!config->timeout[0])
            return THRIFT_INVALID;
        for (digit = config->timeout; *digit; ++digit)
            if (*digit < '0' || *digit > '9')
                return THRIFT_INVALID;
        errno = 0;
        value = strtoul(config->timeout, &end, DECIMAL_BASE);
        saved_errno = errno;
        if (saved_errno || *end || !value || value > INT_MAX)
            return THRIFT_INVALID;
        config->timeout_ms = (uint32_t)value;
    }
    return thrift_log_configure_level(config->log_level);
}

static void log_operation(enum thrift_log_phase phase, enum thrift_status status)
{
    struct thrift_log_event event = {0};
    event.level = status == THRIFT_OK ? THRIFT_LOG_INFO : THRIFT_LOG_ERROR;
    event.phase = phase;
    event.operation = "cadin.get_api_version";
    event.status = status;
    thrift_log_write(&event);
}

static enum thrift_status print_version(const struct thrift_bytes *version)
{
    size_t index;
    /* Keep untrusted server text from injecting terminal control sequences. */
    for (index = 0; index < version->size; ++index)
        if (version->data[index] < ' ' || version->data[index] > '~')
            return THRIFT_INVALID;
    if (version->size && fwrite(version->data, 1, version->size, stdout) != version->size)
        return THRIFT_IO;
    if (putchar('\n') == EOF || fflush(stdout) == EOF)
        return THRIFT_IO;
    return THRIFT_OK;
}

static enum thrift_status query_version(const struct client_config *config)
{
    struct thrift_http_client_options options = {0};
    struct thrift_http_client *client = NULL;
    struct thrift_transport transport = {0};
    struct thrift_protocol protocol;
    struct cpro_integra_common_status_svc_client service_client = {0};
    struct thrift_bytes service = {0};
    struct cpro_integra_common_status_svc_get_api_version_args args = {0};
    struct cpro_integra_common_status_svc_get_api_version_result result = {0};
    char service_name[SERVICE_NAME_CAPACITY];
    enum thrift_status status;
    bool initialized = false;
    log_operation(THRIFT_LOG_BEGIN, THRIFT_OK);
    status = thrift_http_client_library_init();
    if (status != THRIFT_OK)
        goto cleanup;
    initialized = true;
    options.url = config->url;
    options.ca_file = config->ca_file;
    options.timeout_ms = config->timeout_ms;
    options.max_body_size = BODY_LIMIT;
    status = cpro_integra_common_const_k_service_status_svc_get(&service);
    if (status != THRIFT_OK)
        goto cleanup;
    if (!service.size || service.size >= sizeof(service_name) ||
        memchr(service.data, '\0', service.size)) {
        status = THRIFT_INVALID;
        goto cleanup;
    }
    memcpy(service_name, service.data, service.size);
    service_name[service.size] = '\0';
    status = thrift_http_client_create(&options, &client, &transport);
    if (status != THRIFT_OK)
        goto cleanup;
    status = thrift_http_client_set_bearer_token(client, config->token);
    if (status != THRIFT_OK)
        goto cleanup;
    status = thrift_multiplexed_protocol_init(&protocol, transport, THRIFT_COMPACT, service_name);
    if (status != THRIFT_OK)
        goto cleanup;
    status = cpro_integra_common_status_svc_client_init(&service_client, &protocol);
    if (status == THRIFT_OK)
        status = cpro_integra_common_status_svc_client_get_api_version_call(
            &service_client, &args, &result);
    if (status == THRIFT_OK)
        status = result.has_success ? print_version(&result.f_success) : THRIFT_INVALID;
cleanup:
    cpro_integra_common_status_svc_get_api_version_result_clear(&result);
    cpro_integra_common_const_k_service_status_svc_clear(&service);
    thrift_http_client_destroy(client);
    if (initialized)
        thrift_http_client_library_cleanup();
    log_operation(THRIFT_LOG_END, status);
    return status;
}

int main(int argc, char **argv)
{
    struct client_config config = {0};
    enum thrift_status status;
    if (argc == FIRST_ARGUMENT + 1 && !strcmp(argv[FIRST_ARGUMENT], "--help"))
        return usage();
    status = parse_config(argc, argv, &config);
    if (status != THRIFT_OK) {
        struct thrift_log_event event = {0};
        event.level = THRIFT_LOG_ERROR;
        event.phase = THRIFT_LOG_END;
        event.operation = "cadin.configure";
        event.status = status;
        event.detail = "Invalid configuration; see --help";
        thrift_log_write(&event);
        return EXIT_FAILURE;
    }
    return query_version(&config) == THRIFT_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

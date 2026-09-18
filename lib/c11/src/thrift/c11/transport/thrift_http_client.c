/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/transport/thrift_http_client.h>
#include "../thrift_log_internal.h"
#include "thrift_buffer_internal.h"
#include "thrift_curl_internal.h"
#include <curl/curl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { HTTP_OK = 200 };
static const long http_follow_redirects = 0L;
static const long tls_verify_peer = 1L;
static const long tls_verify_hostname = 2L;
static const char http_scheme[] = "http://";
static const char https_scheme[] = "https://";
struct thrift_http_client {
    CURL *curl;
    struct curl_slist *headers;
    struct thrift_buffer request, response;
    enum thrift_status error;
};

static enum thrift_status thrift_http_client_library_init_impl(void)
{
    CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK)
        thrift_log_native("curl", code);
    return code == CURLE_OK ? THRIFT_OK : THRIFT_IO;
}

enum thrift_status thrift_http_client_library_init(void)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "curl.initialize", 0);
    enum thrift_status status = thrift_http_client_library_init_impl();
    thrift_log_end(scope, status);
    return status;
}

static void thrift_http_client_library_cleanup_impl(void)
{
    curl_global_cleanup();
}

void thrift_http_client_library_cleanup(void)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "curl.cleanup", 0);
    thrift_http_client_library_cleanup_impl();
    thrift_log_end(scope, THRIFT_OK);
}

static size_t receive_body(char *data, size_t size, size_t count, void *context)
{
    struct thrift_http_client *client = context;
    size_t bytes;
    if (size && count > SIZE_MAX / size) {
        client->error = THRIFT_LIMIT;
        return 0;
    }
    bytes = size * count;
    client->error = thrift_buffer_append(&client->response, data, bytes);
    return client->error == THRIFT_OK ? bytes : 0;
}

static enum thrift_status client_read(void *context, void *data, size_t size)
{
    struct thrift_http_client *client = context;
    if (!client)
        return THRIFT_INVALID;
    if (client->error != THRIFT_OK)
        return client->error;
    return thrift_buffer_read(&client->response, data, size);
}

static enum thrift_status client_write(void *context, const void *data, size_t size)
{
    struct thrift_http_client *client = context;
    if (!client)
        return THRIFT_INVALID;
    if (client->error == THRIFT_OK)
        client->error = thrift_buffer_append(&client->request, data, size);
    return client->error;
}

static enum thrift_status client_flush_impl(void *context)
{
    struct thrift_http_client *client = context;
    CURLcode code;
    long status;
    if (!client)
        return THRIFT_INVALID;
    if (client->error != THRIFT_OK)
        return client->error;
    client->response.size = client->response.position = 0;
    /* max_body_size is restricted to LONG_MAX at creation. */
    if ((code = curl_easy_setopt(client->curl, CURLOPT_POSTFIELDSIZE, (long)client->request.size)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_POSTFIELDS,
                         client->request.size ? (const char *)client->request.data : "")) != CURLE_OK) {
        thrift_log_native("curl", code);
        return client->error = THRIFT_IO;
    }
    code = curl_easy_perform(client->curl);
    if (client->error != THRIFT_OK)
        return client->error;
    if (code != CURLE_OK) {
        thrift_log_native("curl", code);
        return client->error = code == CURLE_OPERATION_TIMEDOUT ? THRIFT_TIMEOUT : THRIFT_IO;
    }
    if ((code = curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE, &status)) != CURLE_OK) {
        thrift_log_native("curl", code);
        return client->error = THRIFT_IO;
    }
    if (status != HTTP_OK) {
        thrift_log_native("http", status);
        return client->error = THRIFT_REMOTE;
    }
    client->request.size = 0;
    return THRIFT_OK;
}

static enum thrift_status client_flush(void *context)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "http.client.post", context ? ((struct thrift_http_client *)context)->request.size : 0);
    enum thrift_status status = client_flush_impl(context);
    thrift_log_end(scope, status);
    return status;
}

static enum thrift_status configure_client(struct thrift_http_client *client,
    const struct thrift_http_client_options *options)
{
    static const char *const headers[] = {
        "Content-Type: application/x-thrift", "Accept: application/x-thrift", "Expect:"
    };
    size_t index;
    CURLcode code;
    for (index = 0; index < sizeof(headers) / sizeof(headers[0]); ++index) {
        struct curl_slist *next = curl_slist_append(client->headers, headers[index]);
        if (!next)
            return THRIFT_NOMEM;
        client->headers = next;
    }
    if ((code = curl_easy_setopt(client->curl, CURLOPT_URL, options->url)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_PROTOCOLS_STR, "http,https")) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_FOLLOWLOCATION, http_follow_redirects)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_SSL_VERIFYPEER, tls_verify_peer)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_SSL_VERIFYHOST, tls_verify_hostname)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_HTTPHEADER, client->headers)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_POST, 1L)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_NOSIGNAL, 1L)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_TIMEOUT_MS, (long)options->timeout_ms)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_CONNECTTIMEOUT_MS, (long)options->timeout_ms)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, receive_body)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, client)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_USERAGENT, "Thrift/C11")) != CURLE_OK) {
        thrift_log_native("curl", code);
        return THRIFT_IO;
    }
    if (options->ca_file && (code = curl_easy_setopt(client->curl, CURLOPT_CAINFO, options->ca_file)) != CURLE_OK) {
        thrift_log_native("curl", code);
        return THRIFT_IO;
    }
    return THRIFT_OK;
}

static enum thrift_status thrift_http_client_create_impl(
    const struct thrift_http_client_options *options,
    struct thrift_http_client **result, struct thrift_transport *transport)
{
    struct thrift_http_client *client;
    enum thrift_status status;
    if (!result || !transport)
        return THRIFT_INVALID;
    *result = NULL;
    if (!options || !options->url ||
        (strncmp(options->url, http_scheme, sizeof(http_scheme) - 1) && strncmp(options->url, https_scheme, sizeof(https_scheme) - 1)) ||
        !options->max_body_size || options->max_body_size > LONG_MAX ||
        !options->timeout_ms || options->timeout_ms > INT_MAX)
        return THRIFT_INVALID;
    client = calloc(1, sizeof(*client));
    if (!client)
        return THRIFT_NOMEM;
    client->request.limit = client->response.limit = options->max_body_size;
    client->curl = curl_easy_init();
    if (!client->curl) {
        free(client);
        return THRIFT_NOMEM;
    }
    status = configure_client(client, options);
    if (status != THRIFT_OK) {
        thrift_http_client_destroy(client);
        return status;
    }
    transport->context = client;
    transport->read = client_read;
    transport->write = client_write;
    transport->flush = client_flush;
    *result = client;
    return THRIFT_OK;
}

enum thrift_status thrift_http_client_create(
    const struct thrift_http_client_options *options,
    struct thrift_http_client **result, struct thrift_transport *transport)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "http.client.create", options ? options->max_body_size : 0);
    enum thrift_status status = thrift_http_client_create_impl(options, result, transport);
    thrift_log_end(scope, status);
    return status;
}

static enum thrift_status set_bearer_token(struct thrift_http_client *client,
    const char *token)
{
    enum thrift_status status;
    CURLcode code;
    if (!client)
        return THRIFT_INVALID;
    if (client->error != THRIFT_OK)
        return client->error;
    status = thrift_curl_validate_bearer_token(token);
    if (status != THRIFT_OK)
        return status;
    if ((code = curl_easy_setopt(client->curl, CURLOPT_XOAUTH2_BEARER, token)) != CURLE_OK ||
        (code = curl_easy_setopt(client->curl, CURLOPT_HTTPAUTH,
            (long)(token ? CURLAUTH_BEARER : CURLAUTH_NONE))) != CURLE_OK) {
        thrift_log_native("curl", code);
        client->error = code == CURLE_OUT_OF_MEMORY ? THRIFT_NOMEM : THRIFT_IO;
        return client->error;
    }
    return THRIFT_OK;
}

enum thrift_status thrift_http_client_set_bearer_token(
    struct thrift_http_client *client, const char *token)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "http.client.authentication", 0);
    enum thrift_status status = set_bearer_token(client, token);
    thrift_log_end(scope, status);
    return status;
}

static void thrift_http_client_destroy_impl(struct thrift_http_client *client)
{
    if (!client)
        return;
    curl_easy_cleanup(client->curl);
    curl_slist_free_all(client->headers);
    thrift_buffer_clear(&client->request);
    thrift_buffer_clear(&client->response);
    free(client);
}

void thrift_http_client_destroy(struct thrift_http_client *client)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "http.client.destroy", 0);
    thrift_http_client_destroy_impl(client);
    thrift_log_end(scope, THRIFT_OK);
}

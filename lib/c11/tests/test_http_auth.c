/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/transport/thrift_http_client.h>
#include <thrift/c11/thrift_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    result = EXIT_FAILURE; goto cleanup; } } while (0)
enum { BODY_LIMIT = 32, TIMEOUT_MS = 2000, URL_ARGUMENT = 1,
       AUTH_ARGUMENT_COUNT = 2, POLICY_ARGUMENT_COUNT = 4, CA_ARGUMENT = 2, EXPECT_ARGUMENT = 3 };
static const char first_token[] = "test.AZaz09-._~+/==";
static const char second_token[] = "replacement-token";
static const char body[] = "abc";

static enum thrift_status post(struct thrift_transport *transport)
{
    enum thrift_status status = transport->write(transport->context, body, sizeof(body) - 1);
    return status == THRIFT_OK ? transport->flush(transport->context) : status;
}

int main(int argc, char **argv)
{
    static const char *const invalid_tokens[] = {
        "", "=", "a=b", "a b", "a\tb", "a\rb", "a\nb", "a:b", "a\x7f", "a\xff"
    };
    struct thrift_http_client_options options = {0};
    struct thrift_http_client *client = NULL;
    struct thrift_transport transport = {0};
    char copied_token[sizeof(first_token)];
    char *large = NULL;
    bool initialized = false;
    int result = EXIT_SUCCESS;
    size_t index;
    CHECK(argc == 1 || argc == AUTH_ARGUMENT_COUNT || argc == POLICY_ARGUMENT_COUNT);
    CHECK(thrift_log_configure_level("trace") == THRIFT_OK);
    CHECK(thrift_http_client_library_init() == THRIFT_OK);
    initialized = true;
    options.url = argc >= AUTH_ARGUMENT_COUNT ? argv[URL_ARGUMENT] : "http://127.0.0.1/unused";
    options.timeout_ms = TIMEOUT_MS;
    options.max_body_size = BODY_LIMIT;
    if (argc == POLICY_ARGUMENT_COUNT && strcmp(argv[CA_ARGUMENT], "-") != 0)
        options.ca_file = argv[CA_ARGUMENT];
    CHECK(thrift_http_client_create(&options, &client, &transport) == THRIFT_OK);
    if (argc == POLICY_ARGUMENT_COUNT) {
        enum thrift_status expected;
        if (strcmp(argv[EXPECT_ARGUMENT], "ok") == 0) expected = THRIFT_OK;
        else if (strcmp(argv[EXPECT_ARGUMENT], "redirect") == 0) expected = THRIFT_REMOTE;
        else { CHECK(strcmp(argv[EXPECT_ARGUMENT], "tls") == 0); expected = THRIFT_IO; }
        CHECK(post(&transport) == expected);
        goto cleanup;
    }
    CHECK(thrift_http_client_set_bearer_token(NULL, first_token) == THRIFT_INVALID);
    memcpy(copied_token, first_token, sizeof(first_token));
    CHECK(thrift_http_client_set_bearer_token(client, copied_token) == THRIFT_OK);
    memset(copied_token, 'x', sizeof(copied_token));
    if (argc == AUTH_ARGUMENT_COUNT)
        CHECK(post(&transport) == THRIFT_OK);
    for (index = 0; index < sizeof(invalid_tokens) / sizeof(invalid_tokens[0]); ++index)
        CHECK(thrift_http_client_set_bearer_token(client, invalid_tokens[index]) == THRIFT_INVALID);
    if (argc == AUTH_ARGUMENT_COUNT)
        CHECK(post(&transport) == THRIFT_OK);
    CHECK(thrift_http_client_set_bearer_token(client, second_token) == THRIFT_OK);
    if (argc == AUTH_ARGUMENT_COUNT)
        CHECK(post(&transport) == THRIFT_OK);
    CHECK(thrift_http_client_set_bearer_token(client, NULL) == THRIFT_OK);
    if (argc == AUTH_ARGUMENT_COUNT)
        CHECK(post(&transport) == THRIFT_OK);
    large = malloc(THRIFT_HTTP_BEARER_TOKEN_MAX + 2);
    CHECK(large);
    memset(large, 'x', THRIFT_HTTP_BEARER_TOKEN_MAX + 1);
    large[THRIFT_HTTP_BEARER_TOKEN_MAX] = '\0';
    CHECK(thrift_http_client_set_bearer_token(client, large) == THRIFT_OK);
    if (argc == AUTH_ARGUMENT_COUNT)
        CHECK(post(&transport) == THRIFT_OK);
    large[THRIFT_HTTP_BEARER_TOKEN_MAX] = 'x';
    large[THRIFT_HTTP_BEARER_TOKEN_MAX + 1] = '\0';
    CHECK(thrift_http_client_set_bearer_token(client, large) == THRIFT_LIMIT);
    if (argc == AUTH_ARGUMENT_COUNT)
        CHECK(post(&transport) == THRIFT_OK);
cleanup:
    free(large);
    thrift_http_client_destroy(client);
    if (initialized)
        thrift_http_client_library_cleanup();
    return result;
}

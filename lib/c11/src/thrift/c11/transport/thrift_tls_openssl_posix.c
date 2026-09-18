/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_tls_internal.h"
#include "../thrift_log_internal.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
struct thrift_tls_context { SSL_CTX *ssl; bool server; };
static enum thrift_status ssl_error(void)
{
    thrift_log_native("openssl", (int64_t)ERR_peek_last_error());
    return THRIFT_IO;
}
static int password_copy(char *buffer, int capacity, int writing, void *context)
{
    const char *password = context;
    size_t size = password ? strlen(password) : 0;
    (void)writing;
    if (capacity <= 0 || size >= (size_t)capacity) return 0;
    if (size) memcpy(buffer, password, size);
    buffer[size] = 0;
    return (int)size;
}
bool thrift_tls_context_server(const struct thrift_tls_context *context) { return context && context->server; }
enum thrift_status thrift_tls_context_create(const struct thrift_tls_options *options, struct thrift_tls_context **out)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "tls.credentials", 0);
    struct thrift_tls_context *context = NULL;
    enum thrift_status status = THRIFT_INVALID;
    if (!out) goto done;
    *out = NULL;
    if (!options || options->machine_key || (options->server && options->ca_file) || (options->server && (!options->certificate_file || !options->private_key_file)) ||
        (!options->server && (options->certificate_file || options->private_key_file || options->password))) goto done;
    context = calloc(1, sizeof(*context));
    if (!context) { status = THRIFT_NOMEM; goto done; }
    ERR_clear_error();
    context->server = options->server;
    context->ssl = SSL_CTX_new(TLS_method());
    if (!context->ssl) { status = ssl_error(); goto done; }
    if (!SSL_CTX_set_min_proto_version(context->ssl, TLS1_2_VERSION)) { status = ssl_error(); goto done; }
    SSL_CTX_set_max_cert_list(context->ssl, THRIFT_TLS_CIPHER_LIMIT);
    SSL_CTX_set_options(context->ssl, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    if (options->server) {
        SSL_CTX_set_default_passwd_cb(context->ssl, password_copy);
        SSL_CTX_set_default_passwd_cb_userdata(context->ssl, (void *)options->password);
        if (SSL_CTX_use_certificate_chain_file(context->ssl, options->certificate_file) != 1 ||
            SSL_CTX_use_PrivateKey_file(context->ssl, options->private_key_file, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(context->ssl) != 1) status = ssl_error();
        else status = THRIFT_OK;
        SSL_CTX_set_default_passwd_cb_userdata(context->ssl, NULL);
    } else {
        SSL_CTX_set_verify(context->ssl, SSL_VERIFY_PEER, NULL);
        status = (options->ca_file ? SSL_CTX_load_verify_locations(context->ssl, options->ca_file, NULL) :
            SSL_CTX_set_default_verify_paths(context->ssl)) == 1 ? THRIFT_OK : ssl_error();
    }
    if (status == THRIFT_OK) { *out = context; context = NULL; }
done:
    thrift_tls_context_destroy(context);
    thrift_log_end(scope, status);
    return status;
}
void thrift_tls_context_destroy(struct thrift_tls_context *context)
{
    if (context) { SSL_CTX_free(context->ssl); free(context); }
}
enum thrift_status thrift_tls_engine_create(struct thrift_tls_context *context, const char *name, struct thrift_tls_engine *engine)
{
    SSL *ssl = SSL_new(context->ssl);
    BIO *input = NULL, *output = NULL;
    unsigned char numeric[sizeof(struct in6_addr)];
    if (!ssl) return ssl_error();
    input = BIO_new(BIO_s_mem()); output = BIO_new(BIO_s_mem());
    if (!input || !output) { BIO_free(input); BIO_free(output); SSL_free(ssl); return THRIFT_NOMEM; }
    BIO_set_mem_eof_return(input, -1);
    SSL_set_bio(ssl, input, output);
    if (context->server) SSL_set_accept_state(ssl);
    else {
        SSL_set_connect_state(ssl);
        if (inet_pton(AF_INET, name, numeric) == 1 || inet_pton(AF_INET6, name, numeric) == 1) {
            if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), name) != 1) { SSL_free(ssl); return ssl_error(); }
        } else if (SSL_set1_host(ssl, name) != 1 || SSL_set_tlsext_host_name(ssl, name) != 1) {
            SSL_free(ssl); return ssl_error();
        }
    }
    engine->state = ssl;
    engine->output.limit = THRIFT_TLS_CIPHER_LIMIT;
    return THRIFT_OK;
}
void thrift_tls_engine_destroy(struct thrift_tls_engine *engine)
{
    SSL_free(engine->state); thrift_buffer_clear(&engine->output); memset(engine, 0, sizeof(*engine));
}
enum thrift_status thrift_tls_engine_feed(struct thrift_tls_engine *engine, const void *data, size_t size)
{
    BIO *input = SSL_get_rbio(engine->state);
    size_t pending = BIO_ctrl_pending(input);
    if (size > THRIFT_TLS_CIPHER_LIMIT || pending > THRIFT_TLS_CIPHER_LIMIT - size) return THRIFT_LIMIT;
    return BIO_write(input, data, (int)size) == (int)size ? THRIFT_OK : ssl_error();
}
static enum thrift_status collect(struct thrift_tls_engine *engine, enum thrift_status status)
{
    BIO *output = SSL_get_wbio(engine->state);
    uint8_t bytes[THRIFT_TLS_RECORD_SIZE];
    while (BIO_ctrl_pending(output)) {
        int count = BIO_read(output, bytes, sizeof(bytes));
        enum thrift_status copy;
        if (count <= 0) return ssl_error();
        copy = thrift_buffer_append(&engine->output, bytes, (size_t)count);
        if (copy != THRIFT_OK) return copy;
    }
    return status;
}
static enum thrift_status result(struct thrift_tls_engine *engine, int code)
{
    int error = SSL_get_error(engine->state, code);
    if (code > 0) return collect(engine, THRIFT_OK);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return collect(engine, THRIFT_AGAIN);
    if (error == SSL_ERROR_ZERO_RETURN) return THRIFT_EOF;
    return ssl_error();
}
enum thrift_status thrift_tls_engine_handshake(struct thrift_tls_engine *engine)
{
    enum thrift_status status;
    ERR_clear_error();
    status = result(engine, SSL_do_handshake(engine->state));
    if (status == THRIFT_OK) engine->ready = true;
    return status;
}
enum thrift_status thrift_tls_engine_read(struct thrift_tls_engine *engine, void *data, size_t size, size_t *count)
{
    int code;
    ERR_clear_error();
    code = SSL_read(engine->state, data, size > INT_MAX ? INT_MAX : (int)size);
    *count = code > 0 ? (size_t)code : 0;
    return result(engine, code);
}
enum thrift_status thrift_tls_engine_write(struct thrift_tls_engine *engine, const void *data, size_t size, size_t *count)
{
    int code;
    ERR_clear_error();
    code = SSL_write(engine->state, data, size > THRIFT_TLS_RECORD_SIZE ? THRIFT_TLS_RECORD_SIZE : (int)size);
    *count = code > 0 ? (size_t)code : 0;
    return result(engine, code);
}
enum thrift_status thrift_tls_engine_shutdown(struct thrift_tls_engine *engine)
{
    int code;
    ERR_clear_error();
    code = SSL_shutdown(engine->state);
    return code >= 0 ? collect(engine, THRIFT_OK) : result(engine, code);
}

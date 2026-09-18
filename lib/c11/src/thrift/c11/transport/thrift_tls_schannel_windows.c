/* SPDX-License-Identifier: Apache-2.0 */
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#include "thrift_tls_internal.h"
#include "../thrift_log_internal.h"
#include <windows.h>
#include <winternl.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <ncrypt.h>
#include <bcrypt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
enum { CREDENTIAL_FILE_LIMIT = 4 * 1024 * 1024, TLS_BUFFER_COUNT = 4,
       TLS_INPUT_TOKEN = 0, TLS_INPUT_EXTRA = 1,
       TLS_STREAM_HEADER = 0, TLS_STREAM_DATA = 1, TLS_STREAM_TRAILER = 2 };
struct thrift_tls_context {
    CredHandle credentials;
    NCRYPT_KEY_HANDLE persisted_key;
    HCERTSTORE certificates, roots;
    HCERTCHAINENGINE chain_engine;
    PCCERT_CONTEXT certificate;
    bool server, acquired;
};
struct schannel_state {
    struct thrift_tls_context *context;
    CtxtHandle handle;
    struct thrift_buffer input, plain;
    SecPkgContext_StreamSizes sizes;
    wchar_t *name;
    bool initialized, renegotiating;
};
static enum thrift_status security_error(SECURITY_STATUS code)
{
    thrift_log_native("schannel", code);
    return code == SEC_E_INSUFFICIENT_MEMORY ? THRIFT_NOMEM : THRIFT_IO;
}
static enum thrift_status windows_error(DWORD code)
{
    thrift_log_native("win32", code);
    return code == ERROR_NOT_ENOUGH_MEMORY ? THRIFT_NOMEM : THRIFT_IO;
}
static wchar_t *wide_string(const char *text)
{
    int count;
    wchar_t *value;
    if (!text) return NULL;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (count <= 0 || (size_t)count > SIZE_MAX / sizeof(*value)) return NULL;
    value = calloc((size_t)count, sizeof(*value));
    if (value && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, value, count) != count) {
        free(value); return NULL;
    }
    return value;
}
static enum thrift_status load_file(const char *path, struct thrift_buffer *buffer)
{
    wchar_t *wide = wide_string(path);
    HANDLE file;
    LARGE_INTEGER size;
    DWORD count, error;
    enum thrift_status status = THRIFT_IO;
    if (!wide) return THRIFT_INVALID;
    file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    error = GetLastError(); free(wide);
    if (file == INVALID_HANDLE_VALUE) return windows_error(error);
    if (!GetFileSizeEx(file, &size)) { status = windows_error(GetLastError()); goto done; }
    if (size.QuadPart <= 0 || size.QuadPart > CREDENTIAL_FILE_LIMIT) { status = THRIFT_LIMIT; goto done; }
    buffer->data = malloc((size_t)size.QuadPart);
    if (!buffer->data) { status = THRIFT_NOMEM; goto done; }
    buffer->capacity = buffer->size = (size_t)size.QuadPart;
    if (!ReadFile(file, buffer->data, (DWORD)buffer->size, &count, NULL)) status = windows_error(GetLastError());
    else status = count == buffer->size ? THRIFT_OK : THRIFT_IO;
done:
    if (!CloseHandle(file)) { error = GetLastError(); if (status == THRIFT_OK) status = windows_error(error); }
    return status;
}
/* Schannel delegates private-key operations to LSASS, which cannot use an
 * ephemeral in-process key handle. Clone only the selected key into a fresh,
 * CNG container in the explicitly selected user or machine store; never overwrite or delete a pre-existing key.
 * The context deletes this container after releasing its Schannel credentials. */
static enum thrift_status persist_identity(struct thrift_tls_context *context, bool machine_key)
{
    enum { RANDOM_NAME_BYTES = 16, KEY_NAME_CAPACITY = 64 };
    static const wchar_t hex[] = L"0123456789abcdef";
    static const wchar_t prefix[] = L"thrift-tls-";
    wchar_t name[KEY_NAME_CAPACITY] = {0};
    uint8_t random[RANDOM_NAME_BYTES];
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE source = 0;
    NCRYPT_PROV_HANDLE provider = 0;
    DWORD key_spec = 0, size = 0, written = 0;
    DWORD export_policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
    BOOL caller_free = FALSE;
    uint8_t *bytes = NULL;
    SECURITY_STATUS code;
    NTSTATUS random_status;
    CRYPT_KEY_PROV_INFO info = {0};
    NCryptBuffer parameter = {0};
    NCryptBufferDesc parameters = {NCRYPTBUFFER_VERSION, 1, &parameter};
    enum thrift_status status = THRIFT_IO;
    const char *operation = "NCryptExportKey.size";
    size_t index, prefix_size = (sizeof(prefix) / sizeof(prefix[0])) - 1;
    random_status = BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (random_status != 0) return security_error(random_status);
    memcpy(name, prefix, prefix_size * sizeof(*name));
    for (index = 0; index < sizeof(random); ++index) {
        name[prefix_size + index * 2] = hex[random[index] >> 4];
        name[prefix_size + index * 2 + 1] = hex[random[index] & 15];
    }
    if (!CryptAcquireCertificatePrivateKey(context->certificate,
        CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_CACHE_FLAG,
        NULL, &source, &key_spec, &caller_free)) return windows_error(GetLastError());
    if (key_spec != CERT_NCRYPT_KEY_SPEC) {
        thrift_log_native("certificate.key_spec", key_spec); return THRIFT_INVALID;
    }
    code = NCryptSetProperty(source, NCRYPT_EXPORT_POLICY_PROPERTY,
        (PBYTE)&export_policy, sizeof(export_policy), 0);
    if (code != ERROR_SUCCESS) { thrift_log_native("NCryptSetProperty.export", code); status = THRIFT_IO; goto done; }
    code = NCryptExportKey(source, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, NULL, 0, &size, 0);
    if (code != ERROR_SUCCESS) { thrift_log_native(operation, code); status = THRIFT_IO; goto done; }
    if (!size || size > CREDENTIAL_FILE_LIMIT) { status = THRIFT_LIMIT; goto done; }
    bytes = malloc(size);
    if (!bytes) { status = THRIFT_NOMEM; goto done; }
    operation = "NCryptExportKey.data";
    code = NCryptExportKey(source, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, bytes, size, &written, 0);
    if (code != ERROR_SUCCESS) { thrift_log_native(operation, code); status = THRIFT_IO; goto done; }
    operation = "NCryptOpenStorageProvider";
    code = NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0);
    if (code != ERROR_SUCCESS) { thrift_log_native(operation, code); status = THRIFT_IO; goto done; }
    parameter.cbBuffer = (ULONG)((wcslen(name) + 1) * sizeof(*name));
    parameter.BufferType = NCRYPTBUFFER_PKCS_KEY_NAME; parameter.pvBuffer = name;
    operation = "NCryptImportKey";
    code = NCryptImportKey(provider, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, &parameters,
        &context->persisted_key, bytes, written, NCRYPT_SILENT_FLAG | (machine_key ? NCRYPT_MACHINE_KEY_FLAG : 0));
    if (code != ERROR_SUCCESS) { thrift_log_native(operation, code); status = THRIFT_IO; goto done; }
    info.pwszContainerName = name;
    info.pwszProvName = MS_KEY_STORAGE_PROVIDER;
    info.dwKeySpec = AT_KEYEXCHANGE;
    info.dwFlags = machine_key ? CRYPT_MACHINE_KEYSET : 0;
    if (!CertSetCertificateContextProperty(context->certificate, CERT_KEY_PROV_INFO_PROP_ID, 0, &info)) {
        status = windows_error(GetLastError()); goto done;
    }
    if (!CertSetCertificateContextProperty(context->certificate, CERT_KEY_CONTEXT_PROP_ID, 0, NULL)) {
        status = windows_error(GetLastError()); goto done;
    }
    {
        HCRYPTPROV_OR_NCRYPT_KEY_HANDLE reopened = 0;
        DWORD spec = 0;
        BOOL owned = FALSE;
        if (!CryptAcquireCertificatePrivateKey(context->certificate,
            CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_COMPARE_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG,
            NULL, &reopened, &spec, &owned)) {
            DWORD saved = GetLastError();
            thrift_log_native("CryptAcquireCertificatePrivateKey.persisted", saved);
            status = THRIFT_IO; goto done;
        }
        if (owned) {
            code = NCryptFreeObject(reopened);
            if (code != ERROR_SUCCESS) { status = security_error(code); goto done; }
        }
    }
    status = THRIFT_OK;
done:
    if (bytes) { SecureZeroMemory(bytes, size); free(bytes); }
    if (provider) {
        code = NCryptFreeObject(provider);
        if (code != ERROR_SUCCESS) { if (status == THRIFT_OK) status = security_error(code); }
    }
    if (caller_free) {
        code = NCryptFreeObject(source);
        if (code != ERROR_SUCCESS) { if (status == THRIFT_OK) status = security_error(code); }
    }
    return status;
}
static enum thrift_status load_identity(struct thrift_tls_context *context, const struct thrift_tls_options *options)
{
    struct thrift_buffer file = {0};
    CRYPT_DATA_BLOB blob;
    wchar_t *password = wide_string(options->password ? options->password : "");
    enum thrift_status status;
    if (!password) return THRIFT_NOMEM;
    status = load_file(options->certificate_file, &file);
    if (status == THRIFT_OK) {
        blob.cbData = (DWORD)file.size; blob.pbData = file.data;
        context->certificates = PFXImportCertStore(&blob, password, PKCS12_NO_PERSIST_KEY | PKCS12_PREFER_CNG_KSP | PKCS12_ALWAYS_CNG_KSP | CRYPT_USER_KEYSET | CRYPT_EXPORTABLE);
        if (!context->certificates) status = windows_error(GetLastError());
        else {
            PCCERT_CONTEXT certificate = NULL;
            status = THRIFT_INVALID;
            while ((certificate = CertEnumCertificatesInStore(context->certificates, certificate)) != NULL) {
                DWORD bytes = 0;
                if (CertGetCertificateContextProperty(certificate, CERT_KEY_CONTEXT_PROP_ID, NULL, &bytes)) {
                    context->certificate = CertDuplicateCertificateContext(certificate);
                    status = context->certificate ? THRIFT_OK : THRIFT_IO;
                    if (!CertFreeCertificateContext(certificate)) {
                        DWORD saved = GetLastError();
                        if (status == THRIFT_OK) status = windows_error(saved);
                    }
                    break;
                }
            }
        }
    }
    SecureZeroMemory(password, (wcslen(password) + 1) * sizeof(*password)); free(password);
    if (file.data) SecureZeroMemory(file.data, file.size);
    thrift_buffer_clear(&file);
    if (status == THRIFT_OK) status = persist_identity(context, options->machine_key);
    return status;
}
static enum thrift_status load_root(struct thrift_tls_context *context, const char *path)
{
    struct thrift_buffer file = {0};
    CERT_CHAIN_ENGINE_CONFIG config = {0};
    enum thrift_status status = load_file(path, &file);
    if (status != THRIFT_OK) goto done;
    context->roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
    if (!context->roots) { status = windows_error(GetLastError()); goto done; }
    if (!CertAddEncodedCertificateToStore(context->roots, X509_ASN_ENCODING, file.data,
        (DWORD)file.size, CERT_STORE_ADD_NEW, NULL)) { status = windows_error(GetLastError()); goto done; }
    config.cbSize = sizeof(config);
    config.hExclusiveRoot = context->roots;
    config.dwFlags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL;
    if (!CertCreateCertificateChainEngine(&config, &context->chain_engine)) status = windows_error(GetLastError());
done:
    thrift_buffer_clear(&file);
    return status;
}
bool thrift_tls_context_server(const struct thrift_tls_context *context) { return context && context->server; }
enum thrift_status thrift_tls_context_create(const struct thrift_tls_options *options, struct thrift_tls_context **out)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "tls.credentials", 0);
    struct thrift_tls_context *context = NULL;
    SCH_CREDENTIALS credentials = {0};
    TLS_PARAMETERS parameters = {0};
    TimeStamp expires;
    SECURITY_STATUS code;
    enum thrift_status status = THRIFT_INVALID;
    if (!out) goto done;
    *out = NULL;
    if (!options || (options->server && options->ca_file) || options->private_key_file || (options->server && !options->certificate_file) ||
        (!options->server && (options->certificate_file || options->password || options->machine_key))) goto done;
    context = calloc(1, sizeof(*context));
    if (!context) { status = THRIFT_NOMEM; goto done; }
    context->server = options->server;
    status = options->server ? load_identity(context, options) : THRIFT_OK;
    if (status == THRIFT_OK && options->ca_file) status = load_root(context, options->ca_file);
    if (status != THRIFT_OK) goto done;
    credentials.dwVersion = SCH_CREDENTIALS_VERSION;
    credentials.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_CACHE_ONLY_URL_RETRIEVAL_ON_CREATE;
    if (!options->server) credentials.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    if (context->certificate) { credentials.cCreds = 1; credentials.paCred = &context->certificate; }
    parameters.grbitDisabledProtocols = SP_PROT_SSL2 | SP_PROT_SSL3 | SP_PROT_TLS1_0 | SP_PROT_TLS1_1;
    credentials.cTlsParameters = 1; credentials.pTlsParameters = &parameters;
    code = AcquireCredentialsHandleW(NULL, UNISP_NAME_W,
        options->server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND,
        NULL, &credentials, NULL, NULL, &context->credentials, &expires);
    status = code == SEC_E_OK ? THRIFT_OK : security_error(code);
    if (status == THRIFT_OK) { context->acquired = true; *out = context; context = NULL; }
done:
    thrift_tls_context_destroy(context); thrift_log_end(scope, status); return status;
}
void thrift_tls_context_destroy(struct thrift_tls_context *context)
{
    if (!context) return;
    if (context->acquired) {
        SECURITY_STATUS code = FreeCredentialsHandle(&context->credentials);
        if (code != SEC_E_OK) thrift_log_native("schannel", code);
    }
    if (context->persisted_key) {
        SECURITY_STATUS code = NCryptDeleteKey(context->persisted_key, NCRYPT_SILENT_FLAG);
        if (code != ERROR_SUCCESS) {
            const struct thrift_log_event event = {THRIFT_LOG_ERROR, THRIFT_LOG_END,
                "tls.credentials.delete_key", THRIFT_IO, 0, "ncrypt", code,
                "Owned CNG key container could not be deleted."};
            thrift_log_write(&event);
            code = NCryptFreeObject(context->persisted_key);
            if (code != ERROR_SUCCESS) thrift_log_native("ncrypt.free_key", code);
        }
    }
    if (context->certificate && !CertFreeCertificateContext(context->certificate)) thrift_log_native("win32", GetLastError());
    if (context->chain_engine) CertFreeCertificateChainEngine(context->chain_engine);
    if (context->certificates && !CertCloseStore(context->certificates, 0)) thrift_log_native("win32", GetLastError());
    if (context->roots && !CertCloseStore(context->roots, 0)) thrift_log_native("win32", GetLastError());
    free(context);
}
enum thrift_status thrift_tls_engine_create(struct thrift_tls_context *context, const char *name, struct thrift_tls_engine *engine)
{
    struct schannel_state *state = calloc(1, sizeof(*state));
    if (!state) return THRIFT_NOMEM;
    SecInvalidateHandle(&state->handle);
    state->context = context;
    state->input.limit = state->plain.limit = engine->output.limit = THRIFT_TLS_CIPHER_LIMIT;
    if (name) {
        state->name = wide_string(name);
        if (!state->name) { free(state); return THRIFT_INVALID; }
    }
    engine->state = state;
    return THRIFT_OK;
}
void thrift_tls_engine_destroy(struct thrift_tls_engine *engine)
{
    struct schannel_state *state = engine->state;
    if (state) {
        if (SecIsValidHandle(&state->handle)) {
            SECURITY_STATUS code = DeleteSecurityContext(&state->handle);
            if (code != SEC_E_OK) thrift_log_native("schannel", code);
        }
        thrift_buffer_clear(&state->input); thrift_buffer_clear(&state->plain);
        free(state->name); free(state);
    }
    thrift_buffer_clear(&engine->output); memset(engine, 0, sizeof(*engine));
}
enum thrift_status thrift_tls_engine_feed(struct thrift_tls_engine *engine, const void *data, size_t size)
{
    struct schannel_state *state = engine->state;
    return thrift_buffer_append(&state->input, data, size);
}
static enum thrift_status verify_peer(struct schannel_state *state)
{
    PCCERT_CONTEXT certificate = NULL;
    PCCERT_CHAIN_CONTEXT chain = NULL;
    CERT_CHAIN_PARA chain_parameters = {0};
    CERT_CHAIN_POLICY_PARA policy = {0};
    CERT_CHAIN_POLICY_STATUS policy_status = {0};
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl = {0};
    LPSTR usage = szOID_PKIX_KP_SERVER_AUTH;
    enum thrift_status status = THRIFT_IO;
    SECURITY_STATUS code = QueryContextAttributesW(&state->handle, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &certificate);
    if (code != SEC_E_OK) return security_error(code);
    chain_parameters.cbSize = sizeof(chain_parameters);
    chain_parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    chain_parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
    chain_parameters.RequestedUsage.Usage.rgpszUsageIdentifier = &usage;
    /* No network retrieval in the event thread: use supplied intermediates and
     * cached/system roots. No revocation bypass or hostname-ignore flags. */
    if (!CertGetCertificateChain(state->context->chain_engine, certificate, NULL,
        certificate->hCertStore, &chain_parameters,
        CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL | CERT_CHAIN_DISABLE_AUTH_ROOT_AUTO_UPDATE,
        NULL, &chain)) { status = windows_error(GetLastError()); goto done; }
    ssl.cbSize = sizeof(ssl); ssl.dwAuthType = AUTHTYPE_SERVER; ssl.pwszServerName = state->name;
    policy.cbSize = sizeof(policy); policy.pvExtraPolicyPara = &ssl;
    policy_status.cbSize = sizeof(policy_status);
    if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &policy_status))
        status = windows_error(GetLastError());
    else if (policy_status.dwError) status = windows_error(policy_status.dwError);
    else status = THRIFT_OK;
done:
    if (chain) CertFreeCertificateChain(chain);
    if (!CertFreeCertificateContext(certificate)) { DWORD saved = GetLastError(); if (status == THRIFT_OK) status = windows_error(saved); }
    return status;
}
static enum thrift_status keep_extra(struct schannel_state *state, SecBuffer *buffers, size_t count)
{
    size_t index, extra = 0;
    for (index = 0; index < count; ++index)
        if (buffers[index].BufferType == SECBUFFER_EXTRA) extra = buffers[index].cbBuffer;
    if (extra > state->input.size) return THRIFT_IO;
    if (extra) memmove(state->input.data, state->input.data + state->input.size - extra, extra);
    state->input.size = extra;
    return THRIFT_OK;
}
static enum thrift_status handshake(struct thrift_tls_engine *engine, bool shutdown)
{
    struct schannel_state *state = engine->state;
    SecBuffer input[2] = {{0}}, output = {0};
    SecBufferDesc input_desc = {SECBUFFER_VERSION, 2, input}, output_desc = {SECBUFFER_VERSION, 1, &output};
    ULONG attributes = 0;
    TimeStamp expires;
    SECURITY_STATUS code;
    enum thrift_status status = THRIFT_OK;
    bool first = !state->initialized;
    if (!shutdown && (state->context->server || !first) && !state->input.size) return THRIFT_AGAIN;
    input[TLS_INPUT_TOKEN].BufferType = SECBUFFER_TOKEN;
    input[TLS_INPUT_TOKEN].pvBuffer = state->input.data;
    input[TLS_INPUT_TOKEN].cbBuffer = (ULONG)state->input.size;
    input[TLS_INPUT_EXTRA].BufferType = SECBUFFER_EMPTY;
    output.BufferType = SECBUFFER_TOKEN;
    if (state->context->server)
        code = AcceptSecurityContext(&state->context->credentials, first ? NULL : &state->handle,
            shutdown ? NULL : &input_desc, ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_CONFIDENTIALITY |
            ASC_REQ_REPLAY_DETECT | ASC_REQ_SEQUENCE_DETECT | ASC_REQ_STREAM,
            SECURITY_NATIVE_DREP, &state->handle, &output_desc, &attributes, &expires);
    else
        code = InitializeSecurityContextW(&state->context->credentials, first ? NULL : &state->handle,
            state->name, ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT |
            ISC_REQ_SEQUENCE_DETECT | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION,
            0, SECURITY_NATIVE_DREP, first || shutdown ? NULL : &input_desc, 0,
            &state->handle, &output_desc, &attributes, &expires);
    if (SecIsValidHandle(&state->handle)) state->initialized = true;
    if (output.pvBuffer) {
        status = thrift_buffer_append(&engine->output, output.pvBuffer, output.cbBuffer);
        { SECURITY_STATUS freed = FreeContextBuffer(output.pvBuffer);
          if (status == THRIFT_OK && freed != SEC_E_OK) status = security_error(freed); }
    }
    if (status != THRIFT_OK) return status;
    if (code == SEC_E_INCOMPLETE_MESSAGE) return THRIFT_AGAIN;
    if (code != SEC_E_OK && code != SEC_I_CONTINUE_NEEDED && !(shutdown && code == SEC_I_CONTEXT_EXPIRED))
        return security_error(code);
    if (!shutdown) {
        status = keep_extra(state, input, 2);
        if (status != THRIFT_OK) return status;
    }
    if (shutdown) return THRIFT_OK;
    if (code == SEC_I_CONTINUE_NEEDED) return THRIFT_AGAIN;
    if (!state->context->server) {
        status = verify_peer(state);
        if (status != THRIFT_OK) return status;
    }
    code = QueryContextAttributesW(&state->handle, SECPKG_ATTR_STREAM_SIZES, &state->sizes);
    if (code != SEC_E_OK) return security_error(code);
    if (!state->sizes.cbMaximumMessage || state->sizes.cbMaximumMessage > THRIFT_TLS_RECORD_SIZE ||
        state->sizes.cbHeader > THRIFT_TLS_CIPHER_LIMIT / 4 || state->sizes.cbTrailer > THRIFT_TLS_CIPHER_LIMIT / 4)
        return THRIFT_LIMIT;
    engine->ready = true; state->renegotiating = false;
    return THRIFT_OK;
}
enum thrift_status thrift_tls_engine_handshake(struct thrift_tls_engine *engine) { return handshake(engine, false); }
enum thrift_status thrift_tls_engine_read(struct thrift_tls_engine *engine, void *data, size_t size, size_t *count)
{
    struct schannel_state *state = engine->state;
    enum thrift_status status;
    *count = 0;
    while (state->plain.position == state->plain.size) {
        SecBuffer buffers[TLS_BUFFER_COUNT] = {{0}};
        SecBufferDesc desc = {SECBUFFER_VERSION, TLS_BUFFER_COUNT, buffers};
        SECURITY_STATUS code;
        size_t i, previous = state->input.size;
        if (state->renegotiating) {
            status = handshake(engine, false);
            if (status != THRIFT_OK) return status;
        }
        state->plain.size = state->plain.position = 0;
        if (!state->input.size) return THRIFT_AGAIN;
        buffers[0].BufferType = SECBUFFER_DATA; buffers[0].pvBuffer = state->input.data;
        buffers[0].cbBuffer = (ULONG)state->input.size;
        code = DecryptMessage(&state->handle, &desc, 0, NULL);
        if (code == SEC_E_INCOMPLETE_MESSAGE) return THRIFT_AGAIN;
        if (code == SEC_I_CONTEXT_EXPIRED) return THRIFT_EOF;
        if (code != SEC_E_OK && code != SEC_I_RENEGOTIATE) return security_error(code);
        for (i = 0; i < TLS_BUFFER_COUNT; ++i) if (buffers[i].BufferType == SECBUFFER_DATA) {
            status = thrift_buffer_append(&state->plain, buffers[i].pvBuffer, buffers[i].cbBuffer);
            if (status != THRIFT_OK) return status;
        }
        status = keep_extra(state, buffers, TLS_BUFFER_COUNT);
        if (status != THRIFT_OK) return status;
        if (code == SEC_I_RENEGOTIATE) state->renegotiating = true;
        /* Drain buffered control records without waiting for another network
         * readiness event. Each iteration must consume input or yield. */
        if (!state->plain.size && state->input.size >= previous) return THRIFT_AGAIN;
    }
    *count = state->plain.size - state->plain.position;
    if (*count > size) *count = size;
    return thrift_buffer_read(&state->plain, data, *count);
}
enum thrift_status thrift_tls_engine_write(struct thrift_tls_engine *engine, const void *data, size_t size, size_t *count)
{
    struct schannel_state *state = engine->state;
    SecBuffer buffers[TLS_BUFFER_COUNT] = {{0}};
    SecBufferDesc desc = {SECBUFFER_VERSION, TLS_BUFFER_COUNT, buffers};
    uint8_t *bytes;
    size_t capacity, i;
    SECURITY_STATUS code;
    enum thrift_status status = THRIFT_OK;
    *count = 0;
    if (state->renegotiating) return THRIFT_AGAIN;
    if (size > state->sizes.cbMaximumMessage) size = state->sizes.cbMaximumMessage;
    capacity = state->sizes.cbHeader + size + state->sizes.cbTrailer;
    bytes = malloc(capacity);
    if (!bytes) return THRIFT_NOMEM;
    buffers[TLS_STREAM_HEADER].BufferType = SECBUFFER_STREAM_HEADER;
    buffers[TLS_STREAM_HEADER].pvBuffer = bytes; buffers[TLS_STREAM_HEADER].cbBuffer = state->sizes.cbHeader;
    buffers[TLS_STREAM_DATA].BufferType = SECBUFFER_DATA;
    buffers[TLS_STREAM_DATA].pvBuffer = bytes + state->sizes.cbHeader; buffers[TLS_STREAM_DATA].cbBuffer = (ULONG)size;
    memcpy(buffers[TLS_STREAM_DATA].pvBuffer, data, size);
    buffers[TLS_STREAM_TRAILER].BufferType = SECBUFFER_STREAM_TRAILER;
    buffers[TLS_STREAM_TRAILER].pvBuffer = bytes + state->sizes.cbHeader + size;
    buffers[TLS_STREAM_TRAILER].cbBuffer = state->sizes.cbTrailer;
    code = EncryptMessage(&state->handle, 0, &desc, 0);
    if (code != SEC_E_OK) status = security_error(code);
    else for (i = 0; i <= TLS_STREAM_TRAILER && status == THRIFT_OK; ++i)
        status = thrift_buffer_append(&engine->output, buffers[i].pvBuffer, buffers[i].cbBuffer);
    SecureZeroMemory(bytes, capacity); free(bytes);
    if (status == THRIFT_OK) *count = size;
    return status;
}
enum thrift_status thrift_tls_engine_shutdown(struct thrift_tls_engine *engine)
{
    struct schannel_state *state = engine->state;
    DWORD shutdown = SCHANNEL_SHUTDOWN;
    SecBuffer buffer = {sizeof(shutdown), SECBUFFER_TOKEN, &shutdown};
    SecBufferDesc desc = {SECBUFFER_VERSION, 1, &buffer};
    SECURITY_STATUS code = ApplyControlToken(&state->handle, &desc);
    return code == SEC_E_OK ? handshake(engine, true) : security_error(code);
}

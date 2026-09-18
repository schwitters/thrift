/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_curl_internal.h"
#include <thrift/c11/transport/thrift_http_client.h>
enum thrift_status thrift_curl_validate_bearer_token(const char *token)
{
    size_t index;
    bool padding = false;
    if (!token)
        return THRIFT_OK;
    if (!token[0])
        return THRIFT_INVALID;
    for (index = 0; token[index]; ++index) {
        unsigned char character = (unsigned char)token[index];
        if (index == THRIFT_HTTP_BEARER_TOKEN_MAX)
            return THRIFT_LIMIT;
        if (character == '=' && index != 0) {
            padding = true;
            continue;
        }
        if (padding || !((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '.' || character == '_' ||
            character == '~' || character == '+' || character == '/'))
            return THRIFT_INVALID;
    }
    return THRIFT_OK;
}


#include "ca_http.h"

#include <curl/curl.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define CA_HTTP_MAX_RESPONSE_BYTES (256u * 1024u)

typedef struct ca_http_write_ctx {
    ca_http_response_t *response;
    int overflow;
} ca_http_write_ctx_t;

void ca_http_response_init(ca_http_response_t *response)
{
    if (response == NULL) {
        return;
    }

    memset(response, 0, sizeof(*response));
}

void ca_http_response_free(ca_http_response_t *response)
{
    if (response == NULL) {
        return;
    }

    free(response->body);
    response->body = NULL;
    response->body_len = 0;
    response->status_code = 0;
    response->error_message[0] = '\0';
}

static size_t ca_http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    ca_http_write_ctx_t *ctx = (ca_http_write_ctx_t *)userdata;
    ca_http_response_t *response;
    size_t incoming;
    size_t next_len;
    char *next_body;

    if (ctx == NULL || ctx->response == NULL || ptr == NULL) {
        return 0;
    }

    response = ctx->response;
    incoming = size * nmemb;
    if (incoming == 0) {
        return 0;
    }
    if (response->body_len > CA_HTTP_MAX_RESPONSE_BYTES ||
        incoming > CA_HTTP_MAX_RESPONSE_BYTES - response->body_len) {
        ctx->overflow = 1;
        return 0;
    }

    next_len = response->body_len + incoming;
    next_body = (char *)realloc(response->body, next_len + 1);
    if (next_body == NULL) {
        return 0;
    }

    memcpy(next_body + response->body_len, ptr, incoming);
    next_body[next_len] = '\0';
    response->body = next_body;
    response->body_len = next_len;
    return incoming;
}

static ca_status_t ca_http_set_error(ca_http_response_t *response, const char *message)
{
    if (response == NULL || message == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    (void)snprintf(response->error_message, sizeof(response->error_message), "%s", message);
    return CA_ERR_IO;
}

ca_status_t ca_http_post_json(const char *url,
                              const char *bearer_token,
                              const char *json_body,
                              long timeout_seconds,
                              ca_http_response_t *response)
{
    CURL *curl;
    CURLcode code;
    struct curl_slist *headers = NULL;
    struct curl_slist *next_headers;
    char auth_header[1024];
    int auth_written;
    ca_http_write_ctx_t write_ctx;

    if (url == NULL || url[0] == '\0' || bearer_token == NULL || bearer_token[0] == '\0' ||
        json_body == NULL || response == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    ca_http_response_init(response);

    code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK) {
        return ca_http_set_error(response, "curl_global_init failed");
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        curl_global_cleanup();
        return ca_http_set_error(response, "curl_easy_init failed");
    }

    next_headers = curl_slist_append(headers, "Content-Type: application/json");
    if (next_headers == NULL) {
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return ca_http_set_error(response, "failed to allocate HTTP headers");
    }
    headers = next_headers;

    /*
     * API keys only exist in memory long enough to create the Authorization
     * header. This module never logs the header or token.
     */
    auth_written = snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", bearer_token);
    if (auth_written < 0 || (size_t)auth_written >= sizeof(auth_header)) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return ca_http_set_error(response, "Authorization header is too long");
    }
    next_headers = curl_slist_append(headers, auth_header);
    if (next_headers == NULL) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return ca_http_set_error(response, "failed to allocate Authorization header");
    }
    headers = next_headers;

    write_ctx.response = response;
    write_ctx.overflow = 0;

    (void)curl_easy_setopt(curl, CURLOPT_URL, url);
    (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    (void)curl_easy_setopt(curl, CURLOPT_POST, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ca_http_write_cb);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds > 0 ? timeout_seconds : 60L);
    (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    code = curl_easy_perform(curl);
    if (code == CURLE_OK) {
        (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status_code);
    } else {
        (void)snprintf(response->error_message,
                       sizeof(response->error_message),
                       "%s",
                       write_ctx.overflow ? "HTTP response exceeded size limit" : curl_easy_strerror(code));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    if (code != CURLE_OK) {
        return CA_ERR_IO;
    }

    if (response->body == NULL) {
        response->body = (char *)malloc(1);
        if (response->body == NULL) {
            return CA_ERR_NO_MEMORY;
        }
        response->body[0] = '\0';
    }

    return CA_OK;
}

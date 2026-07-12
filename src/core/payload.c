#include "ca_payload.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CA_PAYLOAD_JSON_TAIL_RESERVE 512u

static size_t ca_payload_min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

static ca_status_t ca_payload_reserve(ca_payload_t *payload, size_t needed)
{
    size_t new_cap;
    char *new_data;

    if (payload == NULL || payload->data == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    if (needed <= payload->cap) {
        return CA_OK;
    }
    if (needed > payload->max_total) {
        payload->truncated = 1;
        return CA_OK;
    }

#if CAGENT_ENABLE_LARGE_PAYLOAD
    new_cap = payload->cap;
    while (new_cap < needed && new_cap < payload->max_total) {
        new_cap *= 2u;
        if (new_cap == 0) {
            new_cap = needed;
        }
    }
    new_cap = ca_payload_min_size(new_cap, payload->max_total);
    new_data = (char *)malloc(new_cap + 1u);
    if (new_data == NULL) {
        return CA_ERR_NO_MEMORY;
    }
    memcpy(new_data, payload->data, payload->len + 1u);
    if (payload->heap_allocated) {
        free(payload->data);
    }
    payload->data = new_data;
    payload->cap = new_cap;
    payload->heap_allocated = 1;
    return CA_OK;
#else
    payload->truncated = 1;
    return CA_OK;
#endif
}

ca_status_t ca_payload_init(ca_payload_t *payload, size_t inline_cap, size_t max_total)
{
    if (payload == NULL || max_total == 0) {
        return CA_ERR_INVALID_ARG;
    }

    memset(payload, 0, sizeof(*payload));
    payload->cap = ca_payload_min_size(inline_cap, CA_MAX_PAYLOAD_INLINE);
    if (payload->cap == 0) {
        payload->cap = ca_payload_min_size(CA_MAX_PAYLOAD_INLINE, max_total);
    }
    payload->max_total = max_total < payload->cap ? payload->cap : max_total;
    payload->data = payload->inline_buf;
    payload->inline_buf[0] = '\0';
    return CA_OK;
}

void ca_payload_free(ca_payload_t *payload)
{
    if (payload == NULL) {
        return;
    }
    if (payload->heap_allocated) {
        free(payload->data);
    }
    memset(payload, 0, sizeof(*payload));
}

ca_status_t ca_payload_append(ca_payload_t *payload, const char *data, size_t len)
{
    size_t writable;
    ca_status_t status;

    if (payload == NULL || payload->data == NULL || data == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return CA_OK;
    }

    if (payload->len + len > payload->max_total) {
        len = payload->max_total > payload->len ? payload->max_total - payload->len : 0;
        payload->truncated = 1;
    }

    status = ca_payload_reserve(payload, payload->len + len);
    if (status != CA_OK) {
        return status;
    }

    writable = payload->cap > payload->len ? payload->cap - payload->len : 0;
    if (len > writable) {
        len = writable;
        payload->truncated = 1;
    }
    if (len > 0) {
        memcpy(payload->data + payload->len, data, len);
        payload->len += len;
        payload->data[payload->len] = '\0';
    }
    return CA_OK;
}

ca_status_t ca_payload_set(ca_payload_t *payload, const char *data, size_t len)
{
    if (payload == NULL || payload->data == NULL || data == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    payload->len = 0;
    payload->truncated = 0;
    payload->data[0] = '\0';
    return ca_payload_append(payload, data, len);
}

ca_status_t ca_payload_append_cstr(ca_payload_t *payload, const char *text)
{
    if (text == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    return ca_payload_append(payload, text, strlen(text));
}

ca_status_t ca_payload_appendf(ca_payload_t *payload, const char *format, ...)
{
    char stack_buf[1024];
    va_list args;
    int written;
    char *heap_buf = NULL;
    ca_status_t status;

    if (payload == NULL || format == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    va_start(args, format);
    written = vsnprintf(stack_buf, sizeof(stack_buf), format, args);
    va_end(args);
    if (written < 0) {
        return CA_ERR_INVALID_ARG;
    }
    if ((size_t)written < sizeof(stack_buf)) {
        return ca_payload_append(payload, stack_buf, (size_t)written);
    }

    heap_buf = (char *)malloc((size_t)written + 1u);
    if (heap_buf == NULL) {
        return CA_ERR_NO_MEMORY;
    }
    va_start(args, format);
    (void)vsnprintf(heap_buf, (size_t)written + 1u, format, args);
    va_end(args);
    status = ca_payload_append(payload, heap_buf, (size_t)written);
    free(heap_buf);
    return status;
}

ca_status_t ca_payload_append_json_escaped(ca_payload_t *payload, const char *text, size_t len)
{
    size_t i;

    if (payload == NULL || payload->data == NULL || text == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    for (i = 0; i < len && text[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)text[i];
        char escaped[8];
        const char *piece = NULL;
        size_t piece_len = 0;

        if (payload->max_total > CA_PAYLOAD_JSON_TAIL_RESERVE &&
            payload->len >= payload->max_total - CA_PAYLOAD_JSON_TAIL_RESERVE) {
            payload->truncated = 1;
            return CA_OK;
        }

        switch (ch) {
        case '"':
            piece = "\\\"";
            piece_len = 2;
            break;
        case '\\':
            piece = "\\\\";
            piece_len = 2;
            break;
        case '\n':
            piece = "\\n";
            piece_len = 2;
            break;
        case '\r':
            piece = "\\r";
            piece_len = 2;
            break;
        case '\t':
            piece = "\\t";
            piece_len = 2;
            break;
        case '\b':
            piece = "\\b";
            piece_len = 2;
            break;
        case '\f':
            piece = "\\f";
            piece_len = 2;
            break;
        default:
            if (ch < 0x20) {
                int written = snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned int)ch);
                if (written < 0 || (size_t)written >= sizeof(escaped)) {
                    return CA_ERR_INVALID_ARG;
                }
                piece = escaped;
                piece_len = (size_t)written;
            } else {
                escaped[0] = (char)ch;
                piece = escaped;
                piece_len = 1;
            }
            break;
        }

        if (ca_payload_append(payload, piece, piece_len) != CA_OK) {
            return CA_ERR_NO_MEMORY;
        }
        if (payload->truncated) {
            return CA_OK;
        }
    }

    return CA_OK;
}

const char *ca_payload_data(const ca_payload_t *payload)
{
    return payload != NULL && payload->data != NULL ? payload->data : "";
}

size_t ca_payload_len(const ca_payload_t *payload)
{
    return payload != NULL ? payload->len : 0u;
}

int ca_payload_truncated(const ca_payload_t *payload)
{
    return payload != NULL ? payload->truncated : 0;
}

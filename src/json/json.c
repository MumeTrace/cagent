/*
 * json.c
 * A small shared JSON foundation for Phase 5B. It is intentionally scoped to
 * object-field extraction and string escaping; Agent Loop parsing can later
 * build on this module or replace it with a full parser behind the same border.
 */
#include "ca_json.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ca_json_skip_ws_range(const char *text, const char *end)
{
    while (text != NULL && text < end && isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static int ca_json_is_value_delimiter(const char *cursor, const char *end)
{
    if (cursor == NULL || cursor >= end) {
        return 1;
    }

    return isspace((unsigned char)*cursor) || *cursor == ',' || *cursor == '}' || *cursor == ']';
}

static ca_status_t ca_json_copy_out(char *out, size_t out_size, const char *src)
{
    int written;

    if (out == NULL || out_size == 0 || src == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    written = snprintf(out, out_size, "%s", src);
    if (written < 0 || (size_t)written >= out_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static ca_status_t ca_json_decode_string_range(const char *quote,
                                               const char *end,
                                               char *out,
                                               size_t out_size,
                                               const char **out_after)
{
    const char *cursor;
    size_t used = 0;

    if (quote == NULL || quote >= end || *quote != '"' || out == NULL || out_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    cursor = quote + 1;
    out[0] = '\0';
    while (cursor < end) {
        unsigned char ch = (unsigned char)*cursor;

        if (ch == '"') {
            out[used] = '\0';
            if (out_after != NULL) {
                *out_after = cursor + 1;
            }
            return CA_OK;
        }

        if (ch == '\\') {
            cursor++;
            if (cursor >= end) {
                return CA_ERR_JSON;
            }
            ch = (unsigned char)*cursor;
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            default:
                return CA_ERR_JSON;
            }
        }

        if (used + 1 >= out_size) {
            return CA_ERR_INVALID_ARG;
        }
        out[used++] = (char)ch;
        cursor++;
    }

    return CA_ERR_JSON;
}

static ca_status_t ca_json_find_key_value_range(const char *json,
                                                size_t json_len,
                                                const char *key,
                                                const char **out_value,
                                                const char **out_end)
{
    const char *cursor;
    const char *end;
    char parsed_key[128];
    size_t key_len;

    if (json == NULL || key == NULL || out_value == NULL || out_end == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    key_len = strlen(key);
    if (key_len == 0 || key_len >= sizeof(parsed_key)) {
        return CA_ERR_INVALID_ARG;
    }

    cursor = json;
    end = json + json_len;
    while (cursor < end) {
        const char *after_string = NULL;
        const char *after_ws;
        ca_status_t status;

        if (*cursor != '"') {
            cursor++;
            continue;
        }

        status = ca_json_decode_string_range(cursor, end, parsed_key, sizeof(parsed_key), &after_string);
        if (status != CA_OK) {
            return status;
        }

        after_ws = ca_json_skip_ws_range(after_string, end);
        if (after_ws < end && *after_ws == ':' && strcmp(parsed_key, key) == 0) {
            *out_value = ca_json_skip_ws_range(after_ws + 1, end);
            *out_end = end;
            return CA_OK;
        }

        cursor = after_string;
    }

    return CA_ERR_NOT_FOUND;
}

ca_status_t ca_json_get_string_range(const char *json,
                                     size_t json_len,
                                     const char *key,
                                     char *out,
                                     size_t out_size)
{
    const char *value;
    const char *end;
    const char *after = NULL;
    ca_status_t status;

    if (out == NULL || out_size == 0) {
        return CA_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    status = ca_json_find_key_value_range(json, json_len, key, &value, &end);
    if (status != CA_OK) {
        return status;
    }
    if (value >= end || *value != '"') {
        return CA_ERR_TYPE_MISMATCH;
    }

    status = ca_json_decode_string_range(value, end, out, out_size, &after);
    if (status != CA_OK) {
        out[0] = '\0';
        return status;
    }
    after = ca_json_skip_ws_range(after, end);
    if (!ca_json_is_value_delimiter(after, end)) {
        out[0] = '\0';
        return CA_ERR_JSON;
    }

    return CA_OK;
}

ca_status_t ca_json_get_int_range(const char *json, size_t json_len, const char *key, int *out)
{
    const char *value;
    const char *end;
    char *parse_end;
    long parsed;
    ca_status_t status;

    if (out == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_json_find_key_value_range(json, json_len, key, &value, &end);
    if (status != CA_OK) {
        return status;
    }
    if (value >= end || (!isdigit((unsigned char)*value) && *value != '-')) {
        return CA_ERR_TYPE_MISMATCH;
    }

    errno = 0;
    parsed = strtol(value, &parse_end, 10);
    if (value == parse_end || errno != 0 || parsed < INT_MIN || parsed > INT_MAX) {
        return CA_ERR_JSON;
    }
    parse_end = (char *)ca_json_skip_ws_range(parse_end, end);
    if (!ca_json_is_value_delimiter(parse_end, end)) {
        return CA_ERR_JSON;
    }

    *out = (int)parsed;
    return CA_OK;
}

ca_status_t ca_json_get_double_range(const char *json, size_t json_len, const char *key, double *out)
{
    const char *value;
    const char *end;
    char *parse_end;
    double parsed;
    ca_status_t status;

    if (out == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_json_find_key_value_range(json, json_len, key, &value, &end);
    if (status != CA_OK) {
        return status;
    }
    if (value >= end || (!isdigit((unsigned char)*value) && *value != '-')) {
        return CA_ERR_TYPE_MISMATCH;
    }

    errno = 0;
    parsed = strtod(value, &parse_end);
    if (value == parse_end || errno != 0) {
        return CA_ERR_JSON;
    }
    parse_end = (char *)ca_json_skip_ws_range(parse_end, end);
    if (!ca_json_is_value_delimiter(parse_end, end)) {
        return CA_ERR_JSON;
    }

    *out = parsed;
    return CA_OK;
}

ca_status_t ca_json_get_bool_range(const char *json, size_t json_len, const char *key, int *out)
{
    const char *value;
    const char *end;
    const char *after_bool;
    ca_status_t status;

    if (out == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_json_find_key_value_range(json, json_len, key, &value, &end);
    if (status != CA_OK) {
        return status;
    }
    if ((size_t)(end - value) >= 4 && memcmp(value, "true", 4) == 0) {
        after_bool = ca_json_skip_ws_range(value + 4, end);
        if (!ca_json_is_value_delimiter(after_bool, end)) {
            return CA_ERR_JSON;
        }
        *out = 1;
        return CA_OK;
    }
    if ((size_t)(end - value) >= 5 && memcmp(value, "false", 5) == 0) {
        after_bool = ca_json_skip_ws_range(value + 5, end);
        if (!ca_json_is_value_delimiter(after_bool, end)) {
            return CA_ERR_JSON;
        }
        *out = 0;
        return CA_OK;
    }

    return CA_ERR_TYPE_MISMATCH;
}

ca_status_t ca_json_find_object_range(const char *json,
                                      size_t json_len,
                                      const char *key,
                                      const char **out_start,
                                      size_t *out_len)
{
    const char *value;
    const char *cursor;
    const char *end;
    int depth = 0;
    int in_string = 0;
    int escaped = 0;
    ca_status_t status;

    if (json == NULL || key == NULL || out_start == NULL || out_len == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *out_start = NULL;
    *out_len = 0;

    status = ca_json_find_key_value_range(json, json_len, key, &value, &end);
    if (status != CA_OK) {
        return status;
    }
    if (value >= end || *value != '{') {
        return CA_ERR_TYPE_MISMATCH;
    }

    cursor = value;
    while (cursor < end) {
        char ch = *cursor;

        if (in_string) {
            if (escaped) {
                escaped = 0;
            } else if (ch == '\\') {
                escaped = 1;
            } else if (ch == '"') {
                in_string = 0;
            }
        } else if (ch == '"') {
            in_string = 1;
        } else if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            depth--;
            if (depth == 0) {
                *out_start = value;
                *out_len = (size_t)(cursor - value + 1);
                return CA_OK;
            }
        }

        cursor++;
    }

    return CA_ERR_JSON;
}

ca_status_t ca_json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    if (json == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    return ca_json_get_string_range(json, strlen(json), key, out, out_size);
}

ca_status_t ca_json_get_int(const char *json, const char *key, int *out)
{
    if (json == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    return ca_json_get_int_range(json, strlen(json), key, out);
}

ca_status_t ca_json_get_bool(const char *json, const char *key, int *out)
{
    if (json == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    return ca_json_get_bool_range(json, strlen(json), key, out);
}

ca_status_t ca_json_escape_string(const char *input, char *out, size_t out_size)
{
    const unsigned char *cursor;
    size_t used = 0;

    if (input == NULL || out == NULL || out_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    out[0] = '\0';
    for (cursor = (const unsigned char *)input; *cursor != '\0'; cursor++) {
        int written = 0;

        switch (*cursor) {
        case '"':
            written = snprintf(out + used, out_size - used, "\\\"");
            break;
        case '\\':
            written = snprintf(out + used, out_size - used, "\\\\");
            break;
        case '\n':
            written = snprintf(out + used, out_size - used, "\\n");
            break;
        case '\r':
            written = snprintf(out + used, out_size - used, "\\r");
            break;
        case '\t':
            written = snprintf(out + used, out_size - used, "\\t");
            break;
        case '\b':
            written = snprintf(out + used, out_size - used, "\\b");
            break;
        case '\f':
            written = snprintf(out + used, out_size - used, "\\f");
            break;
        default:
            if (*cursor < 0x20) {
                written = snprintf(out + used, out_size - used, "\\u%04x", (unsigned int)*cursor);
            } else {
                if (used + 1 >= out_size) {
                    return CA_ERR_INVALID_ARG;
                }
                out[used++] = (char)*cursor;
                out[used] = '\0';
                continue;
            }
            break;
        }

        if (written < 0 || (size_t)written >= out_size - used) {
            return CA_ERR_INVALID_ARG;
        }
        used += (size_t)written;
    }

    return CA_OK;
}

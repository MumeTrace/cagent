/*
 * ca_json.h
 * Shared JSON helpers for small object-field extraction and JSON string escaping.
 * This is not a full DOM parser; it centralizes the MVP behavior so tool and
 * config modules do not each grow their own incompatible parser.
 */
#ifndef CA_JSON_H
#define CA_JSON_H

#include <stddef.h>

#include "ca_status.h"

ca_status_t ca_json_get_string(const char *json, const char *key, char *out, size_t out_size);
ca_status_t ca_json_get_int(const char *json, const char *key, int *out);
ca_status_t ca_json_get_bool(const char *json, const char *key, int *out);

ca_status_t ca_json_get_string_range(const char *json,
                                     size_t json_len,
                                     const char *key,
                                     char *out,
                                     size_t out_size);
ca_status_t ca_json_get_int_range(const char *json, size_t json_len, const char *key, int *out);
ca_status_t ca_json_get_bool_range(const char *json, size_t json_len, const char *key, int *out);
ca_status_t ca_json_get_double_range(const char *json, size_t json_len, const char *key, double *out);
ca_status_t ca_json_find_object_range(const char *json,
                                      size_t json_len,
                                      const char *key,
                                      const char **out_start,
                                      size_t *out_len);

/* Escapes string content for JSON. The output does not include surrounding quotes. */
ca_status_t ca_json_escape_string(const char *input, char *out, size_t out_size);

#endif

/*
 * ca_payload.h
 * Bounded string payload builder. It keeps small payloads inline and uses heap
 * only when CAGENT_ENABLE_LARGE_PAYLOAD is enabled and the configured total
 * limit allows it.
 */
#ifndef CA_PAYLOAD_H
#define CA_PAYLOAD_H

#include <stddef.h>

#include "ca_limits.h"
#include "ca_status.h"

typedef struct ca_payload {
    char *data;
    size_t len;
    size_t cap;
    size_t max_total;
    int heap_allocated;
    int truncated;
    char inline_buf[CA_MAX_PAYLOAD_INLINE + 1u];
} ca_payload_t;

ca_status_t ca_payload_init(ca_payload_t *payload, size_t inline_cap, size_t max_total);
void ca_payload_free(ca_payload_t *payload);
ca_status_t ca_payload_set(ca_payload_t *payload, const char *data, size_t len);
ca_status_t ca_payload_append(ca_payload_t *payload, const char *data, size_t len);
ca_status_t ca_payload_append_cstr(ca_payload_t *payload, const char *text);
ca_status_t ca_payload_appendf(ca_payload_t *payload, const char *format, ...);
ca_status_t ca_payload_append_json_escaped(ca_payload_t *payload, const char *text, size_t len);
const char *ca_payload_data(const ca_payload_t *payload);
size_t ca_payload_len(const ca_payload_t *payload);
int ca_payload_truncated(const ca_payload_t *payload);

#endif

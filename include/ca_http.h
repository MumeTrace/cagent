/* ca_http.h
 * Small HTTP boundary for LLM providers. Keeping libcurl behind this module
 * avoids leaking transport details into Agent Loop or provider selection code.
 */
#ifndef CA_HTTP_H
#define CA_HTTP_H

#include <stddef.h>

#include "ca_status.h"

#define CA_HTTP_ERROR_CAP 512

typedef struct ca_http_response {
    long status_code;
    char *body;
    size_t body_len;
    char error_message[CA_HTTP_ERROR_CAP];
} ca_http_response_t;

void ca_http_response_init(ca_http_response_t *response);
void ca_http_response_free(ca_http_response_t *response);

ca_status_t ca_http_post_json(const char *url,
                              const char *bearer_token,
                              const char *json_body,
                              long timeout_seconds,
                              ca_http_response_t *response);

#endif

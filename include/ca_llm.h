/*
 * ca_llm.h
 * Provider abstraction for Agent Loop. Phase 7 wires a fake provider through
 * the same interface that a real OpenAI-compatible provider can implement later.
 */
#ifndef CA_LLM_H
#define CA_LLM_H

#include "ca_status.h"

#define CA_LLM_RESPONSE_CAP 8192

#include "ca_config.h"

typedef struct ca_llm_request {
    const char *prompt;
    int step_index;
} ca_llm_request_t;

typedef struct ca_llm_response {
    char raw_text[CA_LLM_RESPONSE_CAP];
} ca_llm_response_t;

typedef struct ca_llm_provider ca_llm_provider_t;

struct ca_llm_provider {
    ca_status_t (*complete)(ca_llm_provider_t *provider,
                            const ca_llm_request_t *request,
                            ca_llm_response_t *response);
    void (*destroy)(ca_llm_provider_t *provider);
    void *impl;
};

ca_status_t ca_llm_fake_provider_init(ca_llm_provider_t *provider);
void ca_llm_fake_provider_free(ca_llm_provider_t *provider);
ca_status_t ca_llm_openai_compat_provider_init(ca_llm_provider_t *provider, const ca_config_t *config);
void ca_llm_openai_compat_provider_free(ca_llm_provider_t *provider);
void ca_llm_provider_free(ca_llm_provider_t *provider);

#endif

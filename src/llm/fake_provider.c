#include "ca_llm.h"

#include <stdio.h>
#include <string.h>

static int ca_fake_contains(const char *text, const char *needle)
{
    return text != NULL && needle != NULL && strstr(text, needle) != NULL;
}

static const char *ca_fake_user_request(const char *prompt)
{
    const char *marker;

    if (prompt == NULL) {
        return NULL;
    }

    marker = strstr(prompt, "User request:\n");
    if (marker == NULL) {
        return prompt;
    }

    return marker + strlen("User request:\n");
}

static ca_status_t ca_fake_write(ca_llm_response_t *response, const char *text)
{
    int written;

    if (response == NULL || text == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    written = snprintf(response->raw_text, sizeof(response->raw_text), "%s", text);
    if (written < 0 || (size_t)written >= sizeof(response->raw_text)) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static ca_status_t ca_fake_complete(ca_llm_provider_t *provider,
                                    const ca_llm_request_t *request,
                                    ca_llm_response_t *response)
{
    (void)provider;

    if (request == NULL || response == NULL || request->prompt == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    /*
     * Fake provider is deterministic on purpose. It validates Agent Loop wiring
     * without pretending to be a real reasoning model or making HTTP requests.
     */
    if (request->step_index == 0) {
        const char *user_request = ca_fake_user_request(request->prompt);

        if (ca_fake_contains(user_request, "README") || ca_fake_contains(user_request, "readme")) {
            return ca_fake_write(response,
                                 "{\"type\":\"tool_call\",\"tool\":\"read_file\","
                                 "\"arguments\":{\"path\":\"README.md\"},"
                                 "\"reason\":\"Fake provider reads README.md for the requested summary.\"}");
        }
        if (ca_fake_contains(user_request, "project") ||
            ca_fake_contains(user_request, "项目")) {
            return ca_fake_write(response,
                                 "{\"type\":\"tool_call\",\"tool\":\"get_project_summary\","
                                 "\"arguments\":{},"
                                 "\"reason\":\"Fake provider requests the indexed project summary.\"}");
        }
        return ca_fake_write(response,
                             "{\"type\":\"final_answer\","
                             "\"content\":\"Fake provider did not need a tool for this input. Try mentioning README or project.\"}");
    }

    if (ca_fake_contains(request->prompt, "\"type\":\"tool_result\",\"tool\":\"read_file\"")) {
        return ca_fake_write(response,
                             "{\"type\":\"final_answer\","
                             "\"content\":\"Fake final_answer: README.md was read successfully and the tool result is now in context.\"}");
    }
    if (ca_fake_contains(request->prompt, "\"type\":\"tool_result\",\"tool\":\"get_project_summary\"")) {
        return ca_fake_write(response,
                             "{\"type\":\"final_answer\","
                             "\"content\":\"Fake final_answer: project summary was collected successfully.\"}");
    }

    return ca_fake_write(response,
                         "{\"type\":\"final_answer\","
                         "\"content\":\"Fake final_answer: stopping after observing the previous step.\"}");
}

ca_status_t ca_llm_fake_provider_init(ca_llm_provider_t *provider)
{
    if (provider == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    provider->complete = ca_fake_complete;
    provider->destroy = ca_llm_fake_provider_free;
    provider->impl = NULL;
    return CA_OK;
}

void ca_llm_fake_provider_free(ca_llm_provider_t *provider)
{
    if (provider == NULL) {
        return;
    }

    provider->complete = NULL;
    provider->destroy = NULL;
    provider->impl = NULL;
}

void ca_llm_provider_free(ca_llm_provider_t *provider)
{
    if (provider == NULL) {
        return;
    }

    if (provider->destroy != NULL) {
        provider->destroy(provider);
        return;
    }

    provider->complete = NULL;
    provider->impl = NULL;
}

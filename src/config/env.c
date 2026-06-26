#include "ca_config.h"

#include <stdio.h>
#include <stdlib.h>

static ca_status_t ca_env_copy(char *dest, size_t dest_size, const char *value)
{
    int written;

    if (dest == NULL || dest_size == 0 || value == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    written = snprintf(dest, dest_size, "%s", value);
    if (written < 0 || (size_t)written >= dest_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static int ca_env_has_value(const char *value)
{
    return value != NULL && value[0] != '\0';
}

ca_status_t ca_config_apply_env(ca_config_t *config)
{
    const char *provider;
    const char *provider_type;
    const char *compat_profile;
    const char *base_url;
    const char *model;

    if (config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    provider = getenv("CAGENT_PROVIDER");
    if (ca_env_has_value(provider)) {
        ca_status_t status = ca_env_copy(config->default_provider, sizeof(config->default_provider), provider);
        if (status != CA_OK) {
            return status;
        }
        config->provider_from_env = 1;
    }

    /*
     * Provider name and provider type are intentionally separate: the name
     * selects a user-defined config block, while the type selects the future
     * LLM implementation family.
     */
    provider_type = getenv("CAGENT_PROVIDER_TYPE");
    if (ca_env_has_value(provider_type)) {
        config->provider_type = ca_provider_type_from_string(provider_type);
        config->provider_type_from_env = 1;
    }

    /*
     * Compat profile is an OpenAI-compatible detail, not a new provider type.
     * It lets later HTTP code tune small source-specific behavior while still
     * sharing the openai_compat request protocol.
     */
    compat_profile = getenv("CAGENT_COMPAT_PROFILE");
    if (ca_env_has_value(compat_profile)) {
        config->compat_profile = ca_compat_profile_from_string(compat_profile);
        config->compat_profile_from_env = 1;
    }

    base_url = getenv("CAGENT_BASE_URL");
    if (ca_env_has_value(base_url)) {
        ca_status_t status = ca_env_copy(config->base_url, sizeof(config->base_url), base_url);
        if (status != CA_OK) {
            return status;
        }
        config->base_url_from_env = 1;
    }

    model = getenv("CAGENT_MODEL");
    if (ca_env_has_value(model)) {
        ca_status_t status = ca_env_copy(config->model, sizeof(config->model), model);
        if (status != CA_OK) {
            return status;
        }
        config->model_from_env = 1;
    }

    return CA_OK;
}

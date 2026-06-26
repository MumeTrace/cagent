#ifndef CA_CONFIG_H
#define CA_CONFIG_H

#include <stdio.h>

#include "ca_status.h"

#define CA_CONFIG_PROVIDER_CAP 64
#define CA_CONFIG_URL_CAP 512
#define CA_CONFIG_ENV_CAP 128
#define CA_CONFIG_MODEL_CAP 128
#define CA_CONFIG_POLICY_CAP 32
#define CA_CONFIG_PATH_CAP 4096

typedef enum ca_provider_type {
    CA_PROVIDER_OPENAI_COMPAT = 0,
    CA_PROVIDER_GEMINI,
    CA_PROVIDER_CLAUDE,
    CA_PROVIDER_UNKNOWN
} ca_provider_type_t;

typedef enum ca_compat_profile {
    CA_COMPAT_GENERIC = 0,
    CA_COMPAT_NEWAPI,
    CA_COMPAT_DEEPSEEK,
    CA_COMPAT_UNKNOWN
} ca_compat_profile_t;

typedef struct ca_config {
    /* Name selects a configured provider block; type selects the implementation family. */
    char default_provider[CA_CONFIG_PROVIDER_CAP];
    ca_provider_type_t provider_type;
    ca_compat_profile_t compat_profile;
    char base_url[CA_CONFIG_URL_CAP];
    char api_key_env[CA_CONFIG_ENV_CAP];
    char model[CA_CONFIG_MODEL_CAP];
    int agent_max_steps;
    double agent_temperature;
    int agent_stream;
    char permission_default_write[CA_CONFIG_POLICY_CAP];
    char permission_default_shell[CA_CONFIG_POLICY_CAP];
    char config_path[CA_CONFIG_PATH_CAP];
    int loaded_from_file;
    int missing_config_file;
    int provider_from_env;
    int provider_type_from_env;
    int compat_profile_from_env;
    int base_url_from_env;
    int model_from_env;
} ca_config_t;

ca_provider_type_t ca_provider_type_from_string(const char *value);
const char *ca_provider_type_to_string(ca_provider_type_t type);
ca_compat_profile_t ca_compat_profile_from_string(const char *value);
const char *ca_compat_profile_to_string(ca_compat_profile_t profile);
ca_status_t ca_config_init_defaults(ca_config_t *config);
ca_status_t ca_config_default_path(char *buffer, size_t buffer_size);
ca_status_t ca_config_load(ca_config_t *config, const char *config_path);
ca_status_t ca_config_apply_env(ca_config_t *config);
ca_status_t ca_config_print_summary(const ca_config_t *config, FILE *stream);

#endif

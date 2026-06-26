#include "ca_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define CA_CONFIG_PATH_SEP "\\"
#else
#define CA_CONFIG_PATH_SEP "/"
#endif

#define CA_CONFIG_FILE_LIMIT (256u * 1024u)

ca_provider_type_t ca_provider_type_from_string(const char *value)
{
    if (value == NULL) {
        return CA_PROVIDER_UNKNOWN;
    }

    if (strcmp(value, "openai_compat") == 0) {
        return CA_PROVIDER_OPENAI_COMPAT;
    }
    if (strcmp(value, "gemini") == 0) {
        return CA_PROVIDER_GEMINI;
    }
    if (strcmp(value, "claude") == 0) {
        return CA_PROVIDER_CLAUDE;
    }

    return CA_PROVIDER_UNKNOWN;
}

const char *ca_provider_type_to_string(ca_provider_type_t type)
{
    switch (type) {
    case CA_PROVIDER_OPENAI_COMPAT:
        return "openai_compat";
    case CA_PROVIDER_GEMINI:
        return "gemini";
    case CA_PROVIDER_CLAUDE:
        return "claude";
    case CA_PROVIDER_UNKNOWN:
    default:
        return "unknown";
    }
}

ca_compat_profile_t ca_compat_profile_from_string(const char *value)
{
    if (value == NULL) {
        return CA_COMPAT_UNKNOWN;
    }

    if (strcmp(value, "generic") == 0) {
        return CA_COMPAT_GENERIC;
    }
    if (strcmp(value, "newapi") == 0) {
        return CA_COMPAT_NEWAPI;
    }
    if (strcmp(value, "deepseek") == 0) {
        return CA_COMPAT_DEEPSEEK;
    }

    return CA_COMPAT_UNKNOWN;
}

const char *ca_compat_profile_to_string(ca_compat_profile_t profile)
{
    switch (profile) {
    case CA_COMPAT_GENERIC:
        return "generic";
    case CA_COMPAT_NEWAPI:
        return "newapi";
    case CA_COMPAT_DEEPSEEK:
        return "deepseek";
    case CA_COMPAT_UNKNOWN:
    default:
        return "unknown";
    }
}

static ca_status_t ca_config_copy_string(char *dest, size_t dest_size, const char *src)
{
    int written;

    if (dest == NULL || dest_size == 0 || src == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    written = snprintf(dest, dest_size, "%s", src);
    if (written < 0 || (size_t)written >= dest_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static ca_status_t ca_config_apply_provider_name_env(ca_config_t *config)
{
    const char *provider;

    if (config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    provider = getenv("CAGENT_PROVIDER");
    if (provider == NULL || provider[0] == '\0') {
        return CA_OK;
    }

    config->provider_from_env = 1;
    return ca_config_copy_string(config->default_provider, sizeof(config->default_provider), provider);
}

static const char *ca_json_skip_ws(const char *text)
{
    while (text != NULL && *text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static const char *ca_json_find_key_in_range(const char *start, size_t len, const char *key)
{
    size_t key_len;
    const char *cursor;
    const char *end;

    if (start == NULL || key == NULL) {
        return NULL;
    }

    key_len = strlen(key);
    cursor = start;
    end = start + len;

    while (cursor < end) {
        const char *found = memchr(cursor, '"', (size_t)(end - cursor));
        const char *after_key;

        if (found == NULL || found + key_len + 2 > end) {
            return NULL;
        }

        if (memcmp(found + 1, key, key_len) == 0 && found[1 + key_len] == '"') {
            after_key = ca_json_skip_ws(found + key_len + 2);
            if (after_key < end && *after_key == ':') {
                return after_key + 1;
            }
        }

        cursor = found + 1;
    }

    return NULL;
}

static ca_status_t ca_json_find_object(const char *start,
                                       size_t len,
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

    if (start == NULL || key == NULL || out_start == NULL || out_len == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *out_start = NULL;
    *out_len = 0;

    value = ca_json_find_key_in_range(start, len, key);
    if (value == NULL) {
        return CA_ERR_JSON;
    }

    value = ca_json_skip_ws(value);
    end = start + len;
    if (value >= end || *value != '{') {
        return CA_ERR_JSON;
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
        } else {
            if (ch == '"') {
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
        }

        cursor++;
    }

    return CA_ERR_JSON;
}

static ca_status_t ca_json_get_string(const char *start,
                                      size_t len,
                                      const char *key,
                                      char *dest,
                                      size_t dest_size)
{
    const char *value;
    const char *end;
    char temp[CA_CONFIG_URL_CAP];
    size_t used = 0;
    int escaped = 0;

    if (start == NULL || key == NULL || dest == NULL || dest_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    value = ca_json_find_key_in_range(start, len, key);
    if (value == NULL) {
        return CA_ERR_JSON;
    }

    value = ca_json_skip_ws(value);
    end = start + len;
    if (value >= end || *value != '"') {
        return CA_ERR_JSON;
    }

    value++;
    while (value < end && *value != '\0') {
        char ch = *value;

        if (escaped) {
            escaped = 0;
        } else if (ch == '\\') {
            escaped = 1;
            value++;
            continue;
        } else if (ch == '"') {
            temp[used] = '\0';
            return ca_config_copy_string(dest, dest_size, temp);
        }

        if (used + 1 >= sizeof(temp)) {
            return CA_ERR_INVALID_ARG;
        }
        temp[used++] = ch;
        value++;
    }

    return CA_ERR_JSON;
}

static ca_status_t ca_json_get_int(const char *start, size_t len, const char *key, int *out_value)
{
    const char *value;
    const char *number;
    char *parse_end;
    long parsed;

    if (start == NULL || key == NULL || out_value == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    value = ca_json_find_key_in_range(start, len, key);
    if (value == NULL) {
        return CA_ERR_JSON;
    }

    number = ca_json_skip_ws(value);
    errno = 0;
    parsed = strtol(number, &parse_end, 10);
    if (errno != 0 || parse_end == number) {
        return CA_ERR_JSON;
    }

    *out_value = (int)parsed;
    return CA_OK;
}

static ca_status_t ca_json_get_double(const char *start,
                                      size_t len,
                                      const char *key,
                                      double *out_value)
{
    const char *value;
    const char *number;
    char *parse_end;
    double parsed;

    if (start == NULL || key == NULL || out_value == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    value = ca_json_find_key_in_range(start, len, key);
    if (value == NULL) {
        return CA_ERR_JSON;
    }

    number = ca_json_skip_ws(value);
    errno = 0;
    parsed = strtod(number, &parse_end);
    if (errno != 0 || parse_end == number) {
        return CA_ERR_JSON;
    }

    *out_value = parsed;
    return CA_OK;
}

static ca_status_t ca_json_get_bool(const char *start, size_t len, const char *key, int *out_value)
{
    const char *value;
    const char *end;

    if (start == NULL || key == NULL || out_value == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    value = ca_json_find_key_in_range(start, len, key);
    if (value == NULL) {
        return CA_ERR_JSON;
    }

    value = ca_json_skip_ws(value);
    end = start + len;

    if ((size_t)(end - value) >= 4 && memcmp(value, "true", 4) == 0) {
        *out_value = 1;
        return CA_OK;
    }
    if ((size_t)(end - value) >= 5 && memcmp(value, "false", 5) == 0) {
        *out_value = 0;
        return CA_OK;
    }

    return CA_ERR_JSON;
}

static ca_status_t ca_config_read_file(const char *path, char **out_text, size_t *out_len, int *out_missing)
{
    FILE *file;
    long file_size;
    char *text;
    size_t read_size;

    if (path == NULL || out_text == NULL || out_len == NULL || out_missing == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *out_text = NULL;
    *out_len = 0;
    *out_missing = 0;

    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT || errno == ENOTDIR) {
            *out_missing = 1;
        }
        return CA_ERR_IO;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return CA_ERR_IO;
    }

    file_size = ftell(file);
    if (file_size < 0 || (unsigned long)file_size > CA_CONFIG_FILE_LIMIT) {
        fclose(file);
        return CA_ERR_IO;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return CA_ERR_IO;
    }

    text = (char *)malloc((size_t)file_size + 1);
    if (text == NULL) {
        fclose(file);
        return CA_ERR_NO_MEMORY;
    }

    read_size = fread(text, 1, (size_t)file_size, file);
    if (read_size != (size_t)file_size) {
        free(text);
        fclose(file);
        return CA_ERR_IO;
    }

    text[read_size] = '\0';
    fclose(file);

    *out_text = text;
    *out_len = read_size;
    return CA_OK;
}

static ca_status_t ca_config_parse_mvp_json(ca_config_t *config, const char *json, size_t json_len)
{
    const char *providers_obj = NULL;
    const char *provider_obj = NULL;
    const char *agent_obj = NULL;
    const char *permission_obj = NULL;
    size_t providers_len = 0;
    size_t provider_len = 0;
    size_t agent_len = 0;
    size_t permission_len = 0;
    char provider_type_text[CA_CONFIG_PROVIDER_CAP];
    char compat_profile_text[CA_CONFIG_PROVIDER_CAP];
    ca_status_t type_status;
    ca_status_t profile_status;

    if (config == NULL || json == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    /*
     * this only extracts the known config fields used by
     * the current milestone. Keep it private to config.c so a real JSON parser
     * can replace it later without changing CLI or provider code.
     */
    (void)ca_json_get_string(json,
                             json_len,
                             "default_provider",
                             config->default_provider,
                             sizeof(config->default_provider));

    /*
     * CAGENT_PROVIDER names a provider block, so apply it before choosing
     * which object to read. The rest of env overrides still run after file load.
     */
    if (ca_config_apply_provider_name_env(config) != CA_OK) {
        return CA_ERR_INVALID_ARG;
    }

    if (ca_json_find_object(json, json_len, "providers", &providers_obj, &providers_len) == CA_OK) {
        (void)ca_json_find_object(providers_obj, providers_len, config->default_provider, &provider_obj, &provider_len);
    }

    if (provider_obj == NULL) {
        (void)ca_json_find_object(json, json_len, config->default_provider, &provider_obj, &provider_len);
    }

    if (provider_obj != NULL) {
        /*
         * Old Phase 2 configs did not have a "type" field. Treat missing type
         * as openai_compat so existing user configs remain valid.
         */
        type_status = ca_json_get_string(provider_obj,
                                         provider_len,
                                         "type",
                                         provider_type_text,
                                         sizeof(provider_type_text));
        if (type_status == CA_OK) {
            config->provider_type = ca_provider_type_from_string(provider_type_text);
        }

        /*
         * compat_profile is narrower than provider type: it only captures
         * OpenAI-compatible source differences such as NewAPI or DeepSeek.
         * Missing values stay generic so older configs continue to work.
         */
        profile_status = ca_json_get_string(provider_obj,
                                            provider_len,
                                            "compat_profile",
                                            compat_profile_text,
                                            sizeof(compat_profile_text));
        if (profile_status == CA_OK) {
            config->compat_profile = ca_compat_profile_from_string(compat_profile_text);
        }

        (void)ca_json_get_string(provider_obj, provider_len, "base_url", config->base_url, sizeof(config->base_url));
        (void)ca_json_get_string(provider_obj,
                                 provider_len,
                                 "api_key_env",
                                 config->api_key_env,
                                 sizeof(config->api_key_env));
        (void)ca_json_get_string(provider_obj, provider_len, "model", config->model, sizeof(config->model));
    }

    if (ca_json_find_object(json, json_len, "agent", &agent_obj, &agent_len) == CA_OK) {
        (void)ca_json_get_int(agent_obj, agent_len, "max_steps", &config->agent_max_steps);
        (void)ca_json_get_double(agent_obj, agent_len, "temperature", &config->agent_temperature);
        (void)ca_json_get_bool(agent_obj, agent_len, "stream", &config->agent_stream);
    }

    if (ca_json_find_object(json, json_len, "permission", &permission_obj, &permission_len) == CA_OK) {
        (void)ca_json_get_string(permission_obj,
                                 permission_len,
                                 "default_write",
                                 config->permission_default_write,
                                 sizeof(config->permission_default_write));
        (void)ca_json_get_string(permission_obj,
                                 permission_len,
                                 "default_shell",
                                 config->permission_default_shell,
                                 sizeof(config->permission_default_shell));
    }

    return CA_OK;
}

ca_status_t ca_config_init_defaults(ca_config_t *config)
{
    if (config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));

    if (ca_config_copy_string(config->default_provider, sizeof(config->default_provider), "openai_compat") != CA_OK ||
        ca_config_copy_string(config->base_url, sizeof(config->base_url), "https://api.example.com/v1") != CA_OK ||
        ca_config_copy_string(config->api_key_env, sizeof(config->api_key_env), "CAGENT_API_KEY") != CA_OK ||
        ca_config_copy_string(config->model, sizeof(config->model), "gpt-5.5") != CA_OK ||
        ca_config_copy_string(config->permission_default_write,
                              sizeof(config->permission_default_write),
                              "ask") != CA_OK ||
        ca_config_copy_string(config->permission_default_shell,
                              sizeof(config->permission_default_shell),
                              "ask") != CA_OK) {
        return CA_ERR_INVALID_ARG;
    }

    config->provider_type = CA_PROVIDER_OPENAI_COMPAT;
    config->compat_profile = CA_COMPAT_GENERIC;
    config->agent_max_steps = 12;
    config->agent_temperature = 0.5;
    config->agent_stream = 1;

    return CA_OK;
}

ca_status_t ca_config_default_path(char *buffer, size_t buffer_size)
{
    const char *home;
    int written;

    if (buffer == NULL || buffer_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

#ifdef _WIN32
    home = getenv("USERPROFILE");
#else
    home = getenv("HOME");
#endif

    if (home == NULL || home[0] == '\0') {
        home = ".";
    }

    written = snprintf(buffer, buffer_size, "%s%s.cagent%sconfig.json", home, CA_CONFIG_PATH_SEP, CA_CONFIG_PATH_SEP);
    if (written < 0 || (size_t)written >= buffer_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

ca_status_t ca_config_load(ca_config_t *config, const char *config_path)
{
    char default_path[CA_CONFIG_PATH_CAP];
    const char *path_to_load = config_path;
    char *json = NULL;
    size_t json_len = 0;
    int file_missing = 0;
    ca_status_t status;

    if (config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_config_init_defaults(config);
    if (status != CA_OK) {
        return status;
    }

    if (path_to_load == NULL || path_to_load[0] == '\0') {
        status = ca_config_default_path(default_path, sizeof(default_path));
        if (status != CA_OK) {
            return status;
        }
        path_to_load = default_path;
    }

    status = ca_config_copy_string(config->config_path, sizeof(config->config_path), path_to_load);
    if (status != CA_OK) {
        return status;
    }

    status = ca_config_read_file(path_to_load, &json, &json_len, &file_missing);
    if (status == CA_ERR_IO && file_missing) {
        config->missing_config_file = 1;
        return ca_config_apply_env(config);
    }
    if (status != CA_OK) {
        return status;
    }

    status = ca_config_parse_mvp_json(config, json, json_len);
    free(json);
    if (status != CA_OK) {
        return status;
    }

    config->loaded_from_file = 1;
    return ca_config_apply_env(config);
}

ca_status_t ca_config_print_summary(const ca_config_t *config, FILE *stream)
{
    if (config == NULL || stream == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    fprintf(stream, "Config path: %s\n", config->config_path[0] != '\0' ? config->config_path : "<none>");
    fprintf(stream, "Loaded from file: %s\n", config->loaded_from_file ? "yes" : "no");
    fprintf(stream, "Provider: %s%s\n", config->default_provider, config->provider_from_env ? " (env)" : "");
    fprintf(stream,
            "Provider type: %s%s\n",
            ca_provider_type_to_string(config->provider_type),
            config->provider_type_from_env ? " (env)" : "");
    fprintf(stream,
            "Compat profile: %s%s\n",
            ca_compat_profile_to_string(config->compat_profile),
            config->compat_profile_from_env ? " (env)" : "");
    fprintf(stream, "Base URL: %s%s\n", config->base_url, config->base_url_from_env ? " (env)" : "");
    fprintf(stream, "API key env: %s\n", config->api_key_env);
    fprintf(stream, "Model: %s%s\n", config->model, config->model_from_env ? " (env)" : "");
    fprintf(stream, "Agent max steps: %d\n", config->agent_max_steps);
    fprintf(stream, "Agent temperature: %.3g\n", config->agent_temperature);
    fprintf(stream, "Agent stream: %s\n", config->agent_stream ? "true" : "false");
    fprintf(stream, "Permission write: %s\n", config->permission_default_write);
    fprintf(stream, "Permission shell: %s\n", config->permission_default_shell);

    return CA_OK;
}

/*
 * config.c
 * 配置加载：默认值初始化 → JSON 文件读取 → MVP 解析 → 环境变量覆盖。
 * 当前使用手写 JSON 解析器（够用就行），未来可以换成 cJSON / jansson 等完整库。
 *
 * Config loading: defaults → read JSON file → MVP parse → env overlay.
 * Uses a hand-rolled JSON parser for now (good enough); swap in cJSON/jansson later.
 */
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

#define CA_CONFIG_FILE_LIMIT (256u * 1024u)    /* 配置文件最大 256KB / max config file size */

/* ---- 字符串 <-> 枚举互转 / string ⇄ enum converters ---- */

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

/* ---- 内部工具函数 / internal helpers ---- */

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

static int ca_config_env_has_value(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static void ca_config_warn(const char *field, const char *message)
{
    fprintf(stderr, "Warning: config %s: %s\n", field != NULL ? field : "<unknown>", message);
}

/* ---- 手写 JSON 解析器（MVP，够用就行）/ hand-rolled JSON parser (MVP) ---- */

/* 跳过空白 / skip whitespace */
static const char *ca_json_skip_ws(const char *text)
{
    while (text != NULL && *text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static const char *ca_json_skip_ws_to_end(const char *text, const char *end)
{
    while (text != NULL && text < end && isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static const char *ca_json_string_end(const char *quote, const char *end)
{
    const char *cursor;
    int escaped = 0;

    if (quote == NULL || quote >= end || *quote != '"') {
        return NULL;
    }

    for (cursor = quote + 1; cursor < end; cursor++) {
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (*cursor == '\\') {
            escaped = 1;
            continue;
        }
        if (*cursor == '"') {
            return cursor;
        }
    }

    return NULL;
}

/*
 * Find a key token in a JSON range. A string is considered a key only when
 * its closing quote is followed by optional whitespace and ':'.
 */
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
        const char *string_end;
        const char *after_string;

        if (*cursor != '"') {
            cursor++;
            continue;
        }

        string_end = ca_json_string_end(cursor, end);
        if (string_end == NULL) {
            return NULL;
        }

        after_string = ca_json_skip_ws_to_end(string_end + 1, end);
        if (after_string < end && *after_string == ':') {
            size_t token_len = (size_t)(string_end - cursor - 1);

            if (token_len == key_len && memcmp(cursor + 1, key, key_len) == 0) {
                return after_string + 1;
            }
        }

        cursor = string_end + 1;
    }

    return NULL;
}

/*
 * 找一个嵌套对象：找到 "key": { ... }，返回 { } 的范围
 * Find a nested object: locate "key": { ... }, return the { } range.
 * 正确处理字符串中的转义，防止花括号在字符串里被误算
 * Handles escape sequences so braces inside strings don't break depth counting.
 */
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

/* 读取字符串值 "..."（已定位到引号之后）/ read a JSON string value past the opening quote */
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

/* 读取整数值 / read an integer value */
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

/* 读取浮点值 / read a double value */
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

/* 读取布尔值 / read a bool value */
static ca_status_t ca_json_get_bool(const char *start, size_t len, const char *key, int *out_value)
{
    const char *value;
    const char *end;
    const char *after_bool;

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
        after_bool = value + 4;
        if (after_bool < end && !isspace((unsigned char)*after_bool) &&
            *after_bool != ',' && *after_bool != '}' && *after_bool != ']') {
            return CA_ERR_JSON;
        }
        *out_value = 1;
        return CA_OK;
    }
    if ((size_t)(end - value) >= 5 && memcmp(value, "false", 5) == 0) {
        after_bool = value + 5;
        if (after_bool < end && !isspace((unsigned char)*after_bool) &&
            *after_bool != ',' && *after_bool != '}' && *after_bool != ']') {
            return CA_ERR_JSON;
        }
        *out_value = 0;
        return CA_OK;
    }

    return CA_ERR_JSON;
}

/* ---- 文件读取 / file I/O ---- */
static ca_status_t ca_config_read_file(const char *path,
                                       char **out_text,
                                       size_t *out_len,
                                       int *out_missing,
                                       int *out_errno)
{
    FILE *file;
    long file_size;
    char *text;
    size_t read_size;

    if (path == NULL || out_text == NULL || out_len == NULL || out_missing == NULL || out_errno == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *out_text = NULL;
    *out_len = 0;
    *out_missing = 0;
    *out_errno = 0;

    file = fopen(path, "rb");
    if (file == NULL) {
        *out_errno = errno;
        /* 文件不存在不算严重错误，标记一下让调用方处理 / missing file isn't fatal */
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

/*
 * MVP JSON 解析：只提取当前 milestone 所需的字段，不解析完整 JSON。
 * 刻意做成 config.c 私有——后面换成真正的 JSON 库时不影响外部代码。
 *
 * MVP parser: only extracts the fields needed for the current milestone.
 * Kept private to config.c — swapping a real JSON lib later won't touch callers.
 *
 * 解析策略 / parse strategy:
 *  1. 先取 default_provider（顶层字段）
 *  2. 检查 CAGENT_PROVIDER 环境变量，可能重写 provider 名称
 *  3. 在 providers.<name> 下面找 provider 字段
 *  4. 如果 providers 下找不到，回退到顶层查找（扁平结构兼容）
 *  5. 解析 agent 和 permission 子对象
 */
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
    const char *provider_env;
    const char *selected_provider;
    ca_status_t type_status;
    ca_status_t profile_status;

    if (config == NULL || json == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    /* 1. 顶层 default_provider / top-level default_provider */
    if (ca_json_get_string(json,
                           json_len,
                           "default_provider",
                           config->default_provider,
                           sizeof(config->default_provider)) != CA_OK) {
        ca_config_warn("default_provider", "missing or invalid, using default");
    }

    /*
     * CAGENT_PROVIDER selects which provider block to read, but the final
     * env overlay is still centralized in ca_config_apply_env().
     */
    provider_env = getenv("CAGENT_PROVIDER");
    selected_provider = ca_config_env_has_value(provider_env) ? provider_env : config->default_provider;

    /* 3. 用 default_provider 的值作为 key，在 providers 对象里找对应 block
     *    Use default_provider value as the key into the "providers" object. */
    if (ca_json_find_object(json, json_len, "providers", &providers_obj, &providers_len) == CA_OK) {
        (void)ca_json_find_object(providers_obj, providers_len, selected_provider, &provider_obj, &provider_len);
    }

    /* 4. 如果 providers 下没找到，回退到顶层（兼容老配置格式）
     *    Fallback: look for the provider block at top level (backward compat). */
    if (provider_obj == NULL) {
        (void)ca_json_find_object(json, json_len, selected_provider, &provider_obj, &provider_len);
    }

    if (provider_obj != NULL) {
        /*
         * 老 Phase 2 配置文件可能没有 "type" 字段——缺失时默认当作 openai_compat，
         * 这样已有用户配置不用改。
         * Old Phase 2 configs may lack "type" — treat missing as openai_compat,
         * so existing user configs stay valid without changes.
         */
        type_status = ca_json_get_string(provider_obj,
                                         provider_len,
                                         "type",
                                         provider_type_text,
                                         sizeof(provider_type_text));
        if (type_status == CA_OK) {
            config->provider_type = ca_provider_type_from_string(provider_type_text);
        } else {
            ca_config_warn("providers.<selected>.type", "missing or invalid, using openai_compat");
        }

        /*
         * compat_profile 比 provider type 更细粒度——只影响 OpenAI 兼容模式下
         * 的源特定行为（NewAPI / DeepSeek 等）。缺失时保持 generic，老配置继续工作。
         * compat_profile is narrower than provider type: only captures
         * OpenAI-compatible source differences (NewAPI, DeepSeek, etc.).
         * Missing values stay generic so older configs continue to work.
         */
        profile_status = ca_json_get_string(provider_obj,
                                            provider_len,
                                            "compat_profile",
                                            compat_profile_text,
                                            sizeof(compat_profile_text));
        if (profile_status == CA_OK) {
            config->compat_profile = ca_compat_profile_from_string(compat_profile_text);
        } else {
            ca_config_warn("providers.<selected>.compat_profile", "missing or invalid, using generic");
        }

        if (ca_json_get_string(provider_obj, provider_len, "base_url", config->base_url, sizeof(config->base_url)) != CA_OK) {
            ca_config_warn("providers.<selected>.base_url", "missing or invalid, using default");
        }
        if (ca_json_get_string(provider_obj,
                               provider_len,
                               "api_key_env",
                               config->api_key_env,
                               sizeof(config->api_key_env)) != CA_OK) {
            ca_config_warn("providers.<selected>.api_key_env", "missing or invalid, using default");
        }
        if (ca_json_get_string(provider_obj, provider_len, "model", config->model, sizeof(config->model)) != CA_OK) {
            ca_config_warn("providers.<selected>.model", "missing or invalid, using default");
        }
    } else {
        ca_config_warn("providers.<selected>", "not found, using default provider settings");
    }

    /* 5. agent 子对象 / agent sub-object */
    if (ca_json_find_object(json, json_len, "agent", &agent_obj, &agent_len) == CA_OK) {
        if (ca_json_get_int(agent_obj, agent_len, "max_steps", &config->agent_max_steps) != CA_OK) {
            ca_config_warn("agent.max_steps", "missing or invalid, using default");
        }
        if (ca_json_get_double(agent_obj, agent_len, "temperature", &config->agent_temperature) != CA_OK) {
            ca_config_warn("agent.temperature", "missing or invalid, using default");
        }
        if (ca_json_get_bool(agent_obj, agent_len, "stream", &config->agent_stream) != CA_OK) {
            ca_config_warn("agent.stream", "missing or invalid, using default");
        }
    } else {
        ca_config_warn("agent", "not found, using defaults");
    }

    /* 6. permission 子对象 / permission sub-object */
    if (ca_json_find_object(json, json_len, "permission", &permission_obj, &permission_len) == CA_OK) {
        if (ca_json_get_string(permission_obj,
                               permission_len,
                               "default_write",
                               config->permission_default_write,
                               sizeof(config->permission_default_write)) != CA_OK) {
            ca_config_warn("permission.default_write", "missing or invalid, using default");
        }
        if (ca_json_get_string(permission_obj,
                               permission_len,
                               "default_shell",
                               config->permission_default_shell,
                               sizeof(config->permission_default_shell)) != CA_OK) {
            ca_config_warn("permission.default_shell", "missing or invalid, using default");
        }
    } else {
        ca_config_warn("permission", "not found, using defaults");
    }

    return CA_OK;
}

/* ---- 配置管线 / config pipeline ---- */

ca_status_t ca_config_init_defaults(ca_config_t *config)
{
    if (config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));

    /* 硬编码默认值，用户配置文件或环境变量可以覆盖 / hard-coded defaults, overridable */
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

/* 默认配置文件路径：~/.cagent/config.json（Windows 上用 %USERPROFILE%） */
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
        fprintf(stderr, "Warning: home directory environment variable is missing; using current directory for config path.\n");
        home = ".";
    }

    written = snprintf(buffer, buffer_size, "%s%s.cagent%sconfig.json", home, CA_CONFIG_PATH_SEP, CA_CONFIG_PATH_SEP);
    if (written < 0 || (size_t)written >= buffer_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

/*
 * 配置加载顺序 / loading order:
 *  1. 初始化默认值
 *  2. 确定配置文件路径（用户指定 or 默认路径）
 *  3. 读 JSON 文件并解析 MVP 字段
 *  4. 环境变量最后覆盖
 *  文件不存在时用默认值 + 环境变量，不算致命错误。
 */
ca_status_t ca_config_load(ca_config_t *config, const char *config_path)
{
    char default_path[CA_CONFIG_PATH_CAP];
    const char *path_to_load = config_path;
    char *json = NULL;
    size_t json_len = 0;
    int file_missing = 0;
    int file_errno = 0;
    ca_status_t status;

    if (config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    /* 1. 默认值 / defaults */
    status = ca_config_init_defaults(config);
    if (status != CA_OK) {
        return status;
    }

    /* 2. 确定路径 / resolve path */
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

    /* 3. 读文件 + 解析 / read + parse */
    status = ca_config_read_file(path_to_load, &json, &json_len, &file_missing, &file_errno);
    if (status == CA_ERR_IO && file_missing) {
        /* 文件不存在 → 标记并继续 / file missing → note it and continue */
        config->missing_config_file = 1;
        return ca_config_apply_env(config);
    }
    if (status != CA_OK) {
        if (file_errno != 0) {
            fprintf(stderr,
                    "Warning: failed to open config file '%s': %s (errno=%d)\n",
                    path_to_load,
                    strerror(file_errno),
                    file_errno);
        }
        return status;
    }

    status = ca_config_parse_mvp_json(config, json, json_len);
    free(json);
    if (status != CA_OK) {
        return status;
    }

    config->loaded_from_file = 1;

    /* 4. 环境变量最后覆盖 / env overlay last */
    return ca_config_apply_env(config);
}

/* ---- 调试/展示 / debug dump ---- */
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

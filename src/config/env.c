/*
 * env.c
 * 环境变量覆盖层：在配置文件加载之后运行，把 CAGENT_* 环境变量写入 ca_config_t。
 * 环境变量优先级高于配置文件——方便临时切换 provider / model 等。
 *
 * Environment variable overlay: runs after config file load, writes CAGENT_* env vars
 * into ca_config_t. Env vars take priority over the config file — handy for quick overrides.
 *
 * 支持的环境变量 / supported env vars:
 *   CAGENT_PROVIDER        — provider 名称
 *   CAGENT_PROVIDER_TYPE   — provider 实现家族（openai_compat / gemini / claude）
 *   CAGENT_COMPAT_PROFILE  — OpenAI 兼容模式细节（generic / newapi / deepseek）
 *   CAGENT_BASE_URL        — API 端点 URL
 *   CAGENT_MODEL           — 模型名称
 */
#include "ca_config.h"

#include <stdio.h>
#include <stdlib.h>

/* 安全字符串复制（和 config.c 里的 ca_config_copy_string 类似，env 模块自己一份）
 * Safe string copy, separate copy to keep env module self-contained. */
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

/* 检查环境变量存在且非空 / check env var exists and isn't empty */
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

    /* CAGENT_PROVIDER — 选择 provider block 名称 */
    provider = getenv("CAGENT_PROVIDER");
    if (ca_env_has_value(provider)) {
        ca_status_t status = ca_env_copy(config->default_provider, sizeof(config->default_provider), provider);
        if (status != CA_OK) {
            return status;
        }
        config->provider_from_env = 1;
    }

    /*
     * provider 名称和 provider type 是故意分开的两个维度：
     *   名称 → 选择用户定义的配置块
     *   类型 → 选择 LLM 实现家族
     * Provider name selects a user-defined config block, while type selects the
     * future LLM implementation family. Deliberately kept separate.
     */
    provider_type = getenv("CAGENT_PROVIDER_TYPE");
    if (ca_env_has_value(provider_type)) {
        config->provider_type = ca_provider_type_from_string(provider_type);
        config->provider_type_from_env = 1;
    }

    /*
     * compat_profile 是 OpenAI 兼容模式下的微调参数，不是新的 provider 类型。
     * 它让未来的 HTTP 层做一些源特定的行为调整，但请求协议还是 openai_compat。
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

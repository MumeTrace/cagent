/*
 * ca_config.h
 * 配置系统核心类型：provider 选择、兼容模式、agent 参数、权限策略。
 * 支持从 JSON 文件加载 + 环境变量覆盖，所有字段集中在 ca_config_t 里。
 *
 * Core config types: provider selection, compat profiles, agent tuning, permissions.
 * Loads from a JSON file, then env vars overlay — everything lands in ca_config_t.
 */
#ifndef CA_CONFIG_H
#define CA_CONFIG_H

#include <stdio.h>

#include "ca_status.h"

#define CA_CONFIG_PROVIDER_CAP  64     /* max length of provider name            */
#define CA_CONFIG_URL_CAP       512    /* max length of base_url                 */
#define CA_CONFIG_ENV_CAP       128    /* max length of api_key_env               */
#define CA_CONFIG_MODEL_CAP     128    /* max length of model name               */
#define CA_CONFIG_POLICY_CAP    32     /* max length of permission policy string  */
#define CA_CONFIG_PATH_CAP      4096   /* max length of config file path          */

/*
 * Provider family — 决定了后面 LLM 代码走哪套 HTTP 协议
 * Which HTTP protocol family the LLM backend speaks.
 */
typedef enum ca_provider_type {
    CA_PROVIDER_OPENAI_COMPAT = 0,   /* OpenAI / v1 兼容接口 / OpenAI-compatible */
    CA_PROVIDER_GEMINI,              /* Google Gemini */
    CA_PROVIDER_CLAUDE,              /* Anthropic Claude */
    CA_PROVIDER_UNKNOWN
} ca_provider_type_t;

/*
 * OpenAI 兼容下的细分兼容模式（NewAPI / DeepSeek 等）
 * Fine-grained compat tuning inside the openai_compat family.
 */
typedef enum ca_compat_profile {
    CA_COMPAT_GENERIC = 0,    /* 标准 OpenAI / standard OpenAI */
    CA_COMPAT_NEWAPI,         /* NewAPI 网关 / NewAPI gateway */
    CA_COMPAT_DEEPSEEK,       /* DeepSeek 特殊行为 / DeepSeek quirks */
    CA_COMPAT_UNKNOWN
} ca_compat_profile_t;

/*
 * ca_config_t — 运行时的全部配置，一份到处带着
 * Runtime config bag — one struct passed everywhere.
 */
typedef struct ca_config {
    char default_provider[CA_CONFIG_PROVIDER_CAP];   /* provider name（key into "providers"） */
    ca_provider_type_t provider_type;                /* 实现家族 / which protocol family         */
    ca_compat_profile_t compat_profile;              /* 兼容微调 / compat tuning                */
    char base_url[CA_CONFIG_URL_CAP];                /* API endpoint URL                        */
    char api_key_env[CA_CONFIG_ENV_CAP];             /* 存 API key 的环境变量名 / env var name  */
    char model[CA_CONFIG_MODEL_CAP];                 /* 模型名称 / model name                   */
    int agent_max_steps;                             /* agent loop 最大步数 / max iterations    */
    double agent_temperature;                        /* LLM temperature                          */
    int agent_stream;                                /* 是否流式 / streaming on/off              */
    char permission_default_write[CA_CONFIG_POLICY_CAP];   /* 写文件策略: ask/allow/deny        */
    char permission_default_shell[CA_CONFIG_POLICY_CAP];   /* 执行命令策略: ask/allow/deny       */
    char config_path[CA_CONFIG_PATH_CAP];                  /* 实际加载的配置文件路径 / loaded from */

    /* ---- 标记字段，方便 print_summary 和调试 / tracking fields for reporting ---- */
    int loaded_from_file;
    int missing_config_file;
    int provider_from_env;
    int provider_type_from_env;
    int compat_profile_from_env;
    int base_url_from_env;
    int model_from_env;
} ca_config_t;

/* ---- 字符串 <-> 枚举互转 / string ⇄ enum converters ---- */
ca_provider_type_t ca_provider_type_from_string(const char *value);
const char *ca_provider_type_to_string(ca_provider_type_t type);
ca_compat_profile_t ca_compat_profile_from_string(const char *value);
const char *ca_compat_profile_to_string(ca_compat_profile_t profile);

/* ---- 配置加载管线 / config loading pipeline ---- */
ca_status_t ca_config_init_defaults(ca_config_t *config);
ca_status_t ca_config_default_path(char *buffer, size_t buffer_size);
ca_status_t ca_config_load(ca_config_t *config, const char *config_path);
ca_status_t ca_config_apply_env(ca_config_t *config);

/* ---- 调试/展示 / debug ---- */
ca_status_t ca_config_print_summary(const ca_config_t *config, FILE *stream);

#endif

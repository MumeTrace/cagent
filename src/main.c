/*
 * main.c
 * 程序入口：解析命令行参数，加载配置，扫描项目，分发到对应模式。
 * 支持的模式：默认（无参数 → REPL）、--version / --help、--config 指定配置、单次 prompt。
 *
 * Entry point: parse args → load config → scan project → dispatch to mode.
 * Modes: default (no args → REPL), --version / --help, --config <path>, one-shot prompt.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ca_cli.h"
#include "ca_config.h"
#include "ca_file_tools.h"
#include "ca_llm.h"
#include "ca_platform.h"
#include "ca_project.h"
#include "ca_status.h"
#include "ca_tool.h"

/*
 * 把 argc/argv 里剩下的 prompt 片段用空格拼成完整字符串
 * Join remaining argv tokens into one prompt string with spaces.
 */
static ca_status_t ca_main_join_prompt(int argc, char **argv, char **out_prompt)
{
    char *prompt;
    size_t total = 1;
    int i;

    if (argv == NULL || out_prompt == NULL || argc <= 0) {
        return CA_ERR_INVALID_ARG;
    }

    *out_prompt = NULL;

    /* 先算总长度 / figure out total length first */
    for (i = 0; i < argc; i++) {
        total += strlen(argv[i]);
        if (i + 1 < argc) {
            total++;
        }
    }

    prompt = (char *)malloc(total);
    if (prompt == NULL) {
        return CA_ERR_NO_MEMORY;
    }

    /* 逐个拼接，中间插空格 / stitch together with spaces */
    prompt[0] = '\0';
    for (i = 0; i < argc; i++) {
        size_t used = strlen(prompt);
        size_t part_len = strlen(argv[i]);

        memcpy(prompt + used, argv[i], part_len);
        used += part_len;
        if (i + 1 < argc) {
            prompt[used] = ' ';
            used++;
        }
        prompt[used] = '\0';
    }

    *out_prompt = prompt;
    return CA_OK;
}

/*
 * 加载配置 — 文件不存在时给默认值，不致命
 * Load config: missing file → defaults, not fatal.
 */
static ca_status_t ca_main_load_config(const char *config_path, ca_config_t *config)
{
    ca_status_t status;

    if (config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_config_load(config, config_path);
    if (status != CA_OK) {
        fprintf(stderr, "Failed to load configuration.\n");
        return status;
    }

    if (config->missing_config_file) {
        fprintf(stderr, "Config file not found: %s\n", config->config_path);
        fprintf(stderr, "Using defaults. Set CAGENT_API_KEY/CAGENT_BASE_URL or create the config file.\n");
    }

    return CA_OK;
}

/*
 * 加载项目索引 / load project index from workspace
 */
static ca_status_t ca_main_load_project(ca_project_index_t *project)
{
    char workspace[CA_PROJECT_PATH_CAP];
    ca_status_t status;

    if (project == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_project_get_current_workspace(workspace, sizeof(workspace));
    if (status != CA_OK) {
        fprintf(stderr, "Failed to identify workspace.\n");
        return status;
    }

    status = ca_project_index_init(project, workspace);
    if (status != CA_OK) {
        fprintf(stderr, "Failed to initialize project index.\n");
        return status;
    }

    status = ca_project_index_scan(project);
    if (status != CA_OK) {
        ca_project_index_free(project);
        fprintf(stderr, "Failed to scan project index.\n");
        return status;
    }

    return CA_OK;
}

static ca_status_t ca_main_load_tools(ca_tool_registry_t *tools)
{
    ca_status_t status;

    if (tools == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_tool_registry_init(tools);
    if (status != CA_OK) {
        return status;
    }

    status = ca_register_builtin_tools(tools);
    if (status != CA_OK) {
        ca_tool_registry_free(tools);
        fprintf(stderr, "Failed to register builtin tools.\n");
        return status;
    }

    status = ca_register_file_tools(tools);
    if (status != CA_OK) {
        ca_tool_registry_free(tools);
        fprintf(stderr, "Failed to register file tools.\n");
        return status;
    }

    return CA_OK;
}

static ca_status_t ca_main_load_llm(const ca_config_t *config, ca_llm_provider_t *llm)
{
    if (config == NULL || llm == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(llm, 0, sizeof(*llm));
    if (config->provider_type == CA_PROVIDER_FAKE) {
        return ca_llm_fake_provider_init(llm);
    }
    if (config->provider_type == CA_PROVIDER_OPENAI_COMPAT) {
        return ca_llm_openai_compat_provider_init(llm, config);
    }

    fprintf(stderr,
            "Unsupported provider type for Phase 8: %s\n",
            ca_provider_type_to_string(config->provider_type));
    return CA_ERR_LLM;
}

/* ---- main implementation: 参数解析 → 分发 / arg parsing → dispatch ---- */
static int ca_main_run(int argc, char **argv)
{
    ca_status_t status;
    char *prompt = NULL;
    ca_config_t config;
    ca_project_index_t project;
    ca_tool_registry_t tools;
    ca_llm_provider_t llm;
    const char *config_path = NULL;
    int prompt_start = 0;
    int i;

    /* 无参数 → 交互 REPL / no args → interactive */
    if (argc == 1) {
        status = ca_main_load_config(NULL, &config);
        if (status != CA_OK) {
            return status;
        }
        status = ca_main_load_project(&project);
        if (status != CA_OK) {
            return status;
        }
        status = ca_main_load_tools(&tools);
        if (status != CA_OK) {
            ca_project_index_free(&project);
            return status;
        }
        status = ca_main_load_llm(&config, &llm);
        if (status != CA_OK) {
            ca_tool_registry_free(&tools);
            ca_project_index_free(&project);
            return status;
        }
        status = ca_cli_run_default(&config, &project, &tools, &llm);
        ca_llm_provider_free(&llm);
        ca_tool_registry_free(&tools);
        ca_project_index_free(&project);
        return status;
    }

    /* --version / -v */
    if (argc == 2 &&
        (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        return ca_cli_print_version();
    }

    /* --help / -h */
    if (argc == 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        return ca_cli_print_help();
    }

    /* 先扫一遍参数，找 --config 和 prompt 起始位置
     * Scan args: pick out --config, find where prompt starts. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--config requires a path.\n");
                return CA_ERR_INVALID_ARG;
            }
            config_path = argv[i + 1];
            i++;
            continue;
        }

        /* 不认识的 flag → 打印帮助 / unknown flag → show help */
        if (argv[i][0] == '-') {
            ca_cli_print_help();
            return CA_ERR_INVALID_ARG;
        }

        prompt_start = i;
        break;
    }

    status = ca_main_load_config(config_path, &config);
    if (status != CA_OK) {
        return status;
    }

    /* 没有 prompt → 默认模式（REPL or stdin） */
    if (prompt_start == 0) {
        status = ca_main_load_project(&project);
        if (status != CA_OK) {
            return status;
        }
        status = ca_main_load_tools(&tools);
        if (status != CA_OK) {
            ca_project_index_free(&project);
            return status;
        }
        status = ca_main_load_llm(&config, &llm);
        if (status != CA_OK) {
            ca_tool_registry_free(&tools);
            ca_project_index_free(&project);
            return status;
        }
        status = ca_cli_run_default(&config, &project, &tools, &llm);
        ca_llm_provider_free(&llm);
        ca_tool_registry_free(&tools);
        ca_project_index_free(&project);
        return status;
    }

    /* 有 prompt → 单次执行 / one-shot */
    status = ca_main_join_prompt(argc - prompt_start, argv + prompt_start, &prompt);
    if (status != CA_OK) {
        return status;
    }

    status = ca_main_load_project(&project);
    if (status != CA_OK) {
        free(prompt);
        return status;
    }

    status = ca_main_load_tools(&tools);
    if (status != CA_OK) {
        ca_project_index_free(&project);
        free(prompt);
        return status;
    }

    status = ca_main_load_llm(&config, &llm);
    if (status != CA_OK) {
        ca_tool_registry_free(&tools);
        ca_project_index_free(&project);
        free(prompt);
        return status;
    }

    status = ca_cli_run_once(prompt, &config, &project, &tools, &llm);
    ca_llm_provider_free(&llm);
    ca_tool_registry_free(&tools);
    ca_project_index_free(&project);
    free(prompt);

    return status;
}

int main(int argc, char **argv)
{
    int app_argc = argc;
    char **app_argv = argv;
    int result;
    ca_status_t status;

    status = ca_console_init();
    if (status != CA_OK) {
        return status;
    }

    status = ca_platform_argv_to_utf8(argc, argv, &app_argc, &app_argv);
    if (status != CA_OK) {
        return status;
    }

    result = ca_main_run(app_argc, app_argv);
    if (app_argv != argv) {
        ca_platform_argv_free(app_argc, app_argv);
    }

    return result;
}

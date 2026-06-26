/*
 * repl.c
 * CLI 前端：交互 REPL、单次 prompt 执行、stdin 管道模式、帮助和版本打印。
 * 目前 Phase 3 阶段，LLM 和 Agent Loop 未实现，自然语言输入仅 echo 回显。
 *
 * CLI frontend: interactive REPL, one-shot prompt, stdin pipe, help & version.
 * Currently at Phase 3 — LLM / Agent Loop not yet wired; natural-language prompts just echo.
 */
#include "ca_cli.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define CA_GETCWD   _getcwd
#define CA_ISATTY   _isatty
#define CA_FILENO   _fileno
#else
#include <unistd.h>
#define CA_GETCWD   getcwd
#define CA_ISATTY   isatty
#define CA_FILENO   fileno
#endif

#ifndef CAGENT_VERSION
#define CAGENT_VERSION "0.0.0"     /* 构建时 CMake 会覆盖 / overridden by CMake at build */
#endif

#define CA_REPL_LINE_CAP    4096   /* 单行输入上限 / max input line length  */
#define CA_WORKSPACE_CAP    4096   /* 工作区路径上限 / max workspace path   */

/* ---- 信号处理 / signal handling ---- */
static volatile sig_atomic_t g_ca_interrupted = 0;

static void ca_cli_on_sigint(int signal_number)
{
    (void)signal_number;
    g_ca_interrupted = 1;
}

/* ---- 字符串工具 / string utils ---- */
static ca_status_t ca_trim_in_place(char *text, char **out_trimmed)
{
    char *end;

    if (text == NULL || out_trimmed == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *out_trimmed = NULL;

    /* 跳过前导空白 / skip leading whitespace */
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    /* 截掉尾部空白 / trim trailing whitespace */
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    *out_trimmed = text;
    return CA_OK;
}

/* ---- 工作区获取 / get workspace ---- */
static ca_status_t ca_cli_get_workspace(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    if (CA_GETCWD(buffer, buffer_size) == NULL) {
        return CA_ERR_IO;
    }

    buffer[buffer_size - 1] = '\0';
    return CA_OK;
}

/* ---- 交互模式横幅 / interactive banner ---- */
static ca_status_t ca_cli_print_banner(const ca_config_t *config)
{
    char workspace[CA_WORKSPACE_CAP];
    ca_status_t status;

    if (config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_cli_get_workspace(workspace, sizeof(workspace));

    printf("C-Agent CLI\n\n");
    if (status == CA_OK) {
        printf("Workspace: %s\n", workspace);
    } else {
        printf("Workspace: <unavailable>\n");
    }
    printf("Provider: %s\n", config->default_provider);
    printf("Provider type: %s\n", ca_provider_type_to_string(config->provider_type));
    printf("Compat profile: %s\n", ca_compat_profile_to_string(config->compat_profile));
    printf("Model: %s\n", config->model);
    printf("Mode: interactive\n\n");
    printf("Type /help for help, /exit to quit.\n\n");

    return CA_OK;
}

/* ---- REPL 帮助 / REPL help ---- */
static ca_status_t ca_cli_print_repl_help(void)
{
    printf("Commands:\n");
    printf("  /help          Show this help\n");
    printf("  /config        Show current configuration summary\n");
    printf("  /project       Show current project summary\n");
    printf("  /exit, /quit   Exit the REPL\n");
    printf("\n");
    printf("Natural language prompts are accepted, but LLM and Agent Loop are not implemented in Phase 3.\n");
    return CA_OK;
}

/* ---- prompt 处理（Phase 3 占位）/ prompt handler (Phase 3 placeholder) ---- */
static ca_status_t ca_cli_handle_prompt(const char *prompt, const ca_config_t *config)
{
    if (prompt == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    (void)config;

    printf("Phase 3 CLI received: %s\n", prompt);
    printf("LLM, tools, and Agent Loop are not implemented yet.\n");
    return CA_OK;
}

/*
 * 处理一行输入：先 trim，再判断是 /command 还是自然语言 prompt
 * Handle one input line: trim → decide /command vs natural-language prompt.
 */
static ca_status_t ca_cli_handle_input(char *input,
                                       const ca_config_t *config,
                                       const ca_project_index_t *project,
                                       int *should_exit)
{
    char *trimmed;

    if (input == NULL || config == NULL || project == NULL || should_exit == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *should_exit = 0;
    if (ca_trim_in_place(input, &trimmed) != CA_OK) {
        return CA_ERR_INVALID_ARG;
    }
    if (trimmed[0] == '\0') {
        return CA_OK;     /* 空行，啥也不做 / empty line — skip */
    }

    /* /command 路由 / command routing */
    if (trimmed[0] == '/') {
        if (strcmp(trimmed, "/help") == 0) {
            return ca_cli_print_repl_help();
        }
        if (strcmp(trimmed, "/config") == 0) {
            return ca_config_print_summary(config, stdout);
        }
        if (strcmp(trimmed, "/project") == 0) {
            ca_project_index_print_summary(project, stdout);
            return CA_OK;
        }
        if (strcmp(trimmed, "/exit") == 0 || strcmp(trimmed, "/quit") == 0) {
            *should_exit = 1;
            return CA_OK;
        }

        printf("Unknown command: %s\n", trimmed);
        printf("Type /help for help.\n");
        return CA_OK;
    }

    /* 普通文本 → 当成 prompt / plain text → treat as prompt */
    return ca_cli_handle_prompt(trimmed, config);
}

/* ---- stdin 全部读入 / slurp entire stdin ---- */
static ca_status_t ca_cli_read_all_stdin(char **out_text)
{
    char chunk[1024];
    char *buffer = NULL;
    size_t used = 0;
    size_t capacity = 0;

    if (out_text == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    /* 循环 fgets，动态扩容 / keep reading, double buffer as needed */
    while (fgets(chunk, sizeof(chunk), stdin) != NULL) {
        size_t chunk_len = strlen(chunk);
        size_t required = used + chunk_len + 1;

        if (required > capacity) {
            size_t new_capacity = capacity == 0 ? 2048 : capacity * 2;
            char *new_buffer;

            while (new_capacity < required) {
                new_capacity *= 2;
            }

            new_buffer = (char *)realloc(buffer, new_capacity);
            if (new_buffer == NULL) {
                free(buffer);
                return CA_ERR_NO_MEMORY;
            }

            buffer = new_buffer;
            capacity = new_capacity;
        }

        memcpy(buffer + used, chunk, chunk_len);
        used += chunk_len;
        buffer[used] = '\0';
    }

    if (ferror(stdin)) {
        free(buffer);
        return CA_ERR_IO;
    }

    /* 如果 stdin 完全为空，返回空串而非 NULL
     * Completely empty stdin → return empty string, not NULL. */
    if (buffer == NULL) {
        buffer = (char *)malloc(1);
        if (buffer == NULL) {
            return CA_ERR_NO_MEMORY;
        }
        buffer[0] = '\0';
    }

    *out_text = buffer;
    return CA_OK;
}

/* ---- 公共 CLI 入口 / public CLI entry points ---- */

ca_status_t ca_cli_print_help(void)
{
    printf("Usage: cagent [options] [prompt]\n\n");
    printf("Options:\n");
    printf("  --config <path>  Use a specific config file\n");
    printf("  --help       Show this help\n");
    printf("  --version    Show version\n");
    printf("\n");
    printf("Modes:\n");
    printf("  cagent                 Start interactive REPL\n");
    printf("  cagent \"hello\"         Run a single prompt and exit\n");
    printf("  echo \"hello\" | cagent  Read one prompt from stdin and exit\n");
    return CA_OK;
}

ca_status_t ca_cli_print_version(void)
{
    printf("cagent %s\n", CAGENT_VERSION);
    return CA_OK;
}

/* 默认模式：先判断是不是 TTY，不是就走 stdin，是就走 REPL
 * Default: if not a TTY → stdin mode; otherwise → interactive REPL. */
ca_status_t ca_cli_run_default(const ca_config_t *config, const ca_project_index_t *project)
{
    if (config == NULL || project == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    if (!CA_ISATTY(CA_FILENO(stdin))) {
        return ca_cli_run_stdin(config, project);
    }

    return ca_cli_run_interactive(config, project);
}

/* 交互 REPL 主循环 / interactive REPL main loop */
ca_status_t ca_cli_run_interactive(const ca_config_t *config, const ca_project_index_t *project)
{
    char line[CA_REPL_LINE_CAP];
    void (*previous_handler)(int);
    ca_status_t final_status = CA_OK;

    if (config == NULL || project == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    /* 挂 SIGINT handler，Ctrl+C 不会直接退出 / catch Ctrl+C gracefully */
    previous_handler = signal(SIGINT, ca_cli_on_sigint);
    (void)previous_handler;

    ca_cli_print_banner(config);

    for (;;) {
        int should_exit = 0;
        ca_status_t status;

        if (g_ca_interrupted) {
            g_ca_interrupted = 0;
            printf("\nType /exit to quit.\n");
        }

        printf("> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* Ctrl+D 或 EOF → 退出 / EOF → exit */
            if (g_ca_interrupted) {
                clearerr(stdin);
                continue;
            }
            printf("\n");
            break;
        }

        status = ca_cli_handle_input(line, config, project, &should_exit);
        if (status != CA_OK) {
            final_status = status;
            goto cleanup;
        }
        if (should_exit) {
            break;
        }
    }

cleanup:
    /*
     * The REPL owns the temporary SIGINT handler. Restore the previous process
     * handler on every exit path so later non-REPL code keeps its own behavior.
     */
    if (previous_handler != SIG_ERR) {
        signal(SIGINT, previous_handler);
    }
    return final_status;
}

/* 单次 prompt 执行 / one-shot prompt */
ca_status_t ca_cli_run_once(const char *prompt, const ca_config_t *config, const ca_project_index_t *project)
{
    char *copy;
    int should_exit = 0;
    ca_status_t status;
    size_t prompt_len;

    if (prompt == NULL || config == NULL || project == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    /* 复制一份，因为 ca_cli_handle_input 会原地 trim / copy before in-place trim */
    prompt_len = strlen(prompt);
    copy = (char *)malloc(prompt_len + 1);
    if (copy == NULL) {
        return CA_ERR_NO_MEMORY;
    }

    memcpy(copy, prompt, prompt_len + 1);
    status = ca_cli_handle_input(copy, config, project, &should_exit);
    free(copy);

    return status;
}

/* stdin 管道模式 / pipe mode */
ca_status_t ca_cli_run_stdin(const ca_config_t *config, const ca_project_index_t *project)
{
    char *input = NULL;
    ca_status_t status = ca_cli_read_all_stdin(&input);

    if (config == NULL || project == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    if (status != CA_OK) {
        return status;
    }

    status = ca_cli_run_once(input, config, project);
    free(input);

    return status;
}

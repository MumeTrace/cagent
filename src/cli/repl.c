#include "ca_cli.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define CA_GETCWD _getcwd
#define CA_ISATTY _isatty
#define CA_FILENO _fileno
#else
#include <unistd.h>
#define CA_GETCWD getcwd
#define CA_ISATTY isatty
#define CA_FILENO fileno
#endif

#ifndef CAGENT_VERSION
#define CAGENT_VERSION "0.0.0"
#endif

#define CA_REPL_LINE_CAP 4096
#define CA_WORKSPACE_CAP 4096

static volatile sig_atomic_t g_ca_interrupted = 0;

static void ca_cli_on_sigint(int signal_number)
{
    (void)signal_number;
    g_ca_interrupted = 1;
}

static ca_status_t ca_trim_in_place(char *text, char **out_trimmed)
{
    char *end;

    if (text == NULL || out_trimmed == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *out_trimmed = NULL;

    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    *out_trimmed = text;
    return CA_OK;
}

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

static ca_status_t ca_cli_print_banner(const ca_config_t *config)
{
    char workspace[CA_WORKSPACE_CAP];
    ca_status_t status = ca_cli_get_workspace(workspace, sizeof(workspace));

    if (config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

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

static ca_status_t ca_cli_print_repl_help(void)
{
    printf("Commands:\n");
    printf("  /help          Show this help\n");
    printf("  /config        Show current configuration summary\n");
    printf("  /exit, /quit   Exit the REPL\n");
    printf("\n");
    printf("Natural language prompts are accepted, but LLM and Agent Loop are not implemented in Phase 2.2.\n");
    return CA_OK;
}

static ca_status_t ca_cli_handle_prompt(const char *prompt, const ca_config_t *config)
{
    if (prompt == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    (void)config;

    printf("Phase 2.2 CLI received: %s\n", prompt);
    printf("LLM, tools, and Agent Loop are not implemented yet.\n");
    return CA_OK;
}

static ca_status_t ca_cli_handle_input(char *input, const ca_config_t *config, int *should_exit)
{
    char *trimmed;

    if (input == NULL || config == NULL || should_exit == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *should_exit = 0;
    if (ca_trim_in_place(input, &trimmed) != CA_OK) {
        return CA_ERR_INVALID_ARG;
    }
    if (trimmed[0] == '\0') {
        return CA_OK;
    }

    if (trimmed[0] == '/') {
        if (strcmp(trimmed, "/help") == 0) {
            return ca_cli_print_repl_help();
        }
        if (strcmp(trimmed, "/config") == 0) {
            return ca_config_print_summary(config, stdout);
        }
        if (strcmp(trimmed, "/exit") == 0 || strcmp(trimmed, "/quit") == 0) {
            *should_exit = 1;
            return CA_OK;
        }

        printf("Unknown command: %s\n", trimmed);
        printf("Type /help for help.\n");
        return CA_OK;
    }

    return ca_cli_handle_prompt(trimmed, config);
}

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

ca_status_t ca_cli_run_default(const ca_config_t *config)
{
    if (config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    if (!CA_ISATTY(CA_FILENO(stdin))) {
        return ca_cli_run_stdin(config);
    }

    return ca_cli_run_interactive(config);
}

ca_status_t ca_cli_run_interactive(const ca_config_t *config)
{
    char line[CA_REPL_LINE_CAP];
    void (*previous_handler)(int);

    if (config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

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
            if (g_ca_interrupted) {
                clearerr(stdin);
                continue;
            }
            printf("\n");
            break;
        }

        status = ca_cli_handle_input(line, config, &should_exit);
        if (status != CA_OK) {
            return status;
        }
        if (should_exit) {
            break;
        }
    }

    return CA_OK;
}

ca_status_t ca_cli_run_once(const char *prompt, const ca_config_t *config)
{
    char *copy;
    int should_exit = 0;
    ca_status_t status;
    size_t prompt_len;

    if (prompt == NULL || config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    prompt_len = strlen(prompt);
    copy = (char *)malloc(prompt_len + 1);
    if (copy == NULL) {
        return CA_ERR_NO_MEMORY;
    }

    memcpy(copy, prompt, prompt_len + 1);
    status = ca_cli_handle_input(copy, config, &should_exit);
    free(copy);

    return status;
}

ca_status_t ca_cli_run_stdin(const ca_config_t *config)
{
    char *input = NULL;
    ca_status_t status = ca_cli_read_all_stdin(&input);

    if (config == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    if (status != CA_OK) {
        return status;
    }

    status = ca_cli_run_once(input, config);
    free(input);

    return status;
}

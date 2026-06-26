#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ca_cli.h"
#include "ca_config.h"
#include "ca_status.h"

static ca_status_t ca_main_join_prompt(int argc, char **argv, char **out_prompt)
{
    char *prompt;
    size_t total = 1;
    int i;

    if (argv == NULL || out_prompt == NULL || argc <= 0) {
        return CA_ERR_INVALID_ARG;
    }

    *out_prompt = NULL;

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

int main(int argc, char **argv)
{
    ca_status_t status;
    char *prompt = NULL;
    ca_config_t config;
    const char *config_path = NULL;
    int prompt_start = 0;
    int i;

    if (argc == 1) {
        status = ca_main_load_config(NULL, &config);
        if (status != CA_OK) {
            return status;
        }
        return ca_cli_run_default(&config);
    }

    if (argc == 2 &&
        (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        return ca_cli_print_version();
    }

    if (argc == 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        return ca_cli_print_help();
    }

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

    if (prompt_start == 0) {
        return ca_cli_run_default(&config);
    }

    status = ca_main_join_prompt(argc - prompt_start, argv + prompt_start, &prompt);
    if (status != CA_OK) {
        return status;
    }

    status = ca_cli_run_once(prompt, &config);
    free(prompt);

    return status;
}

#include <stdlib.h>
#include <string.h>

#include "ca_cli.h"
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

int main(int argc, char **argv)
{
    ca_status_t status;
    char *prompt = NULL;

    if (argc == 1) {
        return ca_cli_run_default();
    }

    if (argc == 2 &&
        (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        return ca_cli_print_version();
    }

    if (argc == 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        return ca_cli_print_help();
    }

    if (argv[1][0] == '-') {
        ca_cli_print_help();
        return CA_ERR_INVALID_ARG;
    }

    status = ca_main_join_prompt(argc - 1, argv + 1, &prompt);
    if (status != CA_OK) {
        return status;
    }

    status = ca_cli_run_once(prompt);
    free(prompt);

    return status;
}

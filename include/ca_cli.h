#ifndef CA_CLI_H
#define CA_CLI_H

#include "ca_config.h"
#include "ca_status.h"

ca_status_t ca_cli_print_help(void);
ca_status_t ca_cli_print_version(void);
ca_status_t ca_cli_run_default(const ca_config_t *config);
ca_status_t ca_cli_run_interactive(const ca_config_t *config);
ca_status_t ca_cli_run_once(const char *prompt, const ca_config_t *config);
ca_status_t ca_cli_run_stdin(const ca_config_t *config);

#endif

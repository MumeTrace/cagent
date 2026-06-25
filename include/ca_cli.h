#ifndef CA_CLI_H
#define CA_CLI_H

#include "ca_status.h"

ca_status_t ca_cli_print_help(void);
ca_status_t ca_cli_print_version(void);
ca_status_t ca_cli_run_default(void);
ca_status_t ca_cli_run_interactive(void);
ca_status_t ca_cli_run_once(const char *prompt);
ca_status_t ca_cli_run_stdin(void);

#endif

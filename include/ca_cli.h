/*
 * ca_cli.h
 * CLI 入口层：根据参数分发到不同模式——交互 REPL、单次 prompt、管道输入。
 * 是 main.c 和 repl.c 之间的公共接口。
 *
 * CLI dispatch layer: routes argc/argv into interactive REPL, one-shot, or stdin mode.
 * Public surface between main.c and the repl implementation.
 */
#ifndef CA_CLI_H
#define CA_CLI_H

#include "ca_config.h"
#include "ca_project.h"
#include "ca_status.h"

ca_status_t ca_cli_print_help(void);
ca_status_t ca_cli_print_version(void);
ca_status_t ca_cli_run_default(const ca_config_t *config, const ca_project_index_t *project);
ca_status_t ca_cli_run_interactive(const ca_config_t *config, const ca_project_index_t *project);
ca_status_t ca_cli_run_once(const char *prompt, const ca_config_t *config, const ca_project_index_t *project);
ca_status_t ca_cli_run_stdin(const ca_config_t *config, const ca_project_index_t *project);

#endif

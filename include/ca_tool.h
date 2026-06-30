#ifndef CA_TOOL_H
#define CA_TOOL_H

#include <stddef.h>
#include <stdio.h>

#include "ca_config.h"
#include "ca_project.h"
#include "ca_status.h"

#define CA_TOOL_NAME_CAP 64
#define CA_TOOL_ARGS_CAP 4096
#define CA_TOOL_REASON_CAP 512
#define CA_TOOL_RESULT_CAP 8192
#define CA_TOOL_ERROR_CODE_CAP 64
#define CA_TOOL_ERROR_MESSAGE_CAP 512

typedef enum ca_tool_permission {
    CA_TOOL_PERMISSION_SAFE = 0,
    CA_TOOL_PERMISSION_ASK,
    CA_TOOL_PERMISSION_DANGEROUS
} ca_tool_permission_t;

typedef struct ca_tool_call {
    char tool_name[CA_TOOL_NAME_CAP];
    char arguments_json[CA_TOOL_ARGS_CAP];
    char reason[CA_TOOL_REASON_CAP];
} ca_tool_call_t;

typedef struct ca_tool_result {
    int success;
    char tool_name[CA_TOOL_NAME_CAP];
    char result_json[CA_TOOL_RESULT_CAP];
    char error_code[CA_TOOL_ERROR_CODE_CAP];
    char error_message[CA_TOOL_ERROR_MESSAGE_CAP];
} ca_tool_result_t;

/*
 * Shared tool execution context. Phase 5A only needs read-only access to the
 * workspace, project index, and config; later phases can extend this with
 * permission/log/session state without changing every tool signature.
 */
typedef struct ca_tool_context {
    const char *workspace_root;
    const ca_project_index_t *project_index;
    const ca_config_t *config;
} ca_tool_context_t;

typedef struct ca_tool_def {
    const char *name;
    const char *description;
    const char *schema_json;
    ca_tool_permission_t permission;
    ca_status_t (*execute)(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx);
} ca_tool_def_t;

typedef struct ca_tool_registry {
    ca_tool_def_t *tools;
    size_t count;
    size_t capacity;
} ca_tool_registry_t;

ca_status_t ca_tool_registry_init(ca_tool_registry_t *registry);
void ca_tool_registry_free(ca_tool_registry_t *registry);
ca_status_t ca_tool_registry_register(ca_tool_registry_t *registry, const ca_tool_def_t *tool);
const ca_tool_def_t *ca_tool_registry_find(const ca_tool_registry_t *registry, const char *name);
void ca_tool_registry_print(const ca_tool_registry_t *registry, FILE *stream);

ca_status_t ca_tool_execute(const ca_tool_registry_t *registry,
                            const ca_tool_call_t *call,
                            ca_tool_result_t *result,
                            void *ctx);

ca_status_t ca_register_builtin_tools(ca_tool_registry_t *registry);
const char *ca_tool_permission_to_string(ca_tool_permission_t permission);

#endif

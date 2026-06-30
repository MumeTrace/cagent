#include "ca_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *ca_tool_permission_to_string(ca_tool_permission_t permission)
{
    switch (permission) {
    case CA_TOOL_PERMISSION_SAFE:
        return "safe";
    case CA_TOOL_PERMISSION_ASK:
        return "ask";
    case CA_TOOL_PERMISSION_DANGEROUS:
        return "dangerous";
    default:
        return "unknown";
    }
}

ca_status_t ca_tool_registry_init(ca_tool_registry_t *registry)
{
    if (registry == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(registry, 0, sizeof(*registry));
    return CA_OK;
}

void ca_tool_registry_free(ca_tool_registry_t *registry)
{
    if (registry == NULL) {
        return;
    }

    /*
     * The registry owns only the dynamic array. Tool names, descriptions, and
     * schema strings are static data owned by the module that registered them.
     */
    free(registry->tools);
    memset(registry, 0, sizeof(*registry));
}

const ca_tool_def_t *ca_tool_registry_find(const ca_tool_registry_t *registry, const char *name)
{
    size_t i;

    if (registry == NULL || name == NULL) {
        return NULL;
    }

    for (i = 0; i < registry->count; i++) {
        if (registry->tools[i].name != NULL && strcmp(registry->tools[i].name, name) == 0) {
            return &registry->tools[i];
        }
    }

    return NULL;
}

ca_status_t ca_tool_registry_register(ca_tool_registry_t *registry, const ca_tool_def_t *tool)
{
    ca_tool_def_t *new_tools;
    size_t new_capacity;

    if (registry == NULL || tool == NULL || tool->name == NULL || tool->execute == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    if (ca_tool_registry_find(registry, tool->name) != NULL) {
        return CA_ERR_INVALID_ARG;
    }

    if (registry->count == registry->capacity) {
        new_capacity = registry->capacity == 0 ? 8 : registry->capacity * 2;
        new_tools = (ca_tool_def_t *)realloc(registry->tools, new_capacity * sizeof(*new_tools));
        if (new_tools == NULL) {
            return CA_ERR_NO_MEMORY;
        }

        registry->tools = new_tools;
        registry->capacity = new_capacity;
    }

    registry->tools[registry->count] = *tool;
    registry->count++;
    return CA_OK;
}

void ca_tool_registry_print(const ca_tool_registry_t *registry, FILE *stream)
{
    size_t i;

    if (stream == NULL) {
        return;
    }

    if (registry == NULL) {
        fprintf(stream, "Tools: <unavailable>\n");
        return;
    }

    fprintf(stream, "Tools: %zu registered\n", registry->count);
    for (i = 0; i < registry->count; i++) {
        const ca_tool_def_t *tool = &registry->tools[i];
        fprintf(stream,
                "- %s [%s] %s\n",
                tool->name != NULL ? tool->name : "<unnamed>",
                ca_tool_permission_to_string(tool->permission),
                tool->description != NULL ? tool->description : "");
    }
}

#include "ca_tool.h"
#include "ca_permission.h"

#include <stdio.h>
#include <string.h>

static ca_status_t ca_tool_copy(char *dest, size_t dest_size, const char *src)
{
    int written;

    if (dest == NULL || dest_size == 0 || src == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    written = snprintf(dest, dest_size, "%s", src);
    if (written < 0 || (size_t)written >= dest_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static void ca_tool_result_reset(ca_tool_result_t *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
}

static ca_status_t ca_tool_result_error(ca_tool_result_t *result,
                                        const char *tool_name,
                                        const char *code,
                                        const char *message)
{
    if (result == NULL || code == NULL || message == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    ca_tool_result_reset(result);
    result->success = 0;
    if (tool_name != NULL) {
        (void)ca_tool_copy(result->tool_name, sizeof(result->tool_name), tool_name);
    }
    (void)ca_tool_copy(result->error_code, sizeof(result->error_code), code);
    (void)ca_tool_copy(result->error_message, sizeof(result->error_message), message);
    return CA_OK;
}

ca_status_t ca_tool_execute(const ca_tool_registry_t *registry,
                            const ca_tool_call_t *call,
                            ca_tool_result_t *result,
                            void *ctx)
{
    const ca_tool_def_t *tool;
    ca_permission_decision_t decision;
    ca_status_t status;

    if (registry == NULL || call == NULL || result == NULL || call->tool_name[0] == '\0') {
        if (result != NULL) {
            (void)ca_tool_result_error(result, NULL, "INVALID_TOOL_CALL", "Tool call is missing a tool name.");
        }
        return CA_ERR_INVALID_ARG;
    }

    ca_tool_result_reset(result);
    (void)ca_tool_copy(result->tool_name, sizeof(result->tool_name), call->tool_name);

    tool = ca_tool_registry_find(registry, call->tool_name);
    if (tool == NULL) {
        (void)ca_tool_result_error(result, call->tool_name, "TOOL_NOT_FOUND", "Tool is not registered.");
        return CA_ERR_TOOL_NOT_FOUND;
    }

    /*
     * Permission is centralized in the executor so tools cannot accidentally
     * bypass it, and future Agent Loop calls follow the same path as /tool-test.
     */
    status = ca_permission_check_tool(tool, call, &decision);
    if (status != CA_OK) {
        (void)ca_tool_result_error(result, call->tool_name, "PERMISSION_CHECK_FAILED", "Permission check failed.");
        return status;
    }
    if (decision != CA_PERMISSION_DECISION_ALLOW) {
        (void)ca_tool_result_error(result, call->tool_name, "PERMISSION_DENIED", "User denied the operation.");
        return CA_ERR_PERMISSION_DENIED;
    }

    status = tool->execute(call, result, ctx);
    if (result->tool_name[0] == '\0') {
        (void)ca_tool_copy(result->tool_name, sizeof(result->tool_name), call->tool_name);
    }

    return status;
}

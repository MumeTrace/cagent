#include "ca_permission.h"

ca_status_t ca_permission_check_tool(const ca_tool_def_t *tool,
                                     const ca_tool_call_t *call,
                                     ca_permission_decision_t *decision)
{
    if (tool == NULL || call == NULL || decision == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *decision = CA_PERMISSION_DECISION_DENY;

    switch (tool->permission) {
    case CA_TOOL_PERMISSION_SAFE:
        *decision = CA_PERMISSION_DECISION_ALLOW;
        return CA_OK;
    case CA_TOOL_PERMISSION_ASK:
        return ca_permission_confirm_ask(tool, call, decision);
    case CA_TOOL_PERMISSION_DANGEROUS:
        return ca_permission_confirm_dangerous(tool, call, decision);
    default:
        return CA_ERR_PERMISSION_DENIED;
    }
}

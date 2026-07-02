/*
 * ca_permission.h
 * Central permission gate for tool execution. Tools declare their risk level;
 * the executor asks this module before running anything non-SAFE.
 */
#ifndef CA_PERMISSION_H
#define CA_PERMISSION_H

#include "ca_status.h"
#include "ca_tool.h"

typedef enum ca_permission_decision {
    CA_PERMISSION_DECISION_ALLOW = 0,
    CA_PERMISSION_DECISION_DENY
} ca_permission_decision_t;

ca_status_t ca_permission_check_tool(const ca_tool_def_t *tool,
                                     const ca_tool_call_t *call,
                                     ca_permission_decision_t *decision);

ca_status_t ca_permission_confirm_ask(const ca_tool_def_t *tool,
                                      const ca_tool_call_t *call,
                                      ca_permission_decision_t *decision);
ca_status_t ca_permission_confirm_dangerous(const ca_tool_def_t *tool,
                                            const ca_tool_call_t *call,
                                            ca_permission_decision_t *decision);

#endif

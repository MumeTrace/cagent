/*
 * ca_command_tools.h
 * Registration surface for guarded terminal tools.
 */
#ifndef CA_COMMAND_TOOLS_H
#define CA_COMMAND_TOOLS_H

#include "ca_status.h"
#include "ca_tool.h"

ca_status_t ca_register_command_tools(ca_tool_registry_t *registry);

#endif

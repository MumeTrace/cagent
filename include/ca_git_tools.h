/*
 * ca_git_tools.h
 * Registration surface for read-only Git tools.
 */
#ifndef CA_GIT_TOOLS_H
#define CA_GIT_TOOLS_H

#include "ca_status.h"
#include "ca_tool.h"

ca_status_t ca_register_git_tools(ca_tool_registry_t *registry);

#endif

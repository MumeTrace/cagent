/*
 * ca_file_tools.h
 * Registration surface for file tools.
 */
#ifndef CA_FILE_TOOLS_H
#define CA_FILE_TOOLS_H

#include "ca_status.h"
#include "ca_tool.h"

ca_status_t ca_register_file_tools(ca_tool_registry_t *registry);
ca_status_t ca_register_write_file_tools(ca_tool_registry_t *registry);
ca_status_t ca_register_diff_tools(ca_tool_registry_t *registry);
ca_status_t ca_register_edit_tools(ca_tool_registry_t *registry);
ca_status_t ca_register_patch_tools(ca_tool_registry_t *registry);

#endif

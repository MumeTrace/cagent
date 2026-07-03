/*
 * ca_agent.h
 * Minimal Agent Loop interface for one user turn.
 */
#ifndef CA_AGENT_H
#define CA_AGENT_H

#include "ca_config.h"
#include "ca_llm.h"
#include "ca_project.h"
#include "ca_status.h"
#include "ca_tool.h"

#define CA_AGENT_CONTENT_CAP 8192
#define CA_AGENT_REASON_CAP 1024
#define CA_AGENT_ERROR_CAP 512
#define CA_AGENT_PROMPT_CAP 32768
#define CA_AGENT_OBSERVATION_CAP 16384

typedef enum ca_agent_action_type {
    CA_AGENT_ACTION_INVALID = 0,
    CA_AGENT_ACTION_TOOL_CALL,
    CA_AGENT_ACTION_FINAL_ANSWER,
    CA_AGENT_ACTION_ASK_USER,
    CA_AGENT_ACTION_PLAN
} ca_agent_action_type_t;

typedef struct ca_agent_action {
    ca_agent_action_type_t type;
    char tool_name[CA_TOOL_NAME_CAP];
    char arguments_json[CA_TOOL_ARGS_CAP];
    char content[CA_AGENT_CONTENT_CAP];
    char reason[CA_AGENT_REASON_CAP];
    char error_message[CA_AGENT_ERROR_CAP];
} ca_agent_action_t;

typedef struct ca_agent {
    const ca_config_t *config;
    const ca_project_index_t *project;
    const ca_tool_registry_t *tools;
    ca_llm_provider_t *llm;
    int max_steps;
} ca_agent_t;

ca_status_t ca_agent_init(ca_agent_t *agent,
                          const ca_config_t *config,
                          const ca_project_index_t *project,
                          const ca_tool_registry_t *tools,
                          ca_llm_provider_t *llm);
ca_status_t ca_agent_run_turn(ca_agent_t *agent, const char *user_input);

ca_status_t ca_agent_parse_action(const char *raw_text, ca_agent_action_t *action);
ca_status_t ca_agent_build_prompt(const ca_agent_t *agent,
                                  const char *user_input,
                                  const char *observation,
                                  int step_index,
                                  char *out_prompt,
                                  size_t out_size);

#endif

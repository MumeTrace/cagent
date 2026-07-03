#include "ca_agent.h"
#include "ca_json.h"

#include <stdio.h>
#include <string.h>

static ca_status_t ca_agent_copy_text(char *dest, size_t dest_size, const char *src)
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

static ca_status_t ca_agent_append_observation(char *observation,
                                               size_t observation_size,
                                               const ca_tool_result_t *result)
{
    int written;

    if (observation == NULL || observation_size == 0 || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    /*
     * Observations are passed back to the model as Tool Protocol JSON. Session
     * memory and richer compression are later phases, so this keeps one compact
     * tool_result in the prompt.
     */
    if (result->success) {
        written = snprintf(observation,
                           observation_size,
                           "{\"type\":\"tool_result\",\"tool\":\"%s\",\"success\":true,\"result\":%s}",
                           result->tool_name,
                           result->result_json[0] != '\0' ? result->result_json : "{}");
    } else {
        char escaped_message[CA_TOOL_ERROR_MESSAGE_CAP * 2];
        char escaped_code[CA_TOOL_ERROR_CODE_CAP * 2];

        if (ca_json_escape_string(result->error_code, escaped_code, sizeof(escaped_code)) != CA_OK ||
            ca_json_escape_string(result->error_message, escaped_message, sizeof(escaped_message)) != CA_OK) {
            return CA_ERR_INVALID_ARG;
        }

        written = snprintf(observation,
                           observation_size,
                           "{\"type\":\"tool_result\",\"tool\":\"%s\",\"success\":false,"
                           "\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                           result->tool_name,
                           escaped_code,
                           escaped_message);
    }

    if (written < 0 || (size_t)written >= observation_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

ca_status_t ca_agent_init(ca_agent_t *agent,
                          const ca_config_t *config,
                          const ca_project_index_t *project,
                          const ca_tool_registry_t *tools,
                          ca_llm_provider_t *llm)
{
    if (agent == NULL || config == NULL || project == NULL || tools == NULL || llm == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(agent, 0, sizeof(*agent));
    agent->config = config;
    agent->project = project;
    agent->tools = tools;
    agent->llm = llm;
    agent->max_steps = config->agent_max_steps > 0 ? config->agent_max_steps : 12;
    return CA_OK;
}

ca_status_t ca_agent_run_turn(ca_agent_t *agent, const char *user_input)
{
    char prompt[CA_AGENT_PROMPT_CAP];
    char observation[CA_AGENT_OBSERVATION_CAP];
    int step;

    if (agent == NULL || agent->llm == NULL || agent->llm->complete == NULL ||
        user_input == NULL || user_input[0] == '\0') {
        return CA_ERR_INVALID_ARG;
    }

    observation[0] = '\0';
    for (step = 0; step < agent->max_steps; step++) {
        ca_llm_request_t request;
        ca_llm_response_t response;
        ca_agent_action_t action;
        ca_status_t status;

        printf("[agent] step %d\n", step + 1);
        status = ca_agent_build_prompt(agent, user_input, observation, step, prompt, sizeof(prompt));
        if (status != CA_OK) {
            fprintf(stderr, "[agent] failed to build prompt\n");
            return status;
        }

        request.prompt = prompt;
        request.step_index = step;
        memset(&response, 0, sizeof(response));
        status = agent->llm->complete(agent->llm, &request, &response);
        if (status != CA_OK) {
            fprintf(stderr, "[agent] llm provider failed\n");
            return status;
        }

        status = ca_agent_parse_action(response.raw_text, &action);
        if (status != CA_OK) {
            fprintf(stderr,
                    "[agent] action parse error: %s\n",
                    action.error_message[0] != '\0' ? action.error_message : "invalid action");
            return status;
        }

        if (action.type == CA_AGENT_ACTION_TOOL_CALL) {
            ca_tool_call_t call;
            ca_tool_result_t result;
            ca_tool_context_t ctx;

            printf("[agent] action: tool_call %s\n", action.tool_name);
            memset(&call, 0, sizeof(call));
            memset(&result, 0, sizeof(result));
            memset(&ctx, 0, sizeof(ctx));

            status = ca_agent_copy_text(call.tool_name, sizeof(call.tool_name), action.tool_name);
            if (status != CA_OK) {
                return status;
            }
            status = ca_agent_copy_text(call.arguments_json, sizeof(call.arguments_json), action.arguments_json);
            if (status != CA_OK) {
                return status;
            }
            (void)ca_agent_copy_text(call.reason,
                                     sizeof(call.reason),
                                     action.reason[0] != '\0' ? action.reason : "agent tool_call");

            ctx.workspace_root = agent->project->workspace_root;
            ctx.project_index = agent->project;
            ctx.config = agent->config;

            status = ca_tool_execute(agent->tools, &call, &result, &ctx);
            printf("[agent] tool_result: %s\n", result.success ? "success" : "failed");
            if (!result.success) {
                printf("[agent] tool_error: %s %s\n", result.error_code, result.error_message);
            }
            if (ca_agent_append_observation(observation, sizeof(observation), &result) != CA_OK) {
                fprintf(stderr, "[agent] observation too large\n");
                return CA_ERR_INVALID_ARG;
            }
            (void)status;
            continue;
        }

        if (action.type == CA_AGENT_ACTION_FINAL_ANSWER) {
            printf("[agent] action: final_answer\n");
            printf("%s\n", action.content);
            return CA_OK;
        }

        if (action.type == CA_AGENT_ACTION_ASK_USER) {
            printf("[agent] action: ask_user\n");
            printf("%s\n", action.content[0] != '\0' ? action.content : "<question unavailable>");
            return CA_OK;
        }

        if (action.type == CA_AGENT_ACTION_PLAN) {
            printf("[agent] action: plan\n");
            printf("%s\n", action.content);
            return CA_OK;
        }
    }

    fprintf(stderr, "Agent stopped: max steps exceeded.\n");
    return CA_ERR_TOOL_FAILED;
}

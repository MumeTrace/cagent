#include "ca_agent.h"
#include "ca_json.h"

#include <stdio.h>
#include <string.h>

static ca_status_t ca_agent_copy_range(char *dest, size_t dest_size, const char *start, size_t len)
{
    if (dest == NULL || dest_size == 0 || start == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    if (len >= dest_size) {
        return CA_ERR_INVALID_ARG;
    }
    memcpy(dest, start, len);
    dest[len] = '\0';
    return CA_OK;
}

ca_status_t ca_agent_parse_action(const char *raw_text, ca_agent_action_t *action)
{
    char type[64];
    const char *args_start;
    size_t args_len;
    ca_status_t status;

    if (raw_text == NULL || action == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(action, 0, sizeof(*action));
    action->type = CA_AGENT_ACTION_INVALID;

    status = ca_json_get_string(raw_text, "type", type, sizeof(type));
    if (status != CA_OK) {
        (void)snprintf(action->error_message, sizeof(action->error_message), "Missing or invalid action type.");
        return status == CA_ERR_NOT_FOUND ? CA_ERR_JSON : status;
    }

    if (strcmp(type, "tool_call") == 0) {
        action->type = CA_AGENT_ACTION_TOOL_CALL;
        status = ca_json_get_string(raw_text, "tool", action->tool_name, sizeof(action->tool_name));
        if (status != CA_OK || action->tool_name[0] == '\0') {
            (void)snprintf(action->error_message, sizeof(action->error_message), "tool_call is missing tool.");
            return CA_ERR_JSON;
        }
        status = ca_json_find_object_range(raw_text, strlen(raw_text), "arguments", &args_start, &args_len);
        if (status == CA_ERR_NOT_FOUND) {
            (void)snprintf(action->arguments_json, sizeof(action->arguments_json), "{}");
        } else if (status == CA_OK) {
            status = ca_agent_copy_range(action->arguments_json, sizeof(action->arguments_json), args_start, args_len);
            if (status != CA_OK) {
                (void)snprintf(action->error_message, sizeof(action->error_message), "tool_call arguments are too large.");
                return status;
            }
        } else {
            (void)snprintf(action->error_message, sizeof(action->error_message), "tool_call arguments must be an object.");
            return status;
        }
        (void)ca_json_get_string(raw_text, "reason", action->reason, sizeof(action->reason));
        return CA_OK;
    }

    if (strcmp(type, "final_answer") == 0) {
        action->type = CA_AGENT_ACTION_FINAL_ANSWER;
        status = ca_json_get_string(raw_text, "content", action->content, sizeof(action->content));
        if (status != CA_OK) {
            (void)snprintf(action->error_message, sizeof(action->error_message), "final_answer is missing content.");
            return CA_ERR_JSON;
        }
        return CA_OK;
    }

    if (strcmp(type, "ask_user") == 0) {
        action->type = CA_AGENT_ACTION_ASK_USER;
        (void)ca_json_get_string(raw_text, "question", action->content, sizeof(action->content));
        return CA_OK;
    }

    if (strcmp(type, "plan") == 0) {
        action->type = CA_AGENT_ACTION_PLAN;
        (void)snprintf(action->content, sizeof(action->content), "%s", raw_text);
        return CA_OK;
    }

    (void)snprintf(action->error_message, sizeof(action->error_message), "Unknown action type: %s", type);
    return CA_ERR_JSON;
}

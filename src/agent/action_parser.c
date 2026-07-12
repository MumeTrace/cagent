#include "ca_agent.h"
#include "ca_json.h"

#include <stdio.h>
#include <string.h>

static const char *ca_agent_skip_ws(const char *text)
{
    while (text != NULL && (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')) {
        text++;
    }
    return text;
}

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

static ca_status_t ca_agent_find_balanced_json_object(const char *text,
                                                      const char **out_start,
                                                      size_t *out_len)
{
    const char *cursor;

    if (text == NULL || out_start == NULL || out_len == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *out_start = NULL;
    *out_len = 0;
    for (cursor = text; *cursor != '\0'; cursor++) {
        const char *start;
        int depth = 0;
        int in_string = 0;
        int escaped = 0;

        if (*cursor != '{') {
            continue;
        }

        start = cursor;
        for (; *cursor != '\0'; cursor++) {
            char ch = *cursor;

            if (in_string) {
                if (escaped) {
                    escaped = 0;
                } else if (ch == '\\') {
                    escaped = 1;
                } else if (ch == '"') {
                    in_string = 0;
                }
                continue;
            }

            if (ch == '"') {
                in_string = 1;
            } else if (ch == '{') {
                depth++;
            } else if (ch == '}') {
                depth--;
                if (depth == 0) {
                    *out_start = start;
                    *out_len = (size_t)(cursor - start + 1);
                    return CA_OK;
                }
            }
        }

        return CA_ERR_JSON;
    }

    return CA_ERR_NOT_FOUND;
}

static ca_status_t ca_agent_extract_fenced_block(const char *raw_text, char *out, size_t out_size)
{
    const char *fence_start;
    const char *content_start;
    const char *fence_end;

    if (raw_text == NULL || out == NULL || out_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    /*
     * Real LLMs often wrap action JSON in ```json or plain ``` fences despite
     * the system prompt. Keep markdown tolerance here so tools still receive
     * strict JSON after parsing succeeds.
     */
    fence_start = strstr(raw_text, "```json");
    if (fence_start != NULL) {
        content_start = fence_start + strlen("```json");
    } else {
        fence_start = strstr(raw_text, "```");
        if (fence_start == NULL) {
            return CA_ERR_NOT_FOUND;
        }
        content_start = fence_start + strlen("```");
    }

    if (*content_start == '\r' && content_start[1] == '\n') {
        content_start += 2;
    } else if (*content_start == '\n' || *content_start == '\r') {
        content_start++;
    }

    fence_end = strstr(content_start, "```");
    if (fence_end == NULL) {
        return CA_ERR_JSON;
    }

    return ca_agent_copy_range(out, out_size, content_start, (size_t)(fence_end - content_start));
}

static ca_status_t ca_agent_extract_action_json(const char *raw_text,
                                                char *out_json,
                                                size_t out_json_size,
                                                char *error_message,
                                                size_t error_size)
{
    char fenced[CA_LLM_RESPONSE_CAP];
    const char *source;
    const char *json_start;
    size_t json_len;
    ca_status_t status;

    if (raw_text == NULL || out_json == NULL || out_json_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    out_json[0] = '\0';
    status = ca_agent_extract_fenced_block(raw_text, fenced, sizeof(fenced));
    if (status == CA_OK) {
        source = fenced;
    } else if (status == CA_ERR_NOT_FOUND) {
        source = raw_text;
    } else {
        if (error_message != NULL && error_size > 0) {
            (void)snprintf(error_message, error_size, "Malformed markdown code fence around JSON action.");
        }
        return status;
    }

    source = ca_agent_skip_ws(source);
    status = ca_agent_find_balanced_json_object(source, &json_start, &json_len);
    if (status != CA_OK) {
        if (error_message != NULL && error_size > 0) {
            (void)snprintf(error_message, error_size, "No complete JSON object action found in model output.");
        }
        return status == CA_ERR_NOT_FOUND ? CA_ERR_JSON : status;
    }

    if (json_len > CA_MAX_JSON_EXTRACT_SIZE) {
        if (error_message != NULL && error_size > 0) {
            (void)snprintf(error_message, error_size, "ACTION_TOO_LARGE: extracted JSON action exceeds %u bytes.",
                           (unsigned int)CA_MAX_JSON_EXTRACT_SIZE);
        }
        return CA_ERR_INVALID_ARG;
    }

    status = ca_agent_copy_range(out_json, out_json_size, json_start, json_len);
    if (status != CA_OK && error_message != NULL && error_size > 0) {
        (void)snprintf(error_message, error_size, "ACTION_TOO_LARGE: extracted JSON action exceeds parser buffer.");
    }
    return status;
}

ca_status_t ca_agent_parse_action(const char *raw_text, ca_agent_action_t *action)
{
    char action_json[CA_LLM_RESPONSE_CAP];
    char type[64];
    const char *args_start;
    size_t args_len;
    ca_status_t status;

    if (raw_text == NULL || action == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(action, 0, sizeof(*action));
    action->type = CA_AGENT_ACTION_INVALID;

    status = ca_agent_extract_action_json(raw_text,
                                          action_json,
                                          sizeof(action_json),
                                          action->error_message,
                                          sizeof(action->error_message));
    if (status != CA_OK) {
        return status;
    }

    status = ca_json_get_string(action_json, "type", type, sizeof(type));
    if (status != CA_OK) {
        (void)snprintf(action->error_message, sizeof(action->error_message), "Missing or invalid action type.");
        return status == CA_ERR_NOT_FOUND ? CA_ERR_JSON : status;
    }

    if (strcmp(type, "tool_call") == 0) {
        action->type = CA_AGENT_ACTION_TOOL_CALL;
        status = ca_json_get_string(action_json, "tool", action->tool_name, sizeof(action->tool_name));
        if (status != CA_OK || action->tool_name[0] == '\0') {
            (void)snprintf(action->error_message, sizeof(action->error_message), "tool_call is missing tool.");
            return CA_ERR_JSON;
        }
        status = ca_json_find_object_range(action_json, strlen(action_json), "arguments", &args_start, &args_len);
        if (status == CA_ERR_NOT_FOUND) {
            (void)snprintf(action->arguments_json, sizeof(action->arguments_json), "{}");
        } else if (status == CA_OK) {
            if (args_len > CA_MAX_TOOL_ARGUMENTS_TOTAL) {
                (void)snprintf(action->error_message,
                               sizeof(action->error_message),
                               "TOOL_ARGUMENTS_TOO_LARGE: tool_call arguments exceed %u bytes.",
                               (unsigned int)CA_MAX_TOOL_ARGUMENTS_TOTAL);
                return CA_ERR_INVALID_ARG;
            }
            status = ca_agent_copy_range(action->arguments_json, sizeof(action->arguments_json), args_start, args_len);
            if (status != CA_OK) {
                (void)snprintf(action->error_message, sizeof(action->error_message), "TOOL_ARGUMENTS_TOO_LARGE: tool_call arguments exceed runtime buffer.");
                return status;
            }
        } else {
            (void)snprintf(action->error_message, sizeof(action->error_message), "tool_call arguments must be an object.");
            return status;
        }
        (void)ca_json_get_string(action_json, "reason", action->reason, sizeof(action->reason));
        return CA_OK;
    }

    if (strcmp(type, "final_answer") == 0) {
        action->type = CA_AGENT_ACTION_FINAL_ANSWER;
        status = ca_json_get_string(action_json, "content", action->content, sizeof(action->content));
        if (status != CA_OK) {
            (void)snprintf(action->error_message, sizeof(action->error_message), "final_answer is missing content.");
            return CA_ERR_JSON;
        }
        return CA_OK;
    }

    if (strcmp(type, "ask_user") == 0) {
        action->type = CA_AGENT_ACTION_ASK_USER;
        status = ca_json_get_string(action_json, "content", action->content, sizeof(action->content));
        if (status == CA_ERR_NOT_FOUND) {
            (void)ca_json_get_string(action_json, "question", action->content, sizeof(action->content));
        }
        return CA_OK;
    }

    if (strcmp(type, "plan") == 0) {
        action->type = CA_AGENT_ACTION_PLAN;
        (void)snprintf(action->content, sizeof(action->content), "%s", action_json);
        return CA_OK;
    }

    (void)snprintf(action->error_message, sizeof(action->error_message), "Unknown action type: %s", type);
    return CA_ERR_JSON;
}

#include "ca_permission.h"

#include <stdio.h>
#include <string.h>

#define CA_PERMISSION_INPUT_CAP 64

static void ca_permission_trim_newline(char *text)
{
    size_t len;

    if (text == NULL) {
        return;
    }

    len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        len--;
    }
}

static void ca_permission_print_summary(const ca_tool_def_t *tool, const ca_tool_call_t *call)
{
    printf("\nPermission required:\n\n");
    printf("Tool: %s\n", tool != NULL && tool->name != NULL ? tool->name : "<unknown>");
    printf("Reason: %s\n", call != NULL && call->reason[0] != '\0' ? call->reason : "<not provided>");
    printf("Arguments: %s\n", call != NULL && call->arguments_json[0] != '\0' ? call->arguments_json : "{}");
}

ca_status_t ca_permission_confirm_ask(const ca_tool_def_t *tool,
                                      const ca_tool_call_t *call,
                                      ca_permission_decision_t *decision)
{
    char input[CA_PERMISSION_INPUT_CAP];

    if (tool == NULL || call == NULL || decision == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    /*
     * Default deny matters here: empty input, EOF, and interrupted input all
     * leave the decision as DENY instead of accidentally allowing an action.
     */
    *decision = CA_PERMISSION_DECISION_DENY;
    ca_permission_print_summary(tool, call);
    printf("Risk level: ASK\n\n");
    printf("Allow this operation? [y/N] ");
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL) {
        printf("\n");
        return CA_OK;
    }

    ca_permission_trim_newline(input);
    if (strcmp(input, "y") == 0 || strcmp(input, "Y") == 0) {
        *decision = CA_PERMISSION_DECISION_ALLOW;
    }

    return CA_OK;
}

ca_status_t ca_permission_confirm_dangerous(const ca_tool_def_t *tool,
                                            const ca_tool_call_t *call,
                                            ca_permission_decision_t *decision)
{
    char input[CA_PERMISSION_INPUT_CAP];

    if (tool == NULL || call == NULL || decision == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    /*
     * Dangerous tools require a full phrase. This avoids muscle-memory "y"
     * approvals for operations that may delete data or run destructive commands.
     */
    *decision = CA_PERMISSION_DECISION_DENY;
    ca_permission_print_summary(tool, call);
    printf("Risk level: DANGEROUS\n\n");
    printf("Type YES to allow this dangerous operation: ");
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL) {
        printf("\n");
        return CA_OK;
    }

    ca_permission_trim_newline(input);
    if (strcmp(input, "YES") == 0) {
        *decision = CA_PERMISSION_DECISION_ALLOW;
    }

    return CA_OK;
}

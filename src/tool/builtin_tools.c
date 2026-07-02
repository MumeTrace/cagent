#include "ca_tool.h"
#include "ca_json.h"

#include <stdio.h>
#include <string.h>

static ca_status_t ca_builtin_copy(char *dest, size_t dest_size, const char *src)
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

static ca_status_t ca_builtin_write_echo_json(char *dest, size_t dest_size, const char *text)
{
    char escaped[CA_TOOL_RESULT_CAP];
    int written;

    if (dest == NULL || dest_size == 0 || text == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    if (ca_json_escape_string(text, escaped, sizeof(escaped)) != CA_OK) {
        return CA_ERR_INVALID_ARG;
    }

    written = snprintf(dest, dest_size, "{\"echo\":\"%s\"}", escaped);
    if (written < 0 || (size_t)written >= dest_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static ca_status_t ca_builtin_noop_execute(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx)
{
    (void)call;
    (void)ctx;

    if (result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->success = 1;
    (void)ca_builtin_copy(result->tool_name, sizeof(result->tool_name), "noop");
    return ca_builtin_copy(result->result_json, sizeof(result->result_json), "{\"ok\":true}");
}

static ca_status_t ca_builtin_echo_execute(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx)
{
    (void)ctx;

    if (call == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->success = 1;
    (void)ca_builtin_copy(result->tool_name, sizeof(result->tool_name), "echo");
    return ca_builtin_write_echo_json(result->result_json, sizeof(result->result_json), call->arguments_json);
}

static ca_status_t ca_builtin_ask_test_execute(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx)
{
    (void)call;
    (void)ctx;

    if (result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->success = 1;
    (void)ca_builtin_copy(result->tool_name, sizeof(result->tool_name), "ask_test");
    return ca_builtin_copy(result->result_json, sizeof(result->result_json), "{\"ok\":true,\"permission\":\"ask\"}");
}

static ca_status_t ca_builtin_danger_test_execute(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx)
{
    (void)call;
    (void)ctx;

    if (result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->success = 1;
    (void)ca_builtin_copy(result->tool_name, sizeof(result->tool_name), "danger_test");
    return ca_builtin_copy(result->result_json, sizeof(result->result_json), "{\"ok\":true,\"permission\":\"dangerous\"}");
}

ca_status_t ca_register_builtin_tools(ca_tool_registry_t *registry)
{
    /* noop / 空操作测试工具：验证注册和 executor 基础路径。 */
    static const ca_tool_def_t noop_tool = {
        "noop",
        "No-op test tool with no side effects.",
        "{\"type\":\"object\",\"properties\":{}}",
        CA_TOOL_PERMISSION_SAFE,
        ca_builtin_noop_execute
    };
    /* echo / 回显测试工具：验证 arguments_json 到 result_json 的传递。 */
    static const ca_tool_def_t echo_tool = {
        "echo",
        "Echoes arguments_json for executor testing.",
        "{\"type\":\"object\",\"additionalProperties\":true}",
        CA_TOOL_PERMISSION_SAFE,
        ca_builtin_echo_execute
    };
    /* ask_test / ASK 权限测试工具：无副作用，只验证 y/N 确认路径。 */
    static const ca_tool_def_t ask_test_tool = {
        "ask_test",
        "Permission test tool that requires y/N confirmation and has no side effects.",
        "{\"type\":\"object\",\"properties\":{}}",
        CA_TOOL_PERMISSION_ASK,
        ca_builtin_ask_test_execute
    };
    /* danger_test / DANGEROUS 权限测试工具：无副作用，只验证 YES 强确认路径。 */
    static const ca_tool_def_t danger_test_tool = {
        "danger_test",
        "Permission test tool that requires YES confirmation and has no side effects.",
        "{\"type\":\"object\",\"properties\":{}}",
        CA_TOOL_PERMISSION_DANGEROUS,
        ca_builtin_danger_test_execute
    };
    ca_status_t status;

    if (registry == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_tool_registry_register(registry, &noop_tool);
    if (status != CA_OK) {
        return status;
    }

    status = ca_tool_registry_register(registry, &echo_tool);
    if (status != CA_OK) {
        return status;
    }

    status = ca_tool_registry_register(registry, &ask_test_tool);
    if (status != CA_OK) {
        return status;
    }

    return ca_tool_registry_register(registry, &danger_test_tool);
}

/*
 * ca_status.h
 * 整个项目的错误码枚举，所有函数都用它来统一表达成功 / 失败原因。
 * Uniform error code enum — every API in cagent returns one of these.
 */
#ifndef CA_STATUS_H
#define CA_STATUS_H

typedef enum ca_status {
    CA_OK = 0,
    CA_ERR_INVALID_ARG,
    CA_ERR_NO_MEMORY,
    CA_ERR_IO,
    CA_ERR_JSON,
    CA_ERR_LLM,
    CA_ERR_PERMISSION_DENIED,
    CA_ERR_TOOL_NOT_FOUND,
    CA_ERR_TOOL_FAILED,
    CA_ERR_NOT_FOUND,
    CA_ERR_TYPE_MISMATCH
} ca_status_t;

#endif

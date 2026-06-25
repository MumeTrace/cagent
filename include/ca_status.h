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
    CA_ERR_TOOL_FAILED
} ca_status_t;

#endif

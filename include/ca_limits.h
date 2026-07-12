/*
 * ca_limits.h
 * Central resource limits for payload-heavy paths. Desktop builds keep larger
 * caps, while embedded builds can turn off heap-backed payloads and inherit
 * smaller totals without touching tool code.
 */
#ifndef CA_LIMITS_H
#define CA_LIMITS_H

#ifndef CAGENT_ENABLE_LARGE_PAYLOAD
#define CAGENT_ENABLE_LARGE_PAYLOAD 1
#endif

#if CAGENT_ENABLE_LARGE_PAYLOAD
#define CA_MAX_TOOL_ARGUMENTS_INLINE  (16u * 1024u)
#define CA_MAX_TOOL_RESULT_INLINE     (32u * 1024u)
#define CA_MAX_TOOL_ARGUMENTS_TOTAL   (128u * 1024u)
#define CA_MAX_TOOL_RESULT_TOTAL      (128u * 1024u)
#define CA_MAX_PROCESS_OUTPUT         (64u * 1024u)
#define CA_MAX_DIFF_OUTPUT            (64u * 1024u)
#define CA_MAX_PATCH_SIZE             (128u * 1024u)
#define CA_MAX_FILE_READ_SIZE         (64u * 1024u)
#define CA_MAX_JSON_EXTRACT_SIZE      (128u * 1024u)
#else
#define CA_MAX_TOOL_ARGUMENTS_INLINE  (2u * 1024u)
#define CA_MAX_TOOL_RESULT_INLINE     (4u * 1024u)
#define CA_MAX_TOOL_ARGUMENTS_TOTAL   CA_MAX_TOOL_ARGUMENTS_INLINE
#define CA_MAX_TOOL_RESULT_TOTAL      CA_MAX_TOOL_RESULT_INLINE
#define CA_MAX_PROCESS_OUTPUT         (8u * 1024u)
#define CA_MAX_DIFF_OUTPUT            (8u * 1024u)
#define CA_MAX_PATCH_SIZE             (8u * 1024u)
#define CA_MAX_FILE_READ_SIZE         (8u * 1024u)
#define CA_MAX_JSON_EXTRACT_SIZE      (8u * 1024u)
#endif

#define CA_MAX_PAYLOAD_INLINE CA_MAX_TOOL_RESULT_INLINE

#endif

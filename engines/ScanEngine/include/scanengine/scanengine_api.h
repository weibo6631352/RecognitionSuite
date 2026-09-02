#ifndef SCANENGINE_API_H
#define SCANENGINE_API_H

#include "scanengine_version.h"

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifdef SCANENGINE_CORE_EXPORTS
#define SE_API __declspec(dllexport)
#else
#define SE_API __declspec(dllimport)
#endif
#else
#define SE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct se_context se_context;

typedef enum se_status {
    SE_OK = 0,
    SE_ERR_ARGUMENT = 1,
    SE_ERR_RUNTIME = 2,
    SE_ERR_MODEL = 3,
    SE_ERR_PARSE = 4,
    SE_ERR_CANCELLED = 5,
    SE_ERR_INTERNAL = 6,
    SE_ERR_BUSY = 7,
    SE_ERR_NOT_FOUND = 8
} se_status;

typedef enum se_task_state {
    SE_TASK_QUEUED = 0,
    SE_TASK_RUNNING = 1,
    SE_TASK_DONE = 2,
    SE_TASK_FAILED = 3,
    SE_TASK_CANCELLED = 4
} se_task_state;

typedef enum se_effort {
    SE_EFFORT_DEFAULT = 0,
    SE_EFFORT_LOW = 1,
    SE_EFFORT_MEDIUM = 2,
    SE_EFFORT_HIGH = 3
} se_effort;

typedef struct se_api_version {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} se_api_version;

typedef struct se_context_params {
    uint32_t struct_size;
    uint32_t api_version;
    const wchar_t* runtime_root_utf16;
} se_context_params;

typedef struct se_parse_params {
    uint32_t struct_size;
    uint32_t api_version;
    const wchar_t* input_path_utf16;
    const wchar_t* output_root_utf16;
    se_effort effort;
} se_parse_params;

typedef struct se_runtime_info {
    uint32_t struct_size;
    uint32_t api_version;
    int32_t gpu_available;
    char backend_utf8[64];
    char error_utf8[1024];
} se_runtime_info;

typedef struct se_task_status {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t task_id;
    se_task_state state;
    float progress;
    se_status error;
    char message_utf8[1024];
} se_task_status;

typedef struct se_result {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t task_id;
    se_status status;
    char* message_utf8;
    wchar_t* output_dir_utf16;
    wchar_t* excel_path_utf16;
    wchar_t* markdown_path_utf16;
    wchar_t* json_path_utf16;
    int32_t fallback_count;
} se_result;

SE_API void scanengine_api_version(se_api_version* out);
SE_API se_status scanengine_context_create(const se_context_params* params,
                                         se_context** out_context);
SE_API void scanengine_context_destroy(se_context* context);
SE_API se_status scanengine_runtime_check(se_context* context,
                                        se_runtime_info* out);
/* One context is single-flight and retains only its most recent task. */
SE_API se_status scanengine_submit_file(se_context* context,
                                      const se_parse_params* params,
                                      uint64_t* out_task_id);
SE_API se_status scanengine_task_poll(se_context* context, uint64_t task_id,
                                    se_task_status* out);
SE_API se_status scanengine_task_cancel(se_context* context, uint64_t task_id);
SE_API se_status scanengine_task_result(se_context* context, uint64_t task_id,
                                      se_result* out);
/* Release only after a task reaches DONE, FAILED, or CANCELLED. */
SE_API se_status scanengine_task_release(se_context* context, uint64_t task_id);
SE_API const char* scanengine_last_error(se_context* context);
SE_API void scanengine_free(void* memory);
SE_API void scanengine_result_free(se_result* result);

#ifdef __cplusplus
}
#endif

#endif

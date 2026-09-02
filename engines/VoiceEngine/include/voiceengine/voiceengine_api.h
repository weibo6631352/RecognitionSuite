#ifndef VOICEENGINE_API_H
#define VOICEENGINE_API_H

#include "voiceengine_version.h"

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifdef VOICEENGINE_CORE_EXPORTS
#define VE_API __declspec(dllexport)
#else
#define VE_API __declspec(dllimport)
#endif
#else
#define VE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ve_context ve_context;
typedef struct ve_stream ve_stream;

typedef enum ve_status {
    VE_OK = 0,
    VE_ERR_ARGUMENT = 1,
    VE_ERR_AUDIO = 2,
    VE_ERR_MODEL = 3,
    VE_ERR_CUDA = 4,
    VE_ERR_CANCELLED = 5,
    VE_ERR_INTERNAL = 6,
    VE_ERR_NOT_FOUND = 7,
    VE_ERR_BUSY = 8,
    VE_ERR_STATE = 9
} ve_status;

typedef enum ve_task_state {
    VE_TASK_QUEUED = 0,
    VE_TASK_RUNNING = 1,
    VE_TASK_DONE = 2,
    VE_TASK_FAILED = 3,
    VE_TASK_CANCELLED = 4
} ve_task_state;

typedef struct ve_api_version {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} ve_api_version;

typedef struct ve_context_params {
    uint32_t struct_size;
    uint32_t api_version;
    const wchar_t* runtime_dir_utf16;
} ve_context_params;

typedef struct ve_cuda_info {
    uint32_t struct_size;
    uint32_t api_version;
    int32_t available;
    int32_t device_index;
    int32_t compute_major;
    int32_t compute_minor;
    int32_t vram_mib;
    char name[256];
    char error_utf8[512];
} ve_cuda_info;

typedef struct ve_model_params {
    uint32_t struct_size;
    uint32_t api_version;
    const wchar_t* model_dir_utf16;
} ve_model_params;

typedef struct ve_pcm_desc {
    uint32_t struct_size;
    uint32_t api_version;
    const float* samples;
    uint64_t frame_count;
    int32_t sample_rate;
    int32_t channels;
} ve_pcm_desc;

typedef struct ve_task_status {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t task_id;
    ve_task_state state;
    float progress;
    ve_status error;
    char interim_utf8[4096];
} ve_task_status;

typedef struct ve_result {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t task_id;
    ve_status status;
    char* text_utf8;
    char* json_utf8;
    double duration_sec;
} ve_result;

typedef enum ve_stream_event_type {
    VE_STREAM_EVENT_NONE = 0,
    VE_STREAM_EVENT_PARTIAL = 1,
    VE_STREAM_EVENT_FINAL = 2,
    VE_STREAM_EVENT_ERROR = 3,
    VE_STREAM_EVENT_END = 4
} ve_stream_event_type;

typedef struct ve_stream_params {
    uint32_t struct_size;
    uint32_t api_version;
    int32_t sample_rate;
    int32_t channels;
    /* Recognition refresh interval; it is not an utterance boundary. */
    uint32_t chunk_ms;
    /* ABI v1 compatibility field; cumulative streaming does not split/overlap chunks. */
    uint32_t overlap_ms;
    uint32_t endpoint_silence_ms;
    uint32_t pre_roll_ms;
    float vad_rms_threshold;
} ve_stream_params;

typedef struct ve_stream_event {
    uint32_t struct_size;
    uint32_t api_version;
    ve_stream_event_type type;
    uint64_t sequence;
    uint64_t segment_id;
    int64_t begin_ms;
    int64_t end_ms;
    int32_t is_stable;
    ve_status error;
    uint32_t text_bytes;
    int32_t text_truncated;
    /* PARTIAL replaces the previous text for its segment; FINAL is appendable. */
    char text_utf8[4096];
} ve_stream_event;

VE_API void voiceengine_api_version(ve_api_version* out);
VE_API ve_status voiceengine_context_create(const ve_context_params* params,
                                           ve_context** out_context);
VE_API void voiceengine_context_destroy(ve_context* context);
VE_API ve_status voiceengine_cuda_check(ve_context* context, ve_cuda_info* out);
VE_API ve_status voiceengine_model_load(ve_context* context,
                                       const ve_model_params* params);
VE_API ve_status voiceengine_model_unload(ve_context* context);
VE_API int32_t voiceengine_model_loaded(ve_context* context);
VE_API ve_status voiceengine_submit_file(ve_context* context,
                                        const wchar_t* path_utf16,
                                        uint64_t* out_task_id);
VE_API ve_status voiceengine_submit_pcm(ve_context* context,
                                       const ve_pcm_desc* pcm,
                                       uint64_t* out_task_id);
VE_API ve_status voiceengine_task_poll(ve_context* context, uint64_t task_id,
                                     ve_task_status* out);
VE_API ve_status voiceengine_task_result(ve_context* context, uint64_t task_id,
                                       ve_result* out);
VE_API ve_status voiceengine_task_cancel(ve_context* context, uint64_t task_id);
/* Release only after a task reaches DONE, FAILED, or CANCELLED. */
VE_API ve_status voiceengine_task_release(ve_context* context, uint64_t task_id);
VE_API ve_status voiceengine_stream_create(ve_context* context,
                                          const ve_stream_params* params,
                                          ve_stream** out_stream);
VE_API ve_status voiceengine_stream_push_f32(ve_stream* stream,
                                            const float* interleaved_samples,
                                            uint64_t frame_count);
VE_API ve_status voiceengine_stream_poll(ve_stream* stream,
                                        ve_stream_event* out_event);
VE_API ve_status voiceengine_stream_flush(ve_stream* stream);
VE_API ve_status voiceengine_stream_finish(ve_stream* stream);
VE_API ve_status voiceengine_stream_cancel(ve_stream* stream);
/* Destroy streams before their owning context; destroy is not concurrent. */
VE_API void voiceengine_stream_destroy(ve_stream* stream);
VE_API const char* voiceengine_last_error(ve_context* context);
VE_API void voiceengine_free(void* p);
VE_API void voiceengine_result_free(ve_result* result);

#ifdef __cplusplus
}
#endif

#endif

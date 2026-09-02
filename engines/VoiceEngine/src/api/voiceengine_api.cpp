#include "voiceengine/voiceengine_api.h"
#include "core/internal.hpp"

#include <cstdlib>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace {

#define VE_V1_END(type, field) \
    (offsetof(type, field) + sizeof(((type*)nullptr)->field))

template <typename T>
bool valid_input(const T* value, size_t minimum) {
    return value
        && value->struct_size >= minimum
        && (value->api_version == 0
            || value->api_version == VOICEENGINE_API_VERSION);
}

template <typename T>
bool write_output(T* destination, const T& source, size_t minimum) {
    if (!destination)
        return false;
    size_t capacity = destination->struct_size;
    if (capacity == 0)
        capacity = sizeof(T);
    if (capacity < minimum)
        return false;
    std::memcpy(destination, &source, (std::min)(capacity, sizeof(T)));
    return true;
}

void fill_version(ve_api_version* out) {
    if (!out) {
        return;
    }
    out->struct_size = sizeof(*out);
    out->api_version = VOICEENGINE_API_VERSION;
    out->major = VOICEENGINE_VERSION_MAJOR;
    out->minor = VOICEENGINE_VERSION_MINOR;
    out->patch = VOICEENGINE_VERSION_PATCH;
}

}  // namespace

void voiceengine_api_version(ve_api_version* out) {
    if (!out)
        return;
    ve_api_version version{};
    fill_version(&version);
    write_output(out, version, VE_V1_END(ve_api_version, patch));
}

ve_status voiceengine_context_create(const ve_context_params* params,
                                    ve_context** out_context) {
    if (!out_context) {
        return VE_ERR_ARGUMENT;
    }
    *out_context = nullptr;
    if (params && !valid_input(
            params, VE_V1_END(ve_context_params, runtime_dir_utf16))) {
        return VE_ERR_ARGUMENT;
    }
    try {
        const wchar_t* runtime = params ? params->runtime_dir_utf16 : nullptr;
        *out_context = reinterpret_cast<ve_context*>(
            voiceengine_engine_create(false, runtime));
        return *out_context ? VE_OK : VE_ERR_INTERNAL;
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

void voiceengine_context_destroy(ve_context* context) {
    voiceengine_engine_destroy(context);
}

ve_status voiceengine_cuda_check(ve_context* context, ve_cuda_info* out) {
    auto* eng = voiceengine::engine_from_handle(context);
    if (!eng || !out || out->struct_size < VE_V1_END(ve_cuda_info, error_utf8)) {
        return VE_ERR_ARGUMENT;
    }
    try {
        ve_cuda_info info{};
        info.struct_size = sizeof(info);
        info.api_version = VOICEENGINE_API_VERSION;
        const ve_status status = eng->cuda_check(&info);
        return write_output(out, info, VE_V1_END(ve_cuda_info, error_utf8))
            ? status : VE_ERR_ARGUMENT;
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

ve_status voiceengine_model_load(ve_context* context,
                                const ve_model_params* params) {
    auto* eng = voiceengine::engine_from_handle(context);
    if (!eng || (params && !valid_input(
            params, VE_V1_END(ve_model_params, model_dir_utf16)))) {
        return VE_ERR_ARGUMENT;
    }
    try {
        return eng->load(params);
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

ve_status voiceengine_model_unload(ve_context* context) {
    auto* eng = voiceengine::engine_from_handle(context);
    if (!eng) {
        return VE_ERR_ARGUMENT;
    }
    try {
        return eng->unload();
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

int32_t voiceengine_model_loaded(ve_context* context) {
    auto* eng = voiceengine::engine_from_handle(context);
    return eng ? eng->loaded() : 0;
}

ve_status voiceengine_submit_file(ve_context* context,
                                 const wchar_t* path_utf16,
                                 uint64_t* out_task_id) {
    auto* eng = voiceengine::engine_from_handle(context);
    if (!eng) {
        return VE_ERR_ARGUMENT;
    }
    try {
        return eng->submit_file(path_utf16, out_task_id);
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

ve_status voiceengine_submit_pcm(ve_context* context,
                                const ve_pcm_desc* pcm,
                                uint64_t* out_task_id) {
    auto* eng = voiceengine::engine_from_handle(context);
    if (!eng || !valid_input(pcm, VE_V1_END(ve_pcm_desc, channels))) {
        return VE_ERR_ARGUMENT;
    }
    try {
        return eng->submit_pcm(pcm, out_task_id);
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

ve_status voiceengine_task_poll(ve_context* context, uint64_t task_id,
                               ve_task_status* out) {
    auto* eng = voiceengine::engine_from_handle(context);
    if (!eng || !out
        || out->struct_size < VE_V1_END(ve_task_status, interim_utf8)) {
        return VE_ERR_ARGUMENT;
    }
    try {
        ve_task_status status{};
        status.struct_size = sizeof(status);
        status.api_version = VOICEENGINE_API_VERSION;
        const ve_status result = eng->poll(task_id, &status);
        if (result != VE_OK)
            return result;
        return write_output(out, status,
                            VE_V1_END(ve_task_status, interim_utf8))
            ? VE_OK : VE_ERR_ARGUMENT;
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

ve_status voiceengine_task_result(ve_context* context, uint64_t task_id,
                                 ve_result* out) {
    auto* eng = voiceengine::engine_from_handle(context);
    if (!eng || !out || out->struct_size < VE_V1_END(ve_result, duration_sec)) {
        return VE_ERR_ARGUMENT;
    }
    try {
        ve_result result{};
        result.struct_size = sizeof(result);
        result.api_version = VOICEENGINE_API_VERSION;
        const ve_status status = eng->result(task_id, &result);
        if (!write_output(out, result, VE_V1_END(ve_result, duration_sec))) {
            voiceengine_result_free(&result);
            return VE_ERR_ARGUMENT;
        }
        return status;
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

ve_status voiceengine_task_cancel(ve_context* context, uint64_t task_id) {
    auto* eng = voiceengine::engine_from_handle(context);
    if (!eng) {
        return VE_ERR_ARGUMENT;
    }
    try {
        return eng->cancel(task_id);
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

ve_status voiceengine_task_release(ve_context* context, uint64_t task_id) {
    auto* eng = voiceengine::engine_from_handle(context);
    if (!eng)
        return VE_ERR_ARGUMENT;
    try {
        return eng->release(task_id);
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

const char* voiceengine_last_error(ve_context* context) {
    auto* eng = voiceengine::engine_from_handle(context);
    if (!eng)
        return "null context";
    try {
        return eng->last_error();
    } catch (...) {
        return "failed to copy last error";
    }
}

void voiceengine_free(void* p) {
    std::free(p);
}

void voiceengine_result_free(ve_result* result) {
    if (!result) {
        return;
    }
    voiceengine_free(result->text_utf8);
    voiceengine_free(result->json_utf8);
    result->text_utf8 = nullptr;
    result->json_utf8 = nullptr;
}

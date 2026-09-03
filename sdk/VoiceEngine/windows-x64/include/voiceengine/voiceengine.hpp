#ifndef VOICEENGINE_HPP
#define VOICEENGINE_HPP

#include "voiceengine_api.h"
#include "voiceengine_runtime.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace voiceengine {

class Error final : public std::runtime_error {
public:
    Error(ve_status status, std::string message)
        : std::runtime_error(std::move(message)), status_(status) {}

    ve_status status() const noexcept { return status_; }

private:
    ve_status status_;
};

namespace detail {

inline std::string error_message(ve_context* context, const char* operation) {
    const char* message = context ? voiceengine_last_error(context) : nullptr;
    if (message && *message)
        return std::string(operation) + ": " + message;
    return std::string(operation) + " failed";
}

inline void check(ve_status status, ve_context* context,
                  const char* operation) {
    if (status != VE_OK)
        throw Error(status, error_message(context, operation));
}

struct ContextState final {
    explicit ContextState(ve_context* value) noexcept : value(value) {}
    ~ContextState() { voiceengine_context_destroy(value); }

    ContextState(const ContextState&) = delete;
    ContextState& operator=(const ContextState&) = delete;

    ve_context* value = nullptr;
};

}  // namespace detail

struct CudaInfo {
    bool available = false;
    int device_index = -1;
    int compute_major = 0;
    int compute_minor = 0;
    int vram_mib = 0;
    std::string name;
};

struct TaskStatus {
    std::uint64_t id = 0;
    ve_task_state state = VE_TASK_QUEUED;
    float progress = 0.0f;
    ve_status error = VE_OK;
    std::string interim_text;

    bool finished() const noexcept {
        return state == VE_TASK_DONE || state == VE_TASK_FAILED
            || state == VE_TASK_CANCELLED;
    }
};

struct Result {
    std::uint64_t task_id = 0;
    ve_status status = VE_OK;
    std::string text;
    std::string json;
    double duration_sec = 0.0;
};

struct StreamParams {
    int sample_rate = 16000;
    int channels = 1;
    // Recognition refresh interval, not a text or utterance boundary.
    std::uint32_t chunk_ms = 2000;
    // Retained for ABI v1 compatibility; cumulative streaming does not use it.
    std::uint32_t overlap_ms = 200;
    std::uint32_t endpoint_silence_ms = 800;
    std::uint32_t pre_roll_ms = 200;
    float vad_rms_threshold = 0.004f;
};

struct StreamEvent {
    ve_stream_event_type type = VE_STREAM_EVENT_NONE;
    std::uint64_t sequence = 0;
    std::uint64_t segment_id = 0;
    std::int64_t begin_ms = 0;
    std::int64_t end_ms = 0;
    bool stable = false;
    ve_status error = VE_OK;
    std::string text;
    std::uint32_t text_bytes = 0;
    bool text_truncated = false;
};

class Task final {
public:
    Task() noexcept = default;
    ~Task() { reset(); }

    Task(Task&& other) noexcept
        : context_(std::move(other.context_)), id_(other.id_) {
        other.id_ = 0;
    }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            reset();
            context_ = std::move(other.context_);
            id_ = other.id_;
            other.id_ = 0;
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    explicit operator bool() const noexcept {
        return context_ && id_ != 0;
    }

    std::uint64_t id() const noexcept { return id_; }

    TaskStatus poll() const {
        require_valid();
        ve_task_status raw{};
        raw.struct_size = sizeof(raw);
        raw.api_version = VOICEENGINE_API_VERSION;
        detail::check(voiceengine_task_poll(context_->value, id_, &raw),
                      context_->value, "voiceengine_task_poll");
        return {raw.task_id, raw.state, raw.progress, raw.error,
                raw.interim_utf8};
    }

    void cancel() const {
        require_valid();
        detail::check(voiceengine_task_cancel(context_->value, id_),
                      context_->value, "voiceengine_task_cancel");
    }

    Result result() {
        require_valid();
        ve_result raw{};
        raw.struct_size = sizeof(raw);
        raw.api_version = VOICEENGINE_API_VERSION;
        const ve_status status =
            voiceengine_task_result(context_->value, id_, &raw);
        struct ResultGuard {
            ve_result* value;
            ~ResultGuard() { voiceengine_result_free(value); }
        } guard{&raw};
        detail::check(status, context_->value, "voiceengine_task_result");

        Result value;
        value.task_id = raw.task_id;
        value.status = raw.status;
        value.text = raw.text_utf8 ? raw.text_utf8 : "";
        value.json = raw.json_utf8 ? raw.json_utf8 : "";
        value.duration_sec = raw.duration_sec;
        release();
        return value;
    }

    Result wait(std::chrono::milliseconds poll_interval =
                    std::chrono::milliseconds(50)) {
        for (;;) {
            const TaskStatus status = poll();
            if (status.finished())
                return result();
            std::this_thread::sleep_for(poll_interval);
        }
    }

    void release() {
        require_valid();
        detail::check(voiceengine_task_release(context_->value, id_),
                      context_->value, "voiceengine_task_release");
        id_ = 0;
        context_.reset();
    }

private:
    friend class Context;

    Task(std::shared_ptr<detail::ContextState> context,
         std::uint64_t id) noexcept
        : context_(std::move(context)), id_(id) {}

    void require_valid() const {
        if (!*this)
            throw Error(VE_ERR_STATE, "VoiceEngine task is empty");
    }

    void reset() noexcept {
        if (!*this)
            return;
        ve_task_status status{};
        status.struct_size = sizeof(status);
        status.api_version = VOICEENGINE_API_VERSION;
        if (voiceengine_task_poll(context_->value, id_, &status) == VE_OK
            && (status.state == VE_TASK_DONE
                || status.state == VE_TASK_FAILED
                || status.state == VE_TASK_CANCELLED)) {
            voiceengine_task_release(context_->value, id_);
        } else {
            voiceengine_task_cancel(context_->value, id_);
        }
        id_ = 0;
        context_.reset();
    }

    std::shared_ptr<detail::ContextState> context_;
    std::uint64_t id_ = 0;
};

class Stream final {
public:
    Stream() noexcept = default;
    ~Stream() { reset(); }

    Stream(Stream&& other) noexcept
        : context_(std::move(other.context_)), value_(other.value_),
          channels_(other.channels_) {
        other.value_ = nullptr;
        other.channels_ = 0;
    }

    Stream& operator=(Stream&& other) noexcept {
        if (this != &other) {
            reset();
            context_ = std::move(other.context_);
            value_ = other.value_;
            channels_ = other.channels_;
            other.value_ = nullptr;
            other.channels_ = 0;
        }
        return *this;
    }

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    explicit operator bool() const noexcept { return value_ != nullptr; }

    void push(const float* interleaved_samples, std::uint64_t frame_count) {
        require_valid();
        detail::check(voiceengine_stream_push_f32(
                          value_, interleaved_samples, frame_count),
                      context_->value, "voiceengine_stream_push_f32");
    }

    void push(const std::vector<float>& interleaved_samples) {
        require_valid();
        if (channels_ <= 0
            || interleaved_samples.size() % static_cast<std::size_t>(channels_)
                != 0) {
            throw Error(VE_ERR_ARGUMENT,
                        "PCM sample count is not divisible by channel count");
        }
        push(interleaved_samples.data(),
             interleaved_samples.size() /
                 static_cast<std::size_t>(channels_));
    }

    std::optional<StreamEvent> poll() {
        require_valid();
        ve_stream_event raw{};
        raw.struct_size = sizeof(raw);
        raw.api_version = VOICEENGINE_API_VERSION;
        const ve_status status = voiceengine_stream_poll(value_, &raw);
        if (status == VE_ERR_BUSY)
            return std::nullopt;
        detail::check(status, context_->value, "voiceengine_stream_poll");
        return StreamEvent{raw.type,
                           raw.sequence,
                           raw.segment_id,
                           raw.begin_ms,
                           raw.end_ms,
                           raw.is_stable != 0,
                           raw.error,
                           raw.text_utf8,
                           raw.text_bytes,
                           raw.text_truncated != 0};
    }

    void flush() {
        require_valid();
        detail::check(voiceengine_stream_flush(value_), context_->value,
                      "voiceengine_stream_flush");
    }

    void finish() {
        require_valid();
        detail::check(voiceengine_stream_finish(value_), context_->value,
                      "voiceengine_stream_finish");
    }

    void cancel() {
        require_valid();
        detail::check(voiceengine_stream_cancel(value_), context_->value,
                      "voiceengine_stream_cancel");
    }

    void reset() noexcept {
        if (value_)
            voiceengine_stream_destroy(value_);
        value_ = nullptr;
        channels_ = 0;
        context_.reset();
    }

private:
    friend class Context;

    Stream(std::shared_ptr<detail::ContextState> context,
           ve_stream* value, int channels) noexcept
        : context_(std::move(context)), value_(value), channels_(channels) {}

    void require_valid() const {
        if (!value_)
            throw Error(VE_ERR_STATE, "VoiceEngine stream is empty");
    }

    std::shared_ptr<detail::ContextState> context_;
    ve_stream* value_ = nullptr;
    int channels_ = 0;
};

class Context final {
public:
    Context() noexcept = default;

    static Context open(const std::wstring& runtime_root) {
        wchar_t error[1024]{};
        if (!voiceengine_runtime_initialize(runtime_root.c_str(), error, 1024))
            throw Error(VE_ERR_INTERNAL,
                        "voiceengine_runtime_initialize failed");

        ve_context_params params{};
        params.struct_size = sizeof(params);
        params.api_version = VOICEENGINE_API_VERSION;
        params.runtime_dir_utf16 = runtime_root.c_str();
        ve_context* value = nullptr;
        detail::check(voiceengine_context_create(&params, &value), nullptr,
                      "voiceengine_context_create");
        try {
            return Context(std::make_shared<detail::ContextState>(value));
        } catch (...) {
            voiceengine_context_destroy(value);
            throw;
        }
    }

    explicit operator bool() const noexcept {
        return context_ && context_->value;
    }

    CudaInfo cuda_info() const {
        require_valid();
        ve_cuda_info raw{};
        raw.struct_size = sizeof(raw);
        raw.api_version = VOICEENGINE_API_VERSION;
        detail::check(voiceengine_cuda_check(context_->value, &raw),
                      context_->value, "voiceengine_cuda_check");
        return {raw.available != 0, raw.device_index, raw.compute_major,
                raw.compute_minor, raw.vram_mib, raw.name};
    }

    void load_model(const std::wstring& model_dir = {}) const {
        require_valid();
        ve_model_params params{};
        params.struct_size = sizeof(params);
        params.api_version = VOICEENGINE_API_VERSION;
        params.model_dir_utf16 = model_dir.empty() ? nullptr : model_dir.c_str();
        detail::check(voiceengine_model_load(context_->value, &params),
                      context_->value, "voiceengine_model_load");
    }

    void unload_model() const {
        require_valid();
        detail::check(voiceengine_model_unload(context_->value), context_->value,
                      "voiceengine_model_unload");
    }

    bool model_loaded() const {
        require_valid();
        return voiceengine_model_loaded(context_->value) != 0;
    }

    Task submit_file(const std::wstring& path) const {
        require_valid();
        std::uint64_t id = 0;
        detail::check(voiceengine_submit_file(context_->value, path.c_str(), &id),
                      context_->value, "voiceengine_submit_file");
        return Task(context_, id);
    }

    Task submit_pcm(const float* interleaved_samples,
                    std::uint64_t frame_count, int sample_rate,
                    int channels) const {
        require_valid();
        ve_pcm_desc pcm{};
        pcm.struct_size = sizeof(pcm);
        pcm.api_version = VOICEENGINE_API_VERSION;
        pcm.samples = interleaved_samples;
        pcm.frame_count = frame_count;
        pcm.sample_rate = sample_rate;
        pcm.channels = channels;
        std::uint64_t id = 0;
        detail::check(voiceengine_submit_pcm(context_->value, &pcm, &id),
                      context_->value, "voiceengine_submit_pcm");
        return Task(context_, id);
    }

    Stream create_stream(const StreamParams& params = {}) const {
        require_valid();
        ve_stream_params raw{};
        raw.struct_size = sizeof(raw);
        raw.api_version = VOICEENGINE_API_VERSION;
        raw.sample_rate = params.sample_rate;
        raw.channels = params.channels;
        raw.chunk_ms = params.chunk_ms;
        raw.overlap_ms = params.overlap_ms;
        raw.endpoint_silence_ms = params.endpoint_silence_ms;
        raw.pre_roll_ms = params.pre_roll_ms;
        raw.vad_rms_threshold = params.vad_rms_threshold;
        ve_stream* value = nullptr;
        detail::check(voiceengine_stream_create(context_->value, &raw, &value),
                      context_->value, "voiceengine_stream_create");
        return Stream(context_, value, params.channels);
    }

private:
    explicit Context(std::shared_ptr<detail::ContextState> context) noexcept
        : context_(std::move(context)) {}

    void require_valid() const {
        if (!*this)
            throw Error(VE_ERR_STATE, "VoiceEngine context is empty");
    }

    std::shared_ptr<detail::ContextState> context_;
};

}  // namespace voiceengine

#endif

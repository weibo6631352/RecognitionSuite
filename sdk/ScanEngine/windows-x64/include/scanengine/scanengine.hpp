#ifndef SCANENGINE_HPP
#define SCANENGINE_HPP

#include "scanengine_api.h"
#include "scanengine_runtime.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace scanengine {

class Error final : public std::runtime_error {
public:
    Error(se_status status, std::string message)
        : std::runtime_error(std::move(message)), status_(status) {}

    se_status status() const noexcept { return status_; }

private:
    se_status status_;
};

namespace detail {

inline std::string error_message(se_context* context, const char* operation) {
    const char* message = context ? scanengine_last_error(context) : nullptr;
    if (message && *message)
        return std::string(operation) + ": " + message;
    return std::string(operation) + " failed";
}

inline void check(se_status status, se_context* context,
                  const char* operation) {
    if (status != SE_OK)
        throw Error(status, error_message(context, operation));
}

struct ContextState final {
    explicit ContextState(se_context* value) noexcept : value(value) {}
    ~ContextState() { scanengine_context_destroy(value); }

    ContextState(const ContextState&) = delete;
    ContextState& operator=(const ContextState&) = delete;

    se_context* value = nullptr;
};

}  // namespace detail

struct RuntimeInfo {
    bool gpu_available = false;
    std::string backend;
};

struct TaskStatus {
    std::uint64_t id = 0;
    se_task_state state = SE_TASK_QUEUED;
    float progress = 0.0f;
    se_status error = SE_OK;
    std::string message;

    bool finished() const noexcept {
        return state == SE_TASK_DONE || state == SE_TASK_FAILED
            || state == SE_TASK_CANCELLED;
    }
};

struct Result {
    std::uint64_t task_id = 0;
    se_status status = SE_OK;
    std::string message;
    std::wstring output_dir;
    std::wstring excel_path;
    std::wstring markdown_path;
    std::wstring json_path;
    int fallback_count = 0;
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
        se_task_status raw{};
        raw.struct_size = sizeof(raw);
        raw.api_version = SCANENGINE_API_VERSION;
        detail::check(scanengine_task_poll(context_->value, id_, &raw),
                      context_->value, "scanengine_task_poll");
        return {raw.task_id, raw.state, raw.progress, raw.error,
                raw.message_utf8};
    }

    void cancel() const {
        require_valid();
        detail::check(scanengine_task_cancel(context_->value, id_),
                      context_->value, "scanengine_task_cancel");
    }

    Result result() {
        require_valid();
        se_result raw{};
        raw.struct_size = sizeof(raw);
        raw.api_version = SCANENGINE_API_VERSION;
        const se_status status =
            scanengine_task_result(context_->value, id_, &raw);
        struct ResultGuard {
            se_result* value;
            ~ResultGuard() { scanengine_result_free(value); }
        } guard{&raw};
        detail::check(status, context_->value, "scanengine_task_result");

        Result value;
        value.task_id = raw.task_id;
        value.status = raw.status;
        value.message = raw.message_utf8 ? raw.message_utf8 : "";
        value.output_dir = raw.output_dir_utf16 ? raw.output_dir_utf16 : L"";
        value.excel_path = raw.excel_path_utf16 ? raw.excel_path_utf16 : L"";
        value.markdown_path = raw.markdown_path_utf16
            ? raw.markdown_path_utf16 : L"";
        value.json_path = raw.json_path_utf16 ? raw.json_path_utf16 : L"";
        value.fallback_count = raw.fallback_count;
        release();
        return value;
    }

    Result wait(std::chrono::milliseconds poll_interval =
                    std::chrono::milliseconds(100)) {
        for (;;) {
            const TaskStatus status = poll();
            if (status.finished())
                return result();
            std::this_thread::sleep_for(poll_interval);
        }
    }

    void release() {
        require_valid();
        detail::check(scanengine_task_release(context_->value, id_),
                      context_->value, "scanengine_task_release");
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
            throw Error(SE_ERR_NOT_FOUND, "ScanEngine task is empty");
    }

    void reset() noexcept {
        if (!*this)
            return;
        se_task_status status{};
        status.struct_size = sizeof(status);
        status.api_version = SCANENGINE_API_VERSION;
        if (scanengine_task_poll(context_->value, id_, &status) == SE_OK
            && (status.state == SE_TASK_DONE
                || status.state == SE_TASK_FAILED
                || status.state == SE_TASK_CANCELLED)) {
            scanengine_task_release(context_->value, id_);
        } else {
            scanengine_task_cancel(context_->value, id_);
        }
        id_ = 0;
        context_.reset();
    }

    std::shared_ptr<detail::ContextState> context_;
    std::uint64_t id_ = 0;
};

class Context final {
public:
    Context() noexcept = default;

    static Context open(const std::wstring& runtime_root) {
        wchar_t error[1024]{};
        if (!scanengine_runtime_initialize(runtime_root.c_str(), error, 1024))
            throw Error(SE_ERR_RUNTIME,
                        "scanengine_runtime_initialize failed");

        se_context_params params{};
        params.struct_size = sizeof(params);
        params.api_version = SCANENGINE_API_VERSION;
        params.runtime_root_utf16 = runtime_root.c_str();
        se_context* value = nullptr;
        detail::check(scanengine_context_create(&params, &value), nullptr,
                      "scanengine_context_create");
        try {
            return Context(std::make_shared<detail::ContextState>(value));
        } catch (...) {
            scanengine_context_destroy(value);
            throw;
        }
    }

    explicit operator bool() const noexcept {
        return context_ && context_->value;
    }

    RuntimeInfo runtime_info() const {
        require_valid();
        se_runtime_info raw{};
        raw.struct_size = sizeof(raw);
        raw.api_version = SCANENGINE_API_VERSION;
        detail::check(scanengine_runtime_check(context_->value, &raw),
                      context_->value, "scanengine_runtime_check");
        return {raw.gpu_available != 0, raw.backend_utf8};
    }

    Task submit_file(const std::wstring& input_path,
                     const std::wstring& output_root,
                     se_effort effort = SE_EFFORT_DEFAULT) const {
        require_valid();
        se_parse_params params{};
        params.struct_size = sizeof(params);
        params.api_version = SCANENGINE_API_VERSION;
        params.input_path_utf16 = input_path.c_str();
        params.output_root_utf16 = output_root.c_str();
        params.effort = effort;
        std::uint64_t id = 0;
        detail::check(scanengine_submit_file(context_->value, &params, &id),
                      context_->value, "scanengine_submit_file");
        return Task(context_, id);
    }

private:
    explicit Context(std::shared_ptr<detail::ContextState> context) noexcept
        : context_(std::move(context)) {}

    void require_valid() const {
        if (!*this)
            throw Error(SE_ERR_ARGUMENT, "ScanEngine context is empty");
    }

    std::shared_ptr<detail::ContextState> context_;
};

}  // namespace scanengine

#endif

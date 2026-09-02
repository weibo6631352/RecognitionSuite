#include "scanengine/scanengine_api.h"

#include "scanengine/config.hpp"
#include "scanengine/hybrid/device_policy.hpp"
#include "scanengine/parser_service.hpp"

#include <QCoreApplication>
#include <QByteArray>
#include <QDir>
#include <QString>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

#define SE_V1_END(type, field) \
    (offsetof(type, field) + sizeof(((type*)nullptr)->field))

template <typename T>
bool validInput(const T* value, size_t minimum) {
    return value
        && value->struct_size >= minimum
        && (value->api_version == 0
            || value->api_version == SCANENGINE_API_VERSION);
}

template <typename T>
bool writeOutput(T* destination, const T& source, size_t minimum) {
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

struct SdkContext {
    std::mutex mutex;
    std::thread worker;
    scanengine::ParserService* activeService = nullptr;
    uint64_t nextTaskId = 1;
    uint64_t taskId = 0;
    se_task_state state = SE_TASK_DONE;
    se_status status = SE_OK;
    float progress = 0.0f;
    std::string message;
    std::string lastError;
    scanengine::ParseResult result;
    bool cancelRequested = false;
};

std::mutex gAppMutex;
std::unique_ptr<QCoreApplication> gOwnedApplication;
std::mutex gRuntimeRootMutex;
QString gRuntimeRoot;

bool ensureQtApplication(std::string* error) {
    std::lock_guard<std::mutex> lock(gAppMutex);
    if (QCoreApplication::instance())
        return true;
    static int argc = 1;
    static char name[] = "ScanEngineSdk";
    static char* argv[] = {name, nullptr};
    try {
        // A library must not force a Qt platform plugin into an unrelated
        // host process. Core parsing supports QCoreApplication; if the host
        // already owns a QGuiApplication, the optional raw-font path remains
        // available automatically.
        gOwnedApplication.reset(new QCoreApplication(argc, argv));
        return true;
    } catch (...) {
        if (error)
            *error = "failed to initialize the Qt core runtime";
        return false;
    }
}

void copyUtf8(char* destination, size_t capacity, const std::string& value) {
    if (!destination || capacity == 0)
        return;
    const size_t count = (std::min)(capacity - 1, value.size());
    std::memcpy(destination, value.data(), count);
    destination[count] = '\0';
}

char* duplicateUtf8(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    char* result = static_cast<char*>(std::malloc(size_t(bytes.size()) + 1));
    if (!result)
        return nullptr;
    std::memcpy(result, bytes.constData(), size_t(bytes.size()));
    result[bytes.size()] = '\0';
    return result;
}

wchar_t* duplicateWide(const QString& value) {
    const std::wstring wide = value.toStdWString();
    wchar_t* result = static_cast<wchar_t*>(
        std::malloc((wide.size() + 1) * sizeof(wchar_t)));
    if (!result)
        return nullptr;
    std::memcpy(result, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
    return result;
}

float progressFor(scanengine::TaskStatus status) {
    switch (status) {
    case scanengine::TaskStatus::Prepare: return 0.05f;
    case scanengine::TaskStatus::Process: return 0.25f;
    case scanengine::TaskStatus::Outputs: return 0.90f;
    case scanengine::TaskStatus::Done: return 1.0f;
    case scanengine::TaskStatus::Cancelled: return 1.0f;
    case scanengine::TaskStatus::Failed: return 1.0f;
    default: return 0.0f;
    }
}

SdkContext* fromHandle(se_context* handle) {
    return reinterpret_cast<SdkContext*>(handle);
}

bool validTask(SdkContext* context, uint64_t taskId) {
    return context && taskId != 0 && context->taskId == taskId;
}

QString effortName(se_effort effort) {
    switch (effort) {
    case SE_EFFORT_LOW: return QStringLiteral("low");
    case SE_EFFORT_HIGH: return QStringLiteral("high");
    case SE_EFFORT_DEFAULT:
    case SE_EFFORT_MEDIUM: return QStringLiteral("medium");
    default: return {};
    }
}

void runParseTask(SdkContext* context,
                  uint64_t taskId,
                  scanengine::ParseOptions options) noexcept {
    try {
        scanengine::ParserService service;
        bool cancelledBeforeStart = false;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->activeService = &service;
            cancelledBeforeStart = context->cancelRequested;
            if (!cancelledBeforeStart) {
                context->state = SE_TASK_RUNNING;
                context->progress = 0.01f;
                context->message = "running";
            }
        }
        if (cancelledBeforeStart) {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->activeService = nullptr;
            context->state = SE_TASK_CANCELLED;
            context->status = SE_ERR_CANCELLED;
            context->progress = 1.0f;
            context->message = "cancelled";
            context->result.status = scanengine::TaskStatus::Cancelled;
            context->result.message = QString::fromUtf8("任务已取消");
            return;
        }
        const scanengine::ParseResult result = service.run(
            options, {}, [context](scanengine::TaskStatus state,
                                   const QString& message) {
                std::lock_guard<std::mutex> lock(context->mutex);
                context->progress = progressFor(state);
                context->message = message.toUtf8().constData();
            });
        std::lock_guard<std::mutex> lock(context->mutex);
        context->activeService = nullptr;
        context->result = result;
        context->progress = 1.0f;
        context->message = result.message.toUtf8().constData();
        if (result.status == scanengine::TaskStatus::Cancelled) {
            context->state = SE_TASK_CANCELLED;
            context->status = SE_ERR_CANCELLED;
        } else if (result.success) {
            context->state = SE_TASK_DONE;
            context->status = SE_OK;
        } else {
            context->state = SE_TASK_FAILED;
            context->status = SE_ERR_PARSE;
            context->lastError = context->message;
        }
        context->taskId = taskId;
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->activeService = nullptr;
        context->state = SE_TASK_FAILED;
        context->status = SE_ERR_INTERNAL;
        context->progress = 1.0f;
        context->message = error.what();
        context->lastError = context->message;
    } catch (...) {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->activeService = nullptr;
        context->state = SE_TASK_FAILED;
        context->status = SE_ERR_INTERNAL;
        context->progress = 1.0f;
        context->message = "unhandled parser exception";
        context->lastError = context->message;
    }
}

}  // namespace

void scanengine_api_version(se_api_version* out) {
    if (!out)
        return;
    se_api_version version{};
    version.struct_size = sizeof(version);
    version.api_version = SCANENGINE_API_VERSION;
    version.major = SCANENGINE_VERSION_MAJOR;
    version.minor = SCANENGINE_VERSION_MINOR;
    version.patch = SCANENGINE_VERSION_PATCH;
    writeOutput(out, version, SE_V1_END(se_api_version, patch));
}

static se_status contextCreateImpl(const se_context_params* params,
                                   se_context** out_context) {
    if (!out_context)
        return SE_ERR_ARGUMENT;
    *out_context = nullptr;
    if (params && !validInput(
            params, SE_V1_END(se_context_params, runtime_root_utf16))) {
        return SE_ERR_ARGUMENT;
    }
    QString root;
    if (params && params->struct_size >= sizeof(*params)
        && params->runtime_root_utf16) {
        root = QString::fromWCharArray(params->runtime_root_utf16);
        root = QDir(root).absolutePath();
    }
    if (!root.isEmpty()) {
        std::lock_guard<std::mutex> lock(gRuntimeRootMutex);
        if (!gRuntimeRoot.isEmpty()
            && QDir::cleanPath(gRuntimeRoot).compare(
                   QDir::cleanPath(root), Qt::CaseInsensitive) != 0) {
            return SE_ERR_RUNTIME;
        }
        gRuntimeRoot = root;
        scanengine::setRuntimeRoot(root);
    }

    std::string appError;
    if (!ensureQtApplication(&appError))
        return SE_ERR_RUNTIME;
    if (!root.isEmpty())
        QCoreApplication::addLibraryPath(root);
    QString gpuError;
    if (!scanengine::hybrid::initializeHybridRuntime(&gpuError))
        return SE_ERR_RUNTIME;
    try {
        *out_context = reinterpret_cast<se_context*>(new SdkContext);
        return SE_OK;
    } catch (...) {
        return SE_ERR_INTERNAL;
    }
}

se_status scanengine_context_create(const se_context_params* params,
                                  se_context** out_context) {
    try {
        return contextCreateImpl(params, out_context);
    } catch (...) {
        if (out_context)
            *out_context = nullptr;
        return SE_ERR_INTERNAL;
    }
}

void scanengine_context_destroy(se_context* handle) {
    std::unique_ptr<SdkContext> context(fromHandle(handle));
    if (!context)
        return;
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->cancelRequested = true;
        if (context->activeService)
            context->activeService->cancel();
    }
    if (context->worker.joinable())
        context->worker.join();
}

static se_status runtimeCheckImpl(se_context* handle, se_runtime_info* out) {
    SdkContext* context = fromHandle(handle);
    if (!context || !out
        || out->struct_size < SE_V1_END(se_runtime_info, error_utf8))
        return SE_ERR_ARGUMENT;
    se_runtime_info info{};
    info.struct_size = sizeof(info);
    info.api_version = SCANENGINE_API_VERSION;
    QString reason;
    const bool ready = scanengine::hybrid::certifiedGpuBackendReady(&reason);
    info.gpu_available = ready ? 1 : 0;
    copyUtf8(info.backend_utf8, sizeof(info.backend_utf8),
             scanengine::hybrid::certifiedGpuBackendName().toStdString());
    copyUtf8(info.error_utf8, sizeof(info.error_utf8),
             reason.toUtf8().constData());
    if (!writeOutput(out, info, SE_V1_END(se_runtime_info, error_utf8)))
        return SE_ERR_ARGUMENT;
    return ready ? SE_OK : SE_ERR_RUNTIME;
}

se_status scanengine_runtime_check(se_context* handle, se_runtime_info* out) {
    try {
        return runtimeCheckImpl(handle, out);
    } catch (...) {
        return SE_ERR_INTERNAL;
    }
}

se_status scanengine_submit_file(se_context* handle,
                               const se_parse_params* params,
                               uint64_t* out_task_id) {
    SdkContext* context = fromHandle(handle);
    if (!context || !out_task_id
        || !validInput(params, SE_V1_END(se_parse_params, effort))
        || !params->input_path_utf16 || !params->output_root_utf16) {
        return SE_ERR_ARGUMENT;
    }
    try {
        const QString effort = effortName(params->effort);
        if (effort.isEmpty())
            return SE_ERR_ARGUMENT;
        if (context->worker.joinable()) {
            {
                std::lock_guard<std::mutex> lock(context->mutex);
                if (context->state == SE_TASK_QUEUED
                    || context->state == SE_TASK_RUNNING) {
                    return SE_ERR_BUSY;
                }
            }
            context->worker.join();
        }

        scanengine::ParseOptions options;
        options.inputPath = QString::fromWCharArray(params->input_path_utf16);
        options.outputDir = QString::fromWCharArray(params->output_root_utf16);
        options.effort = effort;
        options.device = QStringLiteral("gpu-required");

        uint64_t taskId = 0;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            taskId = context->nextTaskId++;
            context->taskId = taskId;
            context->state = SE_TASK_QUEUED;
            context->status = SE_OK;
            context->progress = 0.0f;
            context->message = "queued";
            context->lastError.clear();
            context->result = {};
            context->cancelRequested = false;
        }
        *out_task_id = taskId;
        context->worker = std::thread(
            runParseTask, context, taskId, std::move(options));
        return SE_OK;
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->state = SE_TASK_FAILED;
        context->status = SE_ERR_INTERNAL;
        context->progress = 1.0f;
        context->message = error.what();
        context->lastError = context->message;
        context->taskId = 0;
        *out_task_id = 0;
        return SE_ERR_INTERNAL;
    } catch (...) {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->state = SE_TASK_FAILED;
        context->status = SE_ERR_INTERNAL;
        context->progress = 1.0f;
        context->message = "failed to start parser task";
        context->lastError = context->message;
        context->taskId = 0;
        *out_task_id = 0;
        return SE_ERR_INTERNAL;
    }
}

se_status scanengine_task_poll(se_context* handle, uint64_t taskId,
                             se_task_status* out) {
    SdkContext* context = fromHandle(handle);
    if (!context || !out
        || out->struct_size < SE_V1_END(se_task_status, message_utf8))
        return SE_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(context->mutex);
    if (!validTask(context, taskId))
        return SE_ERR_NOT_FOUND;
    se_task_status status{};
    status.struct_size = sizeof(status);
    status.api_version = SCANENGINE_API_VERSION;
    status.task_id = taskId;
    status.state = context->state;
    status.progress = context->progress;
    status.error = context->status;
    copyUtf8(status.message_utf8, sizeof(status.message_utf8), context->message);
    return writeOutput(out, status, SE_V1_END(se_task_status, message_utf8))
        ? SE_OK : SE_ERR_ARGUMENT;
}

se_status scanengine_task_cancel(se_context* handle, uint64_t taskId) {
    SdkContext* context = fromHandle(handle);
    if (!context)
        return SE_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(context->mutex);
    if (!validTask(context, taskId))
        return SE_ERR_NOT_FOUND;
    context->cancelRequested = true;
    if (context->activeService)
        context->activeService->cancel();
    return SE_OK;
}

se_status scanengine_task_result(se_context* handle, uint64_t taskId,
                               se_result* out) {
    SdkContext* context = fromHandle(handle);
    if (!context || !out
        || out->struct_size < SE_V1_END(se_result, fallback_count))
        return SE_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(context->mutex);
    if (!validTask(context, taskId))
        return SE_ERR_NOT_FOUND;
    if (context->state == SE_TASK_QUEUED
        || context->state == SE_TASK_RUNNING) {
        return SE_ERR_BUSY;
    }
    try {
        se_result result{};
        result.struct_size = sizeof(result);
        result.api_version = SCANENGINE_API_VERSION;
        result.task_id = taskId;
        result.status = context->status;
        result.message_utf8 = duplicateUtf8(context->result.message);
        result.output_dir_utf16 = duplicateWide(context->result.outputDir);
        result.excel_path_utf16 = duplicateWide(context->result.excelPath);
        result.markdown_path_utf16 = duplicateWide(context->result.markdownPath);
        result.json_path_utf16 = duplicateWide(context->result.jsonPath);
        result.fallback_count = context->result.fallbackCount;
        if (!result.message_utf8 || !result.output_dir_utf16
            || !result.excel_path_utf16 || !result.markdown_path_utf16
            || !result.json_path_utf16) {
            scanengine_result_free(&result);
            context->lastError = "result allocation failed";
            return SE_ERR_INTERNAL;
        }
        if (!writeOutput(out, result, SE_V1_END(se_result, fallback_count))) {
            scanengine_result_free(&result);
            return SE_ERR_ARGUMENT;
        }
        return context->status;
    } catch (const std::exception& error) {
        context->lastError = error.what();
        return SE_ERR_INTERNAL;
    } catch (...) {
        context->lastError = "failed to copy parser result";
        return SE_ERR_INTERNAL;
    }
}

se_status scanengine_task_release(se_context* handle, uint64_t taskId) {
    SdkContext* context = fromHandle(handle);
    if (!context)
        return SE_ERR_ARGUMENT;
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (!validTask(context, taskId))
            return SE_ERR_NOT_FOUND;
        if (context->state == SE_TASK_QUEUED
            || context->state == SE_TASK_RUNNING) {
            return SE_ERR_BUSY;
        }
    }
    if (context->worker.joinable())
        context->worker.join();
    std::lock_guard<std::mutex> lock(context->mutex);
    if (!validTask(context, taskId))
        return SE_ERR_NOT_FOUND;
    context->taskId = 0;
    context->state = SE_TASK_DONE;
    context->status = SE_OK;
    context->progress = 0.0f;
    context->message.clear();
    context->lastError.clear();
    context->result = {};
    context->cancelRequested = false;
    return SE_OK;
}

const char* scanengine_last_error(se_context* handle) {
    SdkContext* context = fromHandle(handle);
    if (!context)
        return "null context";
    thread_local std::string error;
    try {
        std::lock_guard<std::mutex> lock(context->mutex);
        error = context->lastError;
        return error.c_str();
    } catch (...) {
        return "failed to copy last error";
    }
}

void scanengine_free(void* memory) {
    std::free(memory);
}

void scanengine_result_free(se_result* result) {
    if (!result)
        return;
    scanengine_free(result->message_utf8);
    scanengine_free(result->output_dir_utf16);
    scanengine_free(result->excel_path_utf16);
    scanengine_free(result->markdown_path_utf16);
    scanengine_free(result->json_path_utf16);
    result->message_utf8 = nullptr;
    result->output_dir_utf16 = nullptr;
    result->excel_path_utf16 = nullptr;
    result->markdown_path_utf16 = nullptr;
    result->json_path_utf16 = nullptr;
}

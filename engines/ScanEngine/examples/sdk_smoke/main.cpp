#include <scanengine/scanengine_api.h>
#include <scanengine/scanengine_runtime.h>

#include <iostream>
#include <chrono>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 4) {
        std::wcerr << L"usage: scanengine_sdk_smoke RUNTIME_ROOT "
                      L"[INPUT_FILE OUTPUT_ROOT]\n";
        return 2;
    }
    wchar_t error[1024]{};
    std::cout << "runtime bootstrap..." << std::endl;
    if (!scanengine_runtime_initialize(argv[1], error, 1024)) {
        std::wcerr << error << L'\n';
        return 3;
    }
    std::cout << "runtime ready" << std::endl;
    se_api_version version{};
    scanengine_api_version(&version);
    std::cout << "ScanEngine ABI " << version.api_version << std::endl;
    se_context_params params{};
    params.struct_size = sizeof(params);
    params.api_version = SCANENGINE_API_VERSION;
    params.runtime_root_utf16 = argv[1];
    se_context* context = nullptr;
    std::cout << "context create..." << std::endl;
    if (scanengine_context_create(&params, &context) != SE_OK)
        return 4;
    std::cout << "context ready" << std::endl;
    struct ExtendedRuntimeInfo {
        se_runtime_info value;
        uint64_t guard;
    } extendedRuntime{};
    extendedRuntime.value.struct_size = sizeof(extendedRuntime);
    extendedRuntime.value.api_version = SCANENGINE_API_VERSION;
    extendedRuntime.guard = 0x1122334455667788ull;
    const se_status runtimeStatus =
        scanengine_runtime_check(context, &extendedRuntime.value);
    const se_runtime_info& runtime = extendedRuntime.value;
    if (extendedRuntime.guard != 0x1122334455667788ull) {
        scanengine_context_destroy(context);
        return 5;
    }
    std::cout << "backend " << runtime.backend_utf8
              << ", gpu=" << runtime.gpu_available << std::endl;
    if (runtimeStatus != SE_OK) {
        std::cerr << runtime.error_utf8 << std::endl;
        scanengine_context_destroy(context);
        return 5;
    }
    if (argc == 4) {
        se_parse_params parse{};
        parse.struct_size = sizeof(parse);
        parse.api_version = SCANENGINE_API_VERSION;
        parse.input_path_utf16 = argv[2];
        parse.output_root_utf16 = argv[3];
        parse.effort = SE_EFFORT_MEDIUM;
        uint64_t taskId = 0;
        if (scanengine_submit_file(context, &parse, &taskId) != SE_OK) {
            scanengine_context_destroy(context);
            return 6;
        }
        se_task_status task{};
        do {
            task.struct_size = sizeof(task);
            task.api_version = SCANENGINE_API_VERSION;
            if (scanengine_task_poll(context, taskId, &task) != SE_OK) {
                scanengine_context_destroy(context);
                return 7;
            }
            std::cout << int(task.progress * 100.0f) << "% "
                      << task.message_utf8 << std::endl;
            if (task.state == SE_TASK_QUEUED
                || task.state == SE_TASK_RUNNING) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        } while (task.state == SE_TASK_QUEUED
                 || task.state == SE_TASK_RUNNING);
        se_result result{};
        result.struct_size = sizeof(result);
        result.api_version = SCANENGINE_API_VERSION;
        const se_status resultStatus =
            scanengine_task_result(context, taskId, &result);
        if (result.message_utf8)
            std::cout << result.message_utf8 << std::endl;
        scanengine_result_free(&result);
        if (scanengine_task_release(context, taskId) != SE_OK) {
            scanengine_context_destroy(context);
            return 9;
        }
        if (scanengine_task_poll(context, taskId, &task) != SE_ERR_NOT_FOUND) {
            scanengine_context_destroy(context);
            return 10;
        }
        if (resultStatus != SE_OK) {
            scanengine_context_destroy(context);
            return 8;
        }
    }
    scanengine_context_destroy(context);
    std::cout << "context destroyed" << std::endl;
    return 0;
}

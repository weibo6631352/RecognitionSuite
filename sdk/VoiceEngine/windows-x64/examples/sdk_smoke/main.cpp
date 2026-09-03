#include <voiceengine/voiceengine_api.h>
#include <voiceengine/voiceengine_runtime.h>

#include <iostream>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <thread>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3) {
        std::wcerr << L"usage: voiceengine_sdk_smoke RUNTIME_ROOT "
                      L"[AUDIO_FILE|--stream-tone]\n";
        return 2;
    }
    wchar_t error[1024]{};
    if (!voiceengine_runtime_initialize(argv[1], error, 1024)) {
        std::wcerr << error << L'\n';
        return 3;
    }
    ve_context_params params{};
    params.struct_size = sizeof(params);
    params.api_version = VOICEENGINE_API_VERSION;
    params.runtime_dir_utf16 = argv[1];
    ve_context* context = nullptr;
    if (voiceengine_context_create(&params, &context) != VE_OK)
        return 4;
    ve_api_version version{};
    voiceengine_api_version(&version);
    std::cout << "VoiceEngine ABI " << version.api_version << '\n';
    ve_cuda_info cuda{};
    cuda.struct_size = sizeof(cuda);
    cuda.api_version = VOICEENGINE_API_VERSION;
    const ve_status cudaStatus = voiceengine_cuda_check(context, &cuda);
    std::cout << "GPU " << cuda.name << ", available=" << cuda.available
              << std::endl;
    if (cudaStatus != VE_OK) {
        std::cerr << cuda.error_utf8 << std::endl;
        voiceengine_context_destroy(context);
        return 5;
    }
    if (argc == 3) {
        const std::wstring modelDir =
            std::wstring(argv[1]) + L"\\models\\qwen3-asr-1.7b";
        ve_model_params model{};
        model.struct_size = sizeof(model);
        model.api_version = VOICEENGINE_API_VERSION;
        model.model_dir_utf16 = modelDir.c_str();
        if (voiceengine_model_load(context, &model) != VE_OK) {
            std::cerr << voiceengine_last_error(context) << std::endl;
            voiceengine_context_destroy(context);
            return 6;
        }
        if (std::wcscmp(argv[2], L"--stream-tone") == 0) {
            ve_stream_params streamParams{};
            streamParams.struct_size = sizeof(streamParams);
            streamParams.api_version = VOICEENGINE_API_VERSION;
            streamParams.sample_rate = 16000;
            streamParams.channels = 1;
            streamParams.chunk_ms = 1000;
            streamParams.overlap_ms = 100;
            streamParams.endpoint_silence_ms = 300;
            streamParams.pre_roll_ms = 100;
            streamParams.vad_rms_threshold = 0.004f;
            ve_stream* stream = nullptr;
            if (voiceengine_stream_create(context, &streamParams, &stream) != VE_OK) {
                std::cerr << "stream create failed\n";
                voiceengine_context_destroy(context);
                return 7;
            }
            constexpr int sampleRate = 16000;
            constexpr int blockFrames = 320;
            std::vector<float> block(blockFrames);
            for (int blockIndex = 0; blockIndex < 125; ++blockIndex) {
                for (int i = 0; i < blockFrames; ++i) {
                    const int frame = blockIndex * blockFrames + i;
                    block[static_cast<size_t>(i)] = 0.12f * std::sin(
                        2.0 * 3.141592653589793 * 220.0 * frame / sampleRate);
                }
                if (voiceengine_stream_push_f32(stream, block.data(), blockFrames) != VE_OK) {
                    voiceengine_stream_destroy(stream);
                    voiceengine_context_destroy(context);
                    return 8;
                }
            }
            std::fill(block.begin(), block.end(), 0.0f);
            for (int blockIndex = 0; blockIndex < 50; ++blockIndex) {
                if (voiceengine_stream_push_f32(stream, block.data(), blockFrames) != VE_OK) {
                    voiceengine_stream_destroy(stream);
                    voiceengine_context_destroy(context);
                    return 8;
                }
            }
            voiceengine_stream_finish(stream);
            bool sawFinal = false;
            bool sawEnd = false;
            bool sawError = false;
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::minutes(5);
            while (!sawEnd && std::chrono::steady_clock::now() < deadline) {
                ve_stream_event event{};
                event.struct_size = sizeof(event);
                event.api_version = VOICEENGINE_API_VERSION;
                const ve_status streamStatus = voiceengine_stream_poll(stream, &event);
                if (streamStatus == VE_ERR_BUSY) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }
                if (streamStatus != VE_OK) {
                    sawError = true;
                    break;
                }
                std::cout << "stream event=" << static_cast<int>(event.type)
                          << " segment=" << event.segment_id
                          << " text=" << event.text_utf8 << std::endl;
                sawFinal = sawFinal || event.type == VE_STREAM_EVENT_FINAL;
                sawError = sawError || event.type == VE_STREAM_EVENT_ERROR;
                sawEnd = event.type == VE_STREAM_EVENT_END;
            }
            voiceengine_stream_destroy(stream);
            voiceengine_context_destroy(context);
            return sawFinal && sawEnd && !sawError ? 0 : 9;
        }

        uint64_t taskId = 0;
        if (voiceengine_submit_file(context, argv[2], &taskId) != VE_OK) {
            voiceengine_context_destroy(context);
            return 7;
        }
        ve_task_status task{};
        do {
            task.struct_size = sizeof(task);
            task.api_version = VOICEENGINE_API_VERSION;
            if (voiceengine_task_poll(context, taskId, &task) != VE_OK) {
                voiceengine_context_destroy(context);
                return 8;
            }
            std::cout << int(task.progress * 100.0f) << "% "
                      << task.interim_utf8 << std::endl;
            if (task.state == VE_TASK_QUEUED
                || task.state == VE_TASK_RUNNING) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        } while (task.state == VE_TASK_QUEUED
                 || task.state == VE_TASK_RUNNING);
        ve_result result{};
        result.struct_size = sizeof(result);
        result.api_version = VOICEENGINE_API_VERSION;
        const ve_status resultStatus =
            voiceengine_task_result(context, taskId, &result);
        if (result.text_utf8)
            std::cout << result.text_utf8 << std::endl;
        voiceengine_result_free(&result);
        voiceengine_task_release(context, taskId);
        if (resultStatus != VE_OK) {
            voiceengine_context_destroy(context);
            return 9;
        }
    }
    voiceengine_context_destroy(context);
    return 0;
}

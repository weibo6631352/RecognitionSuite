#include <voiceengine/voiceengine.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <iostream>
#include <thread>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3) {
        std::wcerr << L"usage: voiceengine_sdk_cpp_smoke RUNTIME_ROOT "
                      L"[AUDIO_FILE|--stream-tone|--stream-tone-6m]\n";
        return 2;
    }
    try {
        auto context = voiceengine::Context::open(argv[1]);
        const auto gpu = context.cuda_info();
        std::cout << "GPU " << gpu.name << ", available=" << gpu.available
                  << std::endl;
        if (argc == 3) {
            context.load_model();
            const bool shortStream = std::wcscmp(
                argv[2], L"--stream-tone") == 0;
            const bool longStream = std::wcscmp(
                argv[2], L"--stream-tone-6m") == 0;
            if (shortStream || longStream) {
                auto stream = context.create_stream();
                constexpr int sample_rate = 16000;
                constexpr int block_frames = 320;
                std::vector<float> block(block_frames);
                const int tone_blocks = longStream ? 18000 : 125;
                int final_count = 0;
                bool stream_error = false;
                bool stream_end = false;
                auto drain = [&] {
                    for (int index = 0; index < 32; ++index) {
                        const auto event = stream.poll();
                        if (!event)
                            break;
                        if (event->type == VE_STREAM_EVENT_FINAL) {
                            ++final_count;
                            std::cout << "FINAL segment=" << event->segment_id
                                      << " text=" << event->text << std::endl;
                        } else if (event->type == VE_STREAM_EVENT_ERROR) {
                            stream_error = true;
                            std::cerr << "stream error: " << event->text
                                      << std::endl;
                        } else if (event->type == VE_STREAM_EVENT_END) {
                            stream_end = true;
                        }
                    }
                };
                auto push = [&](const std::vector<float>& samples) {
                    for (;;) {
                        try {
                            stream.push(samples);
                            return;
                        } catch (const voiceengine::Error& error) {
                            if (error.status() != VE_ERR_BUSY)
                                throw;
                            drain();
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(10));
                        }
                    }
                };
                for (int block_index = 0; block_index < tone_blocks; ++block_index) {
                    for (int i = 0; i < block_frames; ++i) {
                        const int frame = block_index * block_frames + i;
                        block[static_cast<std::size_t>(i)] =
                            0.12f * static_cast<float>(std::sin(
                                2.0 * 3.141592653589793 * 220.0 * frame
                                / sample_rate));
                    }
                    push(block);
                    if (block_index % 10 == 0)
                        drain();
                }
                std::fill(block.begin(), block.end(), 0.0f);
                for (int i = 0; i < 50; ++i)
                    push(block);
                stream.finish();

                while (!stream_end) {
                    const auto event = stream.poll();
                    if (!event) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(20));
                        continue;
                    }
                    std::cout << "event=" << event->type
                              << " text=" << event->text << std::endl;
                    final_count += event->type == VE_STREAM_EVENT_FINAL ? 1 : 0;
                    if (event->type == VE_STREAM_EVENT_ERROR)
                        stream_error = true;
                    if (event->type == VE_STREAM_EVENT_END)
                        stream_end = true;
                }
                const int minimum_finals = longStream ? 8 : 1;
                return !stream_error && final_count >= minimum_finals ? 0 : 5;
            }
            const auto result = context.submit_file(argv[2]).wait();
            std::cout << result.text << std::endl;
        }
        return 0;
    } catch (const voiceengine::Error& error) {
        std::cerr << error.what() << " (status=" << error.status() << ")\n";
        return 3;
    }
}

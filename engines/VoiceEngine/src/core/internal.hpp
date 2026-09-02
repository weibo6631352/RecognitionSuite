#pragma once

#include "voiceengine/voiceengine_api.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace voiceengine {

struct Pcm16k {
    std::vector<float> samples;
    int sample_rate = 16000;
};

struct Slice {
    uint64_t offset_samples = 0;
    uint64_t n_samples = 0;
    double start_sec = 0;
    double end_sec = 0;
};

struct SliceConfig {
    double target_sec = 45.0;
    double max_sec = 120.0;
    double overlap_sec = 0.8;
    double min_silence_sec = 0.25;
    double frame_sec = 0.02;
};

std::string wide_to_utf8(const wchar_t* s);
std::wstring utf8_to_wide(const std::string& s);
std::string json_escape(const std::string& s);

void set_thread_last_error(std::string* slot, const std::string& msg);

bool cuda_query(ve_cuda_info* out, bool simulate_fail);

std::string exe_or_dll_dir();
bool add_private_runtime_directory(const std::wstring& dir);
void init_private_runtimes(const std::wstring& runtime_dir);

bool decode_audio_file(const wchar_t* path_utf16, Pcm16k* out, std::string* err);
bool decode_wav_file(const wchar_t* path_utf16, Pcm16k* out, std::string* err);
bool decode_ffmpeg_file(const wchar_t* path_utf16, Pcm16k* out, std::string* err);
bool pcm_to_16k_mono(const float* samples, uint64_t n, int sr, int ch, Pcm16k* out, std::string* err);

std::vector<Slice> silence_slice(const float* samples, uint64_t n, int sr, const SliceConfig& cfg);
std::string slices_to_json(const std::vector<Slice>& slices);
bool slices_from_json(const std::string& json, std::vector<Slice>* out);

std::string merge_overlap_text(const std::vector<std::string>& parts);
std::string normalize_zh_text(const std::string& in);
std::string build_result_json(const std::string& text,
                              const std::string& device,
                              const std::string& model,
                              double duration_sec,
                              const std::vector<std::string>& segment_texts,
                              const std::vector<Slice>& slices);

char* dll_strdup(const std::string& s);

extern std::atomic<int32_t> g_cpu_asr_started;

class AsrBackend {
public:
    virtual ~AsrBackend() = default;
    virtual bool load(const std::wstring& model_dir, std::string* err) = 0;
    virtual void unload() = 0;
    virtual bool loaded() const = 0;
    virtual bool infer(const float* samples,
                       uint64_t n,
                       int sample_rate,
                       const std::atomic<bool>& cancel,
                       std::string* interim,
                       std::string* text,
                       std::string* err) = 0;
    virtual std::string device_name() const = 0;
    virtual std::string model_name() const = 0;
};

AsrBackend* create_llamacpp_backend();

class Engine {
public:
    Engine(bool simulate_cuda_fail, std::wstring runtime_dir);
    ~Engine();
    ve_status cuda_check(ve_cuda_info* out);
    ve_status load(const ve_model_params* params);
    ve_status unload();
    int32_t loaded() const;
    ve_status submit_file(const wchar_t* path, uint64_t* out_id);
    ve_status submit_pcm(const ve_pcm_desc* pcm, uint64_t* out_id);
    ve_status submit_stream_pcm(const ve_pcm_desc* pcm, uint64_t* out_id);
    ve_status poll(uint64_t id, ve_task_status* out);
    ve_status result(uint64_t id, ve_result* out);
    ve_status cancel(uint64_t id);
    ve_status release(uint64_t id);
    const char* last_error() const;

private:
    struct Impl;
    Impl* impl_;
};

Engine* engine_from_handle(void* ctx);

}  // namespace voiceengine

void* voiceengine_engine_create(bool cuda_fail, const wchar_t* runtime_dir);
void voiceengine_engine_destroy(void* ctx);

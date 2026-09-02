#include "internal.hpp"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace voiceengine {

struct Task {
    uint64_t id = 0;
    bool is_file = true;
    std::wstring path;
    std::vector<float> pcm;
    int pcm_sr = 16000;
    int pcm_ch = 1;
    bool streaming = false;
    ve_task_state state = VE_TASK_QUEUED;
    ve_status error = VE_OK;
    float progress = 0;
    std::string interim;
    std::string text;
    std::string json;
    double duration = 0;
    std::string message;
};

struct Engine::Impl {
    explicit Impl(bool simulate_cuda_fail, std::wstring runtime_dir)
        : simulate_cuda_fail_(simulate_cuda_fail), runtime_dir_(std::move(runtime_dir)) {
        init_private_runtimes(runtime_dir_);
        backend_.reset(create_llamacpp_backend());
        worker_ = std::thread([this] { loop(); });
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (backend_) {
            backend_->unload();
        }
    }

    ve_status cuda_check(ve_cuda_info* out) {
        std::lock_guard<std::mutex> lock(mu_);
        const bool ok = cuda_query(out, simulate_cuda_fail_);
        if (!ok) {
            last_error_ = out && out->error_utf8[0] ? out->error_utf8
                                                   : "CUDA check failed; CPU ASR is disabled.";
            return VE_ERR_CUDA;
        }
        return VE_OK;
    }

    ve_status load(const ve_model_params* params) {
        ve_cuda_info info{};
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& entry : tasks_) {
                if (entry.second.state == VE_TASK_QUEUED
                    || entry.second.state == VE_TASK_RUNNING) {
                    last_error_ = "cannot load a model while tasks are active";
                    return VE_ERR_BUSY;
                }
            }
            if (simulate_cuda_fail_ || !cuda_query(&info, simulate_cuda_fail_) || !info.available) {
                last_error_ = "CUDA initialization failed; recognition stopped. CPU ASR is not a product path.";
                return VE_ERR_CUDA;
            }
        }
        std::wstring dir;
        if (params && params->model_dir_utf16 && params->model_dir_utf16[0]) {
            dir = params->model_dir_utf16;
        } else {
            dir = utf8_to_wide(exe_or_dll_dir() + "\\models\\qwen3-asr-1.7b");
        }
        std::string err;
        if (!backend_->load(dir, &err)) {
            std::lock_guard<std::mutex> lock(mu_);
            last_error_ = err.empty() ? "model load failed" : err;
            const bool cudaish = last_error_.find("CUDA") != std::string::npos ||
                                 last_error_.find("CPU ASR") != std::string::npos;
            return cudaish ? VE_ERR_CUDA : VE_ERR_MODEL;
        }
        return VE_OK;
    }

    ve_status unload() {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& entry : tasks_) {
            if (entry.second.state == VE_TASK_QUEUED
                || entry.second.state == VE_TASK_RUNNING) {
                last_error_ = "cannot unload the model while tasks are active";
                return VE_ERR_BUSY;
            }
        }
        backend_->unload();
        return VE_OK;
    }

    int32_t loaded() const { return backend_ && backend_->loaded() ? 1 : 0; }

    ve_status submit_file(const wchar_t* path, uint64_t* out_id) {
        if (!path || !path[0] || !out_id) {
            last_error_ = "invalid file path";
            return VE_ERR_ARGUMENT;
        }
        if (simulate_cuda_fail_) {
            last_error_ = "CUDA initialization failed; not starting CPU ASR.";
            return VE_ERR_CUDA;
        }
        Task t;
        t.is_file = true;
        t.path = path;
        return enqueue(std::move(t), out_id);
    }

    ve_status submit_pcm(const ve_pcm_desc* pcm, uint64_t* out_id) {
        return submit_pcm_impl(pcm, false, out_id);
    }

    ve_status submit_stream_pcm(const ve_pcm_desc* pcm, uint64_t* out_id) {
        return submit_pcm_impl(pcm, true, out_id);
    }

    ve_status submit_pcm_impl(const ve_pcm_desc* pcm,
                              bool streaming,
                              uint64_t* out_id) {
        if (!pcm || !pcm->samples || pcm->frame_count == 0 || !out_id
            || pcm->channels <= 0
            || pcm->frame_count > UINT64_MAX / static_cast<uint64_t>(pcm->channels)) {
            last_error_ = "invalid PCM descriptor";
            return VE_ERR_ARGUMENT;
        }
        if (simulate_cuda_fail_) {
            last_error_ = "CUDA initialization failed; not starting CPU ASR.";
            return VE_ERR_CUDA;
        }
        Task t;
        t.is_file = false;
        const uint64_t sampleCount =
            pcm->frame_count * static_cast<uint64_t>(pcm->channels);
        t.pcm.assign(pcm->samples, pcm->samples + sampleCount);
        t.pcm_sr = pcm->sample_rate > 0 ? pcm->sample_rate : 16000;
        t.pcm_ch = pcm->channels > 0 ? pcm->channels : 1;
        t.streaming = streaming;
        return enqueue(std::move(t), out_id);
    }

    ve_status poll(uint64_t id, ve_task_status* out) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = tasks_.find(id);
        if (it == tasks_.end() || !out) {
            last_error_ = "unknown task";
            return VE_ERR_NOT_FOUND;
        }
        const Task& t = it->second;
        std::memset(out, 0, sizeof(*out));
        out->struct_size = sizeof(*out);
        out->api_version = VOICEENGINE_API_VERSION;
        out->task_id = t.id;
        out->state = t.state;
        out->progress = t.progress;
        out->error = t.error;
        std::snprintf(out->interim_utf8, sizeof(out->interim_utf8), "%s", t.interim.c_str());
        return VE_OK;
    }

    ve_status result(uint64_t id, ve_result* out) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = tasks_.find(id);
        if (it == tasks_.end() || !out) {
            last_error_ = "unknown task";
            return VE_ERR_NOT_FOUND;
        }
        const Task& t = it->second;
        if (t.state != VE_TASK_DONE && t.state != VE_TASK_FAILED && t.state != VE_TASK_CANCELLED) {
            last_error_ = "task still running";
            return VE_ERR_INTERNAL;
        }
        std::memset(out, 0, sizeof(*out));
        out->struct_size = sizeof(*out);
        out->api_version = VOICEENGINE_API_VERSION;
        out->task_id = t.id;
        out->status = t.error;
        out->duration_sec = t.duration;
        out->text_utf8 = dll_strdup(t.text);
        out->json_utf8 = dll_strdup(t.json);
        if (!out->text_utf8 || !out->json_utf8) {
            std::free(out->text_utf8);
            std::free(out->json_utf8);
            out->text_utf8 = nullptr;
            out->json_utf8 = nullptr;
            last_error_ = "result allocation failed";
            return VE_ERR_INTERNAL;
        }
        return t.error == VE_OK ? VE_OK : t.error;
    }

    ve_status cancel(uint64_t id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) {
            last_error_ = "unknown task";
            return VE_ERR_NOT_FOUND;
        }
        cancel_ids_.insert(id);
        if (it->second.state == VE_TASK_QUEUED) {
            it->second.state = VE_TASK_CANCELLED;
            it->second.error = VE_ERR_CANCELLED;
            it->second.message = "cancelled";
        }
        return VE_OK;
    }

    ve_status release(uint64_t id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) {
            last_error_ = "unknown task";
            return VE_ERR_NOT_FOUND;
        }
        if (it->second.state == VE_TASK_QUEUED
            || it->second.state == VE_TASK_RUNNING) {
            last_error_ = "task is still active";
            return VE_ERR_BUSY;
        }
        tasks_.erase(it);
        cancel_ids_.erase(id);
        return VE_OK;
    }

    const char* last_error() const {
        thread_local std::string copy;
        std::lock_guard<std::mutex> lock(mu_);
        copy = last_error_;
        return copy.c_str();
    }

    bool simulate_cuda_fail() const { return simulate_cuda_fail_; }

private:
    ve_status enqueue(Task t, uint64_t* out_id) {
        std::lock_guard<std::mutex> lock(mu_);
        t.id = ++next_id_;
        t.state = VE_TASK_QUEUED;
        *out_id = t.id;
        tasks_[t.id] = std::move(t);
        queue_.push_back(*out_id);
        cv_.notify_one();
        return VE_OK;
    }

    bool cancelled(uint64_t id) {
        std::lock_guard<std::mutex> lock(mu_);
        return cancel_ids_.count(id) != 0;
    }

    void loop() {
        for (;;) {
            uint64_t id = 0;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) {
                    return;
                }
                id = queue_.front();
                queue_.pop_front();
                auto it = tasks_.find(id);
                if (it == tasks_.end()) {
                    continue;
                }
                if (it->second.state == VE_TASK_CANCELLED || cancel_ids_.count(id)) {
                    it->second.state = VE_TASK_CANCELLED;
                    it->second.error = VE_ERR_CANCELLED;
                    continue;
                }
                it->second.state = VE_TASK_RUNNING;
            }
            try {
                run_task(id);
            } catch (const std::exception& error) {
                finish(id, VE_ERR_INTERNAL, error.what(), {}, {}, 0);
            } catch (...) {
                finish(id, VE_ERR_INTERNAL, "unhandled recognition exception",
                       {}, {}, 0);
            }
        }
    }

    void run_task(uint64_t id) {
        Task snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            snapshot = tasks_[id];
        }
        std::string err;
        Pcm16k pcm;
        if (snapshot.is_file) {
            if (!decode_audio_file(snapshot.path.c_str(), &pcm, &err)) {
                finish(id, VE_ERR_AUDIO, err, {}, {}, 0);
                return;
            }
        } else if (!pcm_to_16k_mono(snapshot.pcm.data(), snapshot.pcm.size(), snapshot.pcm_sr,
                                    snapshot.pcm_ch, &pcm, &err)) {
            finish(id, VE_ERR_AUDIO, err, {}, {}, 0);
            return;
        }

        const double dur = static_cast<double>(pcm.samples.size()) / 16000.0;
        SliceConfig cfg;
        std::vector<Slice> slices;
        if (snapshot.streaming) {
            Slice whole;
            whole.offset_samples = 0;
            whole.n_samples = static_cast<uint64_t>(pcm.samples.size());
            whole.start_sec = 0.0;
            whole.end_sec = dur;
            slices.push_back(whole);
        } else {
            slices = silence_slice(pcm.samples.data(), pcm.samples.size(), 16000, cfg);
        }
        std::vector<std::string> parts;
        parts.reserve(slices.size());
        std::atomic<bool> cancel{false};

        if (!backend_->loaded()) {
            finish(id, VE_ERR_MODEL, "model is not loaded", {}, {}, dur);
            return;
        }

        for (size_t i = 0; i < slices.size(); ++i) {
            if (cancelled(id)) {
                finish(id, VE_ERR_CANCELLED, "cancelled", merge_overlap_text(parts), parts, dur);
                return;
            }
            cancel.store(false);
            const Slice& sl = slices[i];
            std::string interim;
            std::string text;
            std::string ierr;
            bool ok = false;
            for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
                if (cancelled(id)) {
                    finish(id, VE_ERR_CANCELLED, "cancelled", merge_overlap_text(parts), parts, dur);
                    return;
                }
                ok = backend_->infer(pcm.samples.data() + sl.offset_samples,
                                     sl.n_samples,
                                     16000,
                                     cancel,
                                     &interim,
                                     &text,
                                     &ierr);
            }
            if (!ok) {
                finish(id, ierr.find("cancel") != std::string::npos ? VE_ERR_CANCELLED : VE_ERR_MODEL,
                       ierr, merge_overlap_text(parts), parts, dur);
                return;
            }
            parts.push_back(text);
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto& t = tasks_[id];
                t.progress = static_cast<float>(i + 1) / static_cast<float>(slices.size());
                t.interim = merge_overlap_text(parts);
                if (!interim.empty()) {
                    t.interim = normalize_zh_text(t.interim);
                }
            }
        }
        const std::string merged = merge_overlap_text(parts);
        finish(id, VE_OK, {}, merged, parts, dur, slices);
    }

    void finish(uint64_t id,
                ve_status st,
                const std::string& err,
                const std::string& text,
                const std::vector<std::string>& parts,
                double dur,
                const std::vector<Slice>& slices = {}) {
        std::lock_guard<std::mutex> lock(mu_);
        auto& t = tasks_[id];
        t.error = st;
        t.duration = dur;
        t.text = text;
        t.message = err;
        if (st == VE_OK) {
            t.state = VE_TASK_DONE;
            t.progress = 1.0f;
            t.json = build_result_json(text,
                                       backend_ ? backend_->device_name() : "",
                                       backend_ ? backend_->model_name() : "",
                                       dur,
                                       parts,
                                       slices);
        } else if (st == VE_ERR_CANCELLED) {
            t.state = VE_TASK_CANCELLED;
            last_error_ = err.empty() ? "cancelled" : err;
        } else {
            t.state = VE_TASK_FAILED;
            last_error_ = err;
        }
    }

    bool simulate_cuda_fail_ = false;
    std::wstring runtime_dir_;
    std::unique_ptr<AsrBackend> backend_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_ = false;
    uint64_t next_id_ = 0;
    std::deque<uint64_t> queue_;
    std::unordered_map<uint64_t, Task> tasks_;
    std::unordered_set<uint64_t> cancel_ids_;
    std::string last_error_;
};

Engine::Engine(bool simulate_cuda_fail, std::wstring runtime_dir)
    : impl_(new Impl(simulate_cuda_fail, std::move(runtime_dir))) {}

Engine::~Engine() { delete impl_; }

ve_status Engine::cuda_check(ve_cuda_info* out) { return impl_->cuda_check(out); }
ve_status Engine::load(const ve_model_params* params) { return impl_->load(params); }
ve_status Engine::unload() { return impl_->unload(); }
int32_t Engine::loaded() const { return impl_->loaded(); }
ve_status Engine::submit_file(const wchar_t* path, uint64_t* out_id) {
    return impl_->submit_file(path, out_id);
}
ve_status Engine::submit_pcm(const ve_pcm_desc* pcm, uint64_t* out_id) {
    return impl_->submit_pcm(pcm, out_id);
}
ve_status Engine::submit_stream_pcm(const ve_pcm_desc* pcm, uint64_t* out_id) {
    return impl_->submit_stream_pcm(pcm, out_id);
}
ve_status Engine::poll(uint64_t id, ve_task_status* out) { return impl_->poll(id, out); }
ve_status Engine::result(uint64_t id, ve_result* out) { return impl_->result(id, out); }
ve_status Engine::cancel(uint64_t id) { return impl_->cancel(id); }
ve_status Engine::release(uint64_t id) { return impl_->release(id); }
const char* Engine::last_error() const { return impl_->last_error(); }

Engine* engine_from_handle(void* ctx) {
    return static_cast<Engine*>(ctx);
}

}  // namespace voiceengine

using voiceengine::Engine;
using voiceengine::engine_from_handle;

void* voiceengine_engine_create(bool cuda_fail, const wchar_t* runtime_dir) {
    std::wstring rd = runtime_dir ? runtime_dir : L"";
    return new Engine(cuda_fail, rd);
}

void voiceengine_engine_destroy(void* ctx) {
    delete static_cast<Engine*>(ctx);
}

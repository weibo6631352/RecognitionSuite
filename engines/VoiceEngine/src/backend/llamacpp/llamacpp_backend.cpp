#include "core/internal.hpp"

#include "llama.h"
#include "mtmd.h"
#include "mtmd-helper.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace voiceengine {
namespace {

std::wstring join_path(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) {
        return b;
    }
    if (a.back() == L'\\' || a.back() == L'/') {
        return a + b;
    }
    return a + L"\\" + b;
}

bool file_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool copy_file_append(HANDLE dst, const std::wstring& src_path) {
    HANDLE src = CreateFileW(src_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (src == INVALID_HANDLE_VALUE) {
        return false;
    }
    char buf[1 << 16];
    DWORD n = 0;
    bool ok = true;
    while (ReadFile(src, buf, sizeof(buf), &n, nullptr) && n > 0) {
        DWORD w = 0;
        if (!WriteFile(dst, buf, n, &w, nullptr) || w != n) {
            ok = false;
            break;
        }
    }
    CloseHandle(src);
    return ok;
}

bool assemble_gguf_parts(const std::wstring& assembled) {
    if (file_exists(assembled)) {
        return true;
    }
    const std::wstring p1 = assembled + L".part1";
    if (!file_exists(p1)) {
        return false;
    }
    HANDLE dst = CreateFileW(assembled.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dst == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = copy_file_append(dst, p1);
    for (int i = 2; ok && i < 16; ++i) {
        const std::wstring pn = assembled + L".part" + std::to_wstring(i);
        if (!file_exists(pn)) {
            break;
        }
        ok = copy_file_append(dst, pn);
    }
    CloseHandle(dst);
    if (!ok) {
        DeleteFileW(assembled.c_str());
    }
    return ok && file_exists(assembled);
}

bool find_model_files(const std::wstring& dir, std::wstring* model, std::wstring* mmproj) {
    assemble_gguf_parts(join_path(dir, L"Qwen3-ASR-1.7B-bf16.gguf"));
    WIN32_FIND_DATAW fd;
    const std::wstring glob = join_path(dir, L"*");
    HANDLE h = FindFirstFileW(glob.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::wstring bf16;
    std::wstring mm_bf16;
    std::wstring any_gguf;
    std::wstring any_mm;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        std::wstring name = fd.cFileName;
        std::wstring lower = name;
        for (auto& c : lower) {
            if (c >= L'A' && c <= L'Z') {
                c = static_cast<wchar_t>(c - L'A' + L'a');
            }
        }
        if (lower.size() < 5 || lower.substr(lower.size() - 5) != L".gguf") {
            continue;
        }
        const std::wstring full = join_path(dir, name);
        const bool is_mm = lower.find(L"mmproj") != std::wstring::npos;
        if (is_mm) {
            if (lower.find(L"bf16") != std::wstring::npos) {
                mm_bf16 = full;
            }
            if (any_mm.empty()) {
                any_mm = full;
            }
        } else {
            if (lower.find(L"bf16") != std::wstring::npos) {
                bf16 = full;
            }
            if (any_gguf.empty()) {
                any_gguf = full;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    *model = !bf16.empty() ? bf16 : any_gguf;
    *mmproj = !mm_bf16.empty() ? mm_bf16 : any_mm;
    return file_exists(*model) && file_exists(*mmproj);
}

std::string token_to_string(const llama_vocab* vocab, llama_token tok) {
    char buf[256];
    const int n = llama_token_to_piece(vocab, tok, buf, sizeof(buf), 0, true);
    if (n < 0) {
        std::string s(static_cast<size_t>(-n), '\0');
        llama_token_to_piece(vocab, tok, &s[0], -n, 0, true);
        return s;
    }
    return std::string(buf, buf + n);
}

class LlamaCppBackend final : public AsrBackend {
public:
    ~LlamaCppBackend() override { unload(); }

    bool load(const std::wstring& model_dir, std::string* err) override {
        unload();
        ve_cuda_info info{};
        if (!cuda_query(&info, false) || !info.available) {
            if (err) {
                *err = info.error_utf8[0] ? info.error_utf8
                                          : "CUDA is required; CPU ASR is disabled.";
            }
            return false;
        }

        std::wstring model_path;
        std::wstring mm_path;
        if (!find_model_files(model_dir, &model_path, &mm_path)) {
            if (err) {
                *err = "Qwen3-ASR-1.7B BF16 GGUF or mmproj not found under " +
                       wide_to_utf8(model_dir.c_str());
            }
            return false;
        }

        llama_backend_init();
        backend_inited_ = true;

        llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = -1;
        if (mp.n_gpu_layers == 0) {
            g_cpu_asr_started.store(1);
            if (err) {
                *err = "refusing CPU ASR fallback";
            }
            return false;
        }

        const std::string model_utf8 = wide_to_utf8(model_path.c_str());
        const std::string mm_utf8 = wide_to_utf8(mm_path.c_str());
        model_ = llama_model_load_from_file(model_utf8.c_str(), mp);
        if (!model_) {
            if (err) {
                *err = "failed to load GGUF model: " + model_utf8;
            }
            return false;
        }

        llama_context_params cp = llama_context_default_params();
        cp.n_ctx = 8192;
        cp.n_batch = 2048;
        cp.n_ubatch = 512;
        lctx_ = llama_init_from_model(model_, cp);
        if (!lctx_) {
            if (err) {
                *err = "failed to create llama context on CUDA";
            }
            unload();
            return false;
        }

        mtmd_context_params tp = mtmd_context_params_default();
        tp.use_gpu = true;
        tp.warmup = false;
        mtmd_ = mtmd_init_from_file(mm_utf8.c_str(), model_, tp);
        if (!mtmd_) {
            if (err) {
                *err = "failed to load mmproj: " + mm_utf8;
            }
            unload();
            return false;
        }
        if (!mtmd_support_audio(mtmd_)) {
            if (err) {
                *err = "loaded mmproj does not support audio";
            }
            unload();
            return false;
        }

        vocab_ = llama_model_get_vocab(model_);
        device_ = std::string("CUDA:0 ") + info.name;
        model_name_ = "Qwen3-ASR-1.7B-BF16";
        model_path_ = model_utf8;
        return true;
    }

    void unload() override {
        if (mtmd_) {
            mtmd_free(mtmd_);
            mtmd_ = nullptr;
        }
        if (lctx_) {
            llama_free(lctx_);
            lctx_ = nullptr;
        }
        if (model_) {
            llama_model_free(model_);
            model_ = nullptr;
        }
        vocab_ = nullptr;
        if (backend_inited_) {
            llama_backend_free();
            backend_inited_ = false;
        }
    }

    bool loaded() const override { return model_ && lctx_ && mtmd_; }

    bool infer(const float* samples,
               uint64_t n,
               int sample_rate,
               const std::atomic<bool>& cancel,
               std::string* interim,
               std::string* text,
               std::string* err) override {
        if (!loaded()) {
            if (err) {
                *err = "model not loaded";
            }
            return false;
        }
        if (cancel.load()) {
            if (err) {
                *err = "cancelled";
            }
            return false;
        }
        (void)sample_rate;
        llama_memory_clear(llama_get_memory(lctx_), true);

        mtmd_bitmap* bmp = mtmd_bitmap_init_from_audio(static_cast<size_t>(n), samples);
        if (!bmp) {
            if (err) {
                *err = "mtmd_bitmap_init_from_audio failed";
            }
            return false;
        }

        const char* marker = mtmd_default_marker();
        std::string prompt = "<|im_start|>user\n";
        prompt += marker;
        prompt += "<|im_end|>\n<|im_start|>assistant\n";

        mtmd_input_text in{};
        in.text = prompt.c_str();
        in.text_len = prompt.size();
        in.add_special = true;
        in.parse_special = true;

        mtmd_input_chunks* chunks = mtmd_input_chunks_init();
        const mtmd_bitmap* bitmaps[1] = {bmp};
        const int32_t tok = mtmd_tokenize(mtmd_, chunks, &in, bitmaps, 1);
        if (tok != 0) {
            mtmd_input_chunks_free(chunks);
            mtmd_bitmap_free(bmp);
            if (err) {
                *err = "mtmd_tokenize failed";
            }
            return false;
        }

        llama_pos n_past = 0;
        const int32_t ev = mtmd_helper_eval_chunks(mtmd_, lctx_, chunks, 0, 0, 512, true, &n_past);
        mtmd_input_chunks_free(chunks);
        mtmd_bitmap_free(bmp);
        if (ev != 0) {
            if (err) {
                *err = "mtmd_helper_eval_chunks failed";
            }
            return false;
        }

        llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

        std::string out;
        // A 45-second hard streaming window can approach 256 Chinese tokens
        // during fast speech. Keep headroom for punctuation and the final
        // clause; EOG still ends normal generation before this guard.
        const int max_new = 384;
        for (int i = 0; i < max_new; ++i) {
            if (cancel.load()) {
                llama_sampler_free(smpl);
                if (err) {
                    *err = "cancelled";
                }
                return false;
            }
            const llama_token id = llama_sampler_sample(smpl, lctx_, -1);
            llama_sampler_accept(smpl, id);
            if (llama_vocab_is_eog(vocab_, id)) {
                break;
            }
            out += token_to_string(vocab_, id);
            if (interim) {
                *interim = out;
            }
            llama_batch batch = llama_batch_get_one(const_cast<llama_token*>(&id), 1);
            if (llama_decode(lctx_, batch) != 0) {
                llama_sampler_free(smpl);
                if (err) {
                    *err = "llama_decode failed during generation";
                }
                return false;
            }
            n_past += 1;
        }
        llama_sampler_free(smpl);
        if (text) {
            *text = normalize_zh_text(out);
        }
        return true;
    }

    std::string device_name() const override { return device_; }
    std::string model_name() const override { return model_name_; }

private:
    llama_model* model_ = nullptr;
    llama_context* lctx_ = nullptr;
    mtmd_context* mtmd_ = nullptr;
    const llama_vocab* vocab_ = nullptr;
    bool backend_inited_ = false;
    std::string device_;
    std::string model_name_;
    std::string model_path_;
};

}  // namespace

AsrBackend* create_llamacpp_backend() {
    return new LlamaCppBackend();
}

}  // namespace voiceengine

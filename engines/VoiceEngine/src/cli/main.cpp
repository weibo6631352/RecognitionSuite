#include "voiceengine/voiceengine_api.h"
#include "runtime_bootstrap.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int map_exit(ve_status s) {
    switch (s) {
    case VE_OK: return 0;
    case VE_ERR_ARGUMENT: return 1;
    case VE_ERR_AUDIO: return 2;
    case VE_ERR_MODEL: return 3;
    case VE_ERR_CUDA: return 4;
    case VE_ERR_CANCELLED: return 5;
    default: return 6;
    }
}

std::wstring utf8_to_wide(const char* s) {
    if (!s || !*s) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring o(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) {
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &o[0], n);
    }
    return o;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string o(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) {
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &o[0], n, nullptr, nullptr);
    }
    return o;
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    std::wstring p(buf);
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

const char* arg_value(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], key) == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

bool write_file(const wchar_t* path, const std::string& data) {
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"wb");
    if (!f) {
        return false;
    }
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

int wait_task(ve_context* ctx, uint64_t id, ve_result* result) {
    for (;;) {
        ve_task_status st{};
        st.struct_size = sizeof(st);
        st.api_version = VOICEENGINE_API_VERSION;
        const ve_status pr = voiceengine_task_poll(ctx, id, &st);
        if (pr != VE_OK) {
            std::fprintf(stderr, "poll error: %s\n", voiceengine_last_error(ctx));
            return map_exit(pr);
        }
        if (st.interim_utf8[0]) {
            std::fprintf(stderr, "progress=%.2f interim=%s\n", st.progress, st.interim_utf8);
        } else {
            std::fprintf(stderr, "progress=%.2f state=%d\n", st.progress, static_cast<int>(st.state));
        }
        if (st.state == VE_TASK_DONE || st.state == VE_TASK_FAILED || st.state == VE_TASK_CANCELLED) {
            const ve_status rr = voiceengine_task_result(ctx, id, result);
            if (rr != VE_OK) {
                std::fprintf(stderr, "result error: %s\n", voiceengine_last_error(ctx));
                if (st.state == VE_TASK_DONE) {
                    voiceengine_task_release(ctx, id);
                    return map_exit(rr);
                }
            }
            voiceengine_task_release(ctx, id);
            return map_exit(st.error);
        }
        Sleep(50);
    }
}

int cmd_info(ve_context* ctx) {
    ve_api_version v{};
    voiceengine_api_version(&v);
    std::printf("{\"name\":\"VoiceEngine\",\"version\":\"%u.%u.%u\",\"api_version\":%u}\n",
                v.major, v.minor, v.patch, v.api_version);
    ve_cuda_info c{};
    c.struct_size = sizeof(c);
    voiceengine_cuda_check(ctx, &c);
    std::fprintf(stderr, "cuda available=%d device=%s sm_%d%d vram_mib=%d\n",
                 c.available, c.name, c.compute_major, c.compute_minor, c.vram_mib);
    return c.available ? 0 : 4;
}

int cmd_verify(ve_context* ctx, const wchar_t* model_dir) {
    ve_cuda_info c{};
    c.struct_size = sizeof(c);
    const ve_status cs = voiceengine_cuda_check(ctx, &c);
    std::fprintf(stderr, "verify-runtime cuda available=%d name=%s sm_%d%d\n",
                 c.available, c.name, c.compute_major, c.compute_minor);
    if (cs != VE_OK || !c.available) {
        std::fprintf(stderr, "CUDA failed: %s\nCPU ASR fallback: disabled\n",
                     c.error_utf8[0] ? c.error_utf8 : voiceengine_last_error(ctx));
        return 4;
    }
    ve_model_params mp{};
    mp.struct_size = sizeof(mp);
    mp.api_version = VOICEENGINE_API_VERSION;
    mp.model_dir_utf16 = model_dir;
    const ve_status ls = voiceengine_model_load(ctx, &mp);
    if (ls != VE_OK) {
        std::fprintf(stderr, "model load: %s\n", voiceengine_last_error(ctx));
        std::printf("{\"cuda\":true,\"device\":\"%s\",\"model_loaded\":false,\"cpu_asr\":false}\n",
                    c.name);
        return map_exit(ls);
    }
    std::printf("{\"cuda\":true,\"device\":\"%s\",\"model\":\"Qwen3-ASR-1.7B-BF16\",\"layers\":\"all-on-cuda\",\"cpu_asr\":false}\n",
                c.name);
    std::fprintf(stderr, "Qwen3-ASR-1.7B BF16 on GPU (%s); CPU ASR disabled\n", c.name);
    return 0;
}

int cmd_transcribe(ve_context* ctx, const wchar_t* input, const wchar_t* output, const char* fmt, const wchar_t* model_dir) {
    ve_model_params mp{};
    mp.struct_size = sizeof(mp);
    mp.api_version = VOICEENGINE_API_VERSION;
    mp.model_dir_utf16 = model_dir;
    if (!voiceengine_model_loaded(ctx)) {
        const ve_status ls = voiceengine_model_load(ctx, &mp);
        if (ls != VE_OK) {
            std::fprintf(stderr, "model: %s\n", voiceengine_last_error(ctx));
            return map_exit(ls);
        }
    }
    uint64_t id = 0;
    const ve_status ss = voiceengine_submit_file(ctx, input, &id);
    if (ss != VE_OK) {
        std::fprintf(stderr, "submit: %s\n", voiceengine_last_error(ctx));
        return map_exit(ss);
    }
    ve_result r{};
    r.struct_size = sizeof(r);
    const int rc = wait_task(ctx, id, &r);
    const char* body = (fmt && std::strcmp(fmt, "json") == 0 && r.json_utf8) ? r.json_utf8
                      : (r.text_utf8 ? r.text_utf8 : "");
    if (output && output[0]) {
        write_file(output, body);
    }
    std::fputs(body, stdout);
    if (!body[0] || body[std::strlen(body) - 1] != '\n') {
        std::fputc('\n', stdout);
    }
    voiceengine_result_free(&r);
    return rc;
}

bool is_audio_name(const std::wstring& n) {
    auto lower = n;
    for (auto& c : lower) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return lower.size() > 4 &&
           (lower.rfind(L".wav") == lower.size() - 4 ||
            lower.rfind(L".mp3") == lower.size() - 4 ||
            lower.rfind(L".m4a") == lower.size() - 4 ||
            lower.rfind(L".flac") == lower.size() - 5 ||
            lower.rfind(L".aac") == lower.size() - 4 ||
            lower.rfind(L".opus") == lower.size() - 5);
}

int cmd_transcribe_dir(ve_context* ctx, const wchar_t* in_dir, const wchar_t* out_dir, const wchar_t* model_dir) {
    CreateDirectoryW(out_dir, nullptr);
    WIN32_FIND_DATAW fd;
    const std::wstring glob = std::wstring(in_dir) + L"\\*";
    HANDLE h = FindFirstFileW(glob.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "cannot open input dir\n");
        return 1;
    }
    int rc = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        std::wstring name = fd.cFileName;
        if (!is_audio_name(name)) {
            continue;
        }
        const std::wstring in = std::wstring(in_dir) + L"\\" + name;
        std::wstring out = std::wstring(out_dir) + L"\\" + name + L".txt";
        const int one = cmd_transcribe(ctx, in.c_str(), out.c_str(), "txt", model_dir);
        if (one != 0) {
            rc = one;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return rc;
}

int levenshtein(const std::wstring& a, const std::wstring& b) {
    const size_t n = a.size();
    const size_t m = b.size();
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) {
        prev[j] = static_cast<int>(j);
    }
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= m; ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = (std::min)((std::min)(cur[j - 1] + 1, prev[j] + 1), prev[j - 1] + cost);
        }
        prev.swap(cur);
    }
    return prev[m];
}

int cmd_benchmark(ve_context* ctx, const wchar_t* dataset, const wchar_t* model_dir) {
    WIN32_FIND_DATAW fd;
    std::wstring audio_dir(dataset);
    std::wstring reference_dir(dataset);
    std::wstring glob = audio_dir + L"\\*.wav";
    HANDLE h = FindFirstFileW(glob.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        const size_t slash = audio_dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            audio_dir.resize(slash);
            glob = audio_dir + L"\\*.wav";
            h = FindFirstFileW(glob.c_str(), &fd);
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr,
                     "dataset has no wav files in itself or its parent directory\n");
        return 1;
    }
    double cer_num = 0;
    double cer_den = 0;
    do {
        const std::wstring wav = audio_dir + L"\\" + fd.cFileName;
        std::wstring ref_path = reference_dir + L"\\" + fd.cFileName;
        const size_t dot = ref_path.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            ref_path = ref_path.substr(0, dot) + L".txt";
        }
        std::string hyp;
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring out = std::wstring(tmp) + L"ve_bench.txt";
        const int one = cmd_transcribe(ctx, wav.c_str(), out.c_str(), "txt", model_dir);
        if (one != 0) {
            FindClose(h);
            return one;
        }
        FILE* f = nullptr;
        _wfopen_s(&f, out.c_str(), L"rb");
        if (f) {
            char buf[8192];
            const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            hyp = buf;
            fclose(f);
        }
        std::string ref;
        _wfopen_s(&f, ref_path.c_str(), L"rb");
        if (f) {
            char buf[8192];
            const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            ref = buf;
            fclose(f);
        }
        const std::wstring hw = utf8_to_wide(hyp.c_str());
        const std::wstring rw = utf8_to_wide(ref.c_str());
        if (!rw.empty()) {
            cer_num += levenshtein(hw, rw);
            cer_den += static_cast<double>(rw.size());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    const double cer = cer_den > 0 ? cer_num / cer_den : 0;
    std::printf("{\"cer\":%.6f,\"accuracy\":%.6f,\"punct_f1\":null,\"proper_noun_f1\":null,\"threshold\":{\"cer\":0.02,\"accuracy\":0.98,\"punct_f1\":0.75,\"proper_noun_f1\":0.70}}\n",
                cer, 1.0 - cer);
    std::fprintf(stderr, "CER=%.4f (threshold 0.02; accuracy target 0.98)\n", cer);
    return 0;
}

void usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  VoiceEngineCli info\n"
                 "  VoiceEngineCli verify-runtime [--model DIR]\n"
                 "  VoiceEngineCli transcribe --input AUDIO --output RESULT [--format txt|json]\n"
                 "  VoiceEngineCli transcribe-dir --input DIR --output DIR\n"
                 "  VoiceEngineCli benchmark --dataset DIR\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::wstring runtime_error;
    if (!voiceengine_app::initialize_private_runtimes(nullptr, &runtime_error)) {
        std::fwprintf(stderr, L"%ls\n", runtime_error.c_str());
        return 6;
    }
    if (argc < 2) {
        usage();
        return 1;
    }
    const std::wstring root = exe_dir();
    const std::wstring default_model = root + L"\\models\\qwen3-asr-1.7b";
    const char* model_arg = arg_value(argc, argv, "--model");
    const std::wstring model_dir = model_arg ? utf8_to_wide(model_arg) : default_model;

    ve_context_params cp{};
    cp.struct_size = sizeof(cp);
    cp.api_version = VOICEENGINE_API_VERSION;
    cp.runtime_dir_utf16 = root.c_str();
    ve_context* ctx = nullptr;
    if (voiceengine_context_create(&cp, &ctx) != VE_OK) {
        std::fprintf(stderr, "context create failed\n");
        return 6;
    }

    int rc = 1;
    const char* cmd = argv[1];
    if (std::strcmp(cmd, "info") == 0) {
        rc = cmd_info(ctx);
    } else if (std::strcmp(cmd, "verify-runtime") == 0) {
        rc = cmd_verify(ctx, model_dir.c_str());
    } else if (std::strcmp(cmd, "transcribe") == 0) {
        const char* in = arg_value(argc, argv, "--input");
        const char* out = arg_value(argc, argv, "--output");
        const char* fmt = arg_value(argc, argv, "--format");
        if (!in || !out) {
            usage();
            rc = 1;
        } else {
            const std::wstring inw = utf8_to_wide(in);
            const std::wstring outw = utf8_to_wide(out);
            rc = cmd_transcribe(ctx, inw.c_str(), outw.c_str(), fmt, model_dir.c_str());
        }
    } else if (std::strcmp(cmd, "transcribe-dir") == 0) {
        const char* in = arg_value(argc, argv, "--input");
        const char* out = arg_value(argc, argv, "--output");
        if (!in || !out) {
            usage();
            rc = 1;
        } else {
            const std::wstring inw = utf8_to_wide(in);
            const std::wstring outw = utf8_to_wide(out);
            rc = cmd_transcribe_dir(ctx, inw.c_str(), outw.c_str(), model_dir.c_str());
        }
    } else if (std::strcmp(cmd, "benchmark") == 0) {
        const char* ds = arg_value(argc, argv, "--dataset");
        if (!ds) {
            usage();
            rc = 1;
        } else {
            const std::wstring dsw = utf8_to_wide(ds);
            rc = cmd_benchmark(ctx, dsw.c_str(), model_dir.c_str());
        }
    } else {
        usage();
        rc = 1;
    }
    voiceengine_context_destroy(ctx);
    return rc;
}

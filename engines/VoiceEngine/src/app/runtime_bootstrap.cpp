#include "runtime_bootstrap.hpp"
#include "voiceengine/voiceengine_runtime.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <mutex>
#include <string>
#include <cwchar>

namespace voiceengine_app {
namespace {

struct RuntimeState {
    bool ready = false;
    std::wstring error;
    std::array<DLL_DIRECTORY_COOKIE, 3> cookies{};
};

RuntimeState& runtime_state() {
    static RuntimeState state;
    return state;
}

std::once_flag g_runtime_once;

std::wstring executable_directory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, &path[0], static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool is_directory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool is_file(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace

bool initialize_private_runtimes(const wchar_t* runtime_root,
                                 std::wstring* error) {
    const std::wstring requested_root = runtime_root ? runtime_root : L"";
    std::call_once(g_runtime_once, [requested_root] {
        RuntimeState& state = runtime_state();
        const std::wstring root = requested_root.empty()
            ? executable_directory() : requested_root;
        if (root.empty()) {
            state.error = L"Cannot determine the executable directory.";
            return;
        }
        const std::array<std::wstring, 3> directories = {
            root,
            root + L"\\runtimes\\ffmpeg",
            root + L"\\runtimes\\cuda",
        };
        for (size_t i = 1; i < directories.size(); ++i) {
            const std::wstring& directory = directories[i];
            if (!is_directory(directory)) {
                state.error = L"Private runtime directory is missing: " + directory;
                return;
            }
        }
        const std::array<std::wstring, 8> required_files = {
            root + L"\\VoiceEngineCore.dll",
            directories[1] + L"\\avcodec-62.dll",
            directories[1] + L"\\avformat-62.dll",
            directories[1] + L"\\avutil-60.dll",
            directories[1] + L"\\swresample-6.dll",
            directories[2] + L"\\cudart64_12.dll",
            directories[2] + L"\\cublas64_12.dll",
            directories[2] + L"\\cublasLt64_12.dll",
        };
        for (const std::wstring& file : required_files) {
            if (!is_file(file)) {
                state.error = L"Private runtime file is missing: " + file;
                return;
            }
        }
        if (!SetDefaultDllDirectories(
                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS)) {
            state.error = L"SetDefaultDllDirectories failed: "
                + std::to_wstring(GetLastError());
            return;
        }
        for (size_t i = 0; i < directories.size(); ++i) {
            state.cookies[i] = AddDllDirectory(directories[i].c_str());
            if (!state.cookies[i]) {
                state.error = L"AddDllDirectory failed for " + directories[i]
                    + L": " + std::to_wstring(GetLastError());
                return;
            }
        }
        state.ready = true;
    });
    const RuntimeState& state = runtime_state();
    if (error)
        *error = state.error;
    return state.ready;
}

}  // namespace voiceengine_app

int32_t voiceengine_runtime_initialize(const wchar_t* runtime_root_utf16,
                                      wchar_t* error_buffer,
                                      uint32_t error_buffer_chars) {
    std::wstring error;
    const bool ready = voiceengine_app::initialize_private_runtimes(
        runtime_root_utf16, &error);
    if (error_buffer && error_buffer_chars > 0) {
        wcsncpy_s(error_buffer, error_buffer_chars, error.c_str(), _TRUNCATE);
    }
    return ready ? 1 : 0;
}

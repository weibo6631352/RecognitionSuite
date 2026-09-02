#include "scanengine/scanengine_runtime.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cwchar>
#include <mutex>
#include <string>

namespace {

struct RuntimeState {
    bool ready = false;
    std::wstring error;
    std::array<DLL_DIRECTORY_COOKIE, 2> cookies{};
};

RuntimeState& state() {
    static RuntimeState value;
    return value;
}

std::once_flag g_once;

std::wstring executableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, &path[0], static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool isFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace

int32_t scanengine_runtime_initialize(const wchar_t* runtime_root_utf16,
                                    wchar_t* error_buffer,
                                    uint32_t error_buffer_chars) {
    const std::wstring requested = runtime_root_utf16
        ? runtime_root_utf16 : L"";
    std::call_once(g_once, [requested] {
        RuntimeState& runtime = state();
        const std::wstring root = requested.empty()
            ? executableDirectory() : requested;
        if (root.empty()) {
            runtime.error = L"Cannot determine the ScanEngine runtime root.";
            return;
        }
        constexpr const wchar_t* requiredRootFiles[] = {
            L"ScanEngineCore.dll",
            L"Qt5Core.dll",
            L"Qt5Gui.dll",
            L"onnxruntime.dll",
            L"pdfium.dll",
            L"imageformats\\qjpeg.dll",
        };
        for (const wchar_t* name : requiredRootFiles) {
            const std::wstring path = root + L"\\" + name;
            if (!isFile(path)) {
                runtime.error = L"Runtime file is missing: " + path;
                return;
            }
        }
        const std::wstring cuda = root + L"\\runtimes\\cuda";
        constexpr const wchar_t* required[] = {
            L"cudart64_12.dll",
            L"cublasLt64_12.dll",
            L"nvrtc64_120_0.dll",
            L"nvrtc-builtins64_129.dll",
            L"cudnn64_9.dll",
            L"cudnn_graph64_9.dll",
            L"cudnn_ops64_9.dll",
            L"cudnn_cnn64_9.dll",
            L"cudnn_adv64_9.dll",
            L"cudnn_heuristic64_9.dll",
            L"cudnn_engines_precompiled64_9.dll",
            L"cudnn_engines_runtime_compiled64_9.dll",
        };
        for (const wchar_t* name : required) {
            const std::wstring path = cuda + L"\\" + name;
            if (!isFile(path)) {
                runtime.error = L"Private GPU runtime file is missing: " + path;
                return;
            }
        }
        if (!SetDefaultDllDirectories(
                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS)) {
            runtime.error = L"SetDefaultDllDirectories failed: "
                + std::to_wstring(GetLastError());
            return;
        }
        const std::array<std::wstring, 2> directories = {root, cuda};
        for (size_t i = 0; i < directories.size(); ++i) {
            runtime.cookies[i] = AddDllDirectory(directories[i].c_str());
            if (!runtime.cookies[i]) {
                runtime.error = L"AddDllDirectory failed for " + directories[i]
                    + L": " + std::to_wstring(GetLastError());
                return;
            }
        }
        const std::wstring plugins = root + L"\\platforms";
        SetEnvironmentVariableW(L"QT_QPA_PLATFORM_PLUGIN_PATH", plugins.c_str());
        runtime.ready = true;
    });
    if (error_buffer && error_buffer_chars > 0) {
        wcsncpy_s(error_buffer, error_buffer_chars,
                  state().error.c_str(), _TRUNCATE);
    }
    return state().ready ? 1 : 0;
}

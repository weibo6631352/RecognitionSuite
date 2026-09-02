#include "internal.hpp"

#include <windows.h>

namespace voiceengine {

static HMODULE this_module() {
    HMODULE m = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&this_module),
        &m);
    return m;
}

std::string exe_or_dll_dir() {
    wchar_t buf[MAX_PATH * 4];
    HMODULE m = this_module();
    DWORD n = GetModuleFileNameW(m, buf, MAX_PATH * 4);
    if (n == 0) {
        return ".";
    }
    std::wstring path(buf, buf + n);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return ".";
    }
    return wide_to_utf8(path.substr(0, slash).c_str());
}

bool add_private_runtime_directory(const std::wstring& dir) {
    if (dir.empty()) {
        return false;
    }
    static bool defaults = false;
    if (!defaults) {
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        defaults = true;
    }
    const DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(dir.c_str());
    return cookie != nullptr || GetLastError() == ERROR_FILE_NOT_FOUND;
}

void init_private_runtimes(const std::wstring& runtime_dir) {
    std::wstring root = runtime_dir;
    if (root.empty()) {
        root = utf8_to_wide(exe_or_dll_dir());
    }
    const std::wstring ffmpeg = root + L"\\runtimes\\ffmpeg";
    const std::wstring cuda = root + L"\\runtimes\\cuda";
    add_private_runtime_directory(ffmpeg);
    add_private_runtime_directory(cuda);
    add_private_runtime_directory(root);
}

}  // namespace voiceengine

#include "internal.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace voiceengine {

std::string wide_to_utf8(const wchar_t* s) {
    if (!s || !*s) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    return out;
}

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '\\': o += "\\\\"; break;
        case '"': o += "\\\""; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                o += buf;
            } else {
                o.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return o;
}

char* dll_strdup(const std::string& s) {
    const size_t n = s.size() + 1;
    char* p = static_cast<char*>(std::malloc(n));
    if (!p) {
        return nullptr;
    }
    std::memcpy(p, s.c_str(), n);
    return p;
}

void set_thread_last_error(std::string* slot, const std::string& msg) {
    if (slot) {
        *slot = msg;
    }
}

std::atomic<int32_t> g_cpu_asr_started{0};

}  // namespace voiceengine

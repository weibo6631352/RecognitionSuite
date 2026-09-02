#include "internal.hpp"

#include <cctype>

namespace voiceengine {
namespace {

bool is_cjk(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF);
}

bool next_cp(const std::string& s, size_t& i, uint32_t& cp) {
    if (i >= s.size()) {
        return false;
    }
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        cp = c;
        ++i;
        return true;
    }
    int need = 0;
    if ((c & 0xE0) == 0xC0) {
        need = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        need = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        need = 3;
        cp = c & 0x07;
    } else {
        cp = c;
        ++i;
        return true;
    }
    ++i;
    for (int k = 0; k < need && i < s.size(); ++k, ++i) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    }
    return true;
}

void append_cp(std::string& o, uint32_t cp) {
    if (cp < 0x80) {
        o.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        o.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        o.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        o.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        o.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

uint32_t map_punct(uint32_t cp) {
    switch (cp) {
    case ',': return 0xFF0C;
    case '.': return 0x3002;
    case '!': return 0xFF01;
    case '?': return 0xFF1F;
    case ';': return 0xFF1B;
    case ':': return 0xFF1A;
    case 0xFF0E: return 0x3002;
    default: return cp;
    }
}

bool is_punct(uint32_t cp) {
    return cp == 0x3002 || cp == 0xFF0C || cp == 0xFF01 || cp == 0xFF1F ||
           cp == 0xFF1B || cp == 0xFF1A || cp == 0x2026 || cp == ',' ||
           cp == '.' || cp == '!' || cp == '?';
}

}  // namespace

std::string normalize_zh_text(const std::string& in) {
    std::string src = in;
    const char* tag = "<asr_text>";
    const size_t tp = src.find(tag);
    if (tp != std::string::npos) {
        src = src.substr(tp + 10);
    } else if (src.rfind("language ", 0) == 0) {
        const size_t nl = src.find('\n');
        src = nl == std::string::npos ? src : src.substr(nl + 1);
    }
    std::string tmp;
    tmp.reserve(src.size());
    size_t i = 0;
    uint32_t cp = 0;
    uint32_t prev = 0;
    int punct_run = 0;
    while (next_cp(src, i, cp)) {
        if (cp == '\r') {
            continue;
        }
        if (cp == '\n' || cp == '\t') {
            cp = ' ';
        }
        cp = map_punct(cp);
        if (is_punct(cp)) {
            if (cp == prev) {
                ++punct_run;
                if (punct_run >= 2) {
                    continue;
                }
            } else {
                punct_run = 1;
            }
        } else {
            punct_run = 0;
        }
        if (cp == ' ' && (prev == ' ' || is_cjk(prev))) {
            uint32_t look = 0;
            size_t j = i;
            if (next_cp(src, j, look) && is_cjk(look)) {
                continue;
            }
            if (prev == ' ') {
                continue;
            }
        }
        append_cp(tmp, cp);
        prev = cp;
    }
    size_t a = 0;
    while (a < tmp.size() && tmp[a] == ' ') {
        ++a;
    }
    size_t b = tmp.size();
    while (b > a && tmp[b - 1] == ' ') {
        --b;
    }
    return tmp.substr(a, b - a);
}

std::string merge_overlap_text(const std::vector<std::string>& parts) {
    std::string acc;
    for (const auto& part : parts) {
        std::string p = normalize_zh_text(part);
        if (p.empty()) {
            continue;
        }
        if (acc.empty()) {
            acc = p;
            continue;
        }
        const size_t maxk = acc.size() < p.size() ? acc.size() : p.size();
        const size_t limit = maxk > 48 ? 48 : maxk;
        size_t best = 0;
        for (size_t k = limit; k >= 2; --k) {
            if (acc.size() >= k && p.compare(0, k, acc, acc.size() - k, k) == 0) {
                best = k;
                break;
            }
        }
        if (best == 0 && !acc.empty() && !p.empty()) {
            const unsigned char last = static_cast<unsigned char>(acc.back());
            const unsigned char first = static_cast<unsigned char>(p.front());
            if (last == first && last < 0x80) {
                best = 1;
            }
        }
        acc += p.substr(best);
    }
    return normalize_zh_text(acc);
}

}  // namespace voiceengine

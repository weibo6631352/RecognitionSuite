#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace voiceengine {

std::vector<Slice> silence_slice(const float* samples, uint64_t n, int sr, const SliceConfig& cfg) {
    std::vector<Slice> out;
    if (!samples || n == 0 || sr <= 0) {
        return out;
    }
    const uint64_t frame = std::max<uint64_t>(1, static_cast<uint64_t>(cfg.frame_sec * sr));
    const uint64_t target = static_cast<uint64_t>(cfg.target_sec * sr);
    const uint64_t maxn = static_cast<uint64_t>(cfg.max_sec * sr);
    const uint64_t overlap = static_cast<uint64_t>(cfg.overlap_sec * sr);
    const uint64_t min_sil = static_cast<uint64_t>(cfg.min_silence_sec * sr);

    std::vector<float> rms;
    rms.reserve(static_cast<size_t>(n / frame) + 1);
    for (uint64_t i = 0; i < n; i += frame) {
        const uint64_t e = std::min(n, i + frame);
        double acc = 0;
        for (uint64_t k = i; k < e; ++k) {
            const double v = samples[k];
            acc += v * v;
        }
        rms.push_back(static_cast<float>(std::sqrt(acc / static_cast<double>(e - i))));
    }
    std::vector<float> sorted = rms;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const float med = sorted.empty() ? 0.0f : sorted[sorted.size() / 2];
    const float thr = std::max(0.008f, med * 0.35f);

    auto silent_at = [&](uint64_t pos) {
        const size_t fi = static_cast<size_t>(pos / frame);
        return fi < rms.size() && rms[fi] < thr;
    };

    uint64_t start = 0;
    while (start < n) {
        uint64_t end = std::min(n, start + target);
        if (end < n) {
            uint64_t cut = end;
            const uint64_t search0 = end > min_sil ? end - min_sil : start;
            bool found = false;
            for (uint64_t p = end; p > search0; --p) {
                if (silent_at(p)) {
                    uint64_t q = p;
                    while (q > start && silent_at(q)) {
                        --q;
                    }
                    if (end - q >= min_sil / 2) {
                        cut = std::min(n, q + min_sil / 4);
                        found = true;
                        break;
                    }
                }
            }
            if (found) {
                end = std::max(start + frame, cut);
            }
        }
        if (end - start > maxn) {
            end = start + maxn;
        }
        if (end <= start) {
            end = std::min(n, start + frame);
        }
        Slice sl;
        sl.offset_samples = start;
        sl.n_samples = end - start;
        sl.start_sec = static_cast<double>(start) / sr;
        sl.end_sec = static_cast<double>(end) / sr;
        out.push_back(sl);
        if (end >= n) {
            break;
        }
        uint64_t next = end;
        if (overlap > 0 && end > start + overlap) {
            next = end - overlap;
        }
        if (next <= start) {
            next = end;
        }
        start = next;
    }
    return out;
}

std::string slices_to_json(const std::vector<Slice>& slices) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < slices.size(); ++i) {
        if (i) {
            o << ",";
        }
        o << "{\"start\":" << slices[i].start_sec
          << ",\"end\":" << slices[i].end_sec
          << ",\"offset\":" << slices[i].offset_samples
          << ",\"n_samples\":" << slices[i].n_samples << "}";
    }
    o << "]";
    return o.str();
}

static bool parse_u64_field(const std::string& s, const char* key, uint64_t* v) {
    const std::string pat = std::string("\"") + key + "\":";
    const size_t p = s.find(pat);
    if (p == std::string::npos) {
        return false;
    }
    *v = static_cast<uint64_t>(std::strtoull(s.c_str() + p + pat.size(), nullptr, 10));
    return true;
}

static bool parse_f64_field(const std::string& s, const char* key, double* v) {
    const std::string pat = std::string("\"") + key + "\":";
    const size_t p = s.find(pat);
    if (p == std::string::npos) {
        return false;
    }
    *v = std::strtod(s.c_str() + p + pat.size(), nullptr);
    return true;
}

bool slices_from_json(const std::string& json, std::vector<Slice>* out) {
    if (!out) {
        return false;
    }
    out->clear();
    size_t i = 0;
    while (i < json.size()) {
        const size_t a = json.find('{', i);
        if (a == std::string::npos) {
            break;
        }
        const size_t b = json.find('}', a);
        if (b == std::string::npos) {
            break;
        }
        const std::string obj = json.substr(a, b - a + 1);
        Slice sl;
        parse_f64_field(obj, "start", &sl.start_sec);
        parse_f64_field(obj, "end", &sl.end_sec);
        parse_u64_field(obj, "offset", &sl.offset_samples);
        parse_u64_field(obj, "n_samples", &sl.n_samples);
        out->push_back(sl);
        i = b + 1;
    }
    return true;
}

}  // namespace voiceengine

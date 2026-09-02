#include "core/internal.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace voiceengine {
namespace {

#pragma pack(push, 1)
struct RiffHdr {
    char riff[4];
    uint32_t size;
    char wave[4];
};
struct ChunkHdr {
    char id[4];
    uint32_t size;
};
struct FmtChunk {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
};
#pragma pack(pop)

bool read_all(const wchar_t* path, std::vector<unsigned char>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0) {
        return false;
    }
    f.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(out->data()), n);
    return static_cast<std::streamoff>(f.gcount()) == n;
}

void resample_linear(const std::vector<float>& in, int in_sr, int out_sr, std::vector<float>* out) {
    if (in_sr == out_sr) {
        *out = in;
        return;
    }
    if (in.empty()) {
        out->clear();
        return;
    }
    const double ratio = static_cast<double>(in_sr) / out_sr;
    const size_t nout = static_cast<size_t>(std::max<double>(1.0, std::floor(in.size() / ratio)));
    out->resize(nout);
    for (size_t i = 0; i < nout; ++i) {
        const double src = i * ratio;
        const size_t i0 = static_cast<size_t>(src);
        const size_t i1 = (std::min)(in.size() - 1, i0 + 1);
        const double t = src - static_cast<double>(i0);
        (*out)[i] = static_cast<float>(in[i0] * (1.0 - t) + in[i1] * t);
    }
}

}  // namespace

bool pcm_to_16k_mono(const float* samples, uint64_t n, int sr, int ch, Pcm16k* out, std::string* err) {
    if (!samples || n == 0 || sr <= 0 || ch <= 0 || !out) {
        if (err) {
            *err = "invalid PCM descriptor";
        }
        return false;
    }
    std::vector<float> mono;
    const uint64_t frames = n / static_cast<uint64_t>(ch);
    if (ch == 1) {
        mono.assign(samples, samples + frames);
    } else {
        mono.resize(static_cast<size_t>(frames));
        for (uint64_t i = 0; i < frames; ++i) {
            double acc = 0;
            for (int c = 0; c < ch; ++c) {
                acc += samples[i * static_cast<uint64_t>(ch) + static_cast<uint64_t>(c)];
            }
            mono[static_cast<size_t>(i)] = static_cast<float>(acc / ch);
        }
    }
    resample_linear(mono, sr, 16000, &out->samples);
    out->sample_rate = 16000;
    return true;
}

bool decode_wav_file(const wchar_t* path_utf16, Pcm16k* out, std::string* err) {
    if (!path_utf16 || !out) {
        if (err) {
            *err = "null wav path";
        }
        return false;
    }
    std::vector<unsigned char> buf;
    if (!read_all(path_utf16, &buf) || buf.size() < sizeof(RiffHdr) + sizeof(ChunkHdr)) {
        if (err) {
            *err = "cannot read wav file: " + wide_to_utf8(path_utf16);
        }
        return false;
    }
    RiffHdr rh{};
    std::memcpy(&rh, buf.data(), sizeof(rh));
    if (std::memcmp(rh.riff, "RIFF", 4) != 0 || std::memcmp(rh.wave, "WAVE", 4) != 0) {
        if (err) {
            *err = "not a RIFF/WAVE file";
        }
        return false;
    }
    FmtChunk fmt{};
    bool have_fmt = false;
    const unsigned char* data = nullptr;
    uint32_t data_size = 0;
    size_t off = sizeof(RiffHdr);
    while (off + sizeof(ChunkHdr) <= buf.size()) {
        ChunkHdr ch{};
        std::memcpy(&ch, buf.data() + off, sizeof(ch));
        off += sizeof(ChunkHdr);
        if (off + ch.size > buf.size()) {
            break;
        }
        if (std::memcmp(ch.id, "fmt ", 4) == 0 && ch.size >= 16) {
            std::memcpy(&fmt, buf.data() + off, sizeof(fmt));
            have_fmt = true;
        } else if (std::memcmp(ch.id, "data", 4) == 0) {
            data = buf.data() + off;
            data_size = ch.size;
        }
        off += ch.size + (ch.size & 1);
    }
    if (!have_fmt || !data) {
        if (err) {
            *err = "wav missing fmt/data chunk";
        }
        return false;
    }
    if (fmt.channels == 0 || fmt.sample_rate == 0 || fmt.bits == 0) {
        if (err) {
            *err = "invalid wav format header";
        }
        return false;
    }
    const int ch = fmt.channels;
    const int bytes = fmt.bits / 8;
    if (bytes <= 0) {
        if (err) {
            *err = "unsupported wav bit depth";
        }
        return false;
    }
    const uint32_t frames = data_size / static_cast<uint32_t>(bytes * ch);
    std::vector<float> pcm(static_cast<size_t>(frames) * static_cast<size_t>(ch));
    for (uint32_t i = 0; i < frames; ++i) {
        for (int c = 0; c < ch; ++c) {
            const unsigned char* p = data + (static_cast<size_t>(i) * ch + c) * bytes;
            float v = 0;
            if (fmt.format == 3 && fmt.bits == 32) {
                std::memcpy(&v, p, 4);
            } else if (fmt.bits == 16) {
                int16_t s = 0;
                std::memcpy(&s, p, 2);
                v = static_cast<float>(s) / 32768.0f;
            } else if (fmt.bits == 8) {
                v = (static_cast<float>(p[0]) - 128.0f) / 128.0f;
            } else if (fmt.bits == 24) {
                int32_t s = p[0] | (p[1] << 8) | (p[2] << 16);
                if (s & 0x800000) {
                    s |= ~0xFFFFFF;
                }
                v = static_cast<float>(s) / 8388608.0f;
            } else if (fmt.bits == 32 && fmt.format != 3) {
                int32_t s = 0;
                std::memcpy(&s, p, 4);
                v = static_cast<float>(s) / 2147483648.0f;
            } else {
                if (err) {
                    *err = "unsupported wav sample format";
                }
                return false;
            }
            pcm[static_cast<size_t>(i) * ch + c] = v;
        }
    }
    return pcm_to_16k_mono(pcm.data(), pcm.size(), static_cast<int>(fmt.sample_rate), ch, out, err);
}

}  // namespace voiceengine

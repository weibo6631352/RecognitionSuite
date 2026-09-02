#include "core/internal.hpp"

#include <fstream>

namespace voiceengine {
namespace {

bool looks_like_wav(const wchar_t* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    char hdr[12] = {};
    f.read(hdr, 12);
    return f.gcount() == 12 &&
           hdr[0] == 'R' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == 'F' &&
           hdr[8] == 'W' && hdr[9] == 'A' && hdr[10] == 'V' && hdr[11] == 'E';
}

}  // namespace

bool decode_audio_file(const wchar_t* path_utf16, Pcm16k* out, std::string* err) {
    if (!path_utf16 || !out) {
        if (err) {
            *err = "null audio path";
        }
        return false;
    }
    if (looks_like_wav(path_utf16)) {
        return decode_wav_file(path_utf16, out, err);
    }
    return decode_ffmpeg_file(path_utf16, out, err);
}

}  // namespace voiceengine

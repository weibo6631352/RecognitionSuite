#include "internal.hpp"

#include <sstream>

namespace voiceengine {

std::string build_result_json(const std::string& text,
                              const std::string& device,
                              const std::string& model,
                              double duration_sec,
                              const std::vector<std::string>& segment_texts,
                              const std::vector<Slice>& slices) {
    std::ostringstream o;
    o << "{\"schema_version\":1,\"text\":\"" << json_escape(text) << "\""
      << ",\"language\":\"zh\""
      << ",\"duration_sec\":" << duration_sec
      << ",\"model\":\"" << json_escape(model) << "\""
      << ",\"device\":\"" << json_escape(device) << "\""
      << ",\"segments\":[";
    const size_t n = segment_texts.size();
    for (size_t i = 0; i < n; ++i) {
        if (i) {
            o << ",";
        }
        double start = 0;
        double end = duration_sec;
        if (i < slices.size()) {
            start = slices[i].start_sec;
            end = slices[i].end_sec;
        }
        o << "{\"start\":" << start << ",\"end\":" << end
          << ",\"text\":\"" << json_escape(segment_texts[i]) << "\"}";
    }
    o << "]}";
    return o.str();
}

}  // namespace voiceengine

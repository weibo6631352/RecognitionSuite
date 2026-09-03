#include "internal.hpp"

#include <sstream>

namespace voiceengine {

std::string build_result_json(const std::string& text,
                              const std::string& device,
                              const std::string& model,
                              double duration_sec,
                              const std::vector<std::string>& segment_texts,
                              const std::vector<Slice>& slices,
                              const std::vector<SegmentConfidence>& confidences) {
    std::ostringstream o;
    o << "{\"schema_version\":2,\"text\":\"" << json_escape(text) << "\""
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
          << ",\"text\":\"" << json_escape(segment_texts[i]) << "\"";
        if (i < confidences.size()) {
            o << ",\"confidence\":{\"value\":" << confidences[i].probability
              << ",\"source\":\"decoder_token_geometric_mean\""
              << ",\"calibrated\":false}"
              << ",\"tokens\":[";
            for (size_t tokenIndex = 0;
                 tokenIndex < confidences[i].tokens.size(); ++tokenIndex) {
                if (tokenIndex)
                    o << ",";
                const TokenConfidence& token = confidences[i].tokens[tokenIndex];
                o << "{\"text\":\"" << json_escape(token.text)
                  << "\",\"confidence\":" << token.probability << "}";
            }
            o << "]";
        }
        o << "}";
    }
    o << "]}";
    return o.str();
}

}  // namespace voiceengine

#pragma once

#include <QString>

namespace scanengine {
namespace hybrid {

struct VlmPaths {
    QString snapshotDir;
    QString configJson;
    QString weights;  // model.safetensors
    QString tokenizerJson;
    QString preprocessorJson;
    QString chatTemplate;
};

// Resolve the official VLM directory from scanengine.json models-dir.vlm.
VlmPaths resolveOfficialVlmPaths();
QString vlmPathsMissing(const VlmPaths& p);

}  // namespace hybrid
}  // namespace scanengine

#pragma once

#include "scanengine/hybrid/safetensors.hpp"

#include <QString>

namespace scanengine {
namespace hybrid {

// mlx_vlm.models.qwen2_vl.Model.sanitize key remap (names only; no conv transpose).
QString remapMlxQwen2VlKey(const QString& key);

struct OfficialWeightIndex {
    SafetensorsFile file;
    int tensorCount = 0;
    int visualCount = 0;
    int languageCount = 0;
    bool hasLmHead = false;
    bool hasEmbed = false;
    QString embedOriginalName;
    QString embedRemappedName;
    QVector<qint64> embedShape;
};

bool indexOfficialWeights(const QString& safetensorsPath, OfficialWeightIndex* out, QString* err);

}  // namespace hybrid
}  // namespace scanengine

#pragma once

#include "scanengine/hybrid/preprocessor.hpp"
#include "scanengine/hybrid/qwen2_language.hpp"
#include "scanengine/hybrid/vlm_config.hpp"
#include "scanengine/hybrid/vlm_weights.hpp"

#include <QVector>

namespace scanengine {
namespace hybrid {

// mlx_vlm.models.qwen2_vl.vision.VisionModel
struct Qwen2VisionBlockWeights {
    Qwen2Linear qkv;
    Qwen2Linear proj;
    Qwen2Linear fc1;
    Qwen2Linear fc2;
    QVector<float> norm1W;
    QVector<float> norm1B;
    QVector<float> norm2W;
    QVector<float> norm2B;
};

struct Qwen2VisionModel {
    VlmModelConfig cfg;
    QVector<float> patchEmbed;  // [1280, 3, 2, 14, 14]
    QVector<Qwen2VisionBlockWeights> blocks;
    QVector<float> lnqW;
    QVector<float> lnqB;
    Qwen2Linear merge0;
    Qwen2Linear merge2;
    QVector<float> visionInvFreq;  // 20 freqs (head_dim/4)
};

bool loadOfficialVisionModel(const OfficialWeightIndex& weights,
                             const VlmModelConfig& cfg,
                             Qwen2VisionModel* out,
                             QString* err);

bool officialVisionModel(Qwen2VisionModel** out, QString* err);

// pixelValues: [N, 3*t*p*p] official flatten. outFeatures: [N/merge^2, hidden]
bool forwardOfficialVision(const Qwen2VisionModel& model,
                           const OfficialImagePatches& patches,
                           QVector<float>* outFeatures,
                           QString* err);

}  // namespace hybrid
}  // namespace scanengine

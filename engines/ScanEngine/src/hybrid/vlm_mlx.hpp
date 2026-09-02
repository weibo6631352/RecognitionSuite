#pragma once

#if defined(SCANENGINE_MLX)
#include "scanengine/hybrid/mlx_windows_static.hpp"
#include "scanengine/hybrid/qwen2_language.hpp"
#include "scanengine/hybrid/qwen2_vision.hpp"

#include <mlx/mlx.h>

namespace mx = mlx::core;

namespace scanengine {
namespace hybrid {

bool forwardOfficialVisionDevice(const Qwen2VisionModel& model,
                                 const OfficialImagePatches& patches,
                                 mx::array* outFeatures,
                                 QString* err);

bool mergeOfficialImageEmbeds(const Qwen2LanguageModel& model,
                              const QVector<int>& ids,
                              const mx::array& imageFeatures,
                              mx::array* outEmbeds,
                              QString* err);

bool forwardOfficialLanguageToken(const Qwen2LanguageModel& model,
                                  const QVector<int>& ids,
                                  mx::array* outToken,
                                  QString* err,
                                  const mx::array* inputsEmbeds,
                                  const mx::array* tokenIn,
                                  const int* posT,
                                  const int* posH,
                                  const int* posW,
                                  Qwen2KvCache* cache);

}  // namespace hybrid
}  // namespace scanengine
#endif

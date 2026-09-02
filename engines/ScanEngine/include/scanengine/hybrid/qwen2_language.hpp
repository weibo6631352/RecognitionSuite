#pragma once

#include "scanengine/hybrid/tokenizer.hpp"
#include "scanengine/hybrid/vlm_config.hpp"
#include "scanengine/hybrid/vlm_weights.hpp"

#include <QVector>

namespace scanengine {
namespace hybrid {

// mlx_vlm.models.qwen2_vl.language.LanguageModel (text-only path).
// Weights come from the official model.safetensors (tied embed = lm_head).
struct Qwen2Linear {
    int out = 0;
    int in = 0;
    QVector<float> weight;  // [out, in] PyTorch layout
    QVector<float> bias;    // empty if none
};

struct Qwen2DecoderLayerWeights {
    Qwen2Linear qProj;
    Qwen2Linear kProj;
    Qwen2Linear vProj;
    Qwen2Linear oProj;
    Qwen2Linear gateProj;
    Qwen2Linear upProj;
    Qwen2Linear downProj;
    QVector<float> inputNorm;
    QVector<float> postNorm;
};

struct Qwen2LanguageModel {
    VlmModelConfig cfg;
    QVector<float> embed;  // [vocab, hidden]
    QVector<Qwen2DecoderLayerWeights> layers;
    QVector<float> finalNorm;
    QVector<float> invFreq;  // head_dim/2
    int mropeSection[3] = {8, 12, 12};
};

bool loadOfficialLanguageModel(const OfficialWeightIndex& weights,
                               const VlmModelConfig& cfg,
                               Qwen2LanguageModel* out,
                               QString* err);

// Resolve official snapshot and cache the language stack in-process.
bool officialLanguageModel(Qwen2LanguageModel** out, QString* err);

// mlx_lm KVCache: per-layer K/V, length = cached tokens (after RoPE).
struct Qwen2KvCache {
    int len = 0;
    QVector<QVector<float>> k;
    QVector<QVector<float>> v;
};

void resetOfficialKvCache(const Qwen2LanguageModel& model, Qwen2KvCache* cache);

bool forwardOfficialLanguage(const Qwen2LanguageModel& model,
                             const QVector<int>& ids,
                             QVector<float>* lastLogits,
                             QString* err,
                             const float* inputsEmbeds = nullptr,
                             const int* posT = nullptr,
                             const int* posH = nullptr,
                             const int* posW = nullptr,
                             Qwen2KvCache* cache = nullptr);

// Greedy sample from one forward. Metal evals token id + KV (no vocab memcpy).
bool forwardOfficialLanguageArgmax(const Qwen2LanguageModel& model,
                                   const QVector<int>& ids,
                                   int* lastArgmax,
                                   float* lastMax,
                                   QString* err,
                                   const float* inputsEmbeds = nullptr,
                                   const int* posT = nullptr,
                                   const int* posH = nullptr,
                                   const int* posW = nullptr,
                                   Qwen2KvCache* cache = nullptr);

struct LanguageGenerateResult {
    QVector<int> promptIds;
    QVector<int> newIds;
    QString decoded;
    int lastArgmax = -1;
    float lastMax = 0;
};

// Greedy decode (mineru DEFAULT_SAMPLING: temperature=0, top_k=1).
bool generateOfficialLanguageGreedy(const Qwen2LanguageModel& model,
                                    const OfficialTokenizer& tok,
                                    const QVector<int>& promptIds,
                                    int maxNewTokens,
                                    LanguageGenerateResult* out,
                                    QString* err);

}  // namespace hybrid
}  // namespace scanengine

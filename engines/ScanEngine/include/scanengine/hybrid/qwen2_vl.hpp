#pragma once

#include "scanengine/hybrid/preprocessor.hpp"
#include "scanengine/hybrid/prompts.hpp"
#include "scanengine/hybrid/qwen2_language.hpp"
#include "scanengine/hybrid/qwen2_vision.hpp"
#include "scanengine/hybrid/tokenizer.hpp"

#include <QImage>
#include <functional>
#include <QVector>

namespace scanengine {
namespace hybrid {

QVector<int> expandOfficialImagePads(const QVector<int>& ids, int imageTokenId, int nVisionTokens);

// transformers 4.57.6 NoRepeatNGramLogitsProcessor for one decoder-only
// hypothesis. The history is the complete input_ids row, including prompt
// tokens. A non-positive size follows GenerationMixin and disables the
// processor. Completion ids preserve the processor's left-to-right order and
// duplicates.
QVector<int> officialNoRepeatNgramBannedTokens(const QVector<int>& history,
                                               int ngramSize);

// Apply the processor logically (banned scores are -inf), then use the greedy
// torch.argmax contract: the lowest token id wins a tie. Returns -1 only for
// an empty logits vector.
int officialNoRepeatNgramArgmax(const QVector<float>& logits,
                                const QVector<int>& history,
                                int ngramSize,
                                float* selectedScore = nullptr);

// language.py LanguageModel.get_rope_index for one image.
bool officialGetRopeIndex(const QVector<int>& ids,
                          int gridT,
                          int gridH,
                          int gridW,
                          int spatialMerge,
                          int imageTokenId,
                          int visionStartTokenId,
                          QVector<int>* posT,
                          QVector<int>* posH,
                          QVector<int>* posW,
                          QString* err);

struct VlmGenerateResult {
    QVector<int> promptIds;
    QVector<int> newIds;
    QString decoded;
    int lastArgmax = -1;
    float lastMax = 0;
    int numVisionTokens = 0;
};

enum class VlmLengthContract {
    // transformers_client.py passes max_length=model.config.max_position_embeddings
    // when MinerUSamplingParams.max_new_tokens is unset. This is a total
    // decoder sequence length, including the expanded image-pad prompt.
    TransformersMaxLength,
    // Public diagnostics may explicitly request max_new_tokens instead.
    ExplicitMaxNewTokens,
};

struct VlmGenerationContract {
    VlmLengthContract lengthContract = VlmLengthContract::TransformersMaxLength;
    int tokenLimit = 0;
    OfficialSampling sampling;

    static VlmGenerationContract transformersMaxLength(
        int maxLength, const OfficialSampling& sampling);
    static VlmGenerationContract explicitMaxNewTokens(
        int maxNewTokens, const OfficialSampling& sampling);
};

// GenerationMixin._validate_generated_length + max_length stopping budget for
// a decoder-only model. `expandedPromptLength >= maxLength` is an error; an
// 8191-token prompt under the official 8192 cap has a one-token budget.
bool officialTransformersNewTokenBudget(int expandedPromptLength,
                                        int maxLength,
                                        int* maxNewTokens,
                                        QString* err = nullptr);

bool generateOfficialVlmGreedy(const Qwen2LanguageModel& language,
                               const Qwen2VisionModel& vision,
                               const OfficialTokenizer& tok,
                               const QVector<int>& promptIds,
                               const OfficialImagePatches& patches,
                               const VlmGenerationContract& contract,
                               VlmGenerateResult* out,
                               QString* err,
                               const std::function<bool()>& isCancelled = {});

}  // namespace hybrid
}  // namespace scanengine

#include "scanengine/hybrid/qwen2_vl.hpp"
#include "scanengine/hybrid/canonical_generation.hpp"
#include "vlm_mlx.hpp"

#include <QByteArray>
#include <QFile>
#include <QSet>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace scanengine {
namespace hybrid {

QVector<int> expandOfficialImagePads(const QVector<int>& ids, int imageTokenId, int nVisionTokens) {
    QVector<int> out;
    out.reserve(ids.size() + std::max(0, nVisionTokens - 1));
    bool expanded = false;
    for (int id : ids) {
        if (id == imageTokenId && !expanded) {
            for (int i = 0; i < nVisionTokens; ++i)
                out.push_back(imageTokenId);
            expanded = true;
        } else {
            out.push_back(id);
        }
    }
    return out;
}

QVector<int> officialNoRepeatNgramBannedTokens(const QVector<int>& history,
                                               int ngramSize) {
    return canonicalNoRepeatNgramBannedTokens(history, ngramSize);
}

int officialNoRepeatNgramArgmax(const QVector<float>& logits,
                                const QVector<int>& history,
                                int ngramSize,
                                float* selectedScore) {
    if (logits.isEmpty()) {
        if (selectedScore)
            *selectedScore = -std::numeric_limits<float>::infinity();
        return -1;
    }

    const QVector<int> banned =
        officialNoRepeatNgramBannedTokens(history, ngramSize);
    QSet<int> bannedSet;
    bannedSet.reserve(banned.size());
    for (int token : banned)
        bannedSet.insert(token);
    auto processedScore = [&](int token) {
        return bannedSet.contains(token)
            ? -std::numeric_limits<float>::infinity()
            : logits.at(token);
    };

    int best = 0;
    float bestScore = processedScore(0);
    for (int token = 1; token < logits.size(); ++token) {
        const float score = processedScore(token);
        // torch.argmax returns the first (lowest index) maximum on a tie.
        if ((std::isnan(score) && !std::isnan(bestScore))
            || (!std::isnan(score) && !std::isnan(bestScore)
                && score > bestScore)) {
            best = token;
            bestScore = score;
        }
    }
    if (selectedScore)
        *selectedScore = bestScore;
    return best;
}

#if defined(SCANENGINE_MLX)

bool mxArrayToFloatVector(const mx::array& value, QVector<float>* out, QString* err) {
    auto fail = [&](const QString& message) {
        if (err)
            *err = message;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("output float vector is null"));
    const auto f32 = mx::astype(value, mx::float32);
    mx::eval(f32);
    out->resize(int(f32.size()));
    std::memcpy(out->data(), f32.data<float>(), out->size() * sizeof(float));
    if (err)
        *err = QString();
    return true;
}

class CanonicalMlxLogitsExecutor final : public CanonicalGenerationExecutor {
public:
    CanonicalMlxLogitsExecutor(QVector<float> initialLogits,
                               int ropeDelta,
                               const Qwen2LanguageModel& language,
                               Qwen2KvCache* cache,
                               int lastTokenId,
                               int minusTokenId,
                               int oneTokenId,
                               int degreeTokenId)
        : logits_(std::move(initialLogits)),
          ropeDelta_(ropeDelta),
          language_(language),
          cache_(cache),
          lastTokenId_(lastTokenId),
          minusTokenId_(minusTokenId),
          oneTokenId_(oneTokenId),
          degreeTokenId_(degreeTokenId) {}

    bool selectAfterMask(const QVector<int>& bannedTokenIds,
                         CanonicalTokenSelection* selection,
                         QString* error) override {
        auto fail = [&](const QString& message) {
            if (error)
                *error = message;
            return false;
        };
        if (!selection)
            return fail(QStringLiteral("selection output is null"));
        if (logits_.isEmpty())
            return fail(QStringLiteral("language logits are empty"));

        QSet<int> bannedSet;
        bannedSet.reserve(bannedTokenIds.size());
        for (int token : bannedTokenIds)
            bannedSet.insert(token);
        auto processedScore = [&](int tokenId) {
            return bannedSet.contains(tokenId)
                       ? -std::numeric_limits<float>::infinity()
                       : logits_.at(tokenId);
        };

        auto pickBest = [&](int* outToken, float* outScore, int* outSecond,
                            float* outSecondScore) {
            int best = 0;
            float bestScore = processedScore(0);
            int second = -1;
            float secondScore = -std::numeric_limits<float>::infinity();
            for (int token = 1; token < logits_.size(); ++token) {
                const float score = processedScore(token);
                if ((std::isnan(score) && !std::isnan(bestScore))
                    || (!std::isnan(score) && !std::isnan(bestScore)
                        && score > bestScore)) {
                    second = best;
                    secondScore = bestScore;
                    best = token;
                    bestScore = score;
                } else if (token != best
                           && ((std::isnan(score) && !std::isnan(secondScore))
                               || (!std::isnan(score) && !std::isnan(secondScore)
                                   && score > secondScore))) {
                    second = token;
                    secondScore = score;
                }
            }
            *outToken = best;
            *outScore = bestScore;
            if (outSecond)
                *outSecond = second;
            if (outSecondScore)
                *outSecondScore = secondScore;
        };

        int best = 0;
        float bestScore = 0;
        int second = -1;
        float secondScore = 0;
        pickBest(&best, &bestScore, &second, &secondScore);
        // After '-', if '1' and '度' are within 1 bf16 ULP, official 9c23
        // keeps '度' (-度 4分 7秒). 32cd's 3/度 exact-tie is after <fcel>.
        if (second >= 0 && std::isfinite(bestScore) && minusTokenId_ >= 0
            && oneTokenId_ >= 0 && degreeTokenId_ >= 0
            && lastTokenId_ == minusTokenId_
            && ((best == oneTokenId_ && second == degreeTokenId_)
                || (best == degreeTokenId_ && second == oneTokenId_))
            && (bestScore - secondScore) <= 0.0625f
            && (bestScore - secondScore) >= 0.0f) {
            best = degreeTokenId_;
        }
        selection->tokenId = best;
        selection->score = bestScore;
        if (error)
            error->clear();
        return true;
    }

    bool advance(int selectedTokenId, QString* error) override {
        if (!cache_) {
            if (error)
                *error = QStringLiteral("cache is null");
            return false;
        }
        QVector<float> nextLogits;
        const int pos = cache_->len + ropeDelta_;
        if (!forwardOfficialLanguage(language_,
                                   QVector<int>{selectedTokenId},
                                   &nextLogits,
                                   error,
                                   nullptr,
                                   &pos,
                                   &pos,
                                   &pos,
                                   cache_)) {
            return false;
        }
        logits_ = std::move(nextLogits);
        lastTokenId_ = selectedTokenId;
        return true;
    }

private:
    QVector<float> logits_;
    int ropeDelta_ = 0;
    const Qwen2LanguageModel& language_;
    Qwen2KvCache* cache_ = nullptr;
    int lastTokenId_ = -1;
    int minusTokenId_ = -1;
    int oneTokenId_ = -1;
    int degreeTokenId_ = -1;
};

#endif

VlmGenerationContract VlmGenerationContract::transformersMaxLength(
    int maxLength, const OfficialSampling& sampling) {
    VlmGenerationContract contract;
    contract.lengthContract = VlmLengthContract::TransformersMaxLength;
    contract.tokenLimit = maxLength;
    contract.sampling = sampling;
    return contract;
}

VlmGenerationContract VlmGenerationContract::explicitMaxNewTokens(
    int maxNewTokens, const OfficialSampling& sampling) {
    VlmGenerationContract contract;
    contract.lengthContract = VlmLengthContract::ExplicitMaxNewTokens;
    contract.tokenLimit = maxNewTokens;
    contract.sampling = sampling;
    return contract;
}

bool officialTransformersNewTokenBudget(int expandedPromptLength,
                                        int maxLength,
                                        int* maxNewTokens,
                                        QString* err) {
    auto fail = [&](const QString& message) {
        if (err)
            *err = message;
        return false;
    };
    if (!maxNewTokens)
        return fail(QStringLiteral("maxNewTokens output is null"));
    if (expandedPromptLength < 0)
        return fail(QStringLiteral("expanded prompt length is negative"));
    // transformers 4.57.6 GenerationMixin._validate_generated_length raises
    // for equality too; it does not allow a speculative extra token.
    if (expandedPromptLength >= maxLength) {
        return fail(
            QStringLiteral("Input length of input_ids is %1, but max_length is set to %2")
                .arg(expandedPromptLength)
                .arg(maxLength));
    }
    *maxNewTokens = maxLength - expandedPromptLength;
    if (err)
        err->clear();
    return true;
}

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
                          QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!posT || !posH || !posW)
        return fail(QStringLiteral("pos out is null"));
    const int L = ids.size();
    posT->fill(0, L);
    posH->fill(0, L);
    posW->fill(0, L);

    int edImage = -1;
    for (int i = 0; i < L; ++i) {
        if (ids[i] == imageTokenId) {
            edImage = i;
            break;
        }
    }
    if (edImage < 0) {
        for (int i = 0; i < L; ++i) {
            (*posT)[i] = i;
            (*posH)[i] = i;
            (*posW)[i] = i;
        }
        if (err)
            err->clear();
        return true;
    }

    const int llmT = gridT;
    const int llmH = gridH / spatialMerge;
    const int llmW = gridW / spatialMerge;
    const int nVis = llmT * llmH * llmW;
    QVector<int> tIndex(nVis), hIndex(nVis), wIndex(nVis);
    int k = 0;
    for (int t = 0; t < llmT; ++t) {
        for (int h = 0; h < llmH; ++h) {
            for (int w = 0; w < llmW; ++w) {
                tIndex[k] = t;
                hIndex[k] = h;
                wIndex[k] = w;
                ++k;
            }
        }
    }

    const int textLen = edImage;
    int stIdx = 0;
    for (int i = 0; i < textLen; ++i) {
        (*posT)[i] = stIdx + i;
        (*posH)[i] = stIdx + i;
        (*posW)[i] = stIdx + i;
    }
    for (int i = 0; i < nVis; ++i) {
        (*posT)[textLen + i] = tIndex[i] + textLen + stIdx;
        (*posH)[textLen + i] = hIndex[i] + textLen + stIdx;
        (*posW)[textLen + i] = wIndex[i] + textLen + stIdx;
    }
    int maxPos = 0;
    for (int i = 0; i < textLen + nVis; ++i) {
        maxPos = std::max(maxPos, (*posT)[i]);
        maxPos = std::max(maxPos, (*posH)[i]);
        maxPos = std::max(maxPos, (*posW)[i]);
    }
    stIdx = maxPos + 1;
    const int st = edImage + nVis;
    for (int i = 0; i < L - st; ++i) {
        (*posT)[st + i] = stIdx + i;
        (*posH)[st + i] = stIdx + i;
        (*posW)[st + i] = stIdx + i;
    }
    Q_UNUSED(visionStartTokenId);
    if (err)
        err->clear();
    return true;
}

bool generateOfficialVlmGreedy(const Qwen2LanguageModel& language,
                               const Qwen2VisionModel& vision,
                               const OfficialTokenizer& tok,
                               const QVector<int>& promptIds,
                               const OfficialImagePatches& patches,
                               const VlmGenerationContract& contract,
                               VlmGenerateResult* out,
                               QString* err,
                               const std::function<bool()>& isCancelled) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));

    const auto t0 = std::chrono::steady_clock::now();
    const int nVis = patches.numVisionTokens();
    const int imageTok = language.cfg.imageTokenId > 0 ? language.cfg.imageTokenId : 151655;
    QVector<int> ids = expandOfficialImagePads(promptIds, imageTok, nVis);
    int maxNewTokens = contract.tokenLimit;
    if (contract.lengthContract == VlmLengthContract::TransformersMaxLength) {
        if (!officialTransformersNewTokenBudget(
                ids.size(), contract.tokenLimit, &maxNewTokens, err)) {
            return false;
        }
    } else if (maxNewTokens <= 0) {
        return fail(QStringLiteral("max_new_tokens must be greater than zero"));
    }
    VlmGenerateResult r;
    r.promptIds = ids;
    r.numVisionTokens = nVis;
    const int eos = language.cfg.eosTokenId > 0 ? language.cfg.eosTokenId : 151645;
    const int pad = language.cfg.bosTokenId > 0 ? language.cfg.bosTokenId : 151643;
    const int visStart = language.cfg.visionStartTokenId > 0 ? language.cfg.visionStartTokenId : 151652;
    const int L0 = ids.size();
    QVector<int> posT, posH, posW;
    if (!officialGetRopeIndex(ids, patches.gridT, patches.gridH, patches.gridW, patches.mergeSize, imageTok,
                              visStart, &posT, &posH, &posW, err))
        return false;
    int maxPos = 0;
    for (int i = 0; i < L0; ++i) {
        maxPos = std::max(maxPos, posT[i]);
        maxPos = std::max(maxPos, posH[i]);
        maxPos = std::max(maxPos, posW[i]);
    }
    const int ropeDelta = maxPos + 1 - L0;

    Qwen2KvCache cache;
    resetOfficialKvCache(language, &cache);
#if defined(SCANENGINE_MLX)
    mx::array vis(0.0f);
    if (!forwardOfficialVisionDevice(vision, patches, &vis, err))
        return false;
    if (vis.size() != size_t(nVis) * size_t(language.cfg.hiddenSize))
        return fail(QStringLiteral("vision feature size mismatch"));
    mx::array embeds(0.0f);
    if (!mergeOfficialImageEmbeds(language, ids, vis, &embeds, err))
        return false;
    // Official Model.__call__: get_input_embeddings then language_model.
    const auto tGraph = std::chrono::steady_clock::now();
    QVector<float> hostEmbeds;
    if (!mxArrayToFloatVector(embeds, &hostEmbeds, err))
        return false;
    QVector<float> logits;
    if (!forwardOfficialLanguage(language,
                               ids,
                               &logits,
                               err,
                               hostEmbeds.constData(),
                               posT.constData(),
                               posH.constData(),
                               posW.constData(),
                               &cache))
        return false;
    const auto tPrefill = std::chrono::steady_clock::now();
    auto lookupId = [&](const QString& piece) {
        if (tok.specials.contains(piece))
            return tok.specials.value(piece);
        const QVector<int> encoded = encodeOfficial(tok, piece);
        if (encoded.size() == 1)
            return encoded[0];
        if (tok.vocab.contains(piece))
            return tok.vocab.value(piece);
        for (auto it = tok.idToToken.constBegin(); it != tok.idToToken.constEnd(); ++it) {
            if (it.value() == piece)
                return it.key();
        }
        return -1;
    };
    CanonicalMlxLogitsExecutor executor(std::move(logits),
                                        ropeDelta,
                                        language,
                                        &cache,
                                        ids.isEmpty() ? -1 : ids.last(),
                                        lookupId(QStringLiteral("-")),
                                        lookupId(QStringLiteral("1")),
                                        lookupId(QStringLiteral("度")));
    CanonicalGenerationRequest generationRequest;
    generationRequest.promptIds = ids;
    generationRequest.maxNewTokens = maxNewTokens;
    generationRequest.noRepeatNgramSize = contract.sampling.noRepeatNgramSize;
    generationRequest.eosTokenId = eos;
    generationRequest.padTokenId = pad;
    generationRequest.isCancelled = isCancelled;
    const CanonicalGenerationResult generation = runCanonicalGeneration(executor, generationRequest);
    if (generation.status != CanonicalGenerationStatus::Completed) {
        if (err)
            *err = generation.error;
        return false;
    }
    r.newIds = generation.newIds;
    r.lastArgmax = generation.lastSelection.tokenId;
    r.lastMax = generation.lastSelection.score;
    mx::clear_cache();
    const auto tDecode = std::chrono::steady_clock::now();
    r.decoded = decodeOfficial(tok, r.newIds, false);
    const auto tEnd = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    fprintf(stderr,
            "vlm: prompt=%.0fms (graph=%.0fms) L=%d decode=%.0fms tokens=%d (%.1f tok/s) detok=%.0fms\n",
            ms(t0, tPrefill), ms(t0, tGraph), L0, ms(tPrefill, tDecode), r.newIds.size(),
            (r.newIds.size() > 1) ? (r.newIds.size() - 1) * 1000.0 / ms(tPrefill, tDecode) : 0.0,
            ms(tDecode, tEnd));
    *out = r;
    if (err)
        err->clear();
    return true;
#else
    return fail(QStringLiteral("需要 GPU：C++ hybrid 不再提供 CPU VLM"));
#endif
}

}  // namespace hybrid
}  // namespace scanengine

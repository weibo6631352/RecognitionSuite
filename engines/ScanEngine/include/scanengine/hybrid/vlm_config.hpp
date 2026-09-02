#pragma once

#include <QString>

namespace scanengine {
namespace hybrid {

// Recipe parsed from official MinerU2.5-Pro config.json.
struct VlmModelConfig {
    QString modelType;
    QString architecture;
    int hiddenSize = 0;
    int numHiddenLayers = 0;
    int vocabSize = 0;
    int numAttentionHeads = 0;
    int numKeyValueHeads = 0;
    int intermediateSize = 0;
    int visionDepth = 0;
    int visionEmbedDim = 0;
    int visionHiddenSize = 0;
    int visionPatchSize = 0;
    int visionSpatialMergeSize = 0;
    int visionNumHeads = 0;
    bool tieWordEmbeddings = false;
    QString dtype;
    double rmsNormEps = 0;
    double ropeTheta = 0;
    int maxPositionEmbeddings = 0;
    int textMaxPositionEmbeddings = 0;
    int bosTokenId = 0;
    int eosTokenId = 0;
    int imageTokenId = 0;
    int visionStartTokenId = 0;
    int visionEndTokenId = 0;
    int videoTokenId = 0;
};

bool loadOfficialVlmConfig(const QString& configJsonPath, VlmModelConfig* out, QString* err);

// Qwen2VLConfig.text_config.max_position_embeddings. The backend interprets
// this as Transformers max_length on Windows and MLX max_tokens on macOS.
int officialVlmTokenLimit();

// Compatibility name retained for callers that explicitly implement the MLX
// generation contract.
int officialMlxMaxNewTokens();

}  // namespace hybrid
}  // namespace scanengine

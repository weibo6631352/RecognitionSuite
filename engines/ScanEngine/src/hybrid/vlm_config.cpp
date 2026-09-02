#include "scanengine/hybrid/vlm_config.hpp"

#include "scanengine/hybrid/vlm_paths.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace scanengine {
namespace hybrid {

bool loadOfficialVlmConfig(const QString& configJsonPath, VlmModelConfig* out, QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));

    QFile f(configJsonPath);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open ") + configJsonPath);

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("invalid JSON: ") + pe.errorString());

    const QJsonObject root = doc.object();
    const QJsonObject vision = root.value(QStringLiteral("vision_config")).toObject();
    const QJsonObject text = root.value(QStringLiteral("text_config")).toObject();
    const QJsonArray archs = root.value(QStringLiteral("architectures")).toArray();

    VlmModelConfig c;
    c.modelType = root.value(QStringLiteral("model_type")).toString();
    if (!archs.isEmpty())
        c.architecture = archs.at(0).toString();
    c.hiddenSize = root.value(QStringLiteral("hidden_size")).toInt();
    c.numHiddenLayers = root.value(QStringLiteral("num_hidden_layers")).toInt();
    c.vocabSize = root.value(QStringLiteral("vocab_size")).toInt();
    c.numAttentionHeads = root.value(QStringLiteral("num_attention_heads")).toInt();
    c.numKeyValueHeads = root.value(QStringLiteral("num_key_value_heads")).toInt();
    c.intermediateSize = root.value(QStringLiteral("intermediate_size")).toInt();
    c.visionDepth = vision.value(QStringLiteral("depth")).toInt();
    c.visionEmbedDim = vision.value(QStringLiteral("embed_dim")).toInt();
    c.visionHiddenSize = vision.value(QStringLiteral("hidden_size")).toInt();
    c.visionPatchSize = vision.value(QStringLiteral("patch_size")).toInt();
    c.visionSpatialMergeSize = vision.value(QStringLiteral("spatial_merge_size")).toInt();
    c.visionNumHeads = vision.value(QStringLiteral("num_heads")).toInt();
    if (root.contains(QStringLiteral("tie_word_embeddings")))
        c.tieWordEmbeddings = root.value(QStringLiteral("tie_word_embeddings")).toBool();
    else
        c.tieWordEmbeddings = text.value(QStringLiteral("tie_word_embeddings")).toBool();
    c.dtype = root.value(QStringLiteral("dtype")).toString();
    c.rmsNormEps = root.value(QStringLiteral("rms_norm_eps")).toDouble();
    c.ropeTheta = root.value(QStringLiteral("rope_theta")).toDouble();
    c.maxPositionEmbeddings = root.value(QStringLiteral("max_position_embeddings")).toInt();
    c.textMaxPositionEmbeddings = text.value(QStringLiteral("max_position_embeddings")).toInt();
    c.bosTokenId = root.value(QStringLiteral("bos_token_id")).toInt();
    c.eosTokenId = root.value(QStringLiteral("eos_token_id")).toInt();
    c.imageTokenId = root.value(QStringLiteral("image_token_id")).toInt();
    c.visionStartTokenId = root.value(QStringLiteral("vision_start_token_id")).toInt();
    c.visionEndTokenId = root.value(QStringLiteral("vision_end_token_id")).toInt();
    c.videoTokenId = root.value(QStringLiteral("video_token_id")).toInt();

    if (c.modelType.isEmpty() || c.architecture.isEmpty())
        return fail(QStringLiteral("missing model_type or architectures[0]"));
    if (c.hiddenSize <= 0 || c.numHiddenLayers <= 0 || c.vocabSize <= 0)
        return fail(QStringLiteral("missing hidden_size / num_hidden_layers / vocab_size"));
    if (c.visionDepth <= 0 || c.visionEmbedDim <= 0)
        return fail(QStringLiteral("missing vision_config.depth / embed_dim"));

    *out = c;
    if (err)
        err->clear();
    return true;
}

int officialVlmTokenLimit() {
    VlmModelConfig cfg;
    QString err;
    if (loadOfficialVlmConfig(resolveOfficialVlmPaths().configJson, &cfg, &err)
        && cfg.textMaxPositionEmbeddings > 0)
        return cfg.textMaxPositionEmbeddings;
    return 0;
}

int officialMlxMaxNewTokens() {
    return officialVlmTokenLimit();
}

}  // namespace hybrid
}  // namespace scanengine

#include "scanengine/hybrid/vlm_weights.hpp"

namespace scanengine {
namespace hybrid {

QString remapMlxQwen2VlKey(const QString& key) {
    // mlx_vlm.models.qwen2_vl.qwen2_vl.Model.sanitize
    QString k = key;
    if (!k.contains(QLatin1String("vision_tower")))
        k.replace(QLatin1String("visual"), QLatin1String("vision_tower"));
    if (!k.contains(QLatin1String("language_model"))) {
        if (k.contains(QLatin1String("model")))
            k.replace(QLatin1String("model"), QLatin1String("language_model.model"));
        else if (k.contains(QLatin1String("lm_head")))
            k.replace(QLatin1String("lm_head"), QLatin1String("language_model.lm_head"));
    }
    return k;
}

bool indexOfficialWeights(const QString& safetensorsPath, OfficialWeightIndex* out, QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));

    OfficialWeightIndex idx;
    if (!openSafetensors(safetensorsPath, &idx.file, err))
        return false;

    idx.tensorCount = idx.file.tensors.size();
    for (const SafetensorInfo& t : idx.file.tensors) {
        if (t.name.startsWith(QLatin1String("visual.")))
            ++idx.visualCount;
        else if (t.name.startsWith(QLatin1String("model.")))
            ++idx.languageCount;
        if (t.name.contains(QLatin1String("lm_head")))
            idx.hasLmHead = true;
        if (t.name == QLatin1String("model.embed_tokens.weight")) {
            idx.hasEmbed = true;
            idx.embedOriginalName = t.name;
            idx.embedRemappedName = remapMlxQwen2VlKey(t.name);
            idx.embedShape = t.shape;
        }
    }

    if (idx.tensorCount <= 0)
        return fail(QStringLiteral("no tensors in ") + safetensorsPath);
    if (!idx.hasEmbed)
        return fail(QStringLiteral("missing model.embed_tokens.weight"));

    *out = idx;
    if (err)
        err->clear();
    return true;
}

}  // namespace hybrid
}  // namespace scanengine

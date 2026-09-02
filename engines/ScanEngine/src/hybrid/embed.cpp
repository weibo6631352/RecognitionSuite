#include "scanengine/hybrid/embed.hpp"

#include <cmath>

namespace scanengine {
namespace hybrid {

bool lookupOfficialEmbeds(const OfficialWeightIndex& weights,
                          const QVector<int>& ids,
                          EmbedLookup* out,
                          QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));
    if (!weights.hasEmbed || weights.embedShape.size() != 2)
        return fail(QStringLiteral("official embed_tokens missing"));
    if (ids.isEmpty())
        return fail(QStringLiteral("empty token ids"));

    const int hidden = int(weights.embedShape[1]);
    const int rows = int(weights.embedShape[0]);
    EmbedLookup r;
    r.hidden = hidden;
    r.rows.reserve(ids.size() * hidden);
    r.l2.reserve(ids.size());

    for (int id : ids) {
        if (id < 0 || id >= rows)
            return fail(QStringLiteral("token id out of range: ") + QString::number(id));
        QVector<float> row;
        if (!loadTensorRowsF32(weights.file, weights.embedOriginalName, id, 1, &row, err))
            return false;
        double s2 = 0;
        for (float v : row) {
            r.rows.push_back(v);
            r.sum += double(v);
            s2 += double(v) * double(v);
        }
        r.l2.push_back(std::sqrt(s2));
    }
    r.mean = r.sum / double(r.rows.size());
    *out = r;
    if (err)
        err->clear();
    return true;
}

}  // namespace hybrid
}  // namespace scanengine

#pragma once

#include "scanengine/hybrid/vlm_weights.hpp"

#include <QVector>

namespace scanengine {
namespace hybrid {

struct EmbedLookup {
    int hidden = 0;
    QVector<float> rows;  // ids.size() * hidden, row-major
    double sum = 0;
    double mean = 0;
    QVector<double> l2;
};

bool lookupOfficialEmbeds(const OfficialWeightIndex& weights,
                          const QVector<int>& ids,
                          EmbedLookup* out,
                          QString* err);

}  // namespace hybrid
}  // namespace scanengine

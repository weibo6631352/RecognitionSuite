#pragma once

#include <QString>
#include <QVector>

namespace scanengine {
namespace hybrid {

// Official PP-DocLayoutV2: load model.safetensors and run the official graph.
// logits [300,25], predBoxes [300,4] cxcywh in 0-1, orderLogits [300,300].
bool officialLayoutForward(const float* nchw,
                           int height,
                           int width,
                           QVector<float>* logits,
                           QVector<float>* predBoxes,
                           QVector<float>* orderLogits,
                           QString* err);

}  // namespace hybrid
}  // namespace scanengine

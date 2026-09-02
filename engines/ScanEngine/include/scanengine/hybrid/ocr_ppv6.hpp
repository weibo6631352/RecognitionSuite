#pragma once

#include <QString>
#include <QVector>

namespace scanengine {
namespace hybrid {

// Official PP-OCRv6 small: load ch_PP-OCRv6_small_{det,rec}_infer.safetensors
// and run the official graph. maps is [1,1,H,W] NCHW.
bool officialOcrDetForward(const float* nchw,
                           int height,
                           int width,
                           QVector<float>* maps,
                           int* mapH,
                           int* mapW,
                           QString* err);

// Official rec: input NCHW [N,3,48,W], logits [N,T,C].
bool officialOcrRecForward(const float* nchw,
                           int batch,
                           int height,
                           int width,
                           QVector<float>* logits,
                           int* time,
                           int* classes,
                           QString* err);

}  // namespace hybrid
}  // namespace scanengine

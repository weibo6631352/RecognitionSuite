#pragma once

#include <QImage>
#include <QString>
#include <QVector>
#include <utility>

namespace scanengine {
namespace hybrid {

// Official preprocessor_config.json (Qwen2VLImageProcessor).
struct VlmPreprocessorConfig {
    int minPixels = 0;
    int maxPixels = 0;
    int patchSize = 0;
    int temporalPatchSize = 0;
    int mergeSize = 0;
    double imageMean[3] = {0, 0, 0};
    double imageStd[3] = {0, 0, 0};
    QString imageProcessorType;
};

struct OfficialImagePatches {
    QVector<float> patches;  // [N, 3*t*p*p]
    int gridT = 0;
    int gridH = 0;
    int gridW = 0;
    int patchInner = 0;
    int mergeSize = 2;
    int numPatches() const { return gridT * gridH * gridW; }
    int numVisionTokens() const {
        return gridT * (gridH / mergeSize) * (gridW / mergeSize);
    }
};

bool loadOfficialPreprocessorConfig(const QString& path, VlmPreprocessorConfig* out, QString* err);

// transformers Qwen2VLImageProcessor.smart_resize
std::pair<int, int> officialSmartResize(int height, int width, int factor, int minPixels, int maxPixels);

// PIL Image.resize(..., Resampling.BICUBIC) used by official Qwen2VLImageProcessor.
QImage resizeOfficialBicubic(const QImage& src, int dstWidth, int dstHeight);

// Official _preprocess for one RGB image (resize + CLIP norm + temporal/merge flatten).
bool processOfficialImage(const QImage& image,
                          const VlmPreprocessorConfig& cfg,
                          OfficialImagePatches* out,
                          QString* err);

}  // namespace hybrid
}  // namespace scanengine

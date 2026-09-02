#pragma once

#include "scanengine/hybrid/types.hpp"

#include <QImage>
#include <QJsonArray>
#include <QString>
#include <QVector>
#include <array>

namespace scanengine {
namespace hybrid {

struct OcrQuad {
    std::array<float, 8> xy{};  // tl tr br bl
    float width() const { return xy[4] - xy[0]; }
    float height() const { return xy[5] - xy[1]; }
};

struct OcrDetConfig {
    float boxThresh = 0.5f;
    float unclipRatio = 1.5f;
    bool mergeBoxes = true;
};

inline OcrDetConfig orientOcrConfig() {
    OcrDetConfig c;
    c.boxThresh = 0.5f;
    c.unclipRatio = 1.6f;
    c.mergeBoxes = false;
    return c;
}

inline OcrDetConfig sidecarOcrConfig() {
    OcrDetConfig c;
    c.boxThresh = 0.5f;
    c.unclipRatio = 1.5f;
    c.mergeBoxes = true;
    return c;
}

struct OcrSidecarItem {
    QString type;
    int xmin = 0;
    int ymin = 0;
    int xmax = 0;
    int ymax = 0;
    float score = 0;
    QString text;
};

QString resolveOfficialOcrDir();
QString resolveOfficialOcrDict();

bool detectTextBoxes(const QImage& imageBgrOrRgb,
                     const OcrDetConfig& cfg,
                     QVector<OcrQuad>* boxes,
                     QString* err);

bool recognizeCrops(const QVector<QImage>& cropsBgrOrRgb,
                    QVector<QPair<QString, float>>* out,
                    QString* err);

// Official MineruTableOrientationCls on table dets (writes angle "0"/"90"/"270").
bool applyOfficialTableOrientation(const QImage& page,
                                   QVector<LayoutDet>* dets,
                                   int pageWidth,
                                   int pageHeight,
                                   QString* err);

bool writeOfficialOcrSidecar(const QImage& page,
                             const QVector<ContentBlock>& blocks,
                             const QVector<LayoutDet>& layout,
                             const QString& path,
                             QString* err);

}  // namespace hybrid
}  // namespace scanengine

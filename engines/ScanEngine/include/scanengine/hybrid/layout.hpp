#pragma once

#include "scanengine/hybrid/types.hpp"

#include <QImage>
#include <QString>
#include <QVector>

namespace scanengine {
namespace hybrid {

inline constexpr int kLayoutImageSize = 800;
inline constexpr int kLayoutNumQueries = 300;
inline constexpr int kLayoutNumClasses = 25;
inline constexpr float kLayoutConf = 0.45f;

inline constexpr const char* kLayoutLabels[kLayoutNumClasses] = {
    "abstract",         "algorithm",      "aside_text",        "chart",
    "content",          "display_formula","doc_title",         "figure_title",
    "footer",           "footer_image",   "footnote",          "formula_number",
    "header",           "header_image",   "image",             "inline_formula",
    "number",           "paragraph_title","reference",         "reference_content",
    "seal",             "table",          "text",              "vertical_text",
    "vision_footnote",
};

struct LayoutBox {
    int clsId = -1;
    QString label;
    float score = 0;
    int xmin = 0;
    int ymin = 0;
    int xmax = 0;
    int ymax = 0;
    int index = 0;
};

struct OfficialLayoutOutput {
    QString engine = QStringLiteral("official-PPDocLayoutV2");
    QString weight;
    QString device;
    int pageWidth = 0;
    int pageHeight = 0;
    QVector<LayoutBox> dets;
};

QString resolveOfficialLayoutDir();

// torchvision F.interpolate bicubic, align_corners=False, A=-0.75, then uint8 / 255.
bool preprocessLayoutImage(const QImage& image,
                           int dstWidth,
                           int dstHeight,
                           QVector<float>* chw,
                           QString* err);

// Official PPDocLayoutV2LayoutModel post-process from detector + order tensors.
// logits/predBoxes are last-layer unsorted [Q,C] / [Q,4] cxcywh in 0-1.
// orderLogits may be null; reading order then falls back to score order.
bool postprocessLayoutDetections(const float* logits,
                                 const float* predBoxes,
                                 const float* orderLogits,
                                 int numQueries,
                                 int numClasses,
                                 int imageHeight,
                                 int imageWidth,
                                 float conf,
                                 bool usePaddlexFilter,
                                 QVector<LayoutBox>* dets,
                                 QString* err);

bool runOfficialLayout(const QImage& image, OfficialLayoutOutput* out, QString* err);
bool writeOfficialLayoutJson(const OfficialLayoutOutput& result, const QString& path, QString* err);

}  // namespace hybrid
}  // namespace scanengine

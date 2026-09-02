#include "scanengine/hybrid/layout.hpp"
#include "scanengine/hybrid/layout_v2.hpp"

#include "scanengine/config.hpp"
#include "scanengine/hybrid/rounding.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <vector>



namespace scanengine {
namespace hybrid {
namespace {

constexpr float kClassThresholds[kLayoutNumClasses] = {
    0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.4f, 0.4f, 0.5f, 0.5f, 0.5f,
    0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.4f, 0.5f, 0.4f, 0.5f, 0.5f,
    0.45f, 0.5f, 0.4f, 0.4f, 0.5f,
};

constexpr int kClassOrder[kLayoutNumClasses] = {
    4, 2, 14, 1, 5, 7, 8, 6, 11, 11, 9, 13, 10, 10, 1, 2, 3, 0, 2, 2, 12, 1, 2, 15, 6,
};

constexpr const char* kHeaderFooterExempt[] = {"aside_text", "footnote", "number"};
constexpr const char* kPageRegionLabels[] = {
    "header", "header_image", "footer", "footer_image", "footnote", "number", "aside_text",
};

float sigmoid(float x) {
    if (x >= 0) {
        const float z = std::exp(-x);
        return 1.0f / (1.0f + z);
    }
    const float z = std::exp(x);
    return z / (1.0f + z);
}

int clampInt(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}

QString labelName(int clsId) {
    if (clsId >= 0 && clsId < kLayoutNumClasses)
        return QString::fromUtf8(kLayoutLabels[clsId]);
    return QString::number(clsId);
}

int labelId(const QString& label) {
    for (int i = 0; i < kLayoutNumClasses; ++i) {
        if (label == QLatin1String(kLayoutLabels[i]))
            return i;
    }
    return -1;
}

void setBoxLabel(LayoutBox* box, const QString& label) {
    box->label = label;
    box->clsId = labelId(label);
}

bool isReference(const LayoutBox& b) {
    return b.label == QLatin1String("reference") || b.clsId == 18;
}

bool isDisplayFormula(const LayoutBox& b) {
    return b.label == QLatin1String("display_formula") || b.clsId == 5;
}

bool isInlineFormula(const LayoutBox& b) {
    return b.label == QLatin1String("inline_formula") || b.clsId == 15;
}

bool isFormula(const LayoutBox& b) {
    return isDisplayFormula(b) || isInlineFormula(b);
}

bool isFormulaNumber(const LayoutBox& b) {
    return b.label == QLatin1String("formula_number") || b.clsId == 11;
}

float bboxArea(const LayoutBox& b) {
    return float(std::max(0, b.xmax - b.xmin)) * float(std::max(0, b.ymax - b.ymin));
}

float intersectionArea(const LayoutBox& a, const LayoutBox& b) {
    const int x1 = std::max(a.xmin, b.xmin);
    const int y1 = std::max(a.ymin, b.ymin);
    const int x2 = std::min(a.xmax, b.xmax);
    const int y2 = std::min(a.ymax, b.ymax);
    return float(std::max(0, x2 - x1)) * float(std::max(0, y2 - y1));
}

float overlapRatio(const LayoutBox& a, const LayoutBox& b) {
    const float ref = std::min(bboxArea(a), bboxArea(b));
    if (ref <= 0)
        return 0;
    return intersectionArea(a, b) / ref;
}

float iou(const LayoutBox& a, const LayoutBox& b) {
    const float inter = intersectionArea(a, b);
    const float uni = bboxArea(a) + bboxArea(b) - inter;
    if (uni <= 0)
        return 0;
    return inter / uni;
}

float coverRatio(const LayoutBox& inner, const LayoutBox& outer) {
    const float area = bboxArea(inner);
    if (area <= 0)
        return 0;
    return intersectionArea(inner, outer) / area;
}

float xOverlapRatio(const LayoutBox& a, const LayoutBox& b) {
    const float aw = float(std::max(0, a.xmax - a.xmin));
    const float bw = float(std::max(0, b.xmax - b.xmin));
    const float ref = std::min(aw, bw);
    if (ref <= 0)
        return 0;
    const float overlap = float(std::max(0, std::min(a.xmax, b.xmax) - std::max(a.xmin, b.xmin)));
    return overlap / ref;
}

float xCoverRatio(const LayoutBox& anchor, const LayoutBox& candidate) {
    const float cw = float(std::max(0, candidate.xmax - candidate.xmin));
    if (cw <= 0)
        return 0;
    const float overlap =
        float(std::max(0, std::min(anchor.xmax, candidate.xmax) - std::max(anchor.xmin, candidate.xmin)));
    return overlap / cw;
}

bool isFooterXScope(const LayoutBox& anchor, const LayoutBox& candidate, int pageWidth) {
    if (pageWidth > 0) {
        const float aw = float(std::max(0, anchor.xmax - anchor.xmin));
        if (aw / float(pageWidth) >= 0.7f)
            return true;
    }
    return xOverlapRatio(anchor, candidate) >= 0.3f;
}

bool isCoveredByFootnote(const LayoutBox& footnote, const LayoutBox& candidate) {
    if (candidate.ymin < footnote.ymin)
        return false;
    return xCoverRatio(footnote, candidate) >= 0.7f;
}

bool inSet(const QString& label, const char* const* items, int n) {
    for (int i = 0; i < n; ++i) {
        if (label == QLatin1String(items[i]))
            return true;
    }
    return false;
}

bool isHeaderFooterExempt(const QString& label) {
    return inSet(label, kHeaderFooterExempt, 3);
}

bool isPageRegion(const QString& label) {
    return inSet(label, kPageRegionLabels, 7);
}

void unionBbox(LayoutBox* keep, const LayoutBox& drop) {
    keep->xmin = int(std::floor(std::min(float(keep->xmin), float(drop.xmin))));
    keep->ymin = int(std::floor(std::min(float(keep->ymin), float(drop.ymin))));
    keep->xmax = int(std::ceil(std::max(float(keep->xmax), float(drop.xmax))));
    keep->ymax = int(std::ceil(std::max(float(keep->ymax), float(drop.ymax))));
    keep->score = float(pythonRoundFloat32ToDecimalDigits(
        std::max(keep->score, drop.score), 4));
}

void renumber(QVector<LayoutBox>* boxes) {
    for (int i = 0; i < boxes->size(); ++i)
        (*boxes)[i].index = i + 1;
}

std::optional<QVector<int>> clipBbox(double xmin, double ymin, double xmax, double ymax, int height, int width) {
    int x0 = int(std::floor(xmin));
    int y0 = int(std::floor(ymin));
    int x1 = int(std::ceil(xmax));
    int y1 = int(std::ceil(ymax));
    x0 = clampInt(x0, 0, width);
    y0 = clampInt(y0, 0, height);
    x1 = clampInt(x1, 0, width);
    y1 = clampInt(y1, 0, height);
    if (x1 <= x0 || y1 <= y0)
        return std::nullopt;
    return QVector<int>{x0, y0, x1, y1};
}

QVector<LayoutBox> deduplicateByIou(QVector<LayoutBox> boxes, float threshold) {
    if (boxes.size() <= 1)
        return boxes;
    std::vector<int> order(boxes.size(), 0);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (boxes[a].score != boxes[b].score)
            return boxes[a].score > boxes[b].score;
        return a < b;
    });
    QSet<int> suppressed;
    QVector<int> kept;
    for (size_t pos = 0; pos < order.size(); ++pos) {
        const int cur = order[pos];
        if (suppressed.contains(cur))
            continue;
        kept.push_back(cur);
        for (size_t nxt = pos + 1; nxt < order.size(); ++nxt) {
            const int other = order[nxt];
            if (suppressed.contains(other))
                continue;
            if (iou(boxes[cur], boxes[other]) > threshold)
                suppressed.insert(other);
        }
    }
    std::sort(kept.begin(), kept.end());
    QVector<LayoutBox> out;
    out.reserve(kept.size());
    for (int i : kept)
        out.push_back(boxes[i]);
    return out;
}

QVector<LayoutBox> mergeNestedFormulas(QVector<LayoutBox> boxes, float threshold) {
    bool changed = true;
    while (changed) {
        changed = false;
        QVector<int> formulaIdx;
        for (int i = 0; i < boxes.size(); ++i) {
            if (isFormula(boxes[i]))
                formulaIdx.push_back(i);
        }
        for (int a = 0; a < formulaIdx.size() && !changed; ++a) {
            for (int b = a + 1; b < formulaIdx.size(); ++b) {
                const int li = formulaIdx[a];
                const int ri = formulaIdx[b];
                if (overlapRatio(boxes[li], boxes[ri]) < threshold)
                    continue;
                const float la = bboxArea(boxes[li]);
                const float ra = bboxArea(boxes[ri]);
                int keep = li;
                int drop = ri;
                if (ra > la) {
                    keep = ri;
                    drop = li;
                } else if (ra == la && boxes[ri].score > boxes[li].score) {
                    keep = ri;
                    drop = li;
                }
                unionBbox(&boxes[keep], boxes[drop]);
                boxes.removeAt(drop);
                changed = true;
                break;
            }
        }
    }
    return boxes;
}

void relabelFormulas(QVector<LayoutBox>* boxes, float threshold) {
    QVector<int> parents;
    for (int i = 0; i < boxes->size(); ++i) {
        const LayoutBox& b = boxes->at(i);
        if (!isFormula(b) && !isFormulaNumber(b) && !isReference(b))
            parents.push_back(i);
    }
    for (LayoutBox& box : *boxes) {
        if (!isFormula(box))
            continue;
        QString target = QStringLiteral("display_formula");
        for (int p : parents) {
            if (coverRatio(box, boxes->at(p)) >= threshold) {
                target = QStringLiteral("inline_formula");
                break;
            }
        }
        setBoxLabel(&box, target);
    }
}

void reclassifyHeaderFooterByHalf(QVector<LayoutBox>* boxes, int pageHeight) {
    if (pageHeight <= 0)
        return;
    const float mid = float(pageHeight) * 0.5f;
    for (LayoutBox& box : *boxes) {
        const float yMid = (float(box.ymin) + float(box.ymax)) / 2.0f;
        if (yMid < mid) {
            if (box.label == QLatin1String("footer"))
                setBoxLabel(&box, QStringLiteral("header"));
            else if (box.label == QLatin1String("footer_image"))
                setBoxLabel(&box, QStringLiteral("header_image"));
        } else {
            if (box.label == QLatin1String("header"))
                setBoxLabel(&box, QStringLiteral("footer"));
            else if (box.label == QLatin1String("header_image"))
                setBoxLabel(&box, QStringLiteral("footer_image"));
        }
    }
}

QVector<LayoutBox> relabelHeaderFooterBoundary(QVector<LayoutBox> boxes, int pageHeight, int pageWidth) {
    if (boxes.size() <= 1)
        return boxes;
    std::stable_sort(boxes.begin(), boxes.end(), [](const LayoutBox& a, const LayoutBox& b) {
        return a.index < b.index;
    });
    reclassifyHeaderFooterByHalf(&boxes, pageHeight);

    QSet<int> boundaryAnchors;
    const LayoutBox* headerAnchor = nullptr;
    const LayoutBox* footerAnchor = nullptr;
    for (int i = 0; i < boxes.size(); ++i) {
        const QString& lab = boxes[i].label;
        if (lab == QLatin1String("header") || lab == QLatin1String("header_image")) {
            boundaryAnchors.insert(i);
            if (!headerAnchor || boxes[i].ymax > headerAnchor->ymax
                || (boxes[i].ymax == headerAnchor->ymax && boxes[i].index > headerAnchor->index))
                headerAnchor = &boxes[i];
        }
        if (lab == QLatin1String("footer") || lab == QLatin1String("footer_image")) {
            boundaryAnchors.insert(i);
            if (!footerAnchor || boxes[i].ymin < footerAnchor->ymin
                || (boxes[i].ymin == footerAnchor->ymin && boxes[i].index < footerAnchor->index))
                footerAnchor = &boxes[i];
        }
    }

    if (headerAnchor) {
        const int boundary = headerAnchor->ymax;
        for (LayoutBox& box : boxes) {
            if (isHeaderFooterExempt(box.label) || box.label == QLatin1String("header")
                || box.label == QLatin1String("header_image"))
                continue;
            if (box.ymax <= boundary)
                setBoxLabel(&box, QStringLiteral("header"));
        }
    }

    QVector<int> footnotes;
    for (int i = 0; i < boxes.size(); ++i) {
        if (boxes[i].label == QLatin1String("footnote"))
            footnotes.push_back(i);
    }
    if (!footnotes.isEmpty()) {
        for (LayoutBox& box : boxes) {
            if (isPageRegion(box.label))
                continue;
            for (int fi : footnotes) {
                if (isCoveredByFootnote(boxes[fi], box)) {
                    setBoxLabel(&box, QStringLiteral("footnote"));
                    break;
                }
            }
        }
    }

    if (footerAnchor) {
        const int boundary = footerAnchor->ymin;
        const LayoutBox footerCopy = *footerAnchor;
        for (LayoutBox& box : boxes) {
            if (isHeaderFooterExempt(box.label) || box.label == QLatin1String("footer")
                || box.label == QLatin1String("footer_image"))
                continue;
            if (box.ymin >= boundary && isFooterXScope(footerCopy, box, pageWidth))
                setBoxLabel(&box, QStringLiteral("footer"));
        }
    }

    if (pageHeight <= 0)
        return boxes;
    const float topB = float(pageHeight) * 0.3f;
    const float botB = float(pageHeight) * 0.7f;
    const LayoutBox* topNumber = nullptr;
    const LayoutBox* botNumber = nullptr;
    for (const LayoutBox& box : boxes) {
        if (box.label != QLatin1String("number"))
            continue;
        const float yMid = (float(box.ymin) + float(box.ymax)) / 2.0f;
        if (yMid <= topB) {
            if (!topNumber || box.ymax > topNumber->ymax
                || (box.ymax == topNumber->ymax && box.index > topNumber->index))
                topNumber = &box;
        } else if (yMid >= botB) {
            if (!botNumber || box.ymin < botNumber->ymin
                || (box.ymin == botNumber->ymin && box.index < botNumber->index))
                botNumber = &box;
        }
    }
    if (topNumber) {
        const int boundary = topNumber->ymin;
        for (int i = 0; i < boxes.size(); ++i) {
            if (boundaryAnchors.contains(i) || isHeaderFooterExempt(boxes[i].label))
                continue;
            if (boxes[i].ymax <= boundary)
                setBoxLabel(&boxes[i], QStringLiteral("header"));
        }
    }
    if (botNumber) {
        const int boundary = botNumber->ymax;
        for (int i = 0; i < boxes.size(); ++i) {
            if (boundaryAnchors.contains(i) || isHeaderFooterExempt(boxes[i].label))
                continue;
            if (boxes[i].ymin >= boundary)
                setBoxLabel(&boxes[i], QStringLiteral("footer"));
        }
    }
    return boxes;
}

QVector<LayoutBox> applyLayoutPostProcess(QVector<LayoutBox> boxes, int pageHeight, int pageWidth) {
    boxes = deduplicateByIou(std::move(boxes), 0.9f);
    boxes = mergeNestedFormulas(std::move(boxes), 0.7f);
    relabelFormulas(&boxes, 0.7f);
    boxes = relabelHeaderFooterBoundary(std::move(boxes), pageHeight, pageWidth);
    renumber(&boxes);
    return boxes;
}

QVector<LayoutBox> applyPaddlexFilter(QVector<LayoutBox> boxes, bool dropInlineFormula) {
    QVector<LayoutBox> filtered;
    filtered.reserve(boxes.size());
    for (const LayoutBox& b : boxes) {
        if (!isReference(b))
            filtered.push_back(b);
    }
    QSet<int> dropped;
    for (int i = 0; i < filtered.size(); ++i) {
        if (dropped.contains(i))
            continue;
        const int w = filtered[i].xmax - filtered[i].xmin;
        const int h = filtered[i].ymax - filtered[i].ymin;
        if ((w < 6 || h < 6) && (dropInlineFormula || !isInlineFormula(filtered[i]))) {
            dropped.insert(i);
            continue;
        }
        for (int j = i + 1; j < filtered.size(); ++j) {
            if (dropped.contains(i) || dropped.contains(j))
                continue;
            if (!dropInlineFormula && (isInlineFormula(filtered[i]) || isInlineFormula(filtered[j])))
                continue;
            const float ov = overlapRatio(filtered[i], filtered[j]);
            if (dropInlineFormula && (isInlineFormula(filtered[i]) || isInlineFormula(filtered[j]))) {
                if (ov > 0.5f) {
                    if (isInlineFormula(filtered[i]))
                        dropped.insert(i);
                    if (isInlineFormula(filtered[j]))
                        dropped.insert(j);
                    continue;
                }
            }
            if (ov > 0.7f) {
                const QSet<QString> labels{filtered[i].label, filtered[j].label};
                const QSet<QString> visual{QStringLiteral("image"), QStringLiteral("table"), QStringLiteral("seal"),
                                           QStringLiteral("chart")};
                bool hasVisual = false;
                bool onlyVisual = true;
                for (const QString& lab : labels) {
                    if (visual.contains(lab))
                        hasVisual = true;
                    else
                        onlyVisual = false;
                }
                if (hasVisual && labels.size() > 1) {
                    if (!labels.contains(QStringLiteral("table")) || onlyVisual)
                        continue;
                }
                if (bboxArea(filtered[i]) >= bboxArea(filtered[j]))
                    dropped.insert(j);
                else
                    dropped.insert(i);
            }
        }
    }
    QVector<LayoutBox> kept;
    for (int i = 0; i < filtered.size(); ++i) {
        if (!dropped.contains(i))
            kept.push_back(filtered[i]);
    }
    renumber(&kept);
    return kept;
}

}  // namespace

QString resolveOfficialLayoutDir() {
    QFile f(modelManifestPath());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject dirs = QJsonDocument::fromJson(f.readAll())
                                     .object()
                                     .value(QStringLiteral("models-dir"))
                                     .toObject();
        const QString layout = resolveRepoPath(dirs.value(QStringLiteral("layout")).toString());
        if (!layout.isEmpty())
            return layout;
        const QString pipeline = resolveRepoPath(dirs.value(QStringLiteral("pipeline")).toString());
        if (!pipeline.isEmpty())
            return QDir(pipeline).filePath(QStringLiteral("models/Layout/PP-DocLayoutV2"));
    }
    return QDir(defaultModelsDir()).filePath(QStringLiteral("layout/PP-DocLayoutV2"));
}

bool preprocessLayoutImage(const QImage& image, int dstWidth, int dstHeight, QVector<float>* chw, QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!chw)
        return fail(QStringLiteral("chw is null"));
    if (image.isNull() || dstWidth < 1 || dstHeight < 1)
        return fail(QStringLiteral("empty layout image"));

    const QImage src = image.convertToFormat(QImage::Format_RGB888);
    const int sw = src.width();
    const int sh = src.height();

    struct RawKernel {
        int first = 0;
        int count = 0;
        double weights[4] = {0.0, 0.0, 0.0, 0.0};
    };
    struct Kernel {
        int first = 0;
        int count = 0;
        int weights[4] = {0, 0, 0, 0};
    };

    auto cubic = [](double x) {
        constexpr double a = -0.75;
        x = std::abs(x);
        if (x < 1.0)
            return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
        if (x < 2.0)
            return ((a * x - 5.0 * a) * x + 8.0 * a) * x - 4.0 * a;
        return 0.0;
    };
    auto makeKernels = [&](int inputSize, int outputSize, int* precision) {
        const double scale = double(inputSize) / double(outputSize);
        std::vector<RawKernel> raw(static_cast<size_t>(outputSize));
        double weightMax = 0.0;
        for (int i = 0; i < outputSize; ++i) {
            const double real = scale * (double(i) + 0.5) - 0.5;
            const int ii = std::min(int(std::floor(float(real))), inputSize - 1);
            const double lambda = std::min(std::max(real - double(ii), 0.0), 1.0);
            const int umin = ii - 1;
            const int umax = ii + 3;
            RawKernel& kernel = raw[size_t(i)];
            kernel.first = std::max(umin, 0);
            kernel.count = std::min(umax, inputSize) - kernel.first;
            kernel.count = std::min(std::max(kernel.count, 0), 4);
            int weightIndex = 0;
            for (int j = 0; j < 4; ++j) {
                const double weight = cubic(double(j - 1) - lambda);
                if (umin + j <= 0)
                    weightIndex = 0;
                else if (umin + j >= inputSize - 1)
                    weightIndex = kernel.count - 1;
                kernel.weights[weightIndex] += weight;
                weightMax = std::max(weightMax, kernel.weights[weightIndex]);
                ++weightIndex;
            }
        }

        *precision = 0;
        while (*precision < 22) {
            const double scaled = weightMax * double(1 << (*precision + 1));
            if (int(0.5 + scaled) >= (1 << 15))
                break;
            ++*precision;
        }

        std::vector<Kernel> result(static_cast<size_t>(outputSize));
        const double multiplier = double(1 << *precision);
        for (int i = 0; i < outputSize; ++i) {
            result[size_t(i)].first = raw[size_t(i)].first;
            result[size_t(i)].count = raw[size_t(i)].count;
            for (int j = 0; j < 4; ++j) {
                const double value = raw[size_t(i)].weights[j] * multiplier;
                result[size_t(i)].weights[j] = int(value < 0.0 ? value - 0.5 : value + 0.5);
            }
        }
        return result;
    };

    int horizontalPrecision = 0;
    int verticalPrecision = 0;
    const std::vector<Kernel> horizontal = makeKernels(sw, dstWidth, &horizontalPrecision);
    const std::vector<Kernel> vertical = makeKernels(sh, dstHeight, &verticalPrecision);
    std::vector<std::uint8_t> intermediate(size_t(sh) * size_t(dstWidth) * 3);

    const int horizontalRound = 1 << (horizontalPrecision - 1);
    for (int y = 0; y < sh; ++y) {
        const uchar* srcRow = src.constScanLine(y);
        for (int x = 0; x < dstWidth; ++x) {
            const Kernel& kernel = horizontal[size_t(x)];
            for (int c = 0; c < 3; ++c) {
                int sum = horizontalRound;
                for (int k = 0; k < kernel.count; ++k)
                    sum += int(srcRow[(kernel.first + k) * 3 + c]) * kernel.weights[k];
                intermediate[(size_t(y) * size_t(dstWidth) + size_t(x)) * 3 + size_t(c)] =
                    std::uint8_t(clampInt(sum >> horizontalPrecision, 0, 255));
            }
        }
    }

    chw->resize(3 * dstWidth * dstHeight);
    const int verticalRound = 1 << (verticalPrecision - 1);
    const int planeSize = dstWidth * dstHeight;
    for (int y = 0; y < dstHeight; ++y) {
        const Kernel& kernel = vertical[size_t(y)];
        for (int x = 0; x < dstWidth; ++x) {
            for (int c = 0; c < 3; ++c) {
                int sum = verticalRound;
                for (int k = 0; k < kernel.count; ++k) {
                    const size_t index =
                        (size_t(kernel.first + k) * size_t(dstWidth) + size_t(x)) * 3 + size_t(c);
                    sum += int(intermediate[index]) * kernel.weights[k];
                }
                const std::uint8_t value = std::uint8_t(clampInt(sum >> verticalPrecision, 0, 255));
                (*chw)[c * planeSize + y * dstWidth + x] = float(value) * (1.0f / 255.0f);
            }
        }
    }
    if (err)
        err->clear();
    return true;
}

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
                                 QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!dets || !logits || !predBoxes || numQueries <= 0 || numClasses <= 0)
        return fail(QStringLiteral("invalid layout tensors"));

    std::vector<int> classIds(numQueries, 0);
    std::vector<float> maxProbs(numQueries, 0.f);
    std::vector<char> keepMask(size_t(numQueries), 0);
    for (int q = 0; q < numQueries; ++q) {
        int best = 0;
        float bestv = logits[q * numClasses];
        for (int c = 1; c < numClasses; ++c) {
            const float v = logits[q * numClasses + c];
            if (v > bestv) {
                bestv = v;
                best = c;
            }
        }
        classIds[size_t(q)] = best;
        maxProbs[size_t(q)] = sigmoid(bestv);
        const float thr = (best >= 0 && best < kLayoutNumClasses) ? kClassThresholds[best] : 0.5f;
        keepMask[size_t(q)] = char(maxProbs[size_t(q)] >= thr);
    }

    std::vector<int> order(numQueries, 0);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (keepMask[size_t(a)] != keepMask[size_t(b)])
            return keepMask[size_t(a)] > keepMask[size_t(b)];
        return a < b;
    });

    std::vector<float> sortedLogits(numQueries * numClasses, 0.f);
    std::vector<float> sortedBoxes(numQueries * 4, 0.f);
    std::vector<int> sortedClass(numQueries, 0);
    std::vector<char> sortedMask(numQueries, 0);
    std::vector<float> padBoxes(size_t(numQueries * 4), 0);
    std::vector<int> padClass(size_t(numQueries), 0);
    for (int i = 0; i < numQueries; ++i) {
        const int src = order[size_t(i)];
        std::memcpy(&sortedLogits[size_t(i * numClasses)], &logits[src * numClasses],
                    size_t(numClasses) * sizeof(float));
        std::memcpy(&sortedBoxes[size_t(i * 4)], &predBoxes[src * 4], 4 * sizeof(float));
        sortedClass[size_t(i)] = classIds[size_t(src)];
        sortedMask[size_t(i)] = keepMask[size_t(src)];
        if (sortedMask[size_t(i)]) {
            const float cx = sortedBoxes[size_t(i * 4 + 0)];
            const float cy = sortedBoxes[size_t(i * 4 + 1)];
            const float w = sortedBoxes[size_t(i * 4 + 2)];
            const float h = sortedBoxes[size_t(i * 4 + 3)];
            padBoxes[size_t(i * 4 + 0)] = std::clamp((cx - 0.5f * w) * 1000.0f, 0.0f, 1000.0f);
            padBoxes[size_t(i * 4 + 1)] = std::clamp((cy - 0.5f * h) * 1000.0f, 0.0f, 1000.0f);
            padBoxes[size_t(i * 4 + 2)] = std::clamp((cx + 0.5f * w) * 1000.0f, 0.0f, 1000.0f);
            padBoxes[size_t(i * 4 + 3)] = std::clamp((cy + 0.5f * h) * 1000.0f, 0.0f, 1000.0f);
            padClass[size_t(i)] = sortedClass[size_t(i)];
        }
        const int cls = padClass[size_t(i)];
        padClass[size_t(i)] = (cls >= 0 && cls < kLayoutNumClasses) ? kClassOrder[cls] : 0;
        (void)padBoxes;
    }

    std::vector<int> orderSeq(numQueries, 0);
    if (orderLogits) {
        std::vector<float> votes(numQueries, 0.f);
        // official: triu(S,1).sum(dim=1) + (1-S^T).tril(-1).sum(dim=1)
        // votes[i] = sum_{k<i} S[k,i] + sum_{k>i} (1 - S[i,k])
        for (int i = 0; i < numQueries; ++i) {
            float v = 0;
            for (int k = 0; k < i; ++k)
                v += sigmoid(orderLogits[k * numQueries + i]);
            for (int k = i + 1; k < numQueries; ++k)
                v += 1.0f - sigmoid(orderLogits[i * numQueries + k]);
            votes[size_t(i)] = v;
        }
        std::vector<int> pointers(numQueries, 0);
        std::iota(pointers.begin(), pointers.end(), 0);
        std::stable_sort(pointers.begin(), pointers.end(), [&](int a, int b) {
            if (votes[size_t(a)] != votes[size_t(b)])
                return votes[size_t(a)] < votes[size_t(b)];
            return a < b;
        });
        for (int rank = 0; rank < numQueries; ++rank)
            orderSeq[size_t(pointers[size_t(rank)])] = rank;
    } else {
        std::iota(orderSeq.begin(), orderSeq.end(), 0);
    }

    std::vector<float> xyxy(numQueries * 4, 0.f);
    for (int i = 0; i < numQueries; ++i) {
        const float cx = sortedBoxes[size_t(i * 4 + 0)];
        const float cy = sortedBoxes[size_t(i * 4 + 1)];
        const float w = sortedBoxes[size_t(i * 4 + 2)];
        const float h = sortedBoxes[size_t(i * 4 + 3)];
        xyxy[size_t(i * 4 + 0)] = (cx - 0.5f * w) * float(imageWidth);
        xyxy[size_t(i * 4 + 1)] = (cy - 0.5f * h) * float(imageHeight);
        xyxy[size_t(i * 4 + 2)] = (cx + 0.5f * w) * float(imageWidth);
        xyxy[size_t(i * 4 + 3)] = (cy + 0.5f * h) * float(imageHeight);
    }

    struct Hit {
        float score;
        int flat;
    };
    std::vector<Hit> hits(numQueries * numClasses);
    for (int q = 0; q < numQueries; ++q) {
        for (int c = 0; c < numClasses; ++c) {
            hits[size_t(q * numClasses + c)] = Hit{sigmoid(sortedLogits[size_t(q * numClasses + c)]),
                                                   q * numClasses + c};
        }
    }
    const int topk = numQueries;
    std::partial_sort(hits.begin(), hits.begin() + topk, hits.end(), [](const Hit& a, const Hit& b) {
        if (a.score != b.score)
            return a.score > b.score;
        return a.flat < b.flat;
    });

    struct Kept {
        float score;
        int label;
        int query;
        int order;
    };
    std::vector<Kept> kept;
    kept.reserve(size_t(topk));
    for (int i = 0; i < topk; ++i) {
        if (hits[size_t(i)].score < conf)
            continue;
        const int flat = hits[size_t(i)].flat;
        const int q = flat / numClasses;
        const int c = flat % numClasses;
        kept.push_back(Kept{hits[size_t(i)].score, c, q, orderSeq[size_t(q)]});
    }
    std::stable_sort(kept.begin(), kept.end(), [](const Kept& a, const Kept& b) {
        if (a.order != b.order)
            return a.order < b.order;
        return a.query < b.query;
    });

    QVector<LayoutBox> parsed;
    int index = 1;
    for (const Kept& k : kept) {
        const auto bb = clipBbox(xyxy[size_t(k.query * 4 + 0)], xyxy[size_t(k.query * 4 + 1)],
                                 xyxy[size_t(k.query * 4 + 2)], xyxy[size_t(k.query * 4 + 3)],
                                 imageHeight, imageWidth);
        if (!bb)
            continue;
        LayoutBox box;
        box.clsId = k.label;
        box.label = labelName(k.label);
        box.score = float(pythonRoundFloat32ToDecimalDigits(k.score, 4));
        box.xmin = bb->at(0);
        box.ymin = bb->at(1);
        box.xmax = bb->at(2);
        box.ymax = bb->at(3);
        box.index = index++;
        parsed.push_back(box);
    }
    if (usePaddlexFilter)
        parsed = applyPaddlexFilter(std::move(parsed), false);
    parsed = applyLayoutPostProcess(std::move(parsed), imageHeight, imageWidth);
    *dets = std::move(parsed);
    if (err)
        err->clear();
    return true;
}

bool runOfficialLayout(const QImage& image, OfficialLayoutOutput* out, QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));
    if (image.isNull())
        return fail(QStringLiteral("empty page image"));

    const QString dir = resolveOfficialLayoutDir();
    if (!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("model.safetensors"))))
        return fail(QStringLiteral("missing official PP-DocLayoutV2 model.safetensors"));

    QVector<float> chw;
    if (!preprocessLayoutImage(image, kLayoutImageSize, kLayoutImageSize, &chw, err))
        return false;

    QVector<float> logits, boxes, orderLogits;
    if (!officialLayoutForward(chw.constData(), kLayoutImageSize, kLayoutImageSize, &logits, &boxes, &orderLogits,
                               err))
        return false;

    OfficialLayoutOutput result;
    result.engine = QStringLiteral("official-PPDocLayoutV2");
    result.weight = dir;
    result.device = QStringLiteral("mlx-safetensors");
    result.pageWidth = image.width();
    result.pageHeight = image.height();
    if (!postprocessLayoutDetections(logits.constData(), boxes.constData(), orderLogits.constData(),
                                     kLayoutNumQueries, kLayoutNumClasses, image.height(), image.width(), kLayoutConf,
                                     true, &result.dets, err))
        return false;
    *out = std::move(result);
    if (err)
        err->clear();
    return true;
}

bool writeOfficialLayoutJson(const OfficialLayoutOutput& result, const QString& path, QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(QStringLiteral("cannot create layout json directory"));
    QJsonArray dets;
    for (const LayoutBox& b : result.dets) {
        QJsonObject o;
        o.insert(QStringLiteral("cls_id"), b.clsId);
        o.insert(QStringLiteral("label"), b.label);
        o.insert(QStringLiteral("score"),
                 pythonRoundFloat32ToDecimalDigits(b.score, 4));
        o.insert(QStringLiteral("bbox"), QJsonArray({b.xmin, b.ymin, b.xmax, b.ymax}));
        o.insert(QStringLiteral("index"), b.index);
        dets.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("engine"), result.engine);
    root.insert(QStringLiteral("weight"), result.weight);
    root.insert(QStringLiteral("device"), result.device);
    root.insert(QStringLiteral("page_size"), QJsonArray({result.pageWidth, result.pageHeight}));
    root.insert(QStringLiteral("dets"), dets);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("cannot write layout json"));
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (f.write(bytes) != bytes.size() || !f.flush())
        return fail(QStringLiteral("cannot write layout json bytes"));
    if (err)
        err->clear();
    return true;
}

}  // namespace hybrid
}  // namespace scanengine

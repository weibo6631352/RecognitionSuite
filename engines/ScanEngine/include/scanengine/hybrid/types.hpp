#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

namespace scanengine {
namespace hybrid {

// Official: only medium / high (hybrid_analyze._validate_parse_effort).
enum class Effort { Medium, High };

inline QString effortName(Effort e) {
    return e == Effort::High ? QStringLiteral("high") : QStringLiteral("medium");
}

inline std::optional<Effort> effortFromName(const QString& s) {
    if (s == QLatin1String("medium"))
        return Effort::Medium;
    if (s == QLatin1String("high"))
        return Effort::High;
    return std::nullopt;
}

// Official ContentBlock (mineru_vl_utils). bbox is unit square 0..1.
struct ContentBlock {
    QString type;
    double xmin = 0;
    double ymin = 0;
    double xmax = 0;
    double ymax = 0;
    std::optional<int> angle;
    QString content;
    bool contentNull = false;  // stage 30 layout hint uses JSON null before VLM extraction
    bool mergePrev = false;
    QString subType;  // seal → "seal"
};

// Pipeline layout det (hybrid_analyze._build_medium_vlm_layout_blocks input).
struct LayoutDet {
    QString label;
    float xmin = 0;
    float ymin = 0;
    float xmax = 0;
    float ymax = 0;
    QString angle;
};

struct HybridOptions {
    QString inputPath;
    QString outputDir;
    Effort effort = Effort::Medium;
    bool formulaEnable = true;
    bool tableEnable = true;
    bool imageAnalysis = true;  // ignored on medium (forced false)
    QString parseMethod = QStringLiteral("auto");
};

struct Stage {
    QString id;
    QString referenceOperation;
    QString model;
};

}  // namespace hybrid
}  // namespace scanengine

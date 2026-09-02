#include "scanengine/hybrid/orchestrator.hpp"

#include "scanengine/hybrid/rounding.hpp"

#include <algorithm>
#include <cmath>

namespace scanengine {
namespace hybrid {

namespace {

// hybrid_analyze.MEDIUM_EFFORT_LAYOUT_LABEL_TO_VLM_TYPE
// Values are mineru.utils.enum_class.BlockType strings.
struct MediumLabel {
    const char* label;
    const char* vlmType;
};

constexpr MediumLabel kMediumEffortLayoutLabelToVlmType[] = {
    {"abstract", "text"},
    {"algorithm", "code"},
    {"aside_text", "aside_text"},
    {"content", "index"},
    {"doc_title", "title"},
    {"footer", "footer"},
    {"footer_image", "footer"},
    {"footnote", "page_footnote"},
    {"formula_number", "formula_number"},
    {"header", "header"},
    {"header_image", "header"},
    {"number", "page_number"},
    {"paragraph_title", "title"},
    {"reference_content", "ref_text"},
    {"text", "text"},
    {"vertical_text", "text"},
    {"figure_title", "image_caption"},
    {"vision_footnote", "image_footnote"},
    {"image", "image"},
    {"chart", "chart"},
    {"seal", "image"},
    {"table", "table"},
    {"display_formula", "equation"},
};

}  // namespace

bool effectiveImageAnalysis(Effort effort, bool requested) {
    // hybrid_analyze._resolve_effective_image_analysis
    if (effort == Effort::Medium)
        return false;
    return requested;
}

QStringList mediumEffortLayoutLabels() {
    QStringList labels;
    labels.reserve(int(sizeof(kMediumEffortLayoutLabelToVlmType)
                       / sizeof(kMediumEffortLayoutLabelToVlmType[0])));
    for (const MediumLabel& e : kMediumEffortLayoutLabelToVlmType)
        labels << QString::fromUtf8(e.label);
    return labels;
}

QString mediumLayoutLabelToVlmType(const QString& layoutLabel) {
    for (const MediumLabel& e : kMediumEffortLayoutLabelToVlmType) {
        if (layoutLabel == QLatin1String(e.label))
            return QString::fromUtf8(e.vlmType);
    }
    return QString();
}

int normalizeMediumVlmAngle(const QString& angle) {
    bool ok = false;
    const int v = angle.toInt(&ok);
    if (!ok)
        return 0;
    if (v == 0 || v == 90 || v == 180 || v == 270)
        return v;
    return 0;
}

bool layoutBboxToUnit(double* xmin, double* ymin, double* xmax, double* ymax,
                      int pageWidth, int pageHeight) {
    if (!xmin || !ymin || !xmax || !ymax || pageWidth <= 0 || pageHeight <= 0)
        return false;
    double x0 = double(*xmin), y0 = double(*ymin), x1 = double(*xmax), y1 = double(*ymax);
    const bool alreadyUnit = (x0 >= 0 && x0 <= 1 && y0 >= 0 && y0 <= 1 && x1 >= 0 && x1 <= 1
                              && y1 >= 0 && y1 <= 1);
    if (!alreadyUnit) {
        x0 /= double(pageWidth);
        y0 /= double(pageHeight);
        x1 /= double(pageWidth);
        y1 /= double(pageHeight);
    }
    // hybrid_analyze.normalize_bbox_to_unit.
    auto clamp3 = [](double v) {
        v = std::min(1.0, std::max(0.0, v));
        return roundToDecimalDigits(v, 3);
    };
    *xmin = clamp3(x0);
    *ymin = clamp3(y0);
    *xmax = clamp3(x1);
    *ymax = clamp3(y1);
    return *xmax > *xmin && *ymax > *ymin;
}

QVector<ContentBlock> buildMediumVlmLayoutBlocks(const QVector<LayoutDet>& dets,
                                                 int pageWidth,
                                                 int pageHeight) {
    QVector<ContentBlock> blocks;
    for (const LayoutDet& d : dets) {
        const QString vlm = mediumLayoutLabelToVlmType(d.label);
        if (vlm.isEmpty())
            continue;
        ContentBlock b;
        b.type = vlm;
        b.xmin = d.xmin;
        b.ymin = d.ymin;
        b.xmax = d.xmax;
        b.ymax = d.ymax;
        if (!layoutBboxToUnit(&b.xmin, &b.ymin, &b.xmax, &b.ymax, pageWidth, pageHeight))
            continue;
        b.angle = normalizeMediumVlmAngle(d.angle);
        if (d.label == QLatin1String("seal"))
            b.subType = QStringLiteral("seal");
        b.contentNull = true;
        blocks.push_back(b);
    }
    return blocks;
}

QVector<Stage> planStages(Effort effort, bool ocrEnable) {
    QVector<Stage> s;
    auto add = [&](const char* id, const char* fn, const char* model) {
        Stage st;
        st.id = QString::fromUtf8(id);
        st.referenceOperation = QString::fromUtf8(fn);
        st.model = QString::fromUtf8(model);
        s.push_back(st);
    };
    add("validate_effort", "_validate_parse_effort", "");
    add("resolve_image_analysis", "_resolve_effective_image_analysis", "");
    add("load_vlm", "vlm_analyze.ModelSingleton.get_model", "MinerU2.5-Pro mlx");
    add("ocr_classify", "ocr_classify", "");
    add("init_middle_json", "init_middle_json", "");
    add("load_images", "load_images_from_pdf_doc", "");
    add("predict_layout", "_predict_layout_for_window", "PP-DocLayoutV2");
    if (effort == Effort::Medium) {
        add("table_orientation", "_apply_medium_table_orientation_labels", "TableOrientationCls");
        add("build_vlm_blocks", "_build_medium_vlm_layout_blocks", "");
        add("vlm_extract", "batch_extract_with_layout", "VLM extract");
        add("formula_number", "optimize_hybrid_formula_number_blocks", "");
    } else {
        add("vlm_two_step", "batch_two_step_extract", "VLM layout+extract");
    }
    if (ocrEnable)
        add("ocr_sidecar", "_apply_vlm_ocr_det_sidecars_for_window", "OCR det");
    else
        add("ocr_formula", "_process_ocr_and_formulas", "MFR+OCR det");
    add("title_split", "_apply_layout_title_split", "");
    add("finalize", "finalize_middle_json", "");
    return s;
}

}  // namespace hybrid
}  // namespace scanengine

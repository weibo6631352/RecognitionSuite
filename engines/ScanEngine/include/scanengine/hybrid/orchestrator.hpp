#pragma once

#include "scanengine/hybrid/types.hpp"

namespace scanengine {
namespace hybrid {

// Mirrors hybrid_analyze.doc_analyze effort branches.
// Does not invent pipeline OCR. Model steps are named, not replaced.
QVector<Stage> planStages(Effort effort, bool ocrEnable);

bool effectiveImageAnalysis(Effort effort, bool requested);

// Official medium layout labels that VLM is allowed to see.
QStringList mediumEffortLayoutLabels();

// hybrid_analyze._vlm_type_for_medium_layout_label.
// Empty if the label is unknown (e.g. inline_formula).
QString mediumLayoutLabelToVlmType(const QString& layoutLabel);

int normalizeMediumVlmAngle(const QString& angle);

// hybrid_analyze.normalize_bbox_to_unit. false if bbox invalid.
bool layoutBboxToUnit(double* xmin, double* ymin, double* xmax, double* ymax,
                      int pageWidth, int pageHeight);

// hybrid_analyze._build_medium_vlm_layout_blocks
QVector<ContentBlock> buildMediumVlmLayoutBlocks(const QVector<LayoutDet>& dets,
                                                 int pageWidth,
                                                 int pageHeight);

}  // namespace hybrid
}  // namespace scanengine

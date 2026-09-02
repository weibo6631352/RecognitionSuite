#pragma once

#include <QJsonArray>

namespace scanengine {
namespace hybrid {

// MinerU Hybrid model-list canonicalization. The page array is rewritten in
// place while the order and fields of all retained blocks are preserved.
void optimizeHybridFormulaNumberBlocks(QJsonArray& page);

// Split VLM "title" blocks using only PP-DocLayoutV2 "doc_title"
// detections. Bboxes may be normalized unit coordinates or page pixels.
void applyLayoutTitleSplit(QJsonArray& page,
                           const QJsonArray& layoutDets,
                           double pageWidth,
                           double pageHeight,
                           double overlapThreshold = 0.8);

}  // namespace hybrid
}  // namespace scanengine

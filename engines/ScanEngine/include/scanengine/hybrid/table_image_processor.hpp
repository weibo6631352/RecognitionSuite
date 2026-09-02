#pragma once

#include "scanengine/hybrid/types.hpp"

#include <QHash>
#include <QImage>
#include <QSet>
#include <QSize>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

namespace scanengine {
namespace hybrid {

// Canonical source lock (raw-file SHA-256, mineru-vl-utils 1.2.1):
//   post_process/table_image_processor.py
//     288013481da8215dacfea43102ce997c8f2d815ebdd813d89b14beabd5777b32
//   mineru_client.py (prepare_for_extract)
//     08ece713ee9e6c08b96e4cddeaf098b32c0537f84c7a7174f6a2b9e4a5eb3bd1

inline constexpr double kOfficialTableImageCoverageThreshold = 0.9;
inline constexpr int kOfficialTableImageTokenLength = 4;
inline constexpr char kOfficialTableImageTokenChars[] = "ACDGHKTWXYZ2345678";

// A half-open pixel box: [left, right) x [top, bottom).  QRect's inclusive
// right()/bottom() convention is deliberately avoided because the pinned
// Python implementation performs its rotations on crop coordinates.
struct TableImagePixelBox {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int width() const { return right - left; }
    int height() const { return bottom - top; }
    bool isValid() const { return width() > 0 && height() > 0; }
};

struct TableImageEntry {
    int blockIndex = -1;
    ContentBlock block;
};

struct TableImageToken {
    int imageBlockIndex = -1;
    QString code;       // Four characters, without brackets.
    QString token;      // "[CODE]", as painted into the table crop.
    QString dataUri;    // data:image/jpeg;base64,...
    TableImagePixelBox maskBox;
    bool usedFallbackFontRaster = false;
};

struct TableImageMaskedCrop {
    QImage image;
    QVector<TableImageToken> tokens;
};

// ContentBlock intentionally remains the official public schema.  The two
// private Python dictionary keys are represented by this request-local
// sidecar, so neither key can leak into model.json or middle.json.
struct TableImageMetadata {
    QVector<int> tableOrder;
    QHash<int, QVector<int>> tableToImages;
    QSet<int> absorbedImageIndices;
    QHash<int, QVector<TableImageToken>> tokensByTable;

    void clear();
    bool isEmpty() const;
};

struct TableImageProcessorOptions {
    // Optional deterministic seam for differential tests.  The callback
    // returns an unbracketed candidate code.  Production leaves it empty and
    // uses QRandomGenerator; invalid/duplicate candidates are retried.
    std::function<QString()> tokenCodeGenerator;

    // Supported builds use the vendored FreeType 2.14.3 rasterizer and an
    // explicit Arial/DejaVu file, independent of Qt's font database.  The
    // default therefore fails closed if no candidate can be rendered;
    // diagnostics may explicitly opt into the visibly marked 5x7 fallback.
    bool allowFallbackFontRaster = false;
};

// intersection(inner, outer) / area(inner), matching both
// _bbox_cover_ratio and _overlap_ratio in the pinned Python sources.
double tableImageCoverageRatio(const ContentBlock& inner, const ContentBlock& outer);

// prepare_for_extract first removes image_caption blocks covered >= threshold
// by image/chart/image_block.  The surviving order is stable.
int removeInternalImageCaptions(
    QVector<ContentBlock>* blocks,
    double threshold = kOfficialTableImageCoverageThreshold,
    QVector<int>* removedOriginalIndices = nullptr,
    QVector<int>* keptOriginalIndices = nullptr);

// Build the table -> image assignment, absorbed sidecar and stable table
// order.  Selection priority is higher coverage, then smaller table, then the
// first table in tableIndices.  Images within a table are ordered by (y, x).
TableImageMetadata buildTableImageMap(
    const QVector<ContentBlock>& blocks,
    double threshold = kOfficialTableImageCoverageThreshold);
TableImageMetadata buildTableImageMap(
    const QVector<ContentBlock>& blocks,
    const QVector<int>& tableIndices,
    double threshold = kOfficialTableImageCoverageThreshold);

void markAbsorbedTableImages(TableImageMetadata* metadata);
bool isAbsorbedTableImage(const TableImageMetadata& metadata, int blockIndex);

int normalizeTableImageAngle(const std::optional<int>& angle);
TableImagePixelBox rotateTableImageBox(const TableImagePixelBox& box,
                                       const QSize& originalImageSize,
                                       int angle);
QImage rotateTableImage(const QImage& image, int angle);

// Port of mask_and_encode_table_image.  tableImage is the already-cropped,
// unrotated table crop produced by prepare_for_extract.  The returned crop is
// rotated by tableBlock.angle, each embedded image region is masked and
// labelled, and each embedded crop is encoded as a JPEG data URI after the
// same rotation.
bool maskAndEncodeTableImage(const QImage& pageImage,
                             const ContentBlock& tableBlock,
                             const QVector<TableImageEntry>& imageEntries,
                             const QImage& tableImage,
                             TableImageMaskedCrop* out,
                             QString* error = nullptr,
                             const TableImageProcessorOptions& options = {});

// Integration helper for QVector<ContentBlock>.  It resolves image entries
// from metadata.tableToImages and records the generated token metadata for
// post-processing.
bool prepareTableImageForExtract(const QImage& pageImage,
                                 const QVector<ContentBlock>& blocks,
                                 int tableIndex,
                                 const QImage& tableImage,
                                 TableImageMetadata* metadata,
                                 QImage* maskedTableImage,
                                 QString* error = nullptr,
                                 const TableImageProcessorOptions& options = {});

QString replaceTableImageTokens(const QString& content,
                                const QVector<TableImageToken>& tokens);
void applyTableImageTokens(QVector<ContentBlock>* blocks,
                           const TableImageMetadata& metadata);
void removeAbsorbedTableImages(QVector<ContentBlock>* blocks,
                               const TableImageMetadata& metadata);
void cleanupTableImageMetadata(TableImageMetadata* metadata);

}  // namespace hybrid
}  // namespace scanengine

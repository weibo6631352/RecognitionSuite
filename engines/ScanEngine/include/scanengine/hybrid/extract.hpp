#pragma once

#include "scanengine/hybrid/prompts.hpp"
#include "scanengine/hybrid/qwen2_language.hpp"
#include "scanengine/hybrid/qwen2_vision.hpp"
#include "scanengine/hybrid/table_image_processor.hpp"
#include "scanengine/hybrid/tokenizer.hpp"
#include "scanengine/hybrid/types.hpp"

#include <QImage>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QVector>

#include <functional>

namespace scanengine {
namespace hybrid {

struct PreparedExtract {
    QImage crop;
    QString prompt;
    OfficialSampling sampling;
    int blockIndex = -1;
};

struct OfficialExtractModels {
    const Qwen2LanguageModel* language = nullptr;
    const Qwen2VisionModel* vision = nullptr;
    const OfficialTokenizer* tokenizer = nullptr;
};

// Backend and deterministic seams around the canonical prepare/post-process
// pipeline. Production leaves this null and uses the in-process official VLM.
// A future GPU backend can implement the same crop/prompt contract without
// duplicating MinerU's filtering, table-image or post-process business rules.
struct OfficialExtractHooks {
    std::function<bool(const PreparedExtract&, QString* text, QString* err)> infer;
    TableImageProcessorOptions tableImage;
};

// PIL Image.rotate(angle, expand=True) NEAREST; 90=CCW, 270=CW (cv2 table ori).
QImage rotateOfficialPil(int angle, const QImage& src);

// mineru_client.resize_by_need
QImage resizeByNeed(const QImage& image,
                    int minEdge = kOfficialMinImageEdge,
                    double maxEdgeRatio = kOfficialMaxImageEdgeRatio);

// images_bytes_to_pdf_bytes(resolution=200) + page_to_image(dpi=200, max=3500).
// pdfWidth/pdfHeight are int(page.get_size()) for middle.json.
QImage loadOfficialHybridPage(const QString& path,
                              int* pdfWidth = nullptr,
                              int* pdfHeight = nullptr,
                              double* renderScale = nullptr);

// The exact scalar returned by MinerU page_to_image.  loadOfficialHybridPage
// stores it on the in-memory QImage so downstream cut_image calls do not have
// to reconstruct it from rounded PDF dimensions.
double officialHybridPageRenderScale(const QImage& page);

// mineru.utils.pdf_image_tools.get_crop_img: multiply all PDF-space bbox
// coordinates by one render scale, floor left/top and ceil right/bottom.
QImage cropOfficialHybridPage(const QImage& page,
                              const QJsonArray& pdfBbox,
                              double renderScale);

// Pillow Image.save(..., format="JPEG") defaults used by cut_image.
bool writeOfficialHybridCropJpeg(const QImage& crop,
                                 const QString& path,
                                 QString* err = nullptr);

// Legacy typed projection of mineru_client.prepare_for_extract. Formal Hybrid
// medium callers use extractOfficialPageWithLayout so the authoritative JSON,
// table-image metadata and full post-process remain synchronized.
QVector<PreparedExtract> prepareForExtract(const QImage& page,
                                           const QVector<ContentBlock>& blocks,
                                           bool imageAnalysis,
                                           const QStringList& notExtractList);

// mineru_client.extract_with_layout — one page, official VLM greedy.
bool extractWithLayout(const Qwen2LanguageModel& language,
                       const Qwen2VisionModel& vision,
                       const OfficialTokenizer& tok,
                       const QImage& page,
                       QVector<ContentBlock>* blocks,
                       bool imageAnalysis,
                       const QStringList& notExtractList,
                       int backendTokenLimit,
                       QString* err,
                       const std::function<bool()>& isCancelled = {});

// Authoritative Hybrid medium extract path. The QJsonArray remains the single
// source of truth so prepare/post-process deletions, newly created equation
// blocks, content nullability and caller-owned extension fields stay aligned.
// Model objects are loaded lazily only when prepare produces at least one job;
// callers with cached models may pass them through models.
bool extractOfficialPageWithLayout(const QImage& page,
                                   QJsonArray* pageBlocks,
                                   bool imageAnalysis,
                                   const QStringList& notExtractList,
                                   int backendTokenLimit,
                                   const OfficialExtractModels* models,
                                   QString* err,
                                   const std::function<bool()>& isCancelled = {},
                                   const OfficialExtractHooks* hooks = nullptr);

// Strict typed projection used only for geometry/model dispatch. JSON callers
// must retain their original QJsonArray rather than reserializing this view.
bool projectOfficialContentBlocks(const QJsonArray& page,
                                  QVector<ContentBlock>* blocks,
                                  QString* err);

// Construct the official ContentBlock JSON form for parser-created blocks.
QJsonArray officialContentBlocksJson(const QVector<ContentBlock>& blocks);

// mineru_client.prepare_for_layout / parse_layout_output / two_step_extract
QImage prepareForLayout(const QImage& image);
QVector<ContentBlock> parseOfficialLayoutOutput(const QString& output);
bool twoStepExtract(const Qwen2LanguageModel& language,
                    const Qwen2VisionModel& vision,
                    const OfficialTokenizer& tok,
                    const QImage& page,
                    QVector<ContentBlock>* blocks,
                    bool imageAnalysis,
                    const QStringList& notExtractList,
                    int layoutMaxTokens,
                    int extractMaxTokens,
                    QString* rawLayout,
                    QString* err);

bool officialHybridReady();
QStringList officialHybridMissingFiles();

bool loadOfficialLayoutJson(const QString& path,
                            QVector<LayoutDet>* dets,
                            int* pageWidth,
                            int* pageHeight,
                            QString* err);

// Official PP-DocLayoutV2 in C++ (official model.safetensors + official graph).
bool runOfficialLayoutSidecar(const QString& imagePath, const QString& outJson, QString* err);

// Official MineruTableOrientationCls in C++ (same PP-OCRv6 as mineru).
bool runOfficialTableOrientSidecar(const QString& imagePath,
                                   const QString& layoutJson,
                                   const QString& outJson,
                                   QString* err);

// Official medium VLM layout hints (stage 30). Accepts a C++ layout sidecar
// or an official stage-10/20 trace envelope. Page size comes from the sidecar
// or, for traces, from --image / explicit page dimensions.
bool runOfficialMediumHintsSidecar(const QString& layoutJson,
                                   const QString& outJson,
                                   QString* err,
                                   const QString& imagePath = QString(),
                                   int pageWidth = 0,
                                   int pageHeight = 0);

// Official hybrid OCR det sidecar in C++ (det-only, empty ocr_text).
bool runOfficialOcrSidecar(const QString& imagePath,
                           const QString& blocksJson,
                           const QString& layoutJson,
                           const QString& outJson,
                           QString* err);

// Append sidecar ocr_text / inline_formula items onto a model.json page.
bool mergeOfficialOcrSidecarItems(const QJsonObject& sidecar, QJsonArray* page, QString* err);

// Read/write the single-page model.json envelope: [[{...}, ...]].  The
// QJsonArray form intentionally preserves sidecar-only fields such as score,
// text and latex instead of squeezing them through ContentBlock.
bool readOfficialModelJson(const QString& path, QJsonArray* page, QString* err);
bool writeOfficialModelJson(const QString& path, const QJsonArray& page, QString* err);

// Official model.json: [[{type,bbox,angle,content}, ...]]
bool writeOfficialModelJson(const QString& path,
                            const QVector<ContentBlock>& blocks,
                            QString* err);

// QJsonDocument collapses integral doubles (for example score=1.0) to JSON
// integers.  MinerU's public Hybrid artifacts retain selected float types, so
// canonical contract writers use this serializer instead of raw toJson().
QByteArray officialHybridJsonBytes(const QJsonDocument& document,
                                   bool modelBboxesAreFloat = false);

// C++ MagicModel subset. imageWidth/Height must be PDF page.get_size() ints.
// preprocPath, when non-empty, receives the stage-70 form before finalize.
bool writeOfficialMiddleJson(const QJsonArray& pageModel,
                             int imageWidth,
                             int imageHeight,
                             const QString& path,
                             QString* err,
                             const QString& effort = QStringLiteral("medium"),
                             const QString& preprocPath = QString(),
                             const QImage* renderedPage = nullptr,
                             const QString& imageDir = QString());

// Canonical output path: Markdown is rendered from para_blocks, while
// content_list additionally includes discarded_blocks, matching MinerU.
// Both files are prepared through QSaveFile before publication. Markdown is
// committed first and content_list second; a false return after the second
// commit attempt can therefore leave the new Markdown published while the
// content_list target remains absent or unchanged. Callers must honor the
// return value, and job discovery should use a separate completion marker
// committed only after this function succeeds.
bool writeHybridPageArtifactsFromMiddle(const QString& outputDir,
                                        const QString& sourceStem,
                                        const QString& middleJsonPath,
                                        QString* markdownPath,
                                        QString* jsonPath,
                                        QString* err);

// Pure, model-free canonical transforms used by the pinned Python
// differential fixtures.  Modes: finalize, visual, list, sidecar.
bool runHybridSchemaFixture(const QString& mode,
                            const QJsonObject& input,
                            QJsonObject* output,
                            QString* err);

}  // namespace hybrid
}  // namespace scanengine

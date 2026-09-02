#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace scanengine {
namespace hybrid {

// mineru_vl_utils.vlm_client.base_client.DEFAULT_SYSTEM_PROMPT
QString officialSystemPrompt();

// mineru_vl_utils.mineru_client.DEFAULT_PROMPTS
QString officialPromptForType(const QString& blockType);

// mineru_vl_utils.mineru_client.MinerUSamplingParams / DEFAULT_SAMPLING_PARAMS
struct OfficialSampling {
    double temperature = 0.0;
    double topP = 0.01;
    int topK = 1;
    double presencePenalty = 0.0;
    double frequencyPenalty = 0.0;
    double repetitionPenalty = 1.0;
    int noRepeatNgramSize = 100;
};

OfficialSampling officialSamplingForType(const QString& blockType);

// mineru.utils.enum_class.NotExtractType — used when OCR sidecar is off.
QStringList officialNotExtractTypes();

// Always skipped in prepare_for_extract, plus image/chart when image_analysis is false.
QStringList officialAlwaysSkipTypes();
QStringList officialImageAnalysisTypes();

// mineru_client.prepare_for_extract skip (without table-absorption / coverage).
bool shouldSkipExtract(const QString& blockType,
                       bool imageAnalysis,
                       const QStringList& notExtractList);

// MinerUClient defaults
constexpr int kOfficialLayoutImageSize = 1036;
constexpr int kOfficialMinImageEdge = 28;
constexpr double kOfficialMaxImageEdgeRatio = 50.0;

}  // namespace hybrid
}  // namespace scanengine

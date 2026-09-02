#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>

namespace scanengine {
namespace hybrid {

// Pinned mineru-vl-utils 1.2.1 MinerUClient defaults.  These switches are
// deliberately independent from the parser options: the caller can replay the
// official post-processing contract against captured model blocks without
// loading a model.
struct OfficialPostProcessOptions {
    bool simplePostProcess = false;
    bool handleEquationBlock = true;
    bool abandonList = false;
    bool abandonParatext = false;
    // MinerUClient's generic default is false.  MinerU's formal Hybrid path
    // (vlm_analyze.py) explicitly supplies true and its caller must do likewise.
    bool enableTableFormulaEqWrap = false;
};

struct OfficialImageAnalysisResult {
    QString className;
    QString subClass;
    QString caption;
    QString content;
};

// The OTSL converter has a built-in default (convertOfficialOtslToHtml).  The
// callbacks make the module independently testable and leave the table-image
// token implementation replaceable when the masking/encoding stage is wired.
struct OfficialPostProcessCallbacks {
    std::function<QString(const QString&)> otslToHtml;
    std::function<QString(const QString&, const QJsonObject&)> replaceTableImageTokens;
    // The built-in path reproduces the pinned image/chart parser, Mermaid
    // validation/repair, and chart repetition repair.  This hook is an explicit
    // override for focused callers and fixtures, rather than a requirement for
    // the formal Hybrid path.
    std::function<OfficialImageAnalysisResult(const QString&)> processImageOrChart;
};

// Pure helpers exposed for differential fixtures.
QString processOfficialEquation(const QString& latex);
QString processOfficialText(const QString& text);
QString addOfficialEquationBrackets(const QString& latex);

// One page and batch variants of mineru_vl_utils.post_process.post_process and
// MinerUClientHelper.batch_post_process.  Every input QJsonObject is copied, so
// arbitrary upstream fields survive unless the pinned implementation explicitly
// creates a replacement block or removes private table-image metadata.
QJsonArray postProcessOfficialBlocks(
    const QJsonArray& blocks,
    const OfficialPostProcessOptions& options = OfficialPostProcessOptions(),
    const OfficialPostProcessCallbacks& callbacks = OfficialPostProcessCallbacks());

QVector<QJsonArray> batchPostProcessOfficialBlocks(
    const QVector<QJsonArray>& blocksList,
    const OfficialPostProcessOptions& options = OfficialPostProcessOptions(),
    const OfficialPostProcessCallbacks& callbacks = OfficialPostProcessCallbacks());

}  // namespace hybrid
}  // namespace scanengine

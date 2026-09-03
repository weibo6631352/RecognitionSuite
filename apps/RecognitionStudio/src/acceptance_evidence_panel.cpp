#include "acceptance_evidence_panel.h"

#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QVBoxLayout>

namespace speechdoc {
namespace {

QPlainTextEdit* makeViewer(const QString& objectName,
                           const QString& placeholder,
                           QWidget* parent) {
    auto* viewer = new QPlainTextEdit(parent);
    viewer->setObjectName(objectName);
    viewer->setReadOnly(true);
    viewer->setPlaceholderText(placeholder);
    viewer->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    return viewer;
}

QString percent(double value) {
    return QStringLiteral("%1%").arg(value * 100.0, 0, 'f', 2);
}

}  // namespace

AcceptanceEvidencePanel::AcceptanceEvidencePanel(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("acceptanceEvidencePanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->setSpacing(6);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("evidenceSummary"));
    summary_->setWordWrap(true);
    layout->addWidget(summary_);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("acceptanceEvidenceTabs"));

    auto* comparison = new QWidget(tabs_);
    auto* comparisonLayout = new QHBoxLayout(comparison);
    comparisonLayout->setContentsMargins(6, 6, 6, 6);
    comparisonLayout->setSpacing(8);
    auto* referenceColumn = new QVBoxLayout;
    referenceColumn->addWidget(new QLabel(QStringLiteral("冻结真值"), comparison));
    reference_ = makeViewer(
        QStringLiteral("evidenceReference"),
        QStringLiteral("选择已完成样本后显示冻结真值"), comparison);
    referenceColumn->addWidget(reference_, 1);
    auto* hypothesisColumn = new QVBoxLayout;
    hypothesisColumn->addWidget(new QLabel(QStringLiteral("识别结果"), comparison));
    hypothesis_ = makeViewer(
        QStringLiteral("evidenceHypothesis"),
        QStringLiteral("选择已完成样本后显示识别结果"), comparison);
    hypothesisColumn->addWidget(hypothesis_, 1);
    comparisonLayout->addLayout(referenceColumn, 1);
    comparisonLayout->addLayout(hypothesisColumn, 1);
    tabs_->addTab(comparison, QStringLiteral("文本对照"));

    sampleJson_ = makeViewer(
        QStringLiteral("evidenceRawJson"),
        QStringLiteral("样本状态、SHA-256、产物路径和置信度将在这里显示"),
        tabs_);
    tabs_->addTab(sampleJson_, QStringLiteral("样本证据"));

    reportJson_ = makeViewer(
        QStringLiteral("evidenceReportJson"),
        QStringLiteral("整套验证结束后显示 report.json 与独立哈希校验"), tabs_);
    tabs_->addTab(reportJson_, QStringLiteral("汇总报告与校验"));
    layout->addWidget(tabs_, 1);

    clearEvidence();
}

void AcceptanceEvidencePanel::clearEvidence() {
    summary_->setText(QStringLiteral(
        "请选择已完成样本查看证据。置信度是辅助诊断信号，不等于实际准确率。"));
    reference_->clear();
    hypothesis_->clear();
    sampleJson_->clear();
    reportJson_->clear();
    tabs_->setCurrentIndex(0);
}

void AcceptanceEvidencePanel::showSample(
    const QString& sampleId,
    const QString& reference,
    const QString& hypothesis,
    const QJsonObject& result) {
    QStringList fields;
    fields << QStringLiteral("样本 %1").arg(sampleId)
           << QStringLiteral("状态 %1")
                  .arg(result.value(QStringLiteral("state")).toString());
    if (result.value(QStringLiteral("accuracy")).isDouble()) {
        fields << QStringLiteral("准确率 %1")
                      .arg(percent(result.value(QStringLiteral("accuracy")).toDouble()));
    }
    fields << QStringLiteral("编辑距离 %1 / 真值 %2")
                  .arg(result.value(QStringLiteral("edit_distance")).toInt())
                  .arg(result.value(QStringLiteral("reference_characters")).toInt());
    const QJsonObject confidence =
        result.value(QStringLiteral("confidence")).toObject();
    if (!confidence.isEmpty()) {
        fields << QStringLiteral("置信度 %1（%2）")
                      .arg(percent(confidence.value(QStringLiteral("mean")).toDouble()),
                           confidence.value(QStringLiteral("calibrated")).toBool()
                               ? QStringLiteral("已校准")
                               : QStringLiteral("未校准"));
    }
    summary_->setText(fields.join(QStringLiteral(" · ")));
    summary_->setToolTip(QStringLiteral(
        "准确率来自冻结真值；置信度来自模型内部评分。未校准分数不能解释为真实正确概率。"));
    reference_->setPlainText(reference);
    hypothesis_->setPlainText(hypothesis);
    sampleJson_->setPlainText(QString::fromUtf8(
        QJsonDocument(result).toJson(QJsonDocument::Indented)));
}

void AcceptanceEvidencePanel::showReport(
    const QJsonObject& report,
    const QJsonObject& verification) {
    const QJsonObject metrics =
        report.value(QStringLiteral("metrics")).toObject();
    QStringList fields;
    fields << QStringLiteral("汇总报告")
           << QStringLiteral("结论 %1")
                  .arg(verification.value(QStringLiteral("verdict"))
                           .toString(
                               report.value(QStringLiteral("verdict"))
                                   .toString()));
    if (metrics.value(QStringLiteral("accuracy")).isDouble()) {
        fields << QStringLiteral("实际准确率 %1")
                      .arg(percent(
                          metrics.value(QStringLiteral("accuracy")).toDouble()));
    }
    fields << QStringLiteral("失败样本 %1")
                  .arg(metrics.value(QStringLiteral("failed_samples")).toInt());
    const QJsonObject runtime =
        report.value(QStringLiteral("runtime")).toObject();
    if (runtime.contains(QStringLiteral("published_payloads_match"))) {
        fields << QStringLiteral("运行基线 %1")
                      .arg(runtime.value(
                               QStringLiteral("published_payloads_match"))
                               .toBool()
                           ? QStringLiteral("匹配")
                           : QStringLiteral("不匹配"));
    }
    const QString reportSha256 =
        verification.value(QStringLiteral("report_sha256")).toString();
    if (!reportSha256.isEmpty()) {
        fields << QStringLiteral("报告 SHA-256 %1…")
                      .arg(reportSha256.left(12));
    }
    summary_->setText(fields.join(QStringLiteral(" · ")));
    summary_->setToolTip(QStringLiteral(
        "汇总报告由冻结真值计分；运行基线在识别前后逐项核对；"
        "report_sha256 由独立校验文件保存。"));
    QJsonObject display;
    display.insert(QStringLiteral("report_file"), report);
    display.insert(QStringLiteral("verification"), verification);
    reportJson_->setPlainText(QString::fromUtf8(
        QJsonDocument(display).toJson(QJsonDocument::Indented)));
}

void AcceptanceEvidencePanel::focusSample() {
    tabs_->setCurrentIndex(0);
}

void AcceptanceEvidencePanel::focusReport() {
    tabs_->setCurrentIndex(2);
}

}  // namespace speechdoc

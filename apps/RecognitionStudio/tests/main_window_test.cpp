#include "main_window.h"
#include "acceptance_evidence_panel.h"

#include <QApplication>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>

#include <iostream>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    speechdoc::MainWindow window;
    QTabWidget* tabs = window.findChild<QTabWidget*>();
    if (!tabs) {
        std::cerr << "tab widget was not created\n";
        return 1;
    }
    int acceptanceTab = -1;
    for (int index = 0; index < tabs->count(); ++index) {
        if (tabs->tabText(index) == QStringLiteral("验收验证")) {
            acceptanceTab = index;
            break;
        }
    }
    if (acceptanceTab < 0) {
        std::cerr << "acceptance validation tab was not created\n";
        return 1;
    }
    const QList<QLabel*> labels = tabs->widget(acceptanceTab)->findChildren<QLabel*>();
    bool hasAccuracy = false;
    bool hasConfidence = false;
    bool hasVerdict = false;
    for (const QLabel* label : labels) {
        hasAccuracy = hasAccuracy || label->text() == QStringLiteral("实际准确率");
        hasConfidence = hasConfidence || label->text() == QStringLiteral("模型置信度");
        hasVerdict = hasVerdict || label->text() == QStringLiteral("验收结论");
    }
    if (!hasAccuracy || !hasConfidence || !hasVerdict) {
        std::cerr << "acceptance metric labels are incomplete\n";
        return 1;
    }
    auto* evidencePanel =
        tabs->widget(acceptanceTab)
            ->findChild<speechdoc::AcceptanceEvidencePanel*>(
                QStringLiteral("acceptanceEvidencePanel"));
    if (!evidencePanel
        || !tabs->widget(acceptanceTab)->findChild<QTabWidget*>(
            QStringLiteral("acceptanceEvidenceTabs"))
        || !tabs->widget(acceptanceTab)->findChild<QPlainTextEdit*>(
            QStringLiteral("evidenceReference"))
        || !tabs->widget(acceptanceTab)->findChild<QPlainTextEdit*>(
            QStringLiteral("evidenceHypothesis"))
        || !tabs->widget(acceptanceTab)->findChild<QPlainTextEdit*>(
            QStringLiteral("evidenceRawJson"))
        || !tabs->widget(acceptanceTab)->findChild<QPlainTextEdit*>(
            QStringLiteral("evidenceReportJson"))
        || !tabs->widget(acceptanceTab)->findChild<QPushButton*>(
            QStringLiteral("viewAcceptanceEvidenceButton"))) {
        std::cerr << "acceptance evidence viewer is incomplete\n";
        return 1;
    }

    QJsonObject confidence;
    confidence.insert(QStringLiteral("mean"), 0.91);
    confidence.insert(QStringLiteral("calibrated"), false);
    QJsonObject sample;
    sample.insert(QStringLiteral("state"), QStringLiteral("completed"));
    sample.insert(QStringLiteral("accuracy"), 0.98);
    sample.insert(QStringLiteral("edit_distance"), 1);
    sample.insert(QStringLiteral("reference_characters"), 50);
    sample.insert(QStringLiteral("confidence"), confidence);
    evidencePanel->showSample(
        QStringLiteral("sample-1"), QStringLiteral("冻结真值"),
        QStringLiteral("识别文本"), sample);
    const auto* reference = evidencePanel->findChild<QPlainTextEdit*>(
        QStringLiteral("evidenceReference"));
    const auto* hypothesis = evidencePanel->findChild<QPlainTextEdit*>(
        QStringLiteral("evidenceHypothesis"));
    const auto* rawJson = evidencePanel->findChild<QPlainTextEdit*>(
        QStringLiteral("evidenceRawJson"));
    if (!reference || reference->toPlainText() != QStringLiteral("冻结真值")
        || !hypothesis
        || hypothesis->toPlainText() != QStringLiteral("识别文本")
        || !rawJson
        || !rawJson->toPlainText().contains(QStringLiteral("\"calibrated\": false"))) {
        std::cerr << "acceptance sample evidence is not rendered\n";
        return 1;
    }
    QJsonObject report;
    report.insert(QStringLiteral("verdict"), QStringLiteral("passed"));
    evidencePanel->showReport(report);
    const auto* reportJson = evidencePanel->findChild<QPlainTextEdit*>(
        QStringLiteral("evidenceReportJson"));
    if (!reportJson
        || !reportJson->toPlainText().contains(QStringLiteral("\"passed\""))) {
        std::cerr << "acceptance report evidence is not rendered\n";
        return 1;
    }
    return 0;
}

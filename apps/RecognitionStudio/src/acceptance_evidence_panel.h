#pragma once

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QTabWidget;

namespace speechdoc {

class AcceptanceEvidencePanel final : public QWidget {
    Q_OBJECT
public:
    explicit AcceptanceEvidencePanel(QWidget* parent = nullptr);

    void clearEvidence();
    void showSample(const QString& sampleId,
                    const QString& reference,
                    const QString& hypothesis,
                    const QJsonObject& result);
    void showReport(const QJsonObject& report,
                    const QJsonObject& verification = {});
    void focusSample();
    void focusReport();

private:
    QLabel* summary_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QPlainTextEdit* reference_ = nullptr;
    QPlainTextEdit* hypothesis_ = nullptr;
    QPlainTextEdit* sampleJson_ = nullptr;
    QPlainTextEdit* reportJson_ = nullptr;
};

}  // namespace speechdoc

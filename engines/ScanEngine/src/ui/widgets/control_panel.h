#pragma once

#include "scanengine/models.hpp"

#include <QFrame>
#include <QJsonObject>
#include <QStringList>
#include <optional>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace scanengine {

class DropZone;
class StatusPanel;

class ControlPanel : public QFrame {
    Q_OBJECT
public:
    explicit ControlPanel(QWidget* parent = nullptr);

    QStringList inputFiles() const;
    QString primaryInput() const;
    QString outputDir() const;
    void setOutputDir(const QString& path);
    void importClipboard();

    std::optional<ParseOptions> buildOptions(const QString& inputPath = QString()) const;
    void applyConfig(const QJsonObject& cfg);
    QJsonObject exportConfig() const;

    void setBusy(bool busy);
    void setProgress(int current, int total);
    void setStatus(const QString& status, const QString& message);
    void setResultFiles(const QStringList& paths);
    void setResultText(const QString& text);
    void clearInputs();

signals:
    void convertRequested();
    void clearRequested();
    void cancelRequested();
    void inputChanged();
    void artifactOpenRequested(const QString& path);

private:
    void onPaths(const QStringList& paths);
    void pickFiles();
    void pickDir();
    void pickOutput();
    void openOutputDir();
    void openSelectedArtifact();
    void openArtifact(QListWidgetItem* item);

    DropZone* m_dropZone = nullptr;
    QPushButton* m_btnPickFiles = nullptr;
    QPushButton* m_btnPickDir = nullptr;
    QPushButton* m_btnClipboard = nullptr;
    QLabel* m_lblOutput = nullptr;
    QPushButton* m_btnOutput = nullptr;
    QPushButton* m_btnConvert = nullptr;
    QPushButton* m_btnClear = nullptr;
    QPushButton* m_btnCancel = nullptr;
    QPushButton* m_btnOpenSelected = nullptr;
    QListWidget* m_resultFiles = nullptr;
    StatusPanel* m_statusPanel = nullptr;
    QStringList m_paths;
    QString m_outputDir;
};

}  // namespace scanengine

#pragma once

#include "scanengine/models.hpp"

#include <QMainWindow>
#include <QStringList>
#include <QVector>

class QCloseEvent;

namespace scanengine {

class ControlPanel;
class ParseController;
class PreviewPane;
class ResultPane;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void buildMenu();
    void about();
    void onInputChanged();
    void startParse();
    void startNextJob();
    void cancelParse();
    void clearAll();
    void onLog(const QString& line);
    void onStatus(const QString& status, const QString& message);
    void onFinished(const ParseResult& result);

    ControlPanel* m_control = nullptr;
    PreviewPane* m_preview = nullptr;
    ResultPane* m_result = nullptr;
    ParseController* m_controller = nullptr;
    QVector<ParseOptions> m_queue;
    int m_queueTotal = 0;
    QStringList m_lastArtifacts;
};

}  // namespace scanengine

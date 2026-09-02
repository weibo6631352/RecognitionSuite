#pragma once

#include <QFrame>
#include <QString>

class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTextBrowser;

namespace scanengine {

class ResultPane : public QFrame {
    Q_OBJECT
public:
    explicit ResultPane(QWidget* parent = nullptr);

    void clear();
    void appendLog(const QString& line);
    void loadMarkdown(const QString& path);

private:
    void copyCurrent();

    QTabWidget* m_tabs = nullptr;
    QTextBrowser* m_mdView = nullptr;
    QPlainTextEdit* m_logText = nullptr;
    QPushButton* m_btnCopy = nullptr;
    QString m_markdownSource;
};

}  // namespace scanengine

#pragma once

#include <QFrame>
#include <QVector>

class QLabel;

namespace scanengine {

class StatusPanel : public QFrame {
    Q_OBJECT
public:
    explicit StatusPanel(QWidget* parent = nullptr);

    void setProgress(int current, int total);
    void setStatus(const QString& status, const QString& message);
    void reset();

private:
    void render();

    QLabel* m_text = nullptr;
    int m_current = 0;
    int m_total = 0;
    QString m_status = QStringLiteral("idle");
    QString m_message;
};

}  // namespace scanengine

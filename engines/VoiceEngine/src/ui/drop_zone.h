#pragma once

#include <QFrame>
#include <QStringList>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QLabel;
class QMouseEvent;

namespace voiceengine {

class DropZone : public QFrame {
    Q_OBJECT
public:
    explicit DropZone(QWidget* parent = nullptr);
    void setHint(const QString& text);

signals:
    void filesDropped(const QStringList& paths);
    void clicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    QLabel* m_hint = nullptr;
};

}  // namespace voiceengine

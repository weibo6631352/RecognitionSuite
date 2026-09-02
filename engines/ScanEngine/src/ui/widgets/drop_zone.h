#pragma once

#include <QFrame>
#include <QStringList>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QLabel;
class QMouseEvent;

namespace scanengine {

class DropZone : public QFrame {
    Q_OBJECT
public:
    explicit DropZone(QWidget* parent = nullptr);

    void setPaths(const QStringList& paths);

signals:
    void filesDropped(const QStringList& paths);
    void clicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void refreshStyle();

    QLabel* m_title = nullptr;
    QLabel* m_hint = nullptr;
    QLabel* m_pathLabel = nullptr;
};

}  // namespace scanengine

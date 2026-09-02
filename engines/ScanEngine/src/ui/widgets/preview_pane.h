#pragma once

#include <QFrame>
#include <QString>

class QLabel;
class QScrollArea;

namespace scanengine {

class PreviewPane : public QFrame {
    Q_OBJECT
public:
    explicit PreviewPane(QWidget* parent = nullptr);

    void clear();
    void showPath(const QString& path);

private:
    void showImage(const QString& path);

    QScrollArea* m_scroll = nullptr;
    QLabel* m_content = nullptr;
    QLabel* m_pathLabel = nullptr;
};

}  // namespace scanengine

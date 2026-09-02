#include "drop_zone.h"

#include "scanengine/ui_options.hpp"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QStyle>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

namespace scanengine {

DropZone::DropZone(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("dropZone"));
    setAcceptDrops(true);
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(110);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);

    m_title = new QLabel(QStringLiteral("拖放图片，或按 Ctrl+V 粘贴"));
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 14px;"));

    m_hint = new QLabel(QStringLiteral("PNG · JPEG · BMP · TIFF · WebP"));
    m_hint->setAlignment(Qt::AlignCenter);
    m_hint->setStyleSheet(QStringLiteral("color: #6b7280;"));

    m_pathLabel = new QLabel;
    m_pathLabel->setAlignment(Qt::AlignCenter);
    m_pathLabel->setWordWrap(true);
    m_pathLabel->setStyleSheet(QStringLiteral("color: #173f67;"));

    layout->addStretch(1);
    layout->addWidget(m_title);
    layout->addWidget(m_hint);
    layout->addWidget(m_pathLabel);
    layout->addStretch(1);
}

void DropZone::setPaths(const QStringList& paths) {
    if (paths.isEmpty()) {
        m_pathLabel->setText(QString());
        return;
    }
    if (paths.size() == 1)
        m_pathLabel->setText(paths.first());
    else
        m_pathLabel->setText(QStringLiteral("已选择 %1 个文件/目录").arg(paths.size()));
}

void DropZone::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        emit clicked();
    QFrame::mousePressEvent(event);
}

void DropZone::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        setProperty("dragActive", true);
        refreshStyle();
    } else {
        event->ignore();
    }
}

void DropZone::dragLeaveEvent(QDragLeaveEvent* event) {
    setProperty("dragActive", false);
    refreshStyle();
    QFrame::dragLeaveEvent(event);
}

void DropZone::dropEvent(QDropEvent* event) {
    setProperty("dragActive", false);
    refreshStyle();
    QStringList paths;
    const auto urls = event->mimeData()->urls();
    for (const QUrl& url : urls) {
        const QString local = url.toLocalFile();
        if (local.isEmpty())
            continue;
        const QFileInfo info(local);
        if (info.isDir() || isSupportedPath(local))
            paths.append(local);
    }
    if (!paths.isEmpty()) {
        emit filesDropped(paths);
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void DropZone::refreshStyle() {
    style()->unpolish(this);
    style()->polish(this);
    update();
}

}  // namespace scanengine

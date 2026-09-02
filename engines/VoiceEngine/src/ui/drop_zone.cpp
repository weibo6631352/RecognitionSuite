#include "drop_zone.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

namespace voiceengine {

DropZone::DropZone(QWidget* parent) : QFrame(parent) {
    setAcceptDrops(true);
    setMinimumHeight(120);
    setFrameShape(QFrame::StyledPanel);
    auto* lay = new QVBoxLayout(this);
    m_hint = new QLabel(QString::fromUtf8("拖放音频文件到这里，或点击选择\nWAV / MP3 / AAC / M4A / FLAC / Opus / 视频音轨"), this);
    m_hint->setAlignment(Qt::AlignCenter);
    m_hint->setWordWrap(true);
    lay->addWidget(m_hint);
}

void DropZone::setHint(const QString& text) {
    m_hint->setText(text);
}

void DropZone::mousePressEvent(QMouseEvent*) {
    emit clicked();
}

void DropZone::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        setProperty("hover", true);
        style()->unpolish(this);
        style()->polish(this);
    }
}

void DropZone::dragLeaveEvent(QDragLeaveEvent*) {
    setProperty("hover", false);
    style()->unpolish(this);
    style()->polish(this);
}

void DropZone::dropEvent(QDropEvent* event) {
    setProperty("hover", false);
    style()->unpolish(this);
    style()->polish(this);
    QStringList paths;
    for (const QUrl& u : event->mimeData()->urls()) {
        if (u.isLocalFile()) {
            paths << u.toLocalFile();
        }
    }
    if (!paths.isEmpty()) {
        emit filesDropped(paths);
    }
}

}  // namespace voiceengine

#include "preview_pane.h"

#include <QFileInfo>
#include <QLabel>
#include <QPixmap>
#include <QScrollArea>
#include <QVBoxLayout>

namespace scanengine {

PreviewPane::PreviewPane(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("panel"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);

    auto* title = new QLabel(QStringLiteral("图片预览"));
    title->setStyleSheet(QStringLiteral("font-weight: 600;"));
    layout->addWidget(title);

    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setAlignment(Qt::AlignCenter);
    m_content = new QLabel(QStringLiteral("选择图片后在此预览"));
    m_content->setAlignment(Qt::AlignCenter);
    m_content->setStyleSheet(QStringLiteral("color: #6b7280;"));
    m_content->setMinimumHeight(400);
    m_scroll->setWidget(m_content);
    layout->addWidget(m_scroll, 1);

    m_pathLabel = new QLabel;
    m_pathLabel->setWordWrap(true);
    m_pathLabel->setStyleSheet(QStringLiteral("color: #6b7280; font-size: 11px;"));
    layout->addWidget(m_pathLabel);
}

void PreviewPane::clear() {
    m_content->setPixmap(QPixmap());
    m_content->setText(QStringLiteral("选择图片后在此预览"));
    m_pathLabel->clear();
}

void PreviewPane::showPath(const QString& path) {
    if (path.isEmpty()) {
        clear();
        return;
    }
    m_pathLabel->setText(path);
    const QFileInfo info(path);
    if (info.isDir()) {
        m_content->setPixmap(QPixmap());
        m_content->setText(QStringLiteral("图片文件夹：%1").arg(info.fileName()));
        return;
    }
    showImage(path);
}

void PreviewPane::showImage(const QString& path) {
    const QPixmap pixmap(path);
    if (pixmap.isNull()) {
        m_content->setPixmap(QPixmap());
        m_content->setText(QStringLiteral("图片加载失败：%1").arg(QFileInfo(path).fileName()));
        return;
    }
    const int width = qMax(200, m_scroll->viewport()->width() - 20);
    m_content->setText(QString());
    m_content->setPixmap(pixmap.scaledToWidth(width, Qt::SmoothTransformation));
}

}  // namespace scanengine

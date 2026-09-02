#include "result_pane.h"

#include "scanengine/markdown_preview.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace scanengine {
namespace {

QString readTextFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

}  // namespace

ResultPane::ResultPane(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("panel"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);

    auto* head = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("解析内容"));
    title->setStyleSheet(QStringLiteral("font-weight: 600;"));
    m_btnCopy = new QPushButton(QStringLiteral("复制当前页"));
    connect(m_btnCopy, &QPushButton::clicked, this, &ResultPane::copyCurrent);
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(m_btnCopy);
    layout->addLayout(head);

    m_tabs = new QTabWidget;
    m_mdView = new QTextBrowser;
    m_mdView->setOpenExternalLinks(false);
    m_logText = new QPlainTextEdit;
    m_logText->setReadOnly(true);

    m_tabs->addTab(m_mdView, QStringLiteral("解析结果"));
    m_tabs->addTab(m_logText, QStringLiteral("运行日志"));
    layout->addWidget(m_tabs, 1);
}

void ResultPane::clear() {
    m_mdView->clear();
    m_logText->clear();
    m_markdownSource.clear();
}

void ResultPane::appendLog(const QString& line) {
    m_logText->appendPlainText(line);
}

void ResultPane::loadMarkdown(const QString& path) {
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile()) {
        m_markdownSource.clear();
        m_mdView->setHtml(QStringLiteral("<p style='color:#6b7280;'>暂无 Markdown 结果</p>"));
        return;
    }
    m_markdownSource = readTextFile(path);
    m_mdView->setSearchPaths(QStringList() << info.absolutePath());
    m_mdView->setHtml(markdownToPreviewHtml(m_markdownSource, path));
}

void ResultPane::copyCurrent() {
    QWidget* widget = m_tabs->currentWidget();
    QString text;
    if (widget == m_logText)
        text = m_logText->toPlainText();
    else if (widget == m_mdView)
        text = m_markdownSource;
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

}  // namespace scanengine

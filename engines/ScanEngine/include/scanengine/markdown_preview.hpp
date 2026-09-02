#pragma once

#include <QString>

namespace scanengine {

QString rewritePreviewImages(const QString& markdownText, const QString& imageDir);
QString escapeLatexBlocks(const QString& markdownText);
QString markdownToPreviewHtml(const QString& markdownText, const QString& sourcePath = QString());

}  // namespace scanengine

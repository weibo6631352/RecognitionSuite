#include "scanengine/markdown_preview.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace scanengine {
namespace {

const QSet<QString> kImageSuffixes = {
    QStringLiteral(".jpg"), QStringLiteral(".jpeg"), QStringLiteral(".png"),
    QStringLiteral(".gif"), QStringLiteral(".webp"), QStringLiteral(".bmp"),
    QStringLiteral(".tif"), QStringLiteral(".tiff"), QStringLiteral(".svg")
};

QString resolvePreviewImage(const QString& src, const QString& imageDir) {
    const QString imageSrc = src.trimmed();
    if (imageSrc.isEmpty()
        || imageSrc.startsWith(QLatin1String("http://"))
        || imageSrc.startsWith(QLatin1String("https://"))
        || imageSrc.startsWith(QLatin1String("data:"))
        || imageSrc.startsWith(QLatin1String("file:"))) {
        return QString();
    }
    const QString noQuery = imageSrc.split(QLatin1Char('?')).value(0);
    const QString suffix = QLatin1Char('.') + QFileInfo(noQuery).suffix().toLower();
    if (!kImageSuffixes.contains(suffix) || imageDir.isEmpty())
        return QString();
    const QString resolved = QDir(imageDir).absoluteFilePath(imageSrc);
    return QFileInfo::exists(resolved) ? QFileInfo(resolved).absoluteFilePath() : QString();
}

QString fileUrl(const QString& path) {
    return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
}

QString escapeHtml(const QString& text, bool quote) {
    QString o = text;
    o.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    o.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    o.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    if (quote)
        o.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return o;
}

QString formatTextSpan(QString text) {
    QString escaped = escapeHtml(text, false);
    static const QRegularExpression code(QStringLiteral("`([^`]+)`"));
    static const QRegularExpression bold(QStringLiteral("\\*\\*([^*]+)\\*\\*"));
    static const QRegularExpression italic(QStringLiteral("(?<!\\*)\\*([^*]+)\\*(?!\\*)"));
    escaped.replace(code, QStringLiteral("<code>\\1</code>"));
    escaped.replace(bold, QStringLiteral("<b>\\1</b>"));
    escaped.replace(italic, QStringLiteral("<i>\\1</i>"));
    return escaped;
}

QString inlineFormat(const QString& text) {
    static const QRegularExpression mdImg(QStringLiteral("!\\[([^\\]]*)\\]\\(([^)]+)\\)"));
    QString out;
    int last = 0;
    auto it = mdImg.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += formatTextSpan(text.mid(last, m.capturedStart() - last));
        out += QStringLiteral("<img alt=\"") + escapeHtml(m.captured(1), true)
            + QStringLiteral("\" src=\"") + m.captured(2) + QStringLiteral("\" />");
        last = m.capturedEnd();
    }
    out += formatTextSpan(text.mid(last));
    return out;
}

QString markdownChunkToHtml(const QString& chunk) {
    if (chunk.trimmed().isEmpty())
        return QString();
    const QStringList lines = chunk.split(QLatin1Char('\n'));
    QStringList out;
    bool inCode = false;
    bool inUl = false;
    static const QRegularExpression ulRe(QStringLiteral("^\\s*[-*]\\s+"));
    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.startsWith(QLatin1String("```"))) {
            if (inUl) {
                out << QStringLiteral("</ul>");
                inUl = false;
            }
            if (inCode) {
                out << QStringLiteral("</code></pre>");
                inCode = false;
            } else {
                out << QStringLiteral("<pre><code>");
                inCode = true;
            }
            continue;
        }
        if (inCode) {
            out << escapeHtml(line, false) + QLatin1Char('\n');
            continue;
        }
        if (ulRe.match(line).hasMatch()) {
            if (!inUl) {
                out << QStringLiteral("<ul>");
                inUl = true;
            }
            const QString content = QString(line).replace(ulRe, QString());
            out << QStringLiteral("<li>") + inlineFormat(content) + QStringLiteral("</li>");
            continue;
        }
        if (inUl) {
            out << QStringLiteral("</ul>");
            inUl = false;
        }
        if (line.startsWith(QLatin1String("### ")))
            out << QStringLiteral("<h3>") + inlineFormat(line.mid(4)) + QStringLiteral("</h3>");
        else if (line.startsWith(QLatin1String("## ")))
            out << QStringLiteral("<h2>") + inlineFormat(line.mid(3)) + QStringLiteral("</h2>");
        else if (line.startsWith(QLatin1String("# ")))
            out << QStringLiteral("<h1>") + inlineFormat(line.mid(2).trimmed()) + QStringLiteral("</h1>");
        else if (line.trimmed().isEmpty())
            out << QStringLiteral("<br/>");
        else
            out << QStringLiteral("<p>") + inlineFormat(line) + QStringLiteral("</p>");
    }
    if (inUl)
        out << QStringLiteral("</ul>");
    if (inCode)
        out << QStringLiteral("</code></pre>");
    return out.join(QString());
}

}  // namespace

QString rewritePreviewImages(const QString& markdownText, const QString& imageDir) {
    static const QRegularExpression mdImg(QStringLiteral("!\\[([^\\]]*)\\]\\(([^)]+)\\)"));
    static const QRegularExpression htmlImg(
        QStringLiteral("(<img\\b[^>]*?\\bsrc\\s*=\\s*)([\"'])([^\"']+)(\\2)"),
        QRegularExpression::CaseInsensitiveOption);

    QString result;
    int last = 0;
    auto it = mdImg.globalMatch(markdownText);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        result += markdownText.mid(last, m.capturedStart() - last);
        const QString resolved = resolvePreviewImage(m.captured(2), imageDir);
        if (resolved.isEmpty())
            result += m.captured(0);
        else
            result += QStringLiteral("![") + m.captured(1) + QStringLiteral("](") + fileUrl(resolved)
                + QLatin1Char(')');
        last = m.capturedEnd();
    }
    result += markdownText.mid(last);

    QString htmlOut;
    last = 0;
    auto hit = htmlImg.globalMatch(result);
    while (hit.hasNext()) {
        const QRegularExpressionMatch m = hit.next();
        htmlOut += result.mid(last, m.capturedStart() - last);
        const QString resolved = resolvePreviewImage(m.captured(3), imageDir);
        if (resolved.isEmpty())
            htmlOut += m.captured(0);
        else
            htmlOut += m.captured(1) + m.captured(2) + fileUrl(resolved) + m.captured(2);
        last = m.capturedEnd();
    }
    htmlOut += result.mid(last);
    return htmlOut;
}

QString escapeLatexBlocks(const QString& markdownText) {
    if (markdownText.isEmpty())
        return markdownText;
    static const QPair<QString, QString> delims[] = {
        {QStringLiteral("$$"), QStringLiteral("$$")},
        {QStringLiteral("\\["), QStringLiteral("\\]")},
        {QStringLiteral("\\("), QStringLiteral("\\)")},
        {QStringLiteral("$"), QStringLiteral("$")},
    };
    QString result;
    int position = 0;
    const int n = markdownText.size();
    while (position < n) {
        const QPair<QString, QString>* matched = nullptr;
        for (const auto& d : delims) {
            if (markdownText.midRef(position).startsWith(d.first)) {
                matched = &d;
                break;
            }
        }
        if (!matched) {
            result += markdownText.at(position);
            ++position;
            continue;
        }
        const int contentStart = position + matched->first.size();
        const int contentEnd = markdownText.indexOf(matched->second, contentStart);
        if (contentEnd < 0) {
            result += markdownText.at(position);
            ++position;
            continue;
        }
        QString inner = markdownText.mid(contentStart, contentEnd - contentStart);
        inner.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
        inner.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
        result += matched->first + inner + matched->second;
        position = contentEnd + matched->second.size();
    }
    return result;
}

QString markdownToPreviewHtml(const QString& markdownText, const QString& sourcePath) {
    const QString imageDir = sourcePath.isEmpty() ? QString() : QFileInfo(sourcePath).absolutePath();
    QString prepared = rewritePreviewImages(markdownText, imageDir);
    prepared = escapeLatexBlocks(prepared);

    static const QRegularExpression keep(
        QStringLiteral("(<table\\b[\\s\\S]*?</table>|<img\\b[^>]*>)"),
        QRegularExpression::CaseInsensitiveOption);

    QString body;
    int last = 0;
    auto it = keep.globalMatch(prepared);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        body += markdownChunkToHtml(prepared.mid(last, m.capturedStart() - last));
        body += m.captured(1);
        last = m.capturedEnd();
    }
    body += markdownChunkToHtml(prepared.mid(last));
    if (body.isEmpty())
        body = QStringLiteral("<p style='color:#6b7280;'>暂无内容</p>");

    return QStringLiteral(
               "<html><head><meta charset='utf-8'><style>"
               "body{font-family:'PingFang SC','Hiragino Sans GB','Microsoft YaHei UI',"
               "'Segoe UI',sans-serif;line-height:1.55;color:#111827;padding:8px;}"
               "h1,h2,h3{color:#111827;margin:12px 0 8px;}"
               "p{margin:6px 0;}"
               "code{background:#f3f4f6;padding:1px 4px;border-radius:3px;}"
               "pre{background:#f3f4f6;padding:10px;border-radius:6px;overflow:auto;}"
               "table{border-collapse:collapse;width:100%;margin:8px 0;font-size:13px;}"
               "th,td{border:1px solid #d1d5db;padding:6px 8px;vertical-align:middle;}"
               "th{background:#f9fafb;font-weight:600;}"
               "img{max-width:100%;}"
               "ul{margin:6px 0 6px 20px;}"
               "</style></head><body>")
        + body + QStringLiteral("</body></html>");
}

}  // namespace scanengine

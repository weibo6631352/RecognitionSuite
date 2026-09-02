#include "scanengine/hybrid/otsl.hpp"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>

namespace scanengine {
namespace hybrid {

namespace {

const char* kNl = "<nl>";
const char* kFcel = "<fcel>";
const char* kEcel = "<ecel>";
const char* kLcel = "<lcel>";
const char* kUcel = "<ucel>";
const char* kXcel = "<xcel>";

bool isOtslToken(const QString& s) {
    return s == QLatin1String(kNl) || s == QLatin1String(kFcel) || s == QLatin1String(kEcel)
        || s == QLatin1String(kLcel) || s == QLatin1String(kUcel) || s == QLatin1String(kXcel);
}

QString htmlEscape(const QString& s) {
    // html.escape(..., quote=True)
    QString o;
    o.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('&'))
            o += QStringLiteral("&amp;");
        else if (c == QLatin1Char('<'))
            o += QStringLiteral("&lt;");
        else if (c == QLatin1Char('>'))
            o += QStringLiteral("&gt;");
        else if (c == QLatin1Char('"'))
            o += QStringLiteral("&quot;");
        else if (c == QLatin1Char('\''))
            o += QStringLiteral("&#x27;");
        else
            o += c;
    }
    return o;
}

void extractTokensAndText(const QString& s, QStringList* tokens, QStringList* mixed) {
    static const QRegularExpression re(
        QStringLiteral("(<nl>|<fcel>|<ecel>|<lcel>|<ucel>|<xcel>)"));
    QRegularExpressionMatchIterator it = re.globalMatch(s);
    while (it.hasNext())
        tokens->push_back(it.next().captured(1));

    int last = 0;
    it = re.globalMatch(s);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString before = s.mid(last, m.capturedStart() - last);
        if (!before.trimmed().isEmpty())
            mixed->push_back(before);
        mixed->push_back(m.captured(1));
        last = m.capturedEnd();
    }
    const QString tail = s.mid(last);
    if (!tail.trimmed().isEmpty())
        mixed->push_back(tail);
}

int countRight(const QVector<QStringList>& rows, int c, int r, const QSet<QString>& which) {
    int span = 0;
    int ci = c;
    while (ci < rows[r].size() && which.contains(rows[r][ci])) {
        ++ci;
        ++span;
        if (ci >= rows[r].size())
            return span;
    }
    return span;
}

int countDown(const QVector<QStringList>& rows, int c, int r, const QSet<QString>& which) {
    int span = 0;
    int ri = r;
    while (ri < rows.size() && c < rows[ri].size() && which.contains(rows[ri][c])) {
        ++ri;
        ++span;
        if (ri >= rows.size())
            return span;
    }
    return span;
}

struct Cell {
    int rowSpan = 1;
    int colSpan = 1;
    int startRow = 0;
    int endRow = 0;
    int startCol = 0;
    int endCol = 0;
    QString text;
};

void parseTexts(const QStringList& mixedIn,
                const QStringList& tokens,
                QVector<Cell>* cells,
                QVector<QStringList>* rowsOut) {
    QVector<QStringList> rows;
    QStringList cur;
    for (const QString& t : tokens) {
        if (t == QLatin1String(kNl)) {
            if (!cur.isEmpty() || !rows.isEmpty())
                rows.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(t);
        }
    }
    if (!cur.isEmpty())
        rows.push_back(cur);

    if (!rows.isEmpty()) {
        int maxCols = 0;
        for (const QStringList& row : rows)
            maxCols = std::max(maxCols, row.size());
        for (QStringList& row : rows) {
            while (row.size() < maxCols)
                row.push_back(QString::fromLatin1(kEcel));
        }

        QStringList newTexts;
        int textIdx = 0;
        const QStringList& texts = mixedIn;
        for (const QStringList& row : rows) {
            for (const QString& token : row) {
                newTexts.push_back(token);
                if (textIdx < texts.size() && texts.at(textIdx) == token) {
                    ++textIdx;
                    if (textIdx < texts.size() && !isOtslToken(texts.at(textIdx))) {
                        newTexts.push_back(texts.at(textIdx));
                        ++textIdx;
                    }
                }
            }
            newTexts.push_back(QString::fromLatin1(kNl));
            if (textIdx < texts.size() && texts.at(textIdx) == QLatin1String(kNl))
                ++textIdx;
        }

        const QSet<QString> rightWhich{QString::fromLatin1(kLcel), QString::fromLatin1(kXcel)};
        const QSet<QString> downWhich{QString::fromLatin1(kUcel), QString::fromLatin1(kXcel)};
        int rIdx = 0;
        int cIdx = 0;
        for (int i = 0; i < newTexts.size(); ++i) {
            const QString& text = newTexts.at(i);
            if (text == QLatin1String(kFcel) || text == QLatin1String(kEcel)) {
                int rowSpan = 1;
                int colSpan = 1;
                int rightOffset = 1;
                QString cellText;
                if (text != QLatin1String(kEcel) && i + 1 < newTexts.size()
                    && !isOtslToken(newTexts.at(i + 1))) {
                    cellText = newTexts.at(i + 1);
                    rightOffset = 2;
                }
                QString nextRight;
                if (i + rightOffset < newTexts.size())
                    nextRight = newTexts.at(i + rightOffset);
                QString nextBottom;
                if (rIdx + 1 < rows.size() && cIdx < rows[rIdx + 1].size())
                    nextBottom = rows[rIdx + 1][cIdx];
                if (nextRight == QLatin1String(kLcel) || nextRight == QLatin1String(kXcel))
                    colSpan += countRight(rows, cIdx + 1, rIdx, rightWhich);
                if (nextBottom == QLatin1String(kUcel) || nextBottom == QLatin1String(kXcel))
                    rowSpan += countDown(rows, cIdx, rIdx + 1, downWhich);
                Cell cell;
                cell.text = cellText.trimmed();
                cell.rowSpan = rowSpan;
                cell.colSpan = colSpan;
                cell.startRow = rIdx;
                cell.endRow = rIdx + rowSpan;
                cell.startCol = cIdx;
                cell.endCol = cIdx + colSpan;
                cells->push_back(cell);
            }
            if (text == QLatin1String(kFcel) || text == QLatin1String(kEcel) || text == QLatin1String(kLcel)
                || text == QLatin1String(kUcel) || text == QLatin1String(kXcel))
                ++cIdx;
            if (text == QLatin1String(kNl)) {
                ++rIdx;
                cIdx = 0;
            }
        }
    }
    *rowsOut = rows;
}

}  // namespace

QString replaceOfficialTableFormulaDelimiters(const QString& content) {
    // mineru_vl_utils.replace_table_formula_delimiters(..., enabled=True)
    static const QRegularExpression eqTag(QStringLiteral("(<eq>.*?</eq>)"),
                                          QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression inlinePat(QStringLiteral(R"(\\\((.+?)\\\))"),
                                              QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression blockPat(QStringLiteral(R"(\\\[(.+?)\\\])"),
                                             QRegularExpression::DotMatchesEverythingOption);
    auto wrap = [](const QRegularExpression& re, QString text) {
        QString out;
        int last = 0;
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            out += text.mid(last, m.capturedStart() - last);
            out += QStringLiteral("<eq>%1</eq>").arg(m.captured(1).trimmed());
            last = m.capturedEnd();
        }
        out += text.mid(last);
        return out;
    };

    QString out;
    int last = 0;
    auto it = eqTag.globalMatch(content);
    auto isFullEq = [&](const QString& part) {
        const auto m = eqTag.match(part);
        return m.hasMatch() && m.capturedStart() == 0 && m.capturedEnd() == part.size();
    };
    while (it.hasNext()) {
        const auto m = it.next();
        if (m.capturedStart() > last) {
            QString part = content.mid(last, m.capturedStart() - last);
            part = wrap(inlinePat, part);
            part = wrap(blockPat, part);
            out += part;
        }
        out += m.captured(1);
        last = m.capturedEnd();
    }
    if (last < content.size()) {
        QString part = content.mid(last);
        if (!isFullEq(part)) {
            part = wrap(inlinePat, part);
            part = wrap(blockPat, part);
        }
        out += part;
    }
    return out;
}

QString convertOfficialOtslToHtml(const QString& otsl) {
    if (otsl.startsWith(QLatin1String("<table")) && otsl.endsWith(QLatin1String("</table>")))
        return otsl;

    QStringList tokens;
    QStringList mixed;
    extractTokensAndText(otsl, &tokens, &mixed);

    QVector<Cell> cells;
    QVector<QStringList> rows;
    parseTexts(mixed, tokens, &cells, &rows);
    if (cells.isEmpty())
        return QString();

    const int nrows = rows.size();
    int ncols = 0;
    for (const QStringList& row : rows)
        ncols = std::max(ncols, row.size());

    QVector<QVector<int>> owner(nrows);
    for (int i = 0; i < nrows; ++i)
        owner[i] = QVector<int>(ncols, -1);
    for (int ci = 0; ci < cells.size(); ++ci) {
        const Cell& c = cells.at(ci);
        for (int i = std::min(c.startRow, nrows); i < std::min(c.endRow, nrows); ++i) {
            for (int j = std::min(c.startCol, ncols); j < std::min(c.endCol, ncols); ++j)
                owner[i][j] = ci;
        }
    }

    QString html;
    html += QStringLiteral("<table>");
    for (int i = 0; i < nrows; ++i) {
        html += QStringLiteral("<tr>");
        for (int j = 0; j < ncols; ++j) {
            const int ci = owner[i][j];
            if (ci < 0)
                continue;
            const Cell& c = cells.at(ci);
            if (c.startRow != i || c.startCol != j)
                continue;
            html += QStringLiteral("<td");
            if (c.rowSpan > 1)
                html += QStringLiteral(" rowspan=\"%1\"").arg(c.rowSpan);
            if (c.colSpan > 1)
                html += QStringLiteral(" colspan=\"%1\"").arg(c.colSpan);
            html += QLatin1Char('>');
            html += htmlEscape(c.text.trimmed());
            html += QStringLiteral("</td>");
        }
        html += QStringLiteral("</tr>");
    }
    html += QStringLiteral("</table>");
    return html;
}

void postProcessOfficialTableBlocks(QVector<ContentBlock>* blocks) {
    if (!blocks)
        return;
    for (ContentBlock& b : *blocks) {
        if (b.type != QLatin1String("table") || b.content.isEmpty())
            continue;
        b.content = replaceOfficialTableFormulaDelimiters(convertOfficialOtslToHtml(b.content));
    }
}

QString formatOfficialEmbeddedTableHtml(const QString& html) {
    // vlm_middle_json_mkcontent._replace_eq_tags_in_table_html
    if (html.isEmpty())
        return html;
    static const QRegularExpression eqRe(QStringLiteral("<eq>(.*?)</eq>"),
                                         QRegularExpression::DotMatchesEverythingOption);
    QString out;
    int last = 0;
    auto it = eqRe.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        out += html.mid(last, m.capturedStart() - last);
        QString inner = m.captured(1);
        inner.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        inner.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
        inner.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
        inner.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
        inner.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
        inner.replace(QStringLiteral("&#x27;"), QStringLiteral("'"));
        inner.replace(QStringLiteral("&nbsp;"), QString(QChar(0x00A0)));
        out += QStringLiteral(" $");
        out += inner;
        out += QStringLiteral("$ ");
        last = m.capturedEnd();
    }
    out += html.mid(last);
    return out;
}

}  // namespace hybrid
}  // namespace scanengine

#include "scanengine/excel_export.hpp"
#include "scanengine/strict_json.hpp"

#include "xlsxcellrange.h"
#include "xlsxdocument.h"
#include "xlsxformat.h"
#include "xlsxworksheet.h"

#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace scanengine {
namespace {

QString readUtf8File(const QString& path, bool* ok = nullptr) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (ok)
            *ok = false;
        return {};
    }
    if (ok)
        *ok = true;
    return QString::fromUtf8(f.readAll());
}

void appendCodepoint(QString& out, uint cp) {
    if (cp <= 0xFFFF) {
        out += QChar(ushort(cp));
        return;
    }
    if (cp <= 0x10FFFF) {
        out += QChar(QChar::highSurrogate(cp));
        out += QChar(QChar::lowSurrogate(cp));
    }
}

QString unescapeHtml(const QString& input) {
    QString out;
    out.reserve(input.size());
    for (int i = 0; i < input.size(); ++i) {
        if (input.at(i) != QLatin1Char('&')) {
            out += input.at(i);
            continue;
        }
        const int semi = input.indexOf(QLatin1Char(';'), i + 1);
        if (semi < 0 || semi - i > 32) {
            out += input.at(i);
            continue;
        }
        const QString ent = input.mid(i + 1, semi - i - 1);
        if (ent.startsWith(QLatin1Char('#'))) {
            bool ok = false;
            uint cp = 0;
            if (ent.size() >= 2
                && (ent.at(1) == QLatin1Char('x') || ent.at(1) == QLatin1Char('X'))) {
                cp = ent.mid(2).toUInt(&ok, 16);
            } else {
                cp = ent.mid(1).toUInt(&ok, 10);
            }
            if (ok) {
                appendCodepoint(out, cp);
                i = semi;
                continue;
            }
        } else {
            const QString key = ent.toLower();
            if (key == QLatin1String("amp")) {
                out += QLatin1Char('&');
                i = semi;
                continue;
            }
            if (key == QLatin1String("lt")) {
                out += QLatin1Char('<');
                i = semi;
                continue;
            }
            if (key == QLatin1String("gt")) {
                out += QLatin1Char('>');
                i = semi;
                continue;
            }
            if (key == QLatin1String("quot")) {
                out += QLatin1Char('"');
                i = semi;
                continue;
            }
            if (key == QLatin1String("apos")) {
                out += QLatin1Char('\'');
                i = semi;
                continue;
            }
            if (key == QLatin1String("nbsp")) {
                out += QChar(0x00A0);
                i = semi;
                continue;
            }
        }
        out += input.at(i);
    }
    return out;
}

int skipLatexWs(const QString& s, int i) {
    while (i < s.size() && s.at(i).isSpace())
        ++i;
    return i;
}

bool takeLatexBrace(const QString& s, int* i, QString* inner) {
    if (!i || *i >= s.size() || s.at(*i) != QLatin1Char('{'))
        return false;
    int depth = 0;
    const int start = *i + 1;
    for (int p = *i; p < s.size(); ++p) {
        const QChar c = s.at(p);
        if (c == QLatin1Char('\\') && p + 1 < s.size()) {
            ++p;
            continue;
        }
        if (c == QLatin1Char('{'))
            ++depth;
        else if (c == QLatin1Char('}')) {
            --depth;
            if (depth == 0) {
                *inner = s.mid(start, p - start);
                *i = p + 1;
                return true;
            }
        }
    }
    return false;
}

QString takeLatexGroup(const QString& s, int* i) {
    *i = skipLatexWs(s, *i);
    if (*i >= s.size())
        return {};
    if (s.at(*i) == QLatin1Char('{')) {
        QString inner;
        if (takeLatexBrace(s, i, &inner))
            return inner;
    }
    if (s.at(*i) == QLatin1Char('\\')) {
        const int start = *i;
        ++*i;
        while (*i < s.size() && s.at(*i).isLetter())
            ++*i;
        if (*i == start + 1 && *i < s.size())
            ++*i;
        return s.mid(start, *i - start);
    }
    const QString one = s.mid(*i, 1);
    ++*i;
    return one;
}

QChar superChar(QChar c) {
    switch (c.unicode()) {
    case '0': return QChar(0x2070);
    case '1': return QChar(0x00B9);
    case '2': return QChar(0x00B2);
    case '3': return QChar(0x00B3);
    case '4': return QChar(0x2074);
    case '5': return QChar(0x2075);
    case '6': return QChar(0x2076);
    case '7': return QChar(0x2077);
    case '8': return QChar(0x2078);
    case '9': return QChar(0x2079);
    case '+': return QChar(0x207A);
    case '-': return QChar(0x207B);
    case '=': return QChar(0x207C);
    case '(': return QChar(0x207D);
    case ')': return QChar(0x207E);
    case 'n': return QChar(0x207F);
    case 'i': return QChar(0x2071);
    default: return QChar();
    }
}

QChar subChar(QChar c) {
    switch (c.unicode()) {
    case '0': return QChar(0x2080);
    case '1': return QChar(0x2081);
    case '2': return QChar(0x2082);
    case '3': return QChar(0x2083);
    case '4': return QChar(0x2084);
    case '5': return QChar(0x2085);
    case '6': return QChar(0x2086);
    case '7': return QChar(0x2087);
    case '8': return QChar(0x2088);
    case '9': return QChar(0x2089);
    case '+': return QChar(0x208A);
    case '-': return QChar(0x208B);
    case '=': return QChar(0x208C);
    case '(': return QChar(0x208D);
    case ')': return QChar(0x208E);
    case 'a': return QChar(0x2090);
    case 'e': return QChar(0x2091);
    case 'o': return QChar(0x2092);
    case 'x': return QChar(0x2093);
    case 'i': return QChar(0x1D62);
    case 'r': return QChar(0x1D63);
    case 'u': return QChar(0x1D64);
    case 'v': return QChar(0x1D65);
    case 'n': return QChar(0x2099);
    case 'k': return QChar(0x2096);
    case 'm': return QChar(0x2098);
    default: return QChar();
    }
}

QString mapScript(const QString& inner, bool super) {
    QString out;
    out.reserve(inner.size());
    for (int i = 0; i < inner.size(); ++i) {
        const QChar mapped = super ? superChar(inner.at(i)) : subChar(inner.at(i));
        if (mapped.isNull())
            return super ? (QStringLiteral("^") + inner) : (QStringLiteral("_") + inner);
        out += mapped;
    }
    return out;
}

QString latexToExcelText(const QString& input, bool* failed);

QString wrapParenIfNeeded(const QString& s) {
    if (s.size() <= 1)
        return s;
    if (s.startsWith(QLatin1Char('(')) && s.endsWith(QLatin1Char(')')))
        return s;
    return QLatin1Char('(') + s + QLatin1Char(')');
}

QString convertLatexBody(const QString& s, bool* failed) {
    QString out;
    int i = 0;
    while (i < s.size()) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('\\')) {
            int j = i + 1;
            while (j < s.size() && s.at(j).isLetter())
                ++j;
            QString cmd = s.mid(i + 1, j - i - 1);
            if (cmd.isEmpty() && j < s.size()) {
                cmd = s.mid(j, 1);
                ++j;
            }
            i = j;
            if (cmd == QLatin1String("frac") || cmd == QLatin1String("dfrac")
                || cmd == QLatin1String("tfrac")) {
                const QString num = latexToExcelText(takeLatexGroup(s, &i), failed);
                const QString den = latexToExcelText(takeLatexGroup(s, &i), failed);
                if (num.isEmpty() || den.isEmpty()) {
                    if (failed)
                        *failed = true;
                }
                out += wrapParenIfNeeded(num);
                out += QLatin1Char('/');
                out += wrapParenIfNeeded(den);
                continue;
            }
            if (cmd == QLatin1String("sqrt")) {
                QString index;
                i = skipLatexWs(s, i);
                if (i < s.size() && s.at(i) == QLatin1Char('[')) {
                    const int close = s.indexOf(QLatin1Char(']'), i + 1);
                    if (close > i) {
                        index = latexToExcelText(s.mid(i + 1, close - i - 1), failed);
                        i = close + 1;
                    }
                }
                const QString rad = latexToExcelText(takeLatexGroup(s, &i), failed);
                if (rad.isEmpty()) {
                    if (failed)
                        *failed = true;
                }
                if (!index.isEmpty())
                    out += mapScript(index, true);
                out += QChar(0x221A);
                out += wrapParenIfNeeded(rad);
                continue;
            }
            if (cmd == QLatin1String("text") || cmd == QLatin1String("mathrm")
                || cmd == QLatin1String("mathbf") || cmd == QLatin1String("operatorname")
                || cmd == QLatin1String("operatorname*") || cmd == QLatin1String("operatornamewithlimits")) {
                out += latexToExcelText(takeLatexGroup(s, &i), failed);
                continue;
            }
            if (cmd == QLatin1String("left") || cmd == QLatin1String("right")
                || cmd == QLatin1String("big") || cmd == QLatin1String("Big")
                || cmd == QLatin1String("bigg") || cmd == QLatin1String("Bigg")) {
                i = skipLatexWs(s, i);
                if (i < s.size()) {
                    if (s.at(i) == QLatin1Char('\\')) {
                        ++i;
                        if (i < s.size() && (s.at(i) == QLatin1Char('{') || s.at(i) == QLatin1Char('}'))) {
                            out += s.at(i);
                            ++i;
                            continue;
                        }
                    } else {
                        out += s.at(i);
                        ++i;
                    }
                }
                continue;
            }
            if (cmd == QLatin1String("overline") || cmd == QLatin1String("bar")) {
                const QString inner = latexToExcelText(takeLatexGroup(s, &i), failed);
                out += inner;
                if (!inner.isEmpty())
                    out += QChar(0x0305);
                continue;
            }
            if (cmd == QLatin1String("hat")) {
                const QString inner = latexToExcelText(takeLatexGroup(s, &i), failed);
                out += inner;
                if (!inner.isEmpty())
                    out += QChar(0x0302);
                continue;
            }
            if (cmd == QLatin1String("vec")) {
                const QString inner = latexToExcelText(takeLatexGroup(s, &i), failed);
                out += inner;
                if (!inner.isEmpty())
                    out += QChar(0x20D7);
                continue;
            }
            static const QHash<QString, QString> kCmd({
                {QStringLiteral("pm"), QString(QChar(0x00B1))},
                {QStringLiteral("mp"), QString(QChar(0x2213))},
                {QStringLiteral("times"), QString(QChar(0x00D7))},
                {QStringLiteral("div"), QString(QChar(0x00F7))},
                {QStringLiteral("cdot"), QString(QChar(0x00B7))},
                {QStringLiteral("leq"), QString(QChar(0x2264))},
                {QStringLiteral("le"), QString(QChar(0x2264))},
                {QStringLiteral("geq"), QString(QChar(0x2265))},
                {QStringLiteral("ge"), QString(QChar(0x2265))},
                {QStringLiteral("neq"), QString(QChar(0x2260))},
                {QStringLiteral("ne"), QString(QChar(0x2260))},
                {QStringLiteral("approx"), QString(QChar(0x2248))},
                {QStringLiteral("equiv"), QString(QChar(0x2261))},
                {QStringLiteral("infty"), QString(QChar(0x221E))},
                {QStringLiteral("sum"), QString(QChar(0x2211))},
                {QStringLiteral("prod"), QString(QChar(0x220F))},
                {QStringLiteral("int"), QString(QChar(0x222B))},
                {QStringLiteral("partial"), QString(QChar(0x2202))},
                {QStringLiteral("nabla"), QString(QChar(0x2207))},
                {QStringLiteral("in"), QString(QChar(0x2208))},
                {QStringLiteral("notin"), QString(QChar(0x2209))},
                {QStringLiteral("subset"), QString(QChar(0x2282))},
                {QStringLiteral("subseteq"), QString(QChar(0x2286))},
                {QStringLiteral("cup"), QString(QChar(0x222A))},
                {QStringLiteral("cap"), QString(QChar(0x2229))},
                {QStringLiteral("rightarrow"), QString(QChar(0x2192))},
                {QStringLiteral("to"), QString(QChar(0x2192))},
                {QStringLiteral("leftarrow"), QString(QChar(0x2190))},
                {QStringLiteral("Rightarrow"), QString(QChar(0x21D2))},
                {QStringLiteral("ldots"), QString(QChar(0x2026))},
                {QStringLiteral("cdots"), QString(QChar(0x22EF))},
                {QStringLiteral("circ"), QString(QChar(0x00B0))},
                {QStringLiteral("degree"), QString(QChar(0x00B0))},
                {QStringLiteral("angle"), QString(QChar(0x2220))},
                {QStringLiteral("perp"), QString(QChar(0x22A5))},
                {QStringLiteral("parallel"), QString(QChar(0x2225))},
                {QStringLiteral("alpha"), QString(QChar(0x03B1))},
                {QStringLiteral("beta"), QString(QChar(0x03B2))},
                {QStringLiteral("gamma"), QString(QChar(0x03B3))},
                {QStringLiteral("delta"), QString(QChar(0x03B4))},
                {QStringLiteral("epsilon"), QString(QChar(0x03B5))},
                {QStringLiteral("varepsilon"), QString(QChar(0x03B5))},
                {QStringLiteral("theta"), QString(QChar(0x03B8))},
                {QStringLiteral("lambda"), QString(QChar(0x03BB))},
                {QStringLiteral("mu"), QString(QChar(0x03BC))},
                {QStringLiteral("nu"), QString(QChar(0x03BD))},
                {QStringLiteral("pi"), QString(QChar(0x03C0))},
                {QStringLiteral("rho"), QString(QChar(0x03C1))},
                {QStringLiteral("sigma"), QString(QChar(0x03C3))},
                {QStringLiteral("tau"), QString(QChar(0x03C4))},
                {QStringLiteral("phi"), QString(QChar(0x03C6))},
                {QStringLiteral("varphi"), QString(QChar(0x03C6))},
                {QStringLiteral("omega"), QString(QChar(0x03C9))},
                {QStringLiteral("Gamma"), QString(QChar(0x0393))},
                {QStringLiteral("Delta"), QString(QChar(0x0394))},
                {QStringLiteral("Theta"), QString(QChar(0x0398))},
                {QStringLiteral("Lambda"), QString(QChar(0x039B))},
                {QStringLiteral("Pi"), QString(QChar(0x03A0))},
                {QStringLiteral("Sigma"), QString(QChar(0x03A3))},
                {QStringLiteral("Phi"), QString(QChar(0x03A6))},
                {QStringLiteral("Omega"), QString(QChar(0x03A9))},
                {QStringLiteral("quad"), QStringLiteral(" ")},
                {QStringLiteral("qquad"), QStringLiteral("  ")},
                {QStringLiteral(","), QStringLiteral(" ")},
                {QStringLiteral(";"), QStringLiteral(" ")},
                {QStringLiteral(" "), QStringLiteral(" ")},
                {QStringLiteral("!"), QString()},
                {QStringLiteral("{"), QStringLiteral("{")},
                {QStringLiteral("}"), QStringLiteral("}")},
                {QStringLiteral("%"), QStringLiteral("%")},
                {QStringLiteral("&"), QStringLiteral("&")},
                {QStringLiteral("#"), QStringLiteral("#")},
                {QStringLiteral("_"), QStringLiteral("_")},
                {QStringLiteral("gt"), QStringLiteral(">")},
                {QStringLiteral("lt"), QStringLiteral("<")},
            });
            const auto it = kCmd.constFind(cmd);
            if (it != kCmd.cend()) {
                out += it.value();
                continue;
            }
            if (failed)
                *failed = true;
            out += QLatin1Char('\\');
            out += cmd;
            continue;
        }
        if (c == QLatin1Char('^')) {
            ++i;
            out += mapScript(latexToExcelText(takeLatexGroup(s, &i), failed), true);
            continue;
        }
        if (c == QLatin1Char('_')) {
            ++i;
            out += mapScript(latexToExcelText(takeLatexGroup(s, &i), failed), false);
            continue;
        }
        if (c == QLatin1Char('{') || c == QLatin1Char('}')) {
            ++i;
            continue;
        }
        if (c == QLatin1Char('~')) {
            out += QLatin1Char(' ');
            ++i;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

QString stripMathDelims(QString s) {
    static const QRegularExpression block(QStringLiteral(R"(\$\$([\s\S]+?)\$\$)"));
    static const QRegularExpression display(QStringLiteral(R"(\\\[([\s\S]+?)\\\])"));
    static const QRegularExpression inlineParen(QStringLiteral(R"(\\\(([\s\S]+?)\\\))"));
    static const QRegularExpression inlineDollar(QStringLiteral(R"(\$([^$]+)\$)"));
    s.replace(block, QStringLiteral("\\1"));
    s.replace(display, QStringLiteral("\\1"));
    s.replace(inlineParen, QStringLiteral("\\1"));
    s.replace(inlineDollar, QStringLiteral("\\1"));
    s.remove(QLatin1Char('$'));
    return s;
}

QString latexToExcelText(const QString& input, bool* failed) {
    return convertLatexBody(stripMathDelims(input), failed);
}

QString collapseWs(QString s) {
    static const QRegularExpression wsRe(QStringLiteral("\\s+"));
    s.replace(wsRe, QStringLiteral(" "));
    return s.trimmed();
}

bool looksLikeLatex(const QString& s) {
    if (s.contains(QLatin1Char('$')) || s.contains(QLatin1Char('\\')))
        return true;
    static const QRegularExpression script(QStringLiteral("[\\^_][0-9{]"));
    return script.match(s).hasMatch();
}

// Excel cannot render official $latex$. Convert when we can; otherwise keep official text.
QString excelDisplayText(const QString& official) {
    const QString plain = collapseWs(official);
    if (plain.isEmpty() || !looksLikeLatex(plain))
        return plain;
    bool failed = false;
    const QString converted = collapseWs(latexToExcelText(plain, &failed));
    if (failed || converted.isEmpty())
        return plain;
    return converted;
}

QString cellTextFromInner(QString inner) {
    static const QRegularExpression tagRe(QStringLiteral("<[^>]+>"));
    inner.replace(tagRe, QStringLiteral(" "));
    return excelDisplayText(unescapeHtml(inner));
}

int parseSpan(const QString& raw) {
    const QString s = raw.trimmed();
    if (s.isEmpty())
        return 1;
    bool ok = false;
    const int v = s.toInt(&ok);
    if (!ok)
        return 1;
    return std::max(1, v);
}

int skipWs(const QString& html, int i) {
    while (i < html.size() && html.at(i).isSpace())
        ++i;
    return i;
}

int skipSpecial(const QString& html, int pos) {
    if (html.mid(pos, 4) == QLatin1String("<!--")) {
        const int e = html.indexOf(QLatin1String("-->"), pos + 4);
        return e < 0 ? html.size() : e + 3;
    }
    if (html.mid(pos, 2) == QLatin1String("<!")) {
        const int e = html.indexOf(QLatin1Char('>'), pos + 2);
        return e < 0 ? html.size() : e + 1;
    }
    if (html.mid(pos, 2) == QLatin1String("<?")) {
        const int e = html.indexOf(QLatin1String("?>"), pos + 2);
        return e < 0 ? html.size() : e + 2;
    }
    return pos;
}

struct ParsedTag {
    QString name;
    bool isEnd = false;
    bool selfClosing = false;
    QHash<QString, QString> attrs;
    int end = 0;
};

bool isNameChar(QChar c) {
    return c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char(':')
        || c == QLatin1Char('_');
}

ParsedTag parseTag(const QString& html, int pos) {
    ParsedTag tag;
    int i = pos + 1;
    if (i < html.size() && html.at(i) == QLatin1Char('/')) {
        tag.isEnd = true;
        ++i;
    }
    i = skipWs(html, i);
    const int nameStart = i;
    while (i < html.size() && isNameChar(html.at(i)))
        ++i;
    tag.name = html.mid(nameStart, i - nameStart).toLower();

    while (i < html.size()) {
        i = skipWs(html, i);
        if (i >= html.size())
            break;
        if (html.at(i) == QLatin1Char('>')) {
            ++i;
            break;
        }
        if (html.at(i) == QLatin1Char('/') && i + 1 < html.size()
            && html.at(i + 1) == QLatin1Char('>')) {
            tag.selfClosing = true;
            i += 2;
            break;
        }
        const int keyStart = i;
        while (i < html.size()) {
            const QChar c = html.at(i);
            if (c.isSpace() || c == QLatin1Char('=') || c == QLatin1Char('>')
                || c == QLatin1Char('/')) {
                break;
            }
            ++i;
        }
        const QString key = html.mid(keyStart, i - keyStart).toLower();
        if (key.isEmpty()) {
            // Official content_list keeps raw '<' in $b^2-4ac<0$. Do not spin on '/'.
            if (i < html.size() && html.at(i) != QLatin1Char('>'))
                ++i;
            continue;
        }
        i = skipWs(html, i);
        QString val;
        if (i < html.size() && html.at(i) == QLatin1Char('=')) {
            ++i;
            i = skipWs(html, i);
            if (i < html.size()
                && (html.at(i) == QLatin1Char('"') || html.at(i) == QLatin1Char('\''))) {
                const QChar quote = html.at(i);
                ++i;
                const int valStart = i;
                while (i < html.size() && html.at(i) != quote)
                    ++i;
                val = html.mid(valStart, i - valStart);
                if (i < html.size())
                    ++i;
            } else {
                const int valStart = i;
                while (i < html.size() && !html.at(i).isSpace()
                       && html.at(i) != QLatin1Char('>') && html.at(i) != QLatin1Char('/')) {
                    ++i;
                }
                val = html.mid(valStart, i - valStart);
            }
        }
        if (!key.isEmpty())
            tag.attrs.insert(key, val);
    }
    tag.end = i;
    return tag;
}

int nextTag(const QString& html, int pos, int limit, ParsedTag* tag) {
    while (pos < limit) {
        const int lt = html.indexOf(QLatin1Char('<'), pos);
        if (lt < 0 || lt >= limit)
            return -1;
        const int skipped = skipSpecial(html, lt);
        if (skipped != lt) {
            pos = skipped;
            continue;
        }
        *tag = parseTag(html, lt);
        if (tag->end <= lt)
            return -1;
        if (tag->name.isEmpty() || !tag->name.at(0).isLetter()) {
            pos = lt + 1;
            continue;
        }
        return lt;
    }
    return -1;
}

struct CellTok {
    int rowspan = 1;
    int colspan = 1;
    QString text;
};

QVector<CellTok> parseRowCells(const QString& html, int begin, int end) {
    QVector<CellTok> direct;
    QVector<CellTok> all;
    int pos = begin;
    int nestTable = 0;
    int extraTr = 0;
    ParsedTag tag;
    while (pos < end) {
        const int lt = nextTag(html, pos, end, &tag);
        if (lt < 0)
            break;
        if (tag.name == QLatin1String("table")) {
            if (tag.isEnd || tag.selfClosing) {
                if (nestTable > 0)
                    --nestTable;
            } else {
                ++nestTable;
            }
            pos = tag.end;
            continue;
        }
        if (tag.name == QLatin1String("tr")) {
            if (!tag.isEnd && !tag.selfClosing)
                ++extraTr;
            else if (extraTr > 0)
                --extraTr;
            pos = tag.end;
            continue;
        }
        const bool isCell = (tag.name == QLatin1String("td") || tag.name == QLatin1String("th"));
        if (isCell && !tag.isEnd) {
            int cellEnd = end;
            int depth = 1;
            int p = tag.end;
            ParsedTag inner;
            while (p < end) {
                const int lt2 = nextTag(html, p, end, &inner);
                if (lt2 < 0)
                    break;
                if (inner.name == tag.name) {
                    if (inner.isEnd) {
                        --depth;
                        if (depth == 0) {
                            cellEnd = lt2;
                            p = inner.end;
                            break;
                        }
                    } else if (!inner.selfClosing) {
                        ++depth;
                    }
                }
                p = inner.end;
            }
            CellTok cell;
            cell.rowspan = parseSpan(tag.attrs.value(QStringLiteral("rowspan")));
            cell.colspan = parseSpan(tag.attrs.value(QStringLiteral("colspan")));
            if (tag.selfClosing)
                cell.text = QString();
            else
                cell.text = cellTextFromInner(html.mid(tag.end, cellEnd - tag.end));
            all.append(cell);
            if (nestTable == 0 && extraTr == 0)
                direct.append(cell);
            pos = p > tag.end ? p : tag.end;
            continue;
        }
        if (isCell && tag.selfClosing && nestTable == 0 && extraTr == 0) {
            CellTok cell;
            cell.rowspan = parseSpan(tag.attrs.value(QStringLiteral("rowspan")));
            cell.colspan = parseSpan(tag.attrs.value(QStringLiteral("colspan")));
            all.append(cell);
            direct.append(cell);
        }
        pos = tag.end;
    }
    return direct.isEmpty() ? all : direct;
}

QString jsonToPyStr(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::String:
        return value.toString();
    case QJsonValue::Double: {
        const double d = value.toDouble();
        if (std::isfinite(d) && d == std::floor(d))
            return QString::number(qint64(d));
        return QString::number(d);
    }
    case QJsonValue::Bool:
        return value.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    case QJsonValue::Null:
        return QStringLiteral("None");
    case QJsonValue::Array:
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    case QJsonValue::Object:
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    default:
        return QString();
    }
}

QString captionText(const QJsonValue& captions) {
    if (captions.isArray()) {
        QStringList parts;
        const QJsonArray arr = captions.toArray();
        for (const QJsonValue& x : arr)
            parts.append(jsonToPyStr(x));
        return parts.join(QLatin1Char(' ')).trimmed();
    }
    if (captions.isUndefined() || captions.isNull())
        return QString();
    return jsonToPyStr(captions).trimmed();
}

QString safeSheetName(QString name, QSet<QString>& used) {
    static const QRegularExpression illegal(QStringLiteral("[:\\\\/?*\\[\\]]"));
    name.replace(illegal, QStringLiteral("_"));
    name = name.trimmed();
    if (name.isEmpty())
        name = QStringLiteral("Sheet");
    name = name.left(31);
    const QString base = name;
    int i = 2;
    while (used.contains(name)) {
        const QString suffix = QLatin1Char('_') + QString::number(i);
        name = base.left(31 - suffix.size()) + suffix;
        ++i;
    }
    used.insert(name);
    return name;
}

QString safeFileStem(QString stem) {
    static const QRegularExpression illegal(QStringLiteral("[<>:\"/\\\\|?*]"));
    stem.replace(illegal, QStringLiteral("_"));
    return stem;
}

QStringList collectFiles(const QString& root, const QString& filter) {
    QStringList out;
    QDirIterator it(root, QStringList{filter}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        out.append(it.filePath());
    }
    return out;
}

}  // namespace

std::pair<std::vector<QStringList>, std::vector<std::tuple<int, int, int, int>>>
htmlTableToGrid(const QString& html) {
    if (html.isEmpty() || !html.contains(QLatin1String("<table"), Qt::CaseInsensitive))
        return {};

    int pos = 0;
    int tableInner = -1;
    ParsedTag tag;
    while (pos < html.size()) {
        const int lt = nextTag(html, pos, html.size(), &tag);
        if (lt < 0)
            break;
        if (!tag.isEnd && tag.name == QLatin1String("table")) {
            tableInner = tag.end;
            break;
        }
        pos = tag.end;
    }
    if (tableInner < 0)
        return {};

    struct RowSpan {
        int begin = 0;
        int end = 0;
    };
    QVector<RowSpan> rowSpans;
    QVector<int> trStack;
    int tableDepth = 1;
    pos = tableInner;
    while (pos < html.size() && tableDepth > 0) {
        const int lt = nextTag(html, pos, html.size(), &tag);
        if (lt < 0)
            break;
        if (tag.name == QLatin1String("table")) {
            if (tag.isEnd || tag.selfClosing)
                --tableDepth;
            else
                ++tableDepth;
            pos = tag.end;
            continue;
        }
        if (tableDepth < 1)
            break;
        if (tag.name == QLatin1String("tr")) {
            if (!tag.isEnd && !tag.selfClosing) {
                trStack.append(tag.end);
            } else if (!trStack.isEmpty()) {
                RowSpan row;
                row.begin = trStack.takeLast();
                row.end = lt;
                rowSpans.append(row);
            }
        }
        pos = tag.end;
    }
    while (!trStack.isEmpty()) {
        RowSpan row;
        row.begin = trStack.takeLast();
        row.end = html.size();
        rowSpans.append(row);
    }

    std::map<std::pair<int, int>, QString> gridMap;
    std::set<std::pair<int, int>> occupied;
    std::vector<std::tuple<int, int, int, int>> merges;

    int rowIdx = 0;
    for (const RowSpan& row : rowSpans) {
        const QVector<CellTok> cells = parseRowCells(html, row.begin, row.end);
        int colIdx = 0;
        for (const CellTok& cell : cells) {
            while (occupied.count({rowIdx, colIdx}) != 0)
                ++colIdx;
            const int rs = cell.rowspan;
            const int cs = cell.colspan;
            gridMap[{rowIdx, colIdx}] = cell.text;
            for (int r = rowIdx; r < rowIdx + rs; ++r) {
                for (int c = colIdx; c < colIdx + cs; ++c) {
                    occupied.insert({r, c});
                    if (gridMap.find({r, c}) == gridMap.end())
                        gridMap[{r, c}] = QString();
                }
            }
            if (rs > 1 || cs > 1)
                merges.emplace_back(rowIdx, colIdx, rowIdx + rs - 1, colIdx + cs - 1);
            colIdx += cs;
        }
        ++rowIdx;
    }

    if (gridMap.empty())
        return {};

    int maxR = 0;
    int maxC = 0;
    for (const auto& kv : gridMap) {
        maxR = std::max(maxR, kv.first.first);
        maxC = std::max(maxC, kv.first.second);
    }

    std::vector<QStringList> rows;
    rows.reserve(size_t(maxR + 1));
    for (int r = 0; r <= maxR; ++r) {
        QStringList row;
        row.reserve(maxC + 1);
        for (int c = 0; c <= maxC; ++c) {
            const auto it = gridMap.find({r, c});
            row.append(it == gridMap.end() ? QString() : it->second);
        }
        rows.push_back(row);
    }
    return {rows, merges};
}

namespace {

enum class ContentListReadStatus {
    Tables,
    NoTables,
    OpenFailed,
    ReadFailed,
    ParseFailed,
    RootSchemaFailed,
};

struct ContentListReadResult {
    ContentListReadStatus status = ContentListReadStatus::NoTables;
    std::vector<TableSheet> sheets;
    QString message;
};

bool contentListReadFailed(ContentListReadStatus status) {
    return status == ContentListReadStatus::OpenFailed
           || status == ContentListReadStatus::ReadFailed
           || status == ContentListReadStatus::ParseFailed
           || status == ContentListReadStatus::RootSchemaFailed;
}

ContentListReadResult readTablesFromContentList(const QString& path) {
    ContentListReadResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.status = ContentListReadStatus::OpenFailed;
        result.message = QStringLiteral("cannot open content_list: %1 (%2)")
                             .arg(path, file.errorString());
        return result;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        result.status = ContentListReadStatus::ReadFailed;
        result.message = QStringLiteral("cannot read content_list: %1 (%2)")
                             .arg(path, file.errorString());
        return result;
    }

    QJsonDocument document;
    QString jsonError;
    if (!parseJsonDocumentStrict(bytes, &document, &jsonError)) {
        result.status = ContentListReadStatus::ParseFailed;
        result.message = QStringLiteral("invalid content_list JSON: %1 (%2)")
                             .arg(path, jsonError);
        return result;
    }
    if (!document.isArray()) {
        result.status = ContentListReadStatus::RootSchemaFailed;
        result.message = QStringLiteral("invalid content_list root schema; expected a JSON array: %1")
                             .arg(path);
        return result;
    }

    int tableI = 0;
    const QJsonArray arr = document.array();
    for (int itemIndex = 0; itemIndex < arr.size(); ++itemIndex) {
        const QJsonValue itemVal = arr.at(itemIndex);
        if (!itemVal.isObject()) {
            result.status = ContentListReadStatus::RootSchemaFailed;
            result.sheets.clear();
            result.message = QStringLiteral("invalid content_list item %1; expected a JSON object: %2")
                                 .arg(itemIndex)
                                 .arg(path);
            return result;
        }
        const QJsonObject item = itemVal.toObject();
        if (item.value(QStringLiteral("type")).toString() != QLatin1String("table"))
            continue;
        QString html = item.value(QStringLiteral("table_body")).toString();
        if (html.isEmpty())
            html = item.value(QStringLiteral("html")).toString();
        if (html.isEmpty())
            continue;
        auto grid = htmlTableToGrid(html);
        if (grid.first.empty())
            continue;
        ++tableI;
        const QString cap = captionText(item.value(QStringLiteral("table_caption")));
        QString title = cap.isEmpty() ? QStringLiteral("表格%1").arg(tableI) : cap;
        const QJsonValue page = item.value(QStringLiteral("page_idx"));
        if (cap.isEmpty() && !page.isUndefined() && !page.isNull())
            title = QStringLiteral("表格%1_p%2").arg(tableI).arg(jsonToPyStr(page));
        TableSheet sheet;
        sheet.title = title;
        sheet.rows = std::move(grid.first);
        sheet.merges = std::move(grid.second);
        result.sheets.push_back(std::move(sheet));
    }
    if (result.sheets.empty()) {
        result.status = ContentListReadStatus::NoTables;
        result.message = QStringLiteral("valid content_list contains no exportable tables: %1")
                             .arg(path);
    } else {
        result.status = ContentListReadStatus::Tables;
    }
    return result;
}

}  // namespace

std::vector<TableSheet> extractTablesFromContentList(const QString& path) {
    return readTablesFromContentList(path).sheets;
}

std::vector<TableSheet> extractTablesFromMarkdown(const QString& path) {
    bool ok = false;
    const QString text = readUtf8File(path, &ok);
    if (!ok)
        return {};
    static const QRegularExpression tableRe(
        QStringLiteral("(<table[\\s\\S]*?</table>)"),
        QRegularExpression::CaseInsensitiveOption);
    std::vector<TableSheet> sheets;
    int i = 0;
    QRegularExpressionMatchIterator it = tableRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        ++i;
        auto grid = htmlTableToGrid(m.captured(1));
        if (grid.first.empty())
            continue;
        TableSheet sheet;
        sheet.title = QStringLiteral("表格%1").arg(i);
        sheet.rows = std::move(grid.first);
        sheet.merges = std::move(grid.second);
        sheets.push_back(std::move(sheet));
    }
    return sheets;
}

std::optional<TableSheet> extractTextSheetFromMarkdown(const QString& path) {
    bool ok = false;
    QString text = readUtf8File(path, &ok);
    if (!ok)
        return std::nullopt;
    text = text.trimmed();
    if (text.isEmpty())
        return std::nullopt;

    static const QRegularExpression tableRe(
        QStringLiteral("<table[\\s\\S]*?</table>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tagRe(QStringLiteral("<[^>]+>"));
    QString cleaned = text;
    cleaned.replace(tableRe, QString());
    cleaned.replace(tagRe, QString());
    cleaned.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    cleaned.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QStringList lines;
    const QStringList raw = cleaned.split(QLatin1Char('\n'));
    for (const QString& ln : raw) {
        const QString s = excelDisplayText(ln);
        if (!s.isEmpty())
            lines.append(s);
    }
    if (lines.isEmpty())
        return std::nullopt;

    TableSheet sheet;
    sheet.title = QStringLiteral("文本");
    sheet.rows.reserve(size_t(lines.size()));
    for (const QString& ln : lines)
        sheet.rows.push_back(QStringList{ln});
    return sheet;
}

QStringList writeWorkbook(const std::vector<TableSheet>& sheets, const QString& outPath) {
    const QString absOut = QFileInfo(outPath).absoluteFilePath();
    QDir().mkpath(QFileInfo(absOut).absolutePath());

    QXlsx::Document xlsx;
    const QStringList defaultSheets = xlsx.sheetNames();

    QXlsx::Format cellFmt;
    cellFmt.setBorderStyle(QXlsx::Format::BorderThin);
    cellFmt.setBorderColor(Qt::black);
    cellFmt.setTextWrap(true);
    cellFmt.setVerticalAlignment(QXlsx::Format::AlignVCenter);

    QXlsx::Format headerFmt = cellFmt;
    headerFmt.setFontBold(true);

    QSet<QString> used;
    QStringList names;

    for (const TableSheet& sheet : sheets) {
        const QString title = safeSheetName(sheet.title, used);
        names.append(title);
        if (!xlsx.sheetNames().contains(title))
            xlsx.addSheet(title);
        xlsx.selectSheet(title);

        QXlsx::Worksheet* ws = xlsx.currentWorksheet();
        int colCount = 0;
        for (const QStringList& row : sheet.rows)
            colCount = std::max(colCount, int(row.size()));

        for (int r = 0; r < int(sheet.rows.size()); ++r) {
            const QStringList& row = sheet.rows[size_t(r)];
            const QXlsx::Format& fmt = (r == 0) ? headerFmt : cellFmt;
            for (int c = 0; c < colCount; ++c) {
                // Keep math as visible text. QXlsx::write() would treat "=..." as a formula.
                if (c < row.size()) {
                    const QString& value = row.at(c);
                    if (ws) {
                        if (value.isEmpty())
                            ws->writeBlank(r + 1, c + 1, fmt);
                        else
                            ws->writeString(r + 1, c + 1, value, fmt);
                    } else {
                        xlsx.write(r + 1, c + 1, value, fmt);
                    }
                } else if (ws) {
                    ws->writeBlank(r + 1, c + 1, fmt);
                } else {
                    xlsx.write(r + 1, c + 1, QString(), fmt);
                }
            }
        }
        for (const auto& merge : sheet.merges) {
            const int r1 = std::get<0>(merge);
            const int c1 = std::get<1>(merge);
            const int r2 = std::get<2>(merge);
            const int c2 = std::get<3>(merge);
            if (r2 > r1 || c2 > c1) {
                const QXlsx::Format& fmt = (r1 == 0) ? headerFmt : cellFmt;
                xlsx.mergeCells(QXlsx::CellRange(r1 + 1, c1 + 1, r2 + 1, c2 + 1), fmt);
            }
        }
        if (!sheet.rows.empty()) {
            for (int c = 1; c <= colCount; ++c) {
                int maxLen = 0;
                for (const QStringList& row : sheet.rows) {
                    if (c - 1 < row.size())
                        maxLen = std::max(maxLen, row.at(c - 1).size());
                }
                const double width = std::min(40.0, std::max(8.0, double(maxLen + 2)));
                xlsx.setColumnWidth(c, width);
            }
        }
    }

    if (names.isEmpty()) {
        const QString emptyName = QStringLiteral("空");
        if (!xlsx.sheetNames().contains(emptyName))
            xlsx.addSheet(emptyName);
        xlsx.selectSheet(emptyName);
        xlsx.write(1, 1, QStringLiteral("未识别到表格"), cellFmt);
        names.append(emptyName);
    }

    for (const QString& name : defaultSheets) {
        if (!names.contains(name))
            xlsx.deleteSheet(name);
    }

    if (!xlsx.saveAs(absOut))
        return {};
    return names;
}

ExcelExportResult exportExcelFromParseDir(const QString& parseDir,
                                          const QString& outXlsx,
                                          const QString& sourceName) {
    ExcelExportResult result;
    const QFileInfo dirInfo(parseDir);
    if (!dirInfo.exists() || !dirInfo.isDir()) {
        result.success = false;
        result.tableCount = 0;
        result.message = QStringLiteral("目录不存在: %1").arg(parseDir);
        return result;
    }

    QStringList contentLists;
    const QStringList primary = collectFiles(parseDir, QStringLiteral("*_content_list.json"));
    for (const QString& p : primary) {
        if (!QFileInfo(p).fileName().toLower().contains(QLatin1String("v2")))
            contentLists.append(p);
    }
    if (contentLists.isEmpty())
        contentLists = collectFiles(parseDir, QStringLiteral("*content_list*.json"));

    QStringList mds;
    const QStringList mdFiles = collectFiles(parseDir, QStringLiteral("*.md"));
    for (const QString& p : mdFiles) {
        if (QFileInfo(p).fileName() != QLatin1String("verify_summary.md"))
            mds.append(p);
    }

    std::vector<TableSheet> sheets;
    for (const QString& cl : contentLists) {
        ContentListReadResult readResult = readTablesFromContentList(cl);
        if (contentListReadFailed(readResult.status)) {
            result.success = false;
            result.tableCount = 0;
            result.message = readResult.message;
            return result;
        }
        sheets.insert(sheets.end(),
                      std::make_move_iterator(readResult.sheets.begin()),
                      std::make_move_iterator(readResult.sheets.end()));
    }
    const bool contentListTablesFound = !sheets.empty();
    if (sheets.empty()) {
        for (const QString& md : mds) {
            const std::vector<TableSheet> found = extractTablesFromMarkdown(md);
            sheets.insert(sheets.end(), found.begin(), found.end());
        }
    }
    if (sheets.empty()) {
        for (const QString& md : mds) {
            const std::optional<TableSheet> textSheet = extractTextSheetFromMarkdown(md);
            if (textSheet) {
                sheets.push_back(*textSheet);
                break;
            }
        }
    }

    QString outPath = outXlsx;
    if (outPath.isEmpty()) {
        QString stem = sourceName;
        if (stem.isEmpty())
            stem = mds.isEmpty() ? dirInfo.fileName() : QFileInfo(mds.front()).completeBaseName();
        stem = safeFileStem(stem);
        outPath = QDir(parseDir).filePath(stem + QStringLiteral(".xlsx"));
    }

    const QStringList names = writeWorkbook(sheets, outPath);
    if (names.isEmpty()) {
        result.success = false;
        result.tableCount = 0;
        result.message = QStringLiteral("写 Excel 失败");
        return result;
    }

    int tableLike = 0;
    for (const TableSheet& s : sheets) {
        if (s.title != QStringLiteral("文本"))
            ++tableLike;
    }

    result.success = true;
    result.path = outPath;
    result.tableCount = tableLike;
    if (!contentLists.isEmpty() && !contentListTablesFound) {
        if (sheets.empty()) {
            result.message = QStringLiteral("content_list is valid but contains no exportable tables; "
                                            "created an empty worksheet");
        } else {
            result.message = QStringLiteral("content_list is valid but contains no exportable tables; "
                                            "exported %1 worksheet(s) from Markdown")
                                 .arg(int(sheets.size()));
        }
    } else {
        result.message = QStringLiteral("导出 %1 个工作表").arg(int(sheets.size()));
    }
    result.sheetNames = names;
    return result;
}

}  // namespace scanengine

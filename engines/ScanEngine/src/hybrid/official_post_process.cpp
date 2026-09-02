#include "scanengine/hybrid/official_post_process.hpp"

#include "scanengine/hybrid/otsl.hpp"

#include <QJsonValue>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <utility>

namespace scanengine {
namespace hybrid {

namespace {

const QString kType = QStringLiteral("type");
const QString kBbox = QStringLiteral("bbox");
const QString kAngle = QStringLiteral("angle");
const QString kContent = QStringLiteral("content");
const QString kSubType = QStringLiteral("sub_type");
const QString kTableImageTokenMap = QStringLiteral("_table_image_token_map");
const QString kTableImageAbsorbed = QStringLiteral("_absorbed_by_table");
const QString kNonText = QStringLiteral("[Non-Text]");

using RegexReplacement = std::function<QString(const QRegularExpressionMatch&)>;

QRegularExpression pythonRegex(
    const QString& pattern,
    QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption) {
    // Python 3's re module applies Unicode properties to \d, \s, \w and \b by
    // default.  PCRE2 (and therefore QRegularExpression) requires UCP to be
    // selected explicitly for the same contract.
    options |= QRegularExpression::UseUnicodePropertiesOption;
    return QRegularExpression(pattern, options);
}

QString replaceMatches(const QString& input,
                       const QRegularExpression& expression,
                       const RegexReplacement& replacement) {
    QString output;
    output.reserve(input.size());
    int previousEnd = 0;
    QRegularExpressionMatchIterator matches = expression.globalMatch(input);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        output.append(input.mid(previousEnd, match.capturedStart() - previousEnd));
        output.append(replacement(match));
        previousEnd = match.capturedEnd();
    }
    output.append(input.mid(previousEnd));
    return output;
}

bool isPythonLineBoundary(QChar character) {
    const ushort value = character.unicode();
    return value == '\n' || value == '\r' || value == 0x0b || value == 0x0c
        || (value >= 0x1c && value <= 0x1e) || value == 0x85 || value == 0x2028
        || value == 0x2029;
}

QStringList pythonSplitLines(const QString& input) {
    QStringList lines;
    int start = 0;
    for (int index = 0; index < input.size(); ++index) {
        if (!isPythonLineBoundary(input.at(index)))
            continue;
        lines.append(input.mid(start, index - start));
        if (input.at(index) == QLatin1Char('\r') && index + 1 < input.size()
            && input.at(index + 1) == QLatin1Char('\n')) {
            ++index;
        }
        start = index + 1;
    }
    if (start < input.size())
        lines.append(input.mid(start));
    return lines;
}

QString joinPreservingTerminalNewline(const QStringList& lines, const QString& original) {
    QString joined = lines.join(QLatin1Char('\n'));
    if (original.endsWith(QLatin1Char('\n')))
        joined.append(QLatin1Char('\n'));
    return joined;
}

bool jsonTruthy(const QJsonValue& value) {
    if (value.isUndefined() || value.isNull())
        return false;
    if (value.isBool())
        return value.toBool();
    if (value.isDouble())
        return value.toDouble() != 0.0;
    if (value.isString())
        return !value.toString().isEmpty();
    if (value.isArray())
        return !value.toArray().isEmpty();
    if (value.isObject())
        return !value.toObject().isEmpty();
    return false;
}

bool hasStringContent(const QJsonObject& block) {
    return block.value(kContent).isString() && !block.value(kContent).toString().isEmpty();
}

void setBlockType(QJsonObject* block, const QString& type) {
    if (!block)
        return;
    const QJsonValue previousMerge = block->value(QStringLiteral("merge_prev"));
    block->insert(kType, type);
    if (type == QLatin1String("text")) {
        block->insert(QStringLiteral("merge_prev"),
                      previousMerge.isBool() ? previousMerge : QJsonValue(false));
    } else {
        block->remove(QStringLiteral("merge_prev"));
    }
}

bool isAbsorbedTableImage(const QJsonObject& block) {
    return jsonTruthy(block.value(kTableImageAbsorbed));
}

QJsonObject cleanupPrivateMetadata(QJsonObject block) {
    block.remove(kTableImageTokenMap);
    block.remove(kTableImageAbsorbed);
    return block;
}

QJsonArray cleanupFailureBlocks(const QJsonArray& blocks) {
    QJsonArray output;
    for (const QJsonValue& value : blocks) {
        if (!value.isObject()) {
            output.append(value);
            continue;
        }
        QJsonObject block = value.toObject();
        if (block.value(kType).toString() == QLatin1String("image")
            && isAbsorbedTableImage(block)) {
            continue;
        }
        output.append(cleanupPrivateMetadata(std::move(block)));
    }
    return output;
}

QString fixEquationDelimiters(const QString& latex) {
    QString fixed = latex.trimmed();
    if (fixed.startsWith(QStringLiteral("\\[")))
        fixed.remove(0, 2);
    if (fixed.endsWith(QStringLiteral("\\]")))
        fixed.chop(2);
    return fixed.trimmed();
}

QString fixEquationDoubleSubscript(const QString& latex) {
    static const QRegularExpression pattern = pythonRegex(
        QStringLiteral(R"(_\s*\{([^{}]|\{[^{}]*\})*\}\s*_\s*\{([^{}]|\{[^{}]*\})*\})"));
    QString fixed = latex;
    fixed.replace(pattern, QString());
    return fixed;
}

QString fixEquationEqColon(QString latex) {
    latex.replace(QStringLiteral("\\eqqcolon"), QStringLiteral("=:"));
    latex.replace(QStringLiteral("\\coloneqq"), QStringLiteral(":="));
    return latex;
}

QString fixEquationBig(QString latex) {
    // equation_big.py spells out the Cartesian product below.  Keeping it as
    // data preserves the exact substitutions while avoiding 470 repetitive
    // regex statements.
    const QStringList macros = {
        QStringLiteral("big"),   QStringLiteral("bigr"),  QStringLiteral("bigm"),
        QStringLiteral("bigl"),  QStringLiteral("bigg"),  QStringLiteral("biggr"),
        QStringLiteral("biggm"), QStringLiteral("biggl"), QStringLiteral("Big"),
        QStringLiteral("Bigr"),  QStringLiteral("Bigm"),  QStringLiteral("Bigl"),
        QStringLiteral("Bigg"),  QStringLiteral("Biggr"), QStringLiteral("Biggm"),
        QStringLiteral("Biggl"),
    };

    for (const QString& macro : macros) {
        const QString command = QLatin1Char('\\') + macro;
        for (const QString& space : {QString(), QStringLiteral(" ")}) {
            const QString prefix = command + space;
            latex.replace(prefix + QStringLiteral("{)}"), command + QLatin1Char(')'));
            latex.replace(prefix + QStringLiteral("{(}"), command + QLatin1Char('('));
            // The pinned file omits Biggm from these two generated groups.
            if (macro != QLatin1String("Biggm")) {
                latex.replace(prefix + QStringLiteral("{\\}}"), command + QStringLiteral("\\}"));
                latex.replace(prefix + QStringLiteral("{\\{}"), command + QStringLiteral("\\{"));
            }
            latex.replace(prefix + QStringLiteral("{|}"), command + QLatin1Char('|'));
            latex.replace(prefix + QStringLiteral("{\\|}"), command + QStringLiteral("\\|"));

            const bool escapedBracket = macro == QLatin1String("biggm")
                || macro == QLatin1String("biggl") || macro == QLatin1String("Biggm")
                || macro == QLatin1String("Biggl");
            latex.replace(prefix + QStringLiteral("{[}"),
                          command + (escapedBracket ? QStringLiteral("\\[")
                                                    : QStringLiteral("[")));
            latex.replace(prefix + QStringLiteral("{]}"),
                          command + (escapedBracket ? QStringLiteral("\\]")
                                                    : QStringLiteral("]")));
            if (macro != QLatin1String("Biggm")) {
                latex.replace(prefix + QStringLiteral("{\\rangle}"),
                              command + QStringLiteral("\\rangle "));
                latex.replace(prefix + QStringLiteral("{\\langle}"),
                              command + QStringLiteral("\\langle "));
            }
        }
    }
    latex.replace(QStringLiteral("\\bigtimes"), QStringLiteral("\\times"));
    return latex;
}

int countOccurrences(const QString& text, const QString& needle) {
    if (needle.isEmpty())
        return 0;
    int count = 0;
    int from = 0;
    while ((from = text.indexOf(needle, from)) >= 0) {
        ++count;
        from += needle.size();
    }
    return count;
}

const QStringList& validLeftTokens() {
    static const QStringList tokens = {
        QStringLiteral("\\left\\lbrace"), QStringLiteral("\\left\\lVert"),
        QStringLiteral("\\left\\lvert"),  QStringLiteral("\\left\\rvert"),
        QStringLiteral("\\left\\rVert"),  QStringLiteral("\\left\\vert"),
        QStringLiteral("\\left\\Vert"),   QStringLiteral("\\left\\lfloor"),
        QStringLiteral("\\left\\lbrack"), QStringLiteral("\\left\\langle"),
        QStringLiteral("\\left|"),          QStringLiteral("\\left\\|"),
        QStringLiteral("\\left["),          QStringLiteral("\\left]"),
        QStringLiteral("\\left("),          QStringLiteral("\\left)"),
        QStringLiteral("\\left\\{"),       QStringLiteral("\\left\\}"),
        QStringLiteral("\\left."),          QStringLiteral("\\left/"),
    };
    return tokens;
}

const QStringList& validRightTokens() {
    static const QStringList tokens = {
        QStringLiteral("\\right\\rbrace"), QStringLiteral("\\right\\lVert"),
        QStringLiteral("\\right\\lvert"),  QStringLiteral("\\right\\rvert"),
        QStringLiteral("\\right\\rVert"),  QStringLiteral("\\right\\vert"),
        QStringLiteral("\\right\\Vert"),   QStringLiteral("\\right\\rfloor"),
        QStringLiteral("\\right\\rbrack"), QStringLiteral("\\right\\rangle"),
        QStringLiteral("\\right|"),          QStringLiteral("\\right\\|"),
        QStringLiteral("\\right]"),          QStringLiteral("\\right["),
        QStringLiteral("\\right)"),          QStringLiteral("\\right("),
        QStringLiteral("\\right\\}"),       QStringLiteral("\\right\\{"),
        QStringLiteral("\\right."),          QStringLiteral("\\right/"),
    };
    return tokens;
}

QRegularExpression literalAlternation(const QStringList& tokens) {
    QStringList alternatives;
    alternatives.reserve(tokens.size());
    for (const QString& token : tokens)
        alternatives.append(QRegularExpression::escape(token));
    return pythonRegex(QStringLiteral("(") + alternatives.join(QLatin1Char('|'))
                       + QLatin1Char(')'));
}

int countTokenMatches(const QString& latex, const QStringList& tokens) {
    const QRegularExpression pattern = literalAlternation(tokens);
    int count = 0;
    QRegularExpressionMatchIterator iterator = pattern.globalMatch(latex);
    while (iterator.hasNext()) {
        iterator.next();
        ++count;
    }
    return count;
}

QStringList countedLeftTokens() {
    QStringList tokens = validLeftTokens();
    tokens.removeAll(QStringLiteral("\\left/"));
    tokens.removeAll(QStringLiteral("\\left)"));
    return tokens;
}

QStringList countedRightTokens() {
    // RIGHT_TOKEN_LIST ends in the regex token r"\\right.", whose dot is a
    // wildcard in the pinned source.  Consequently all valid right tokens,
    // including right( and right/, contribute to the count.
    return validRightTokens();
}

QStringList splitKeepingMatches(const QString& input, const QRegularExpression& pattern) {
    QStringList parts;
    int previousEnd = 0;
    QRegularExpressionMatchIterator iterator = pattern.globalMatch(input);
    while (iterator.hasNext()) {
        const QRegularExpressionMatch match = iterator.next();
        parts.append(input.mid(previousEnd, match.capturedStart() - previousEnd));
        parts.append(match.captured(0));
        previousEnd = match.capturedEnd();
    }
    parts.append(input.mid(previousEnd));
    return parts;
}

bool isPairLeftRight(const QString& left, const QString& right) {
    if (!left.startsWith(QStringLiteral("\\left"))
        || !right.startsWith(QStringLiteral("\\right"))) {
        return false;
    }
    if (left == QLatin1String("\\left." ) || right == QLatin1String("\\right."))
        return true;

    static const QVector<QPair<QString, QString>> pairs = {
        {QStringLiteral("\\left\\lbrace"), QStringLiteral("\\right\\rbrace")},
        {QStringLiteral("\\left\\lVert"), QStringLiteral("\\right\\lVert")},
        {QStringLiteral("\\left\\lvert"), QStringLiteral("\\right\\lvert")},
        {QStringLiteral("\\left\\vert"), QStringLiteral("\\right\\vert")},
        {QStringLiteral("\\left\\Vert"), QStringLiteral("\\right\\Vert")},
        {QStringLiteral("\\left\\lfloor"), QStringLiteral("\\right\\rfloor")},
        {QStringLiteral("\\left\\lbrack"), QStringLiteral("\\right\\rbrack")},
        {QStringLiteral("\\left\\langle"), QStringLiteral("\\right\\rangle")},
        {QStringLiteral("\\left|"), QStringLiteral("\\right|")},
        {QStringLiteral("\\left\\|"), QStringLiteral("\\right\\|")},
        {QStringLiteral("\\left["), QStringLiteral("\\right]")},
        {QStringLiteral("\\left]"), QStringLiteral("\\right[")},
        {QStringLiteral("\\left("), QStringLiteral("\\right)")},
        {QStringLiteral("\\left)"), QStringLiteral("\\right(")},
        {QStringLiteral("\\left\\{"), QStringLiteral("\\right\\}")},
        {QStringLiteral("\\left\\}"), QStringLiteral("\\right\\{")},
        {QStringLiteral("\\left/"), QStringLiteral("\\right/")},
    };
    for (const auto& pair : pairs) {
        if (left == pair.first && right == pair.second)
            return true;
    }
    return false;
}

struct ArrayRange {
    int tag = 0;
    int start = 0;
    int end = 0;
    QVector<int> containedTags;
};

bool tagArrays(const QStringList& nodes, QVector<ArrayRange>* ranges) {
    QVector<QPair<int, int>> stack;
    int nextTag = 0;
    for (int index = 0; index < nodes.size(); ++index) {
        const QString& node = nodes.at(index);
        if (node.contains(QStringLiteral("\\begin{array}"))) {
            ++nextTag;
            stack.append(qMakePair(nextTag, index));
        } else if (node == QLatin1String("\\end{array}")) {
            if (stack.isEmpty())
                return false;
            const QPair<int, int> opening = stack.takeLast();
            ArrayRange range;
            range.tag = opening.first;
            range.start = opening.second;
            range.end = index;
            ranges->append(range);
        }
    }
    if (!stack.isEmpty())
        return false;

    for (int first = 0; first < ranges->size(); ++first) {
        for (int second = 0; second < ranges->size(); ++second) {
            if (first == second)
                continue;
            if (ranges->at(first).start < ranges->at(second).start
                && ranges->at(first).end > ranges->at(second).end) {
                (*ranges)[first].containedTags.append(ranges->at(second).tag);
            }
        }
    }
    return true;
}

QVector<QString> tagArrayElements(const QStringList& nodes, const QVector<ArrayRange>& ranges) {
    if (ranges.isEmpty())
        return QVector<QString>(1);

    QVector<QVector<QString>> allTags;
    for (const ArrayRange& range : ranges) {
        QVector<bool> visible(nodes.size(), true);
        for (int containedTag : range.containedTags) {
            for (const ArrayRange& candidate : ranges) {
                if (candidate.tag != containedTag)
                    continue;
                for (int index = candidate.start; index <= candidate.end; ++index)
                    visible[index] = false;
            }
        }
        for (int index = 0; index < visible.size(); ++index) {
            if (index < range.start || index > range.end)
                visible[index] = false;
        }

        QVector<QString> tags(nodes.size());
        int element = 0;
        for (int index = 0; index < nodes.size(); ++index) {
            if (!visible.at(index))
                continue;
            if (nodes.at(index) == QLatin1String("&")
                || nodes.at(index) == QLatin1String("\\\\")) {
                ++element;
            } else {
                tags[index] = QString::number(range.tag) + QLatin1Char(':')
                    + QString::number(element);
            }
        }
        allTags.append(tags);
    }

    QVector<QString> tags(nodes.size());
    for (int index = 0; index < nodes.size(); ++index) {
        for (const QVector<QString>& candidate : allTags) {
            if (!candidate.at(index).isEmpty()) {
                tags[index] = candidate.at(index);
                break;
            }
        }
    }
    return tags;
}

QStringList splitWithLeftRight(const QString& input) {
    // split_with_left_right uses LEFT_TOKEN_LIST/RIGHT_TOKEN_LIST rather than
    // VALID_*; left) and left/ therefore remain ordinary text in the oracle.
    QStringList all = countedLeftTokens();
    all.append(validRightTokens());
    return splitKeepingMatches(input, literalAlternation(all));
}

void repairLeftRightSpan(QStringList* fragments) {
    QVector<QString> tokenStack;
    QVector<QPair<int, int>> positionStack;
    QSet<QString> left;
    QSet<QString> right;
    for (const QString& token : validLeftTokens())
        left.insert(token);
    for (const QString& token : validRightTokens())
        right.insert(token);

    for (int fragmentIndex = 0; fragmentIndex < fragments->size(); ++fragmentIndex) {
        QStringList pieces = splitWithLeftRight(fragments->at(fragmentIndex));
        pieces.erase(std::remove_if(pieces.begin(), pieces.end(), [](const QString& piece) {
                         return piece.trimmed().isEmpty();
                     }),
                     pieces.end());
        (*fragments)[fragmentIndex] = pieces.join(QChar(0xffff));
    }

    QVector<QStringList> splitFragments;
    splitFragments.reserve(fragments->size());
    for (const QString& encoded : std::as_const(*fragments))
        splitFragments.append(encoded.split(QChar(0xffff), QString::KeepEmptyParts));

    for (int spanIndex = 0; spanIndex < splitFragments.size(); ++spanIndex) {
        for (int tokenIndex = 0; tokenIndex < splitFragments.at(spanIndex).size(); ++tokenIndex) {
            const QString token = splitFragments.at(spanIndex).at(tokenIndex);
            if (!left.contains(token) && !right.contains(token))
                continue;
            if (tokenStack.isEmpty()) {
                tokenStack.append(token);
                positionStack.append(qMakePair(spanIndex, tokenIndex));
            } else if (isPairLeftRight(tokenStack.last(), token)) {
                tokenStack.removeLast();
                positionStack.removeLast();
            } else {
                tokenStack.append(token);
                positionStack.append(qMakePair(spanIndex, tokenIndex));
            }
        }
    }

    for (int index = 0; index < tokenStack.size(); ++index) {
        const QString token = tokenStack.at(index);
        const QPair<int, int> position = positionStack.at(index);
        QString& piece = splitFragments[position.first][position.second];
        if (token.contains(QStringLiteral("\\left")))
            piece.append(QStringLiteral(" \\right."));
        else if (token.contains(QStringLiteral("\\right")))
            piece.prepend(QStringLiteral("\\left. "));
    }

    for (int index = 0; index < splitFragments.size(); ++index)
        (*fragments)[index] = splitFragments.at(index).join(QString());
}

QString fixEquationLeftRight(const QString& latex) {
    static const QStringList leftCountTokens = countedLeftTokens();
    static const QStringList rightCountTokens = countedRightTokens();
    if (countTokenMatches(latex, leftCountTokens) == countTokenMatches(latex, rightCountTokens))
        return latex;
    if (countOccurrences(latex, QStringLiteral("\\begin{array}"))
        != countOccurrences(latex, QStringLiteral("\\end{array}"))) {
        return latex;
    }

    static const QRegularExpression delimiterPattern = pythonRegex(
        QStringLiteral(R"((&|\\\\|\\begin\{array\}\s*\{[a-zA-Z\s]*\}|\\end\{array\}))"));
    QStringList nodes = splitKeepingMatches(latex.trimmed(), delimiterPattern);
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [](const QString& node) {
                    return node.isEmpty();
                }),
                nodes.end());
    for (QString& node : nodes)
        node = node.trimmed();

    QVector<ArrayRange> ranges;
    if (!tagArrays(nodes, &ranges))
        return latex;
    std::stable_sort(ranges.begin(), ranges.end(), [](const ArrayRange& first, const ArrayRange& second) {
        return first.containedTags.size() > second.containedTags.size();
    });
    const QVector<QString> nodeTags = tagArrayElements(nodes, ranges);

    QVector<int> validIndices;
    QSet<QString> spans;
    // tag_element() intentionally returns [None] for formulas without an array
    // environment.  clean_span() then considers only that first tagged node;
    // aligned and other delimiter-bearing environments retain the remaining
    // nodes verbatim.  Limiting the iteration reproduces that pinned quirk.
    for (int index = 0; index < nodeTags.size() && index < nodes.size(); ++index) {
        const QString& node = nodes.at(index);
        if (node == QLatin1String("&") || node == QLatin1String("\\\\")
            || node.contains(QStringLiteral("\\begin{array}"))
            || node.contains(QStringLiteral("\\end{array}"))) {
            continue;
        }
        validIndices.append(index);
        spans.insert(nodeTags.at(index));
    }

    for (const QString& span : spans) {
        QVector<int> sameSpan;
        QStringList fragments;
        for (int index : validIndices) {
            if (nodeTags.at(index) == span) {
                sameSpan.append(index);
                fragments.append(nodes.at(index));
            }
        }
        int leftCount = 0;
        int rightCount = 0;
        for (const QString& fragment : fragments) {
            leftCount += countTokenMatches(fragment, leftCountTokens);
            rightCount += countTokenMatches(fragment, rightCountTokens);
        }
        if (leftCount == rightCount)
            continue;
        repairLeftRightSpan(&fragments);
        for (int index = 0; index < sameSpan.size(); ++index)
            nodes[sameSpan.at(index)] = fragments.at(index);
    }
    return nodes.join(QString());
}

QString fixUnbalancedBraces(const QString& latex) {
    QVector<int> stack;
    QSet<int> unmatched;
    for (int index = 0; index < latex.size(); ++index) {
        const QChar character = latex.at(index);
        if (character != QLatin1Char('{') && character != QLatin1Char('}'))
            continue;
        int backslashes = 0;
        for (int before = index - 1; before >= 0 && latex.at(before) == QLatin1Char('\\'); --before)
            ++backslashes;
        if ((backslashes % 2) == 1)
            continue;
        if (character == QLatin1Char('{')) {
            stack.append(index);
        } else if (!stack.isEmpty()) {
            stack.removeLast();
        } else {
            unmatched.insert(index);
        }
    }
    for (int index : stack)
        unmatched.insert(index);

    QString output;
    output.reserve(latex.size() - unmatched.size());
    for (int index = 0; index < latex.size(); ++index) {
        if (!unmatched.contains(index))
            output.append(latex.at(index));
    }
    return output;
}

QString convertDisplayToInline(const QString& text) {
    static const QRegularExpression display = pythonRegex(
        QStringLiteral(R"(\\\[(.*?)\\\])"), QRegularExpression::DotMatchesEverythingOption);
    return replaceMatches(text, display, [](const QRegularExpressionMatch& match) {
        const QString inner = match.captured(1);
        static const QRegularExpression numeric = pythonRegex(
            QString::fromUtf8(u8R"(\A[–\d\-,\s]+\z)"));
        if (numeric.match(inner).hasMatch())
            return QStringLiteral("\\[") + inner + QStringLiteral("\\]");
        return QStringLiteral("\\(") + inner + QStringLiteral("\\)");
    });
}

QString fixMacroSpacing(QString inner) {
    static const QStringList targetMacros = {
        QStringLiteral("\\cong"), QStringLiteral("\\to"), QStringLiteral("\\times"),
        QStringLiteral("\\subset"), QStringLiteral("\\in"),
    };
    static const QSet<QString> knownMacros = {
        QStringLiteral("\\top"), QStringLiteral("\\int"), QStringLiteral("\\inf"),
    };
    for (const QString& macro : targetMacros) {
        const QRegularExpression pattern = pythonRegex(QRegularExpression::escape(macro)
                                                       + QStringLiteral("([a-zA-Z])(?![a-zA-Z])"));
        inner = replaceMatches(inner, pattern, [&macro](const QRegularExpressionMatch& match) {
            const QString combined = macro + match.captured(1);
            if (knownMacros.contains(combined))
                return match.captured(0);
            return macro + QLatin1Char(' ') + match.captured(1);
        });
    }
    return inner;
}

QString fixMacroSpacingInMarkdown(const QString& text) {
    static const QRegularExpression inlineFormula = pythonRegex(
        QStringLiteral(R"(\\\(.*?\\\))"), QRegularExpression::DotMatchesEverythingOption);
    return replaceMatches(text, inlineFormula, [](const QRegularExpressionMatch& match) {
        const QString formula = match.captured(0);
        return QStringLiteral("\\(") + fixMacroSpacing(formula.mid(2, formula.size() - 4))
            + QStringLiteral("\\)");
    });
}

QString moveUnderscoresOutside(const QString& text) {
    static const QRegularExpression formula = pythonRegex(
        QStringLiteral(R"(\\\((.+?)\\\))"), QRegularExpression::DotMatchesEverythingOption);
    return replaceMatches(text, formula, [](const QRegularExpressionMatch& match) {
        const QString inner = match.captured(1);
        static const QRegularExpression splitPattern = pythonRegex(QStringLiteral(R"((_{3,}))"));
        if (!splitPattern.match(inner).hasMatch())
            return match.captured(0);
        const QStringList parts = splitKeepingMatches(inner, splitPattern);
        QStringList output;
        static const QRegularExpression exact = pythonRegex(QStringLiteral(R"(\A_{3,}\z)"));
        for (const QString& part : parts) {
            if (exact.match(part).hasMatch())
                output.append(part);
            else if (!part.trimmed().isEmpty())
                output.append(QStringLiteral("\\(") + part + QStringLiteral("\\)"));
        }
        return output.join(QLatin1Char(' '));
    });
}

QString replaceTableFormulaDelimiters(const QString& content, bool enabled) {
    if (!enabled || content.isEmpty())
        return content;

    static const QRegularExpression existingEq = pythonRegex(
        QStringLiteral(R"((<eq>.*?</eq>))"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression inlineFormula = pythonRegex(
        QStringLiteral(R"(\\\((.+?)\\\))"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression blockFormula = pythonRegex(
        QStringLiteral(R"(\\\[(.+?)\\\])"), QRegularExpression::DotMatchesEverythingOption);
    auto wrap = [](const QString& text, const QRegularExpression& expression) {
        return replaceMatches(text, expression, [](const QRegularExpressionMatch& match) {
            return QStringLiteral("<eq>") + match.captured(1).trimmed() + QStringLiteral("</eq>");
        });
    };

    QString output;
    int previousEnd = 0;
    QRegularExpressionMatchIterator iterator = existingEq.globalMatch(content);
    while (iterator.hasNext()) {
        const QRegularExpressionMatch match = iterator.next();
        QString gap = content.mid(previousEnd, match.capturedStart() - previousEnd);
        gap = wrap(gap, inlineFormula);
        gap = wrap(gap, blockFormula);
        output.append(gap);
        output.append(match.captured(0));
        previousEnd = match.capturedEnd();
    }
    QString tail = content.mid(previousEnd);
    tail = wrap(tail, inlineFormula);
    tail = wrap(tail, blockFormula);
    output.append(tail);
    return output;
}

QString defaultReplaceTableImageTokens(QString content, const QJsonObject& tokenMap) {
    if (content.isEmpty() || tokenMap.isEmpty())
        return content;
    for (auto iterator = tokenMap.constBegin(); iterator != tokenMap.constEnd(); ++iterator) {
        if (!iterator.value().isString())
            continue;
        const QString token = iterator.key();
        if (token.size() < 2)
            continue;
        const QString inner = token.mid(1, token.size() - 2);
        const QRegularExpression pattern = pythonRegex(QStringLiteral(R"(\[\s*)")
                                                       + QRegularExpression::escape(inner)
                                                       + QStringLiteral(R"(\s*\])"));
        content.replace(pattern,
                        QStringLiteral("<img src=\"") + iterator.value().toString()
                            + QStringLiteral("\"/>"));
    }
    return content;
}

QString htmlEscape(const QString& text) {
    QString escaped;
    escaped.reserve(text.size());
    for (const QChar character : text) {
        if (character == QLatin1Char('&'))
            escaped.append(QStringLiteral("&amp;"));
        else if (character == QLatin1Char('<'))
            escaped.append(QStringLiteral("&lt;"));
        else if (character == QLatin1Char('>'))
            escaped.append(QStringLiteral("&gt;"));
        else if (character == QLatin1Char('"'))
            escaped.append(QStringLiteral("&quot;"));
        else if (character == QLatin1Char('\''))
            escaped.append(QStringLiteral("&#x27;"));
        else
            escaped.append(character);
    }
    return escaped;
}

int markdownColumnCount(QString line) {
    line = line.trimmed();
    if (line.startsWith(QLatin1Char('|')))
        line.remove(0, 1);
    if (line.endsWith(QLatin1Char('|')))
        line.chop(1);
    return line.count(QLatin1Char('|')) + 1;
}

bool isMarkdownSeparator(const QString& line) {
    static const QRegularExpression separator = pythonRegex(
        QStringLiteral(R"(^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)+\|?\s*$)"));
    return separator.match(line).hasMatch();
}

bool isMarkdownRow(const QString& line) {
    const QString stripped = line.trimmed();
    return !stripped.isEmpty() && stripped.contains(QLatin1Char('|'))
        && markdownColumnCount(stripped) >= 2;
}

QStringList splitMarkdownRow(QString line) {
    line = line.trimmed();
    if (line.startsWith(QLatin1Char('|')))
        line.remove(0, 1);
    if (line.endsWith(QLatin1Char('|')))
        line.chop(1);

    QStringList cells;
    QString current;
    bool escaped = false;
    for (const QChar character : line) {
        if (escaped) {
            if (character == QLatin1Char('|'))
                current.append(character);
            else {
                current.append(QLatin1Char('\\'));
                current.append(character);
            }
            escaped = false;
        } else if (character == QLatin1Char('\\')) {
            escaped = true;
        } else if (character == QLatin1Char('|')) {
            cells.append(current.trimmed());
            current.clear();
        } else {
            current.append(character);
        }
    }
    if (escaped)
        current.append(QLatin1Char('\\'));
    cells.append(current.trimmed());
    return cells;
}

bool markdownTableToHtml(const QString& content, QString* html) {
    if (!html || content.trimmed().isEmpty())
        return false;
    QStringList lines = content.trimmed().split(QLatin1Char('\n'));
    for (QString& line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        line = line.trimmed();
        if (line.isEmpty())
            return false;
    }
    if (lines.size() < 2 || !isMarkdownRow(lines.at(0)) || !isMarkdownSeparator(lines.at(1)))
        return false;
    const QStringList headerCells = splitMarkdownRow(lines.at(0));
    const QStringList separatorCells = splitMarkdownRow(lines.at(1));
    if (headerCells.size() < 2 || separatorCells.size() != headerCells.size())
        return false;

    QVector<QStringList> rows;
    for (int index = 2; index < lines.size(); ++index) {
        if (!isMarkdownRow(lines.at(index)))
            return false;
        const QStringList row = splitMarkdownRow(lines.at(index));
        if (row.size() != headerCells.size())
            return false;
        rows.append(row);
    }

    QString result = QStringLiteral("<table><tr>");
    for (const QString& cell : headerCells)
        result += QStringLiteral("<th>") + htmlEscape(cell) + QStringLiteral("</th>");
    result += QStringLiteral("</tr>");
    for (const QStringList& row : rows) {
        result += QStringLiteral("<tr>");
        for (const QString& cell : row)
            result += QStringLiteral("<td>") + htmlEscape(cell) + QStringLiteral("</td>");
        result += QStringLiteral("</tr>");
    }
    result += QStringLiteral("</table>");
    *html = result;
    return true;
}

bool splitChartMarkdownRow(QString line, QStringList* cells) {
    if (!cells)
        return false;
    line = line.trimmed();
    if (line.isEmpty() || !line.contains(QLatin1Char('|')))
        return false;
    if (line.startsWith(QLatin1Char('|')))
        line.remove(0, 1);
    if (line.endsWith(QLatin1Char('|')))
        line.chop(1);

    QStringList output;
    QString buffer;
    bool escaped = false;
    bool inCode = false;
    for (const QChar character : line) {
        if (escaped) {
            buffer.append(character);
            escaped = false;
            continue;
        }
        if (character == QLatin1Char('\\')) {
            escaped = true;
            buffer.append(character);
            continue;
        }
        if (character == QLatin1Char('`')) {
            inCode = !inCode;
            buffer.append(character);
            continue;
        }
        if (character == QLatin1Char('|') && !inCode) {
            output.append(buffer.trimmed());
            buffer.clear();
            continue;
        }
        buffer.append(character);
    }
    output.append(buffer.trimmed());
    *cells = output;
    return true;
}

bool isChartSeparatorRow(const QStringList& cells) {
    if (cells.isEmpty())
        return false;
    static const QRegularExpression separator = pythonRegex(
        QStringLiteral(R"(\A:?-{3,}:?\z)"));
    for (QString cell : cells) {
        cell.remove(QLatin1Char(' '));
        if (!separator.match(cell).hasMatch())
            return false;
    }
    return true;
}

bool isChartMarkdownSeparatorLine(const QString& line) {
    QStringList cells;
    return splitChartMarkdownRow(line, &cells) && isChartSeparatorRow(cells);
}

QString normalizeChartFragment(QString fragment) {
    static const QRegularExpression spaces = pythonRegex(QStringLiteral(R"(\s+)"));
    fragment.replace(spaces, QStringLiteral(" "));
    return fragment.trimmed().toLower();
}

bool chartCellHasRepetitionIssue(const QString& cellText) {
    if (cellText.size() > 300 || cellText.count(QStringLiteral("::")) > 20
        || cellText.count(QLatin1Char(';')) > 30) {
        return true;
    }
    static const QRegularExpression fragments = pythonRegex(
        QStringLiteral(R"(\s*(?:::|;|,|\|)\s*)"));
    QHash<QString, int> counts;
    for (const QString& fragment : cellText.split(fragments)) {
        const QString normalized = normalizeChartFragment(fragment);
        if (normalized.size() < 3)
            continue;
        if (++counts[normalized] >= 8)
            return true;
    }
    return false;
}

bool noisyChartTableRow(const QStringList& cells) {
    if (isChartSeparatorRow(cells))
        return false;
    for (const QString& cell : cells) {
        const QString text = cell.trimmed();
        if (!text.isEmpty() && chartCellHasRepetitionIssue(text))
            return true;
    }
    return false;
}

bool chartTableCellRepetition(const QString& content) {
    bool inTableBlock = false;
    bool separatorSeen = false;
    for (const QString& line : pythonSplitLines(content)) {
        QStringList row;
        if (!splitChartMarkdownRow(line, &row)) {
            inTableBlock = false;
            separatorSeen = false;
            continue;
        }
        if (!inTableBlock) {
            inTableBlock = true;
            separatorSeen = false;
            continue;
        }
        if (!separatorSeen) {
            if (isChartSeparatorRow(row))
                separatorSeen = true;
            continue;
        }
        for (const QString& cell : row) {
            const QString text = cell.trimmed();
            if (!text.isEmpty() && chartCellHasRepetitionIssue(text))
                return true;
        }
    }
    return false;
}

bool chartHasRepetitionIssue(const QString& content) {
    if (content.contains(QStringLiteral("\\|")))
        return true;
    static const QRegularExpression excessiveNewlines = pythonRegex(QStringLiteral(R"(\n{5,})"));
    if (excessiveNewlines.match(content).hasMatch())
        return true;

    static const QRegularExpression spaces = pythonRegex(QStringLiteral(R"(\s+)"));
    QString compact = content;
    compact.replace(spaces, QStringLiteral(" "));
    compact = compact.trimmed();
    if (compact.size() >= 80) {
        const int period = (compact + compact).indexOf(compact, 1);
        if (period > 0 && period < compact.size() && compact.size() % period == 0
            && compact.size() / period >= 2) {
            return true;
        }
    }

    static const QRegularExpression repeatedCharacter = pythonRegex(
        QStringLiteral(R"(([A-Za-z0-9])\1{19,})"));
    if (repeatedCharacter.match(content).hasMatch() || chartTableCellRepetition(content))
        return true;

    QString trimmedNewlines = content;
    while (trimmedNewlines.startsWith(QLatin1Char('\n')))
        trimmedNewlines.remove(0, 1);
    while (trimmedNewlines.endsWith(QLatin1Char('\n')))
        trimmedNewlines.chop(1);
    QStringList lines = pythonSplitLines(trimmedNewlines);
    for (QString& line : lines)
        line = line.trimmed();
    for (int index = 0; index < lines.size();) {
        int next = index + 1;
        while (next < lines.size() && lines.at(next) == lines.at(index))
            ++next;
        const int count = next - index;
        if ((count >= 2 && lines.at(index).size() >= 30) || count >= 3)
            return true;
        index = next;
    }

    QHash<QString, int> lineCounts;
    for (const QString& line : lines) {
        if (line.size() >= 30 && !isChartMarkdownSeparatorLine(line)
            && ++lineCounts[line] >= 4) {
            return true;
        }
    }

    QStringList nonSeparatorLines;
    for (const QString& line : pythonSplitLines(content)) {
        if (!isChartMarkdownSeparatorLine(line))
            nonSeparatorLines.append(line);
    }
    const QString tokenText = nonSeparatorLines.join(QLatin1Char('\n'));
    static const QRegularExpression token = pythonRegex(
        QStringLiteral(R"([A-Za-z0-9]+(?:[-_./][A-Za-z0-9]+)*|[^\s])"));
    QStringList tokens;
    QRegularExpressionMatchIterator tokenMatches = token.globalMatch(tokenText);
    while (tokenMatches.hasNext())
        tokens.append(tokenMatches.next().captured(0));
    for (int index = 0; index < tokens.size();) {
        int next = index + 1;
        const QString lowered = tokens.at(index).toLower();
        while (next < tokens.size() && tokens.at(next).toLower() == lowered)
            ++next;
        if (next - index >= 8)
            return true;
        index = next;
    }
    return false;
}

QString normalizeChartTableSourceLine(QString line) {
    line = line.trimmed();
    line.replace(QChar(0xff5c), QLatin1Char('|'));
    line.replace(QChar(0x00a6), QLatin1Char('|'));
    line.replace(QStringLiteral("\\|"), QStringLiteral("|"));
    return line;
}

bool splitRepairedChartRow(const QString& line, QStringList* cells) {
    const QString normalized = normalizeChartTableSourceLine(line);
    if (!normalized.contains(QLatin1Char('|')) || !splitChartMarkdownRow(normalized, cells))
        return false;
    return cells->size() >= 2;
}

QString cleanChartCell(QString cell) {
    cell.replace(QLatin1Char('\t'), QLatin1Char(' '));
    static const QRegularExpression spaces = pythonRegex(QStringLiteral(R"(\s+)"));
    cell.replace(spaces, QStringLiteral(" "));
    cell = cell.trimmed();
    if ((cell.count(QLatin1Char('`')) % 2) == 1)
        cell.append(QLatin1Char('`'));
    cell.replace(QStringLiteral("|"), QStringLiteral("\\|"));
    return cell;
}

QString formatChartMarkdownRow(const QStringList& cells) {
    QStringList cleaned;
    for (const QString& cell : cells)
        cleaned.append(cleanChartCell(cell));
    return QStringLiteral("| ") + cleaned.join(QStringLiteral(" | ")) + QStringLiteral(" |");
}

QString formatChartMarkdownSeparator(int columns) {
    QStringList cells;
    for (int index = 0; index < columns; ++index)
        cells.append(QStringLiteral("---"));
    return QStringLiteral("| ") + cells.join(QStringLiteral(" | ")) + QStringLiteral(" |");
}

using ChartRows = QVector<QStringList>;
using ChartBlocks = QVector<ChartRows>;

ChartBlocks parsedChartTableBlocks(const QString& content) {
    ChartBlocks blocks;
    ChartRows current;
    bool separatorSeen = false;
    for (const QString& line : pythonSplitLines(content)) {
        QStringList cells;
        if (!splitRepairedChartRow(line, &cells)) {
            if (!current.isEmpty()) {
                blocks.append(current);
                current.clear();
            }
            separatorSeen = false;
            continue;
        }
        if (current.isEmpty()) {
            current.append(cells);
            separatorSeen = false;
            continue;
        }
        if (!separatorSeen) {
            current.append(cells);
            if (isChartSeparatorRow(cells))
                separatorSeen = true;
            continue;
        }
        if (!noisyChartTableRow(cells))
            current.append(cells);
    }
    if (!current.isEmpty())
        blocks.append(current);
    return blocks;
}

QString buildChartTableCandidate(const ChartRows& rows, int separatorIndex) {
    const int headerIndex = separatorIndex - 1;
    if (headerIndex < 0)
        return QString();
    const QStringList header = rows.at(headerIndex);
    if (header.size() < 2 || isChartSeparatorRow(header))
        return QString();
    const int expectedColumns = header.size();
    ChartRows dataRows;
    for (int index = separatorIndex + 1; index < rows.size(); ++index) {
        const QStringList row = rows.at(index);
        if (isChartSeparatorRow(row) || row.size() != expectedColumns)
            break;
        dataRows.append(row);
    }
    if (dataRows.isEmpty())
        return QString();
    QStringList fixed{formatChartMarkdownRow(header),
                      formatChartMarkdownSeparator(expectedColumns)};
    for (const QStringList& row : dataRows)
        fixed.append(formatChartMarkdownRow(row));
    return fixed.join(QLatin1Char('\n'));
}

bool chartTableHasPipeBoundaries(const QString& line) {
    const QString stripped = line.trimmed();
    return stripped.startsWith(QLatin1Char('|')) && stripped.endsWith(QLatin1Char('|'));
}

bool validateRepairedChartTable(const QString& table) {
    const QStringList lines = pythonSplitLines(table.trimmed());
    if (lines.size() < 3)
        return false;
    QStringList header;
    QStringList separator;
    if (!splitChartMarkdownRow(lines.at(0), &header)
        || !splitChartMarkdownRow(lines.at(1), &separator)
        || !isChartSeparatorRow(separator) || !chartTableHasPipeBoundaries(lines.at(0))
        || !chartTableHasPipeBoundaries(lines.at(1)) || header.size() != separator.size()) {
        return false;
    }
    for (int index = 2; index < lines.size(); ++index) {
        QStringList row;
        if (!splitChartMarkdownRow(lines.at(index), &row)
            || !chartTableHasPipeBoundaries(lines.at(index)) || row.size() != header.size()) {
            return false;
        }
    }
    return true;
}

struct ChartTableScore {
    int rows = 0;
    int columns = 0;
    int length = 0;
};

ChartTableScore scoreChartTable(const QString& table) {
    const QStringList lines = pythonSplitLines(table);
    QStringList firstRow;
    if (!lines.isEmpty())
        splitChartMarkdownRow(lines.first(), &firstRow);
    return {lines.size(), firstRow.size(), table.size()};
}

bool greaterChartScore(const ChartTableScore& first, const ChartTableScore& second) {
    if (first.rows != second.rows)
        return first.rows > second.rows;
    if (first.columns != second.columns)
        return first.columns > second.columns;
    return first.length > second.length;
}

QString extractLargestLegalChartTable(const QString& content) {
    QStringList candidates;
    for (const ChartRows& rows : parsedChartTableBlocks(content)) {
        for (int index = 0; index < rows.size(); ++index) {
            if (!isChartSeparatorRow(rows.at(index)))
                continue;
            const QString candidate = buildChartTableCandidate(rows, index);
            if (!candidate.isEmpty() && validateRepairedChartTable(candidate))
                candidates.append(candidate);
        }

        for (int index = 0; index < rows.size();) {
            if (isChartSeparatorRow(rows.at(index)) || rows.at(index).size() < 2) {
                ++index;
                continue;
            }
            const int expectedColumns = rows.at(index).size();
            ChartRows run{rows.at(index)};
            int next = index + 1;
            while (next < rows.size() && !isChartSeparatorRow(rows.at(next))
                   && rows.at(next).size() == expectedColumns) {
                run.append(rows.at(next));
                ++next;
            }
            if (run.size() >= 2) {
                QStringList fixed{formatChartMarkdownRow(run.first()),
                                  formatChartMarkdownSeparator(expectedColumns)};
                for (int row = 1; row < run.size(); ++row)
                    fixed.append(formatChartMarkdownRow(run.at(row)));
                const QString candidate = fixed.join(QLatin1Char('\n'));
                if (validateRepairedChartTable(candidate))
                    candidates.append(candidate);
            }
            index = std::max(next, index + 1);
        }
    }
    if (candidates.isEmpty())
        return QString();
    QString best = candidates.first();
    ChartTableScore bestScore = scoreChartTable(best);
    for (int index = 1; index < candidates.size(); ++index) {
        const ChartTableScore candidateScore = scoreChartTable(candidates.at(index));
        if (greaterChartScore(candidateScore, bestScore)) {
            best = candidates.at(index);
            bestScore = candidateScore;
        }
    }
    return best;
}

bool malformedChartMarkdownTable(const QString& content) {
    const QStringList lines = pythonSplitLines(content);
    QVector<int> separators;
    QVector<int> rowCandidates;
    for (int index = 0; index < lines.size(); ++index) {
        if (isMarkdownSeparator(lines.at(index)))
            separators.append(index);
        if (isMarkdownRow(lines.at(index)))
            rowCandidates.append(index);
    }
    bool foundValid = false;
    for (int separatorIndex : separators) {
        int headerIndex = separatorIndex - 1;
        while (headerIndex >= 0 && lines.at(headerIndex).trimmed().isEmpty())
            --headerIndex;
        if (headerIndex < 0 || !isMarkdownRow(lines.at(headerIndex)))
            return true;
        const int headerColumns = markdownColumnCount(lines.at(headerIndex));
        if (headerColumns < 2 || markdownColumnCount(lines.at(separatorIndex)) != headerColumns)
            return true;
        for (int row = separatorIndex + 1; row < lines.size(); ++row) {
            if (lines.at(row).trimmed().isEmpty() || !isMarkdownRow(lines.at(row)))
                break;
            if (markdownColumnCount(lines.at(row)) != headerColumns)
                return true;
        }
        foundValid = true;
    }
    return rowCandidates.size() >= 2 && !foundValid;
}

QString normalizeChart(const QString& content) {
    QString fixed = content;
    if (chartHasRepetitionIssue(content)) {
        const QString extracted = extractLargestLegalChartTable(content);
        if (!extracted.isEmpty())
            fixed = extracted;
    }
    if (!fixed.isEmpty() && malformedChartMarkdownTable(fixed))
        fixed.clear();
    return fixed;
}

QString taggedField(const QString& text, const QString& startTag, const QString& endTag) {
    int start = text.indexOf(startTag);
    if (start < 0)
        return QString();
    start += startTag.size();
    const int end = text.indexOf(endTag, start);
    if (end < 0)
        return QString();
    return text.mid(start, end - start).trimmed();
}

QString normalizeEscapedMermaidNewlines(QString text) {
    static const QRegularExpression escaped = pythonRegex(QStringLiteral(R"(\\+n)"));
    static const QRegularExpression br = pythonRegex(
        QStringLiteral(R"(<br\s*/?>)"), QRegularExpression::CaseInsensitiveOption);
    text.replace(escaped, QStringLiteral("\n"));
    text.replace(br, QStringLiteral("\n"));
    return text;
}

bool mermaidNodeIdHasRepetitionIssue(const QString& nodeId,
                                     int minTokenRepeats = 8,
                                     int maxNodeIdLength = 128) {
    if (nodeId.isEmpty())
        return false;
    if (nodeId.size() > maxNodeIdLength)
        return true;

    QHash<QString, int> counts;
    static const QRegularExpression pairs = pythonRegex(QStringLiteral(R"([A-Z][a-z]|[A-Z]{2})"));
    QRegularExpressionMatchIterator pairMatches = pairs.globalMatch(nodeId);
    while (pairMatches.hasNext()) {
        const QString token = pairMatches.next().captured(0);
        if (++counts[token] >= minTokenRepeats)
            return true;
    }
    if (!counts.isEmpty())
        return false;

    static const QRegularExpression chunks = pythonRegex(QStringLiteral(R"([A-Za-z]{8,})"));
    QRegularExpressionMatchIterator chunkMatches = chunks.globalMatch(nodeId);
    while (chunkMatches.hasNext()) {
        const QString chunk = chunkMatches.next().captured(0);
        for (int index = 0; index + 1 < chunk.size(); ++index) {
            if (++counts[chunk.mid(index, 2)] >= minTokenRepeats)
                return true;
        }
    }
    return false;
}

QPair<int, QString> maxConsecutiveLineRepeat(const QStringList& lines) {
    if (lines.isEmpty())
        return qMakePair(0, QString());
    int bestCount = 1;
    QString bestLine = lines.first();
    int currentCount = 1;
    QString currentLine = lines.first();
    for (int index = 1; index < lines.size(); ++index) {
        if (lines.at(index) == currentLine) {
            ++currentCount;
        } else {
            if (currentCount > bestCount) {
                bestCount = currentCount;
                bestLine = currentLine;
            }
            currentLine = lines.at(index);
            currentCount = 1;
        }
    }
    if (currentCount > bestCount) {
        bestCount = currentCount;
        bestLine = currentLine;
    }
    return qMakePair(bestCount, bestLine);
}

bool isMermaidStructureLine(const QString& line) {
    const QString stripped = line.trimmed();
    if (stripped.isEmpty())
        return true;
    const QString lowered = stripped.toLower();
    return lowered.startsWith(QStringLiteral("graph "))
        || lowered.startsWith(QStringLiteral("flowchart "))
        || lowered.startsWith(QStringLiteral("subgraph ")) || lowered == QLatin1String("end")
        || lowered.startsWith(QStringLiteral("direction "))
        || lowered.startsWith(QStringLiteral("%%")) || stripped.contains(QStringLiteral("-->"));
}

QString normalizeMermaidFreeTextLine(QString line) {
    line = line.trimmed();
    static const QRegularExpression leading = pythonRegex(
        QStringLiteral(R"rx(^[\[\]\(\)"'\s>]+)rx"));
    static const QRegularExpression trailing = pythonRegex(
        QStringLiteral(R"rx([\[\]\(\)"'\s,.;:]+$)rx"));
    static const QRegularExpression spaces = pythonRegex(QStringLiteral(R"(\s+)"));
    line.remove(leading);
    line.remove(trailing);
    line.replace(spaces, QStringLiteral(" "));
    return line.trimmed();
}

QString mermaidEdgeEndpointId(QString segment) {
    segment = segment.trimmed();
    if (segment.isEmpty())
        return QString();
    static const QRegularExpression label = pythonRegex(QStringLiteral(R"(^\|[^|\n]*\|\s*)"));
    segment.remove(label);
    static const QRegularExpression endpoint = pythonRegex(
        QString::fromUtf8(u8R"(^([A-Za-z0-9_\x{4e00}-\x{9fa5}\-]+))"));
    const QRegularExpressionMatch match = endpoint.match(segment);
    return match.hasMatch() ? match.captured(1) : QString();
}

bool detectMermaidRepetitionIssues(const QString& content) {
    static const QRegularExpression nodeLabel = pythonRegex(
        QString::fromUtf8(
            u8R"rx((?P<node>[A-Za-z0-9_\x{4e00}-\x{9fa5}\-]+)\s*\["(?P<label>(?:[^"\\]|\\.)*)"\])rx"),
        QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatchIterator labels = nodeLabel.globalMatch(content);
    while (labels.hasNext()) {
        const QRegularExpressionMatch match = labels.next();
        if (mermaidNodeIdHasRepetitionIssue(match.captured(QStringLiteral("node"))))
            return true;
        const QString label = normalizeEscapedMermaidNewlines(
            match.captured(QStringLiteral("label")));
        QStringList lines;
        for (const QString& line : pythonSplitLines(label)) {
            if (!line.trimmed().isEmpty())
                lines.append(line.trimmed());
        }
        if (lines.isEmpty())
            continue;
        if (maxConsecutiveLineRepeat(lines).first >= 8)
            return true;
        QHash<QString, int> frequencies;
        for (const QString& line : lines) {
            if (++frequencies[line] >= 20)
                return true;
        }
        QSet<QString> unique;
        for (const QString& line : lines)
            unique.insert(line);
        if (label.size() >= 2000
            && static_cast<double>(unique.size()) / std::max(lines.size(), 1) <= 0.2) {
            return true;
        }
    }

    static const QRegularExpression edgeLine = pythonRegex(
        QString::fromUtf8(
            u8R"(^\s*(?P<src>[A-Za-z0-9_\x{4e00}-\x{9fa5}\-]+)\s*-->\s*(?:\|[^|\n]*\|\s*)?(?P<dst>[A-Za-z0-9_\x{4e00}-\x{9fa5}\-]+)(?:\s*(?:\[[^\]]*\]|\([^\)]*\)|\{[^\}]*\}))?\s*$)"));
    const QString normalized = normalizeEscapedMermaidNewlines(content);
    QHash<QString, int> edgeCounts;
    for (const QString& line : pythonSplitLines(normalized)) {
        const QRegularExpressionMatch match = edgeLine.match(line);
        if (!match.hasMatch())
            continue;
        const QString key = match.captured(QStringLiteral("src")) + QStringLiteral("-->")
            + match.captured(QStringLiteral("dst"));
        if (++edgeCounts[key] >= 8)
            return true;
    }

    for (const QString& line : pythonSplitLines(normalized)) {
        const QString stripped = line.trimmed();
        if (!stripped.contains(QStringLiteral("-->")))
            continue;
        if (stripped.count(QStringLiteral("-->")) >= 2) {
            for (const QString& segment : stripped.split(QStringLiteral("-->"))) {
                if (mermaidNodeIdHasRepetitionIssue(mermaidEdgeEndpointId(segment)))
                    return true;
            }
            continue;
        }
        const QRegularExpressionMatch match = edgeLine.match(line);
        if (match.hasMatch()
            && (mermaidNodeIdHasRepetitionIssue(match.captured(QStringLiteral("src")))
                || mermaidNodeIdHasRepetitionIssue(match.captured(QStringLiteral("dst"))))) {
            return true;
        }
    }

    static const QRegularExpression labeledEdge = pythonRegex(
        QString::fromUtf8(
            u8R"rx(^\s*(?P<src>[A-Za-z0-9_\x{4e00}-\x{9fa5}\-]+)\s*-->\s*(?P<dst>[A-Za-z0-9_\x{4e00}-\x{9fa5}\-]+)\s*\["(?P<label>(?:[^"\\]|\\\\.)*)"\]\s*$)rx"));
    QHash<QString, int> labelCounts;
    QString consecutiveLabel;
    int consecutiveCount = 0;
    for (const QString& line : pythonSplitLines(normalized)) {
        const QRegularExpressionMatch match = labeledEdge.match(line);
        if (!match.hasMatch()) {
            consecutiveLabel.clear();
            consecutiveCount = 0;
            continue;
        }
        const QString label = match.captured(QStringLiteral("label")).trimmed();
        if (label.isEmpty())
            continue;
        if (++labelCounts[label] >= 8)
            return true;
        if (label == consecutiveLabel)
            ++consecutiveCount;
        else {
            consecutiveLabel = label;
            consecutiveCount = 1;
        }
        if (consecutiveCount >= 8)
            return true;
    }

    QStringList freeLines;
    for (const QString& line : pythonSplitLines(normalized)) {
        if (isMermaidStructureLine(line))
            continue;
        const QString cleaned = normalizeMermaidFreeTextLine(line);
        if (!cleaned.isEmpty())
            freeLines.append(cleaned);
    }
    if (maxConsecutiveLineRepeat(freeLines).first >= 8)
        return true;
    QHash<QString, int> freeCounts;
    for (const QString& line : freeLines) {
        if (++freeCounts[line] >= 20)
            return true;
    }
    return false;
}

QString sanitizeMermaidNodeId(const QString& rawId) {
    static const QRegularExpression partPattern = pythonRegex(QStringLiteral(R"([A-Za-z0-9]+)"));
    QStringList parts;
    QRegularExpressionMatchIterator iterator = partPattern.globalMatch(rawId);
    while (iterator.hasNext())
        parts.append(iterator.next().captured(0));

    QString candidate;
    if (!parts.isEmpty()) {
        candidate = parts.first();
        for (int index = 1; index < parts.size(); ++index) {
            const QString part = parts.at(index);
            candidate += part.left(1).toUpper() + part.mid(1).toLower();
        }
    } else {
        candidate = rawId;
        static const QRegularExpression invalid = pythonRegex(QStringLiteral(R"([^A-Za-z0-9_])"));
        candidate.remove(invalid);
    }
    if (candidate.isEmpty())
        candidate = QStringLiteral("node");
    if (candidate.at(0).isDigit())
        candidate.prepend(QLatin1Char('n'));
    return candidate;
}

bool validMermaidNodeId(const QString& value) {
    static const QRegularExpression valid = pythonRegex(QStringLiteral(R"(\A[A-Za-z_][A-Za-z0-9_]*\z)"));
    return valid.match(value).hasMatch();
}

bool plainMermaidEndpoint(const QString& value) {
    if (value.isEmpty())
        return false;
    for (const QChar character : value) {
        if (QStringLiteral("[](){}\"|").contains(character))
            return false;
    }
    return true;
}

QString convertMermaidEndpoint(const QString& token,
                               QHash<QString, QString>* idMap,
                               QSet<QString>* declaredIds) {
    const QString raw = token.trimmed();
    if (!plainMermaidEndpoint(raw) || validMermaidNodeId(raw))
        return raw;

    QString fixed = idMap->value(raw);
    if (fixed.isEmpty()) {
        fixed = sanitizeMermaidNodeId(raw);
        const QString base = fixed;
        int suffix = 2;
        while (declaredIds->contains(fixed) || idMap->values().contains(fixed))
            fixed = base + QString::number(suffix++);
        idMap->insert(raw, fixed);
    }

    if (!declaredIds->contains(fixed)) {
        declaredIds->insert(fixed);
        return fixed + QStringLiteral("[\"") + raw + QStringLiteral("\"]");
    }
    return fixed;
}

struct MermaidStatements {
    QStringList values;
    bool trailingSemicolon = false;
};

MermaidStatements splitMermaidStatements(const QString& line) {
    MermaidStatements output;
    int start = 0;
    bool inQuote = false;
    bool escaped = false;
    int bracketDepth = 0;
    bool inPipeLabel = false;
    for (int index = 0; index < line.size(); ++index) {
        const QChar character = line.at(index);
        if (escaped) {
            escaped = false;
            continue;
        }
        if (inQuote) {
            if (character == QLatin1Char('\\'))
                escaped = true;
            else if (character == QLatin1Char('"'))
                inQuote = false;
            continue;
        }
        if (character == QLatin1Char('"')) {
            inQuote = true;
            continue;
        }
        if (QStringLiteral("[({").contains(character)) {
            ++bracketDepth;
            continue;
        }
        if (QStringLiteral("])}").contains(character)) {
            if (bracketDepth > 0)
                --bracketDepth;
            continue;
        }
        if (character == QLatin1Char('|') && bracketDepth == 0) {
            inPipeLabel = !inPipeLabel;
            continue;
        }
        if (character == QLatin1Char(';') && bracketDepth == 0 && !inPipeLabel) {
            output.values.append(line.mid(start, index - start));
            start = index + 1;
        }
    }
    output.values.append(line.mid(start));
    output.trailingSemicolon = output.values.size() > 1
        && output.values.last().trimmed().isEmpty();
    if (output.trailingSemicolon)
        output.values.removeLast();
    return output;
}

QStringList splitMermaidEdgeChain(const QString& statement) {
    QStringList parts;
    int start = 0;
    bool inQuote = false;
    bool escaped = false;
    int bracketDepth = 0;
    bool inPipeLabel = false;
    for (int index = 0; index < statement.size();) {
        if (escaped) {
            escaped = false;
            ++index;
            continue;
        }
        const QChar character = statement.at(index);
        if (inQuote) {
            if (character == QLatin1Char('\\'))
                escaped = true;
            else if (character == QLatin1Char('"'))
                inQuote = false;
            ++index;
            continue;
        }
        if (character == QLatin1Char('"')) {
            inQuote = true;
            ++index;
            continue;
        }
        if (QStringLiteral("[({").contains(character)) {
            ++bracketDepth;
            ++index;
            continue;
        }
        if (QStringLiteral("])}").contains(character)) {
            if (bracketDepth > 0)
                --bracketDepth;
            ++index;
            continue;
        }
        if (character == QLatin1Char('|') && bracketDepth == 0) {
            inPipeLabel = !inPipeLabel;
            ++index;
            continue;
        }
        if (statement.midRef(index, 3) == QLatin1String("-->") && bracketDepth == 0
            && !inPipeLabel) {
            parts.append(statement.mid(start, index - start));
            start = index + 3;
            index += 3;
            continue;
        }
        ++index;
    }
    parts.append(statement.mid(start));
    return parts;
}

QString sanitizeMermaidEdgeLabel(QString label) {
    label.remove(QLatin1Char('('));
    label.remove(QLatin1Char(')'));
    static const QRegularExpression spaces = pythonRegex(QStringLiteral(R"( {2,})"));
    label.replace(spaces, QStringLiteral(" "));
    return label.trimmed();
}

QString fixMermaidEdgeTarget(const QString& segment,
                             QHash<QString, QString>* idMap,
                             QSet<QString>* declaredIds) {
    const QString stripped = segment.trimmed();
    static const QRegularExpression labeled = pythonRegex(
        QStringLiteral(R"(^\|(?P<label>.*?)\|\s*(?P<dst>.+?)\s*$)"));
    const QRegularExpressionMatch match = labeled.match(stripped);
    if (match.hasMatch()) {
        return QLatin1Char('|')
            + sanitizeMermaidEdgeLabel(match.captured(QStringLiteral("label")))
            + QStringLiteral("| ")
            + convertMermaidEndpoint(match.captured(QStringLiteral("dst")), idMap, declaredIds);
    }
    return convertMermaidEndpoint(stripped, idMap, declaredIds);
}

QString fixMermaidEdgeStatement(const QString& statement,
                                 QHash<QString, QString>* idMap,
                                 QSet<QString>* declaredIds) {
    const QString stripped = statement.trimmed();
    if (!stripped.contains(QStringLiteral("-->")) || stripped.startsWith(QStringLiteral("%%")))
        return stripped;
    const QStringList chain = splitMermaidEdgeChain(stripped);
    if (chain.size() < 2)
        return stripped;
    QString fixed = convertMermaidEndpoint(chain.first().trimmed(), idMap, declaredIds);
    for (int index = 1; index < chain.size(); ++index) {
        const QString target = fixMermaidEdgeTarget(chain.at(index), idMap, declaredIds);
        fixed += target.startsWith(QLatin1Char('|')) ? QStringLiteral(" -->") + target
                                                    : QStringLiteral(" --> ") + target;
    }
    return fixed;
}

QString fixMermaidNodeIdLegalization(const QString& content) {
    const QStringList lines = pythonSplitLines(content);
    QSet<QString> declaredIds;
    static const QRegularExpression declaration = pythonRegex(
        QStringLiteral(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*[\[\(\{])"));
    for (const QString& line : lines) {
        const QRegularExpressionMatch match = declaration.match(line.trimmed());
        if (match.hasMatch())
            declaredIds.insert(match.captured(1));
    }

    static const QRegularExpression bidirectional = pythonRegex(QStringLiteral(R"(<\s*-\s*-\s*>)"));
    QHash<QString, QString> idMap;
    QStringList output;
    for (const QString& line : lines) {
        const QString stripped = line.trimmed();
        if (bidirectional.match(line).hasMatch() || !line.contains(QStringLiteral("-->"))
            || stripped.startsWith(QStringLiteral("%%"))) {
            output.append(line);
            continue;
        }
        const MermaidStatements statements = splitMermaidStatements(line);
        QStringList fixedStatements;
        for (const QString& statement : statements.values) {
            if (!statement.trimmed().isEmpty())
                fixedStatements.append(fixMermaidEdgeStatement(statement, &idMap, &declaredIds));
        }
        if (fixedStatements.isEmpty()) {
            output.append(line);
            continue;
        }
        QString newLine = QStringLiteral("  ") + fixedStatements.join(QStringLiteral("; "));
        if (statements.trailingSemicolon)
            newLine.append(QLatin1Char(';'));
        output.append(newLine);
    }
    return joinPreservingTerminalNewline(output, content);
}

int countUnquotedPipes(const QString& line) {
    bool inSingle = false;
    bool inDouble = false;
    bool escaped = false;
    int count = 0;
    for (const QChar character : line) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == QLatin1Char('\\')) {
            escaped = true;
            continue;
        }
        if (character == QLatin1Char('\'') && !inDouble) {
            inSingle = !inSingle;
            continue;
        }
        if (character == QLatin1Char('"') && !inSingle) {
            inDouble = !inDouble;
            continue;
        }
        if (character == QLatin1Char('|') && !inSingle && !inDouble)
            ++count;
    }
    return count;
}

QString dropInvalidMermaidEdgeLines(const QString& content) {
    static const QRegularExpression valid = pythonRegex(
        QStringLiteral(R"(-->\s*\|[^|\n]+\|\s*\S)"));
    static const QRegularExpression suspect = pythonRegex(
        QStringLiteral(R"(-->\s*[^|\n]*\|\s*[^|\n]+$)"));
    QStringList output;
    for (const QString& line : pythonSplitLines(content)) {
        const QString stripped = line.trimmed();
        const bool candidate = !stripped.isEmpty() && !stripped.startsWith(QStringLiteral("%%"))
            && line.contains(QStringLiteral("-->")) && line.contains(QLatin1Char('|'));
        if (candidate && !valid.match(line).hasMatch() && countUnquotedPipes(line) == 1
            && suspect.match(stripped).hasMatch()) {
            continue;
        }
        output.append(line);
    }
    return joinPreservingTerminalNewline(output, content);
}

QString sanitizeMermaidSubgraphId(QString raw) {
    static const QRegularExpression invalid = pythonRegex(QStringLiteral(R"([^A-Za-z0-9_]+)"));
    static const QRegularExpression underscores = pythonRegex(QStringLiteral(R"(_+)"));
    raw.replace(invalid, QStringLiteral("_"));
    raw.replace(underscores, QStringLiteral("_"));
    while (raw.startsWith(QLatin1Char('_')))
        raw.remove(0, 1);
    while (raw.endsWith(QLatin1Char('_')))
        raw.chop(1);
    if (raw.isEmpty())
        raw = QStringLiteral("sg");
    if (raw.at(0).isDigit())
        raw.prepend(QStringLiteral("sg_"));
    return raw;
}

QString fixInvalidMermaidSubgraphLines(const QString& content) {
    static const QRegularExpression subgraph = pythonRegex(
        QStringLiteral(R"(^(\s*)subgraph\s+(.+?)\s*$)"));
    static const QRegularExpression explicitId = pythonRegex(
        QStringLiteral(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*(?=[\[\(]))"));
    static const QRegularExpression validId = pythonRegex(
        QStringLiteral(R"(\A[A-Za-z_][A-Za-z0-9_]*\z)"));
    static const QRegularExpression firstToken = pythonRegex(QStringLiteral(R"(^(\S+))"));
    QSet<QString> seen;
    QStringList output;
    for (const QString& line : pythonSplitLines(content)) {
        const QRegularExpressionMatch match = subgraph.match(line);
        if (!match.hasMatch()) {
            output.append(line);
            continue;
        }
        const QString indent = match.captured(1);
        const QString body = match.captured(2);
        const QRegularExpressionMatch explicitMatch = explicitId.match(body.trimmed());
        if (explicitMatch.hasMatch()) {
            seen.insert(explicitMatch.captured(1));
            output.append(line);
            continue;
        }
        const QRegularExpressionMatch tokenMatch = firstToken.match(body);
        if (!tokenMatch.hasMatch()) {
            output.append(line);
            continue;
        }
        const QString token = tokenMatch.captured(1);
        if (validId.match(token).hasMatch()) {
            seen.insert(token);
            output.append(line);
            continue;
        }
        QString id = sanitizeMermaidSubgraphId(body);
        const QString base = id;
        int suffix = 2;
        while (seen.contains(id))
            id = base + QLatin1Char('_') + QString::number(suffix++);
        seen.insert(id);
        QString title = body;
        title.replace(QStringLiteral("\""), QStringLiteral("\\\""));
        output.append(indent + QStringLiteral("subgraph ") + id + QStringLiteral("[\"")
                      + title + QStringLiteral("\"]"));
    }
    return joinPreservingTerminalNewline(output, content);
}

QString normalizeMermaidStrict(QString content) {
    if (content.trimmed().isEmpty())
        return QString();
    content = content.trimmed();
    static const QRegularExpression mermaidFence = pythonRegex(
        QStringLiteral(R"(```mermaid\s*(.*?)\s*```)"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression genericFence = pythonRegex(
        QStringLiteral(R"(```\s*(.*?)\s*```)"),
        QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatch match = mermaidFence.match(content);
    if (match.hasMatch())
        content = match.captured(1).trimmed();
    else {
        match = genericFence.match(content);
        if (match.hasMatch())
            content = match.captured(1).trimmed();
    }
    if (content.startsWith(QStringLiteral("mermaid"), Qt::CaseInsensitive))
        content = content.mid(7).trimmed();

    static const QRegularExpression graphTypo = pythonRegex(
        QStringLiteral(R"(^(grap|grapg|graphh)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression flowTypo = pythonRegex(
        QStringLiteral(R"(^(flowchar|flowchartt)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    content.replace(graphTypo, QStringLiteral("graph"));
    content.replace(flowTypo, QStringLiteral("flowchart"));

    const QString bidirectionalToken = QStringLiteral("__MERMAID_BIDIR_ARROW__");
    content.replace(pythonRegex(QStringLiteral(R"(<\s*-\s*-\s*>)")), bidirectionalToken);
    content.replace(pythonRegex(QStringLiteral(R"(-\s+->)")), QStringLiteral("-->"));
    content.replace(pythonRegex(QStringLiteral(R"(--\s+>)")), QStringLiteral("-->"));
    content.replace(pythonRegex(QStringLiteral(R"(-\s+-\s+>)")), QStringLiteral("-->"));
    content.replace(pythonRegex(QStringLiteral(R"((?<![-=])\s+->\s+)")),
                    QStringLiteral(" --> "));
    content.replace(bidirectionalToken, QStringLiteral("<-->"));

    static const QRegularExpression node = pythonRegex(
        QString::fromUtf8(
            u8R"(([a-zA-Z0-9_\x{4e00}-\x{9fa5}\-]+(?:[ \t]+[a-zA-Z0-9_\x{4e00}-\x{9fa5}\-]+)*)[ \t]*\[(.*?)\])"),
        QRegularExpression::DotMatchesEverythingOption);
    content = replaceMatches(content, node, [](const QRegularExpressionMatch& nodeMatch) {
        const QString rawId = nodeMatch.captured(1).trimmed();
        if (rawId.toLower().startsWith(QStringLiteral("subgraph ")))
            return nodeMatch.captured(0);
        QString id = rawId;
        id.replace(pythonRegex(QStringLiteral(R"([ \t]+)")), QStringLiteral("_"));
        QString rawText = nodeMatch.captured(2);
        if (rawText.startsWith(QStringLiteral("\"[")))
            return nodeMatch.captured(0);
        if (rawText.isEmpty())
            return id + QStringLiteral("[]");
        if (rawText.startsWith(QLatin1Char('"')) && rawText.endsWith(QLatin1Char('"')))
            rawText = rawText.mid(1, rawText.size() - 2);
        rawText.replace(QStringLiteral("\""), QStringLiteral("&quot;"));
        rawText.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        return id + QStringLiteral("[\"") + rawText + QStringLiteral("\"]");
    });
    return QStringLiteral("```mermaid\n") + content + QStringLiteral("\n```");
}

QString normalizeMermaid(QString content) {
    if (detectMermaidRepetitionIssues(content))
        content.clear();
    else {
        content = fixMermaidNodeIdLegalization(content);
        content = dropInvalidMermaidEdgeLines(content);
        content = fixInvalidMermaidSubgraphLines(content);
    }
    return normalizeMermaidStrict(content);
}

OfficialImageAnalysisResult defaultImageAnalysis(const QString& raw) {
    OfficialImageAnalysisResult result;
    result.className = taggedField(raw, QStringLiteral("<|class_start|>"),
                                   QStringLiteral("<|class_end|>"))
                           .toLower();
    result.subClass = taggedField(raw, QStringLiteral("<|sub_class_start|>"),
                                  QStringLiteral("<|sub_class_end|>"))
                          .toLower();
    result.caption = taggedField(raw, QStringLiteral("<|caption_start|>"),
                                 QStringLiteral("<|caption_end|>"));
    result.content = taggedField(raw, QStringLiteral("<|content_start|>"),
                                 QStringLiteral("<|content_end|>"));
    if (result.content.isEmpty() && raw.count(QStringLiteral("<|content_start|>")) == 1
        && !raw.contains(QStringLiteral("<|content_end|>"))) {
        result.content = raw.section(QStringLiteral("<|content_start|>"), -1);
    }
    if (result.className == QLatin1String("chemical"))
        result.content.clear();
    else if (result.className == QLatin1String("flowchart"))
        result.content = normalizeMermaid(result.content);
    else if (result.className == QLatin1String("chart"))
        result.content = normalizeChart(result.content);
    return result;
}

QString convertPureTableToHtml(const QString& content,
                               const OfficialPostProcessCallbacks& callbacks) {
    if (content.trimmed().isEmpty())
        return QString();
    const QString stripped = content.trimmed();
    if (stripped.startsWith(QStringLiteral("<table"), Qt::CaseInsensitive)
        && stripped.endsWith(QStringLiteral("</table>"), Qt::CaseInsensitive)) {
        return stripped;
    }
    QString markdownHtml;
    if (markdownTableToHtml(content, &markdownHtml))
        return markdownHtml;

    const QStringList tokens = {
        QStringLiteral("<nl>"), QStringLiteral("<fcel>"), QStringLiteral("<ecel>"),
        QStringLiteral("<lcel>"), QStringLiteral("<ucel>"), QStringLiteral("<xcel>"),
    };
    bool hasOtsl = false;
    for (const QString& token : tokens) {
        if (content.contains(token)) {
            hasOtsl = true;
            break;
        }
    }
    if (!hasOtsl)
        return QString();
    try {
        const QString html = callbacks.otslToHtml ? callbacks.otslToHtml(content)
                                                   : convertOfficialOtslToHtml(content);
        return html.trimmed().isEmpty() ? QString() : html;
    } catch (...) {
        return QString();
    }
}

QJsonArray simpleProcess(QJsonArray blocks,
                         const OfficialPostProcessOptions& options,
                         const OfficialPostProcessCallbacks& callbacks) {
    for (int index = 0; index < blocks.size(); ++index) {
        if (!blocks.at(index).isObject())
            continue;
        QJsonObject block = blocks.at(index).toObject();
        const QString type = block.value(kType).toString();
        if (type == QLatin1String("table") && hasStringContent(block)) {
            QString content = block.value(kContent).toString();
            try {
                content = callbacks.otslToHtml ? callbacks.otslToHtml(content)
                                               : convertOfficialOtslToHtml(content);
            } catch (...) {
                content = block.value(kContent).toString();
            }
            const QJsonObject tokenMap = block.value(kTableImageTokenMap).toObject();
            content = callbacks.replaceTableImageTokens
                ? callbacks.replaceTableImageTokens(content, tokenMap)
                : defaultReplaceTableImageTokens(content, tokenMap);
            block.insert(kContent,
                         replaceTableFormulaDelimiters(content, options.enableTableFormulaEqWrap));
        }

        if ((type == QLatin1String("image") || type == QLatin1String("chart"))
            && hasStringContent(block)) {
            try {
                const OfficialImageAnalysisResult analysis = callbacks.processImageOrChart
                    ? callbacks.processImageOrChart(block.value(kContent).toString())
                    : defaultImageAnalysis(block.value(kContent).toString());
                if (analysis.className == QLatin1String("pure_table")) {
                    setBlockType(&block, QStringLiteral("table"));
                    const QString html = convertPureTableToHtml(analysis.content, callbacks);
                    block.insert(kContent,
                                 html.isEmpty()
                                     ? QString()
                                     : replaceTableFormulaDelimiters(
                                           html, options.enableTableFormulaEqWrap));
                } else if (analysis.className == QLatin1String("pure_formula")) {
                    setBlockType(&block, QStringLiteral("equation"));
                    block.insert(kContent, analysis.content);
                } else if (analysis.className == QLatin1String("chart")) {
                    setBlockType(&block, QStringLiteral("chart"));
                    block.insert(kSubType, analysis.subClass);
                    block.insert(kContent, analysis.content);
                } else {
                    setBlockType(&block, QStringLiteral("image"));
                    block.insert(kSubType, analysis.className);
                    block.insert(kContent,
                                 analysis.className == QLatin1String("natural_image")
                                         || analysis.content.isEmpty()
                                     ? analysis.caption
                                     : analysis.content);
                }
            } catch (...) {
                block.insert(kContent, QJsonValue::Null);
            }
        }
        blocks.replace(index, block);
    }
    return blocks;
}

QJsonArray cleanupNonTextPlaceholders(const QJsonArray& blocks) {
    static const QSet<QString> keep = {
        QStringLiteral("header"), QStringLiteral("footer"),
    };
    static const QSet<QString> protect = {
        QStringLiteral("image"), QStringLiteral("chart"), QStringLiteral("table"),
        QStringLiteral("equation"), QStringLiteral("list"), QStringLiteral("image_block"),
        QStringLiteral("equation_block"),
    };
    QJsonArray output;
    for (const QJsonValue& value : blocks) {
        if (!value.isObject()) {
            output.append(value);
            continue;
        }
        QJsonObject block = value.toObject();
        const QJsonValue content = block.value(kContent);
        if (!content.isString() || content.toString().trimmed() != kNonText) {
            output.append(block);
            continue;
        }
        const QString type = block.value(kType).toString();
        if (keep.contains(type)) {
            block.insert(kContent, QString());
            output.append(block);
        } else if (protect.contains(type)) {
            output.append(block);
        }
    }
    return output;
}

bool readBbox(const QJsonObject& block, double bbox[4]) {
    const QJsonArray array = block.value(kBbox).toArray();
    if (array.size() != 4)
        return false;
    for (int index = 0; index < 4; ++index) {
        if (!array.at(index).isDouble())
            return false;
        bbox[index] = array.at(index).toDouble();
    }
    return true;
}

double bboxCoverRatio(const QJsonObject& outer, const QJsonObject& inner) {
    double a[4];
    double b[4];
    if (!readBbox(outer, a) || !readBbox(inner, b))
        return 0.0;
    const double left = std::max(a[0], b[0]);
    const double top = std::max(a[1], b[1]);
    const double right = std::min(a[2], b[2]);
    const double bottom = std::min(a[3], b[3]);
    const double intersection = std::max(0.0, right - left) * std::max(0.0, bottom - top);
    const double innerArea = (b[2] - b[0]) * (b[3] - b[1]);
    return innerArea == 0.0 ? 0.0 : intersection / innerArea;
}

QString combineEquations(QStringList contents) {
    static const QRegularExpression tag = pythonRegex(
        QStringLiteral(R"(\\tag\s*\{[^}]*\})"));
    int totalTags = 0;
    for (const QString& content : contents) {
        QRegularExpressionMatchIterator iterator = tag.globalMatch(content);
        while (iterator.hasNext()) {
            iterator.next();
            ++totalTags;
        }
    }
    if (totalTags > 1) {
        static const QRegularExpression captureTag = pythonRegex(
            QStringLiteral(R"(\\tag\s*\{([^}]*)\})"));
        for (QString& content : contents) {
            content = replaceMatches(content, captureTag, [](const QRegularExpressionMatch& match) {
                return QLatin1Char('(') + match.captured(1) + QLatin1Char(')');
            });
        }
    }

    QString combined = QStringLiteral("\\begin{array}{l} ");
    for (const QString& content : contents)
        combined += content + QStringLiteral(" \\\\ ");
    combined += QStringLiteral("\\end{array}");
    return combined;
}

QJsonArray handleEquationBlocks(const QJsonArray& blocks) {
    QVector<int> semanticIndices;
    QVector<int> equationIndices;
    for (int index = 0; index < blocks.size(); ++index) {
        if (!blocks.at(index).isObject())
            continue;
        const QString type = blocks.at(index).toObject().value(kType).toString();
        if (type == QLatin1String("equation_block"))
            semanticIndices.append(index);
        else if (type == QLatin1String("equation"))
            equationIndices.append(index);
    }

    QVector<QPair<int, QVector<int>>> semanticSpans;
    for (int semanticIndex : semanticIndices) {
        QVector<int> covered;
        const QJsonObject semantic = blocks.at(semanticIndex).toObject();
        for (int equationIndex : equationIndices) {
            if (bboxCoverRatio(semantic, blocks.at(equationIndex).toObject()) > 0.9)
                covered.append(equationIndex);
        }
        if (covered.size() > 1)
            semanticSpans.append(qMakePair(semanticIndex, covered));
    }

    QSet<int> absorbedEquations;
    for (const auto& entry : semanticSpans) {
        for (int index : entry.second)
            absorbedEquations.insert(index);
    }

    QJsonArray output;
    for (int index = 0; index < blocks.size(); ++index) {
        if (absorbedEquations.contains(index))
            continue;
        auto found = std::find_if(semanticSpans.cbegin(), semanticSpans.cend(), [index](const auto& entry) {
            return entry.first == index;
        });
        if (found != semanticSpans.cend()) {
            QStringList contents;
            for (int equationIndex : found->second) {
                const QJsonValue content = blocks.at(equationIndex).toObject().value(kContent);
                if (!content.isString())
                    throw std::runtime_error("equation span content is not a string");
                contents.append(content.toString());
            }
            const QJsonObject semantic = blocks.at(index).toObject();
            QJsonObject equation;
            equation.insert(kType, QStringLiteral("equation"));
            equation.insert(kBbox, semantic.value(kBbox));
            equation.insert(kAngle, semantic.contains(kAngle) ? semantic.value(kAngle) : QJsonValue::Null);
            equation.insert(kContent, combineEquations(contents));
            output.append(equation);
            continue;
        }
        const QJsonValue value = blocks.at(index);
        if (value.isObject()
            && value.toObject().value(kType).toString() == QLatin1String("equation_block")) {
            continue;
        }
        output.append(value);
    }
    return output;
}

QJsonArray finalizedSimpleBlocks(const QJsonArray& blocks) {
    QJsonArray output;
    for (const QJsonValue& value : blocks) {
        if (!value.isObject()) {
            output.append(value);
            continue;
        }
        QJsonObject block = value.toObject();
        if (block.value(kType).toString() == QLatin1String("image")
            && isAbsorbedTableImage(block)) {
            continue;
        }
        output.append(cleanupPrivateMetadata(std::move(block)));
    }
    return output;
}

}  // namespace

QString processOfficialEquation(const QString& latex) {
    QString content = fixEquationDelimiters(latex);
    content = fixEquationLeftRight(content);
    content = fixEquationDoubleSubscript(content);
    content = fixEquationEqColon(content);
    content = fixEquationBig(content);
    content.replace(QLatin1Char('<'), QStringLiteral("< "));
    return fixUnbalancedBraces(content);
}

QString processOfficialText(const QString& text) {
    QString content = convertDisplayToInline(text);
    content = fixMacroSpacingInMarkdown(content);
    return moveUnderscoresOutside(content);
}

QString addOfficialEquationBrackets(const QString& latex) {
    QString content = latex.trimmed();
    if (!content.startsWith(QStringLiteral("\\[")))
        content.prepend(QStringLiteral("\\[\n"));
    if (!content.endsWith(QStringLiteral("\\]")))
        content.append(QStringLiteral("\n\\]"));
    return content;
}

QJsonArray postProcessOfficialBlocks(const QJsonArray& input,
                                     const OfficialPostProcessOptions& options,
                                     const OfficialPostProcessCallbacks& callbacks) {
    QJsonArray blocks = input;
    try {
        blocks = simpleProcess(blocks, options, callbacks);

        for (int index = 0; index < blocks.size(); ++index) {
            if (!blocks.at(index).isObject())
                continue;
            QJsonObject block = blocks.at(index).toObject();
            if (block.value(kType).toString() == QLatin1String("list_item"))
                setBlockType(&block, QStringLiteral("text"));
            blocks.replace(index, block);
        }

        blocks = cleanupNonTextPlaceholders(blocks);
        if (options.simplePostProcess)
            return finalizedSimpleBlocks(blocks);

        for (int index = 0; index < blocks.size(); ++index) {
            if (!blocks.at(index).isObject())
                continue;
            QJsonObject block = blocks.at(index).toObject();
            const QString type = block.value(kType).toString();
            if (type == QLatin1String("equation") && hasStringContent(block)) {
                try {
                    block.insert(kContent, processOfficialEquation(block.value(kContent).toString()));
                } catch (...) {
                    // Pinned implementation logs and preserves the original block.
                }
            } else if (type == QLatin1String("text") && hasStringContent(block)) {
                try {
                    block.insert(kContent, processOfficialText(block.value(kContent).toString()));
                } catch (...) {
                    // Pinned implementation logs and preserves the original block.
                }
            }
            blocks.replace(index, block);
        }

        if (options.handleEquationBlock)
            blocks = handleEquationBlocks(blocks);

        for (int index = 0; index < blocks.size(); ++index) {
            if (!blocks.at(index).isObject())
                continue;
            QJsonObject block = blocks.at(index).toObject();
            if (block.value(kType).toString() == QLatin1String("equation")
                && hasStringContent(block)) {
                block.insert(kContent, addOfficialEquationBrackets(block.value(kContent).toString()));
                blocks.replace(index, block);
            }
        }

        static const QSet<QString> paratextTypes = {
            QStringLiteral("header"), QStringLiteral("footer"), QStringLiteral("page_number"),
            QStringLiteral("aside_text"), QStringLiteral("page_footnote"),
            QStringLiteral("unknown"),
        };
        QJsonArray output;
        for (const QJsonValue& value : blocks) {
            if (!value.isObject()) {
                output.append(value);
                continue;
            }
            QJsonObject block = value.toObject();
            const QString type = block.value(kType).toString();
            if (type == QLatin1String("equation_block"))
                continue;
            if (type == QLatin1String("image") && isAbsorbedTableImage(block))
                continue;
            if (options.abandonList && type == QLatin1String("list"))
                continue;
            if (options.abandonParatext && paratextTypes.contains(type))
                continue;
            output.append(cleanupPrivateMetadata(std::move(block)));
        }
        return output;
    } catch (...) {
        // MinerUClientHelper.post_process uses this page-local fall-back.
        return cleanupFailureBlocks(blocks);
    }
}

QVector<QJsonArray> batchPostProcessOfficialBlocks(
    const QVector<QJsonArray>& blocksList,
    const OfficialPostProcessOptions& options,
    const OfficialPostProcessCallbacks& callbacks) {
    QVector<QJsonArray> output;
    output.reserve(blocksList.size());
    for (const QJsonArray& blocks : blocksList)
        output.append(postProcessOfficialBlocks(blocks, options, callbacks));
    return output;
}

}  // namespace hybrid
}  // namespace scanengine

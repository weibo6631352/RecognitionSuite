#include "scanengine/hybrid/canonical_model.hpp"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVector>

#include <utility>

namespace scanengine {
namespace hybrid {

namespace {

const QString kType = QStringLiteral("type");
const QString kContent = QStringLiteral("content");
const QString kLines = QStringLiteral("lines");
const QString kSpans = QStringLiteral("spans");
const QString kBbox = QStringLiteral("bbox");
const QString kLabel = QStringLiteral("label");

const QString kText = QStringLiteral("text");
const QString kTitle = QStringLiteral("title");
const QString kDocTitle = QStringLiteral("doc_title");
const QString kParagraphTitle = QStringLiteral("paragraph_title");
const QString kEquation = QStringLiteral("equation");
const QString kFormulaNumber = QStringLiteral("formula_number");

bool hasType(const QJsonValue& value, const QString& type) {
    return value.isObject() && value.toObject().value(kType).toString() == type;
}

QString fullToHalf(const QString& text) {
    QString result;
    result.reserve(text.size());
    for (const QChar character : text) {
        const ushort code = character.unicode();
        if (code >= 0xff01U && code <= 0xff5eU)
            result.append(QChar(static_cast<ushort>(code - 0xfee0U)));
        else
            result.append(character);
    }
    return result;
}

QString extractFormulaNumberText(const QJsonObject& block) {
    const QJsonValue content = block.value(kContent);
    if (content.isString()) {
        const QString trimmed = content.toString().trimmed();
        if (!trimmed.isEmpty())
            return trimmed;
    }

    QString text;
    const QJsonArray lines = block.value(kLines).toArray();
    for (const QJsonValue& lineValue : lines) {
        const QJsonArray spans = lineValue.toObject().value(kSpans).toArray();
        for (const QJsonValue& spanValue : spans) {
            const QJsonObject span = spanValue.toObject();
            if (span.value(kType).toString() == kText)
                text.append(span.value(kContent).toString());
        }
    }
    return text.trimmed();
}

QString normalizeFormulaTagContent(const QJsonObject& block) {
    QString tag = fullToHalf(extractFormulaNumberText(block).trimmed());
    if (tag.startsWith(QLatin1Char('(')))
        tag = tag.mid(1).trimmed();
    if (tag.endsWith(QLatin1Char(')'))) {
        tag.chop(1);
        tag = tag.trimmed();
    }
    return tag;
}

QString cleanIsolatedFormula(const QString& content) {
    // Keep the official order: delimiters are checked before whitespace is
    // stripped. Thus "  \\[x\\]  " deliberately retains its delimiters.
    QString formula = content;
    if (formula.startsWith(QStringLiteral("\\[")))
        formula.remove(0, 2);
    if (formula.endsWith(QStringLiteral("\\]")))
        formula.chop(2);
    return formula.trimmed();
}

bool appendFormulaNumberTag(QVector<QJsonValue>& blocks,
                            int equationIndex,
                            const QJsonObject& numberBlock) {
    QJsonObject equation = blocks.at(equationIndex).toObject();
    const QJsonValue contentValue = equation.value(kContent);
    const QString formula = cleanIsolatedFormula(
        contentValue.isString() ? contentValue.toString() : QString());
    if (formula.isEmpty())
        return false;

    equation.insert(kContent,
                    formula + QStringLiteral("\\tag{")
                        + normalizeFormulaTagContent(numberBlock) + QLatin1Char('}'));
    blocks[equationIndex] = equation;
    return true;
}

void downgradeFormulaNumber(QVector<QJsonValue>& blocks, int index) {
    QJsonObject block = blocks.at(index).toObject();
    block.insert(kType, kText);
    blocks[index] = block;
}

struct PixelBbox {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
};

bool jsonFloat(const QJsonValue& value, double* result) {
    if (!result)
        return false;
    if (value.isDouble()) {
        *result = value.toDouble();
        return true;
    }
    if (value.isBool()) {
        *result = value.toBool() ? 1.0 : 0.0;
        return true;
    }
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok) {
            *result = parsed;
            return true;
        }
    }
    return false;
}

bool bboxToPixels(const QJsonValue& value,
                  double pageWidth,
                  double pageHeight,
                  PixelBbox* result) {
    if (!result || !value.isArray())
        return false;
    const QJsonArray bbox = value.toArray();
    if (bbox.size() != 4)
        return false;

    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    if (!jsonFloat(bbox.at(0), &x0) || !jsonFloat(bbox.at(1), &y0)
        || !jsonFloat(bbox.at(2), &x1) || !jsonFloat(bbox.at(3), &y1)) {
        return false;
    }

    const bool normalized = x0 >= 0.0 && x0 <= 1.0 && y0 >= 0.0 && y0 <= 1.0
        && x1 >= 0.0 && x1 <= 1.0 && y1 >= 0.0 && y1 <= 1.0;
    if (normalized) {
        x0 *= pageWidth;
        x1 *= pageWidth;
        y0 *= pageHeight;
        y1 *= pageHeight;
    }

    if (x1 < x0)
        std::swap(x0, x1);
    if (y1 < y0)
        std::swap(y0, y1);
    if (x1 <= x0 || y1 <= y0)
        return false;

    result->left = x0;
    result->top = y0;
    result->right = x1;
    result->bottom = y1;
    return true;
}

double minAreaOverlapRatio(const PixelBbox& first, const PixelBbox& second) {
    const double intersectionLeft = second.left > first.left ? second.left : first.left;
    const double intersectionTop = second.top > first.top ? second.top : first.top;
    const double intersectionRight = second.right < first.right ? second.right : first.right;
    const double intersectionBottom = second.bottom < first.bottom ? second.bottom : first.bottom;
    if (intersectionRight < intersectionLeft || intersectionBottom < intersectionTop)
        return 0.0;

    const double intersectionArea = (intersectionRight - intersectionLeft)
        * (intersectionBottom - intersectionTop);
    const double firstArea = (first.right - first.left) * (first.bottom - first.top);
    const double secondArea = (second.right - second.left) * (second.bottom - second.top);
    const double minArea = secondArea < firstArea ? secondArea : firstArea;
    return minArea == 0.0 ? 0.0 : intersectionArea / minArea;
}

}  // namespace

void optimizeHybridFormulaNumberBlocks(QJsonArray& page) {
    QVector<QJsonValue> blocks;
    blocks.reserve(page.size());
    for (const QJsonValue& block : page)
        blocks.append(block);

    QVector<unsigned char> keep(blocks.size(), 1U);
    for (int index = 0; index < blocks.size(); ++index) {
        if (!hasType(blocks.at(index), kFormulaNumber))
            continue;

        const QJsonObject formulaNumber = blocks.at(index).toObject();
        if (index > 0 && hasType(blocks.at(index - 1), kEquation)) {
            if (appendFormulaNumberTag(blocks, index - 1, formulaNumber))
                keep[index] = 0U;
            else
                downgradeFormulaNumber(blocks, index);
            continue;
        }

        const bool nextIsEquation = index + 1 < blocks.size()
            && hasType(blocks.at(index + 1), kEquation);
        const bool nextNextIsFormulaNumber = index + 2 < blocks.size()
            && hasType(blocks.at(index + 2), kFormulaNumber);
        if (nextIsEquation && !nextNextIsFormulaNumber) {
            if (appendFormulaNumberTag(blocks, index + 1, formulaNumber))
                keep[index] = 0U;
            else
                downgradeFormulaNumber(blocks, index);
            continue;
        }

        downgradeFormulaNumber(blocks, index);
    }

    QJsonArray optimized;
    for (int index = 0; index < blocks.size(); ++index) {
        if (keep.at(index))
            optimized.append(blocks.at(index));
    }
    page = optimized;
}

void applyLayoutTitleSplit(QJsonArray& page,
                           const QJsonArray& layoutDets,
                           double pageWidth,
                           double pageHeight,
                           double overlapThreshold) {
    QVector<PixelBbox> docTitleBboxes;
    docTitleBboxes.reserve(layoutDets.size());
    for (const QJsonValue& layoutValue : layoutDets) {
        if (!layoutValue.isObject())
            continue;
        const QJsonObject layoutItem = layoutValue.toObject();
        if (layoutItem.value(kLabel).toString() != kDocTitle)
            continue;
        PixelBbox bbox;
        if (bboxToPixels(layoutItem.value(kBbox), pageWidth, pageHeight, &bbox))
            docTitleBboxes.append(bbox);
    }

    for (int index = 0; index < page.size(); ++index) {
        const QJsonValue value = page.at(index);
        if (!hasType(value, kTitle))
            continue;

        QJsonObject block = value.toObject();
        PixelBbox titleBbox;
        if (!bboxToPixels(block.value(kBbox), pageWidth, pageHeight, &titleBbox))
            continue;

        bool isDocTitle = false;
        for (const PixelBbox& docTitleBbox : docTitleBboxes) {
            if (minAreaOverlapRatio(titleBbox, docTitleBbox) >= overlapThreshold) {
                isDocTitle = true;
                break;
            }
        }
        block.insert(kType, isDocTitle ? kDocTitle : kParagraphTitle);
        page.replace(index, block);
    }
}

}  // namespace hybrid
}  // namespace scanengine

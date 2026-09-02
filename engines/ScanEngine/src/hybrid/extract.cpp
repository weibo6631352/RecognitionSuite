#include "scanengine/hybrid/extract.hpp"

#include "scanengine/config.hpp"
#include "scanengine/hybrid/chat.hpp"
#include "scanengine/hybrid/layout.hpp"
#include "scanengine/hybrid/ocr.hpp"
#include "scanengine/hybrid/official_code_language.hpp"
#include "scanengine/hybrid/official_language_detection.hpp"
#include "scanengine/hybrid/official_post_process.hpp"
#include "scanengine/hybrid/orchestrator.hpp"
#include "scanengine/hybrid/otsl.hpp"
#include "scanengine/hybrid/preprocessor.hpp"
#include "scanengine/hybrid/rounding.hpp"
#include "scanengine/hybrid/qwen2_vl.hpp"
#include "scanengine/hybrid/table_image_processor.hpp"
#include "scanengine/hybrid/vlm_paths.hpp"
#include "scanengine/strict_json.hpp"

#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace scanengine {
namespace hybrid {

QImage resizeByNeed(const QImage& image, int minEdge, double maxEdgeRatio) {
    if (image.isNull() || image.width() < 1 || image.height() < 1)
        return image;
    QImage out = image.convertToFormat(QImage::Format_RGB888);
    const int w = out.width();
    const int h = out.height();
    const int mx = std::max(w, h);
    const int mn = std::min(w, h);
    const double ratio = double(mx) / double(mn);
    if (ratio > maxEdgeRatio) {
        int newW = w;
        int newH = h;
        if (w > h)
            newH = int(std::ceil(w / maxEdgeRatio));
        else
            newW = int(std::ceil(h / maxEdgeRatio));
        QImage padded(newW, newH, QImage::Format_RGB888);
        padded.fill(QColor(255, 255, 255));
        QPainter p(&padded);
        p.drawImage((newW - w) / 2, (newH - h) / 2, out);
        p.end();
        out = padded;
    }
    if (std::min(out.width(), out.height()) < minEdge) {
        const double scale = double(minEdge) / double(std::min(out.width(), out.height()));
        const int newW = int(std::ceil(out.width() * scale));
        const int newH = int(std::ceil(out.height() * scale));
        out = resizeOfficialBicubic(out, newW, newH);
    }
    return out;
}

// PIL Image.rotate(angle, expand=True) default NEAREST, counter-clockwise.
// Official table/text extract uses this, not bilinear.
QImage rotateOfficialPil(int angle, const QImage& srcIn) {
    const QImage src = srcIn.convertToFormat(QImage::Format_RGB888);
    const int w = src.width();
    const int h = src.height();
    if (w < 1 || h < 1)
        return src;
    if (angle == 180) {
        QImage out(w, h, QImage::Format_RGB888);
        for (int y = 0; y < h; ++y) {
            const uchar* s = src.constScanLine(h - 1 - y);
            uchar* d = out.scanLine(y);
            for (int x = 0; x < w; ++x)
                std::memcpy(d + 3 * x, s + 3 * (w - 1 - x), 3);
        }
        return out;
    }
    if (angle == 90) {
        QImage out(h, w, QImage::Format_RGB888);
        for (int y = 0; y < h; ++y) {
            const uchar* s = src.constScanLine(y);
            for (int x = 0; x < w; ++x) {
                uchar* d = out.scanLine(w - 1 - x);
                std::memcpy(d + 3 * y, s + 3 * x, 3);
            }
        }
        return out;
    }
    if (angle == 270) {
        QImage out(h, w, QImage::Format_RGB888);
        for (int y = 0; y < h; ++y) {
            const uchar* s = src.constScanLine(y);
            for (int x = 0; x < w; ++x) {
                uchar* d = out.scanLine(x);
                std::memcpy(d + 3 * (h - 1 - y), s + 3 * x, 3);
            }
        }
        return out;
    }
    return src;
}

QVector<PreparedExtract> prepareForExtract(const QImage& page,
                                           const QVector<ContentBlock>& blocks,
                                           bool imageAnalysis,
                                           const QStringList& notExtractList) {
    QVector<PreparedExtract> out;
    if (page.isNull())
        return out;
    const QImage rgb = page.convertToFormat(QImage::Format_RGB888);
    const int width = rgb.width();
    const int height = rgb.height();
    for (int idx = 0; idx < blocks.size(); ++idx) {
        const ContentBlock& block = blocks.at(idx);
        if (shouldSkipExtract(block.type, imageAnalysis, notExtractList))
            continue;
        // hybrid_analyze.normalize_bbox_to_unit; Pillow Image._crop.
        const int x0 = roundHalfToEven(roundToDecimalDigits(double(block.xmin), 3) * double(width));
        const int y0 = roundHalfToEven(roundToDecimalDigits(double(block.ymin), 3) * double(height));
        const int x1 = roundHalfToEven(roundToDecimalDigits(double(block.xmax), 3) * double(width));
        const int y1 = roundHalfToEven(roundToDecimalDigits(double(block.ymax), 3) * double(height));
        const int cw = x1 - x0;
        const int ch = y1 - y0;
        if (cw < 1 || ch < 1)
            continue;
        QImage crop = rgb.copy(x0, y0, cw, ch);
        if (crop.width() < 1 || crop.height() < 1)
            continue;
        if (block.angle && (*block.angle == 90 || *block.angle == 180 || *block.angle == 270))
            crop = rotateOfficialPil(*block.angle, crop);
        crop = resizeByNeed(crop);
        PreparedExtract p;
        p.crop = crop;
        p.prompt = officialPromptForType(block.type);
        p.sampling = officialSamplingForType(block.type);
        p.blockIndex = idx;
        out.push_back(p);
    }
    return out;
}

bool extractWithLayout(const Qwen2LanguageModel& language,
                       const Qwen2VisionModel& vision,
                       const OfficialTokenizer& tok,
                       const QImage& page,
                       QVector<ContentBlock>* blocks,
                       bool imageAnalysis,
                       const QStringList& notExtractList,
                       int backendTokenLimit,
                       QString* err,
                       const std::function<bool()>& isCancelled) {
    if (!blocks) {
        if (err)
            *err = QStringLiteral("blocks is null");
        return false;
    }
    QJsonArray pageBlocks = officialContentBlocksJson(*blocks);
    const OfficialExtractModels models{&language, &vision, &tok};
    if (!extractOfficialPageWithLayout(page,
                                       &pageBlocks,
                                       imageAnalysis,
                                       notExtractList,
                                       backendTokenLimit,
                                       &models,
                                       err,
                                       isCancelled)) {
        return false;
    }
    return projectOfficialContentBlocks(pageBlocks, blocks, err);
}

QImage prepareForLayout(const QImage& image) {
    if (image.isNull())
        return image;
    return resizeOfficialBicubic(image.convertToFormat(QImage::Format_RGB888),
                                 kOfficialLayoutImageSize, kOfficialLayoutImageSize);
}

namespace {

bool officialBlockType(const QString& t) {
    static const QStringList types = {
        QStringLiteral("text"),        QStringLiteral("title"),
        QStringLiteral("doc_title"),   QStringLiteral("paragraph_title"),
        QStringLiteral("table"),       QStringLiteral("equation"),
        QStringLiteral("formula_number"), QStringLiteral("code"),
        QStringLiteral("algorithm"),   QStringLiteral("aside_text"),
        QStringLiteral("ref_text"),    QStringLiteral("index"),
        QStringLiteral("phonetic"),    QStringLiteral("list_item"),
        QStringLiteral("table_caption"), QStringLiteral("image_caption"),
        QStringLiteral("code_caption"), QStringLiteral("caption"),
        QStringLiteral("table_footnote"), QStringLiteral("image_footnote"),
        QStringLiteral("footnote"),    QStringLiteral("header"),
        QStringLiteral("footer"),      QStringLiteral("page_number"),
        QStringLiteral("page_footnote"), QStringLiteral("image"),
        QStringLiteral("chart"),       QStringLiteral("list"),
        QStringLiteral("image_block"), QStringLiteral("equation_block"),
        QStringLiteral("unknown"),
    };
    return types.contains(t);
}

double blockArea(const ContentBlock& b) {
    return std::max(0.0, b.xmax - b.xmin) * std::max(0.0, b.ymax - b.ymin);
}

double coverage(const ContentBlock& inner, const ContentBlock& outer) {
    const double x0 = std::max(inner.xmin, outer.xmin);
    const double y0 = std::max(inner.ymin, outer.ymin);
    const double x1 = std::min(inner.xmax, outer.xmax);
    const double y1 = std::min(inner.ymax, outer.ymax);
    const double inter = std::max(0.0, x1 - x0) * std::max(0.0, y1 - y0);
    const double a = blockArea(inner);
    return a > 0 ? inter / a : 0;
}

}  // namespace

bool projectOfficialContentBlocks(const QJsonArray& page,
                                  QVector<ContentBlock>* blocks,
                                  QString* err) {
    auto fail = [&](int index, const QString& message) {
        if (err) {
            *err = index >= 0
                ? QStringLiteral("invalid ContentBlock %1: %2").arg(index).arg(message)
                : message;
        }
        return false;
    };
    if (!blocks)
        return fail(-1, QStringLiteral("ContentBlock output is null"));

    QVector<ContentBlock> projected;
    projected.reserve(page.size());
    for (int index = 0; index < page.size(); ++index) {
        if (!page.at(index).isObject())
            return fail(index, QStringLiteral("block must be a JSON object"));
        const QJsonObject object = page.at(index).toObject();
        const QJsonValue typeValue = object.value(QStringLiteral("type"));
        if (!typeValue.isString() || !officialBlockType(typeValue.toString()))
            return fail(index, QStringLiteral("unknown or missing type"));

        const QJsonValue bboxValue = object.value(QStringLiteral("bbox"));
        if (!bboxValue.isArray() || bboxValue.toArray().size() != 4)
            return fail(index, QStringLiteral("bbox must contain four numbers"));
        const QJsonArray bbox = bboxValue.toArray();
        double coordinates[4] = {};
        for (int coordinate = 0; coordinate < 4; ++coordinate) {
            if (!bbox.at(coordinate).isDouble())
                return fail(index, QStringLiteral("bbox contains a non-number"));
            coordinates[coordinate] = bbox.at(coordinate).toDouble();
            if (!std::isfinite(coordinates[coordinate])
                || coordinates[coordinate] < 0.0
                || coordinates[coordinate] > 1.0) {
                return fail(index, QStringLiteral("bbox must be finite and normalized"));
            }
        }
        if (coordinates[0] >= coordinates[2] || coordinates[1] >= coordinates[3])
            return fail(index, QStringLiteral("bbox must have positive area"));

        ContentBlock block;
        block.type = typeValue.toString();
        block.xmin = coordinates[0];
        block.ymin = coordinates[1];
        block.xmax = coordinates[2];
        block.ymax = coordinates[3];

        const QJsonValue angleValue = object.value(QStringLiteral("angle"));
        if (!angleValue.isUndefined() && !angleValue.isNull()) {
            if (!angleValue.isDouble())
                return fail(index, QStringLiteral("angle must be null or 0/90/180/270"));
            const double number = angleValue.toDouble();
            const int angle = int(number);
            if (!std::isfinite(number) || number != double(angle)
                || (angle != 0 && angle != 90 && angle != 180 && angle != 270)) {
                return fail(index, QStringLiteral("angle must be null or 0/90/180/270"));
            }
            block.angle = angle;
        }

        const QJsonValue contentValue = object.value(QStringLiteral("content"));
        if (!contentValue.isUndefined() && !contentValue.isNull()
            && !contentValue.isString()) {
            return fail(index, QStringLiteral("content must be absent, null or a string"));
        }
        block.contentNull = contentValue.isNull();
        if (contentValue.isString())
            block.content = contentValue.toString();

        const QJsonValue mergePrevValue = object.value(QStringLiteral("merge_prev"));
        if (!mergePrevValue.isUndefined()) {
            if (!mergePrevValue.isBool())
                return fail(index, QStringLiteral("merge_prev must be a boolean"));
            block.mergePrev = mergePrevValue.toBool();
        }
        if (block.mergePrev && block.type != QLatin1String("text"))
            return fail(index, QStringLiteral("merge_prev is only valid for text"));

        const QJsonValue subTypeValue = object.value(QStringLiteral("sub_type"));
        if (subTypeValue.isString())
            block.subType = subTypeValue.toString();
        projected.push_back(block);
    }
    *blocks = std::move(projected);
    if (err)
        err->clear();
    return true;
}

QJsonArray officialContentBlocksJson(const QVector<ContentBlock>& blocks) {
    QJsonArray page;
    for (const ContentBlock& block : blocks) {
        QJsonObject object;
        object.insert(QStringLiteral("type"), block.type);
        object.insert(QStringLiteral("bbox"),
                      QJsonArray{block.xmin, block.ymin, block.xmax, block.ymax});
        object.insert(QStringLiteral("angle"),
                      block.angle ? QJsonValue(*block.angle) : QJsonValue(QJsonValue::Null));
        object.insert(QStringLiteral("content"),
                      block.contentNull ? QJsonValue(QJsonValue::Null)
                                        : QJsonValue(block.content));
        if (block.type == QLatin1String("text"))
            object.insert(QStringLiteral("merge_prev"), block.mergePrev);
        if (!block.subType.isEmpty())
            object.insert(QStringLiteral("sub_type"), block.subType);
        page.append(object);
    }
    return page;
}

namespace {

const QString kTableImageTokenMapKey = QStringLiteral("_table_image_token_map");
const QString kTableImageAbsorbedKey = QStringLiteral("_absorbed_by_table");

void clearTableImagePrivateKeys(QJsonArray* page) {
    if (!page)
        return;
    for (int index = 0; index < page->size(); ++index) {
        if (!page->at(index).isObject())
            continue;
        QJsonObject object = page->at(index).toObject();
        object.remove(kTableImageTokenMapKey);
        object.remove(kTableImageAbsorbedKey);
        (*page)[index] = object;
    }
}

QSet<int> coveredVisualIndices(const QVector<ContentBlock>& blocks) {
    QVector<int> containers;
    for (int index = 0; index < blocks.size(); ++index) {
        if (blocks.at(index).type == QLatin1String("image_block"))
            containers.push_back(index);
    }
    QSet<int> covered;
    for (int index = 0; index < blocks.size(); ++index) {
        const QString& type = blocks.at(index).type;
        if (type != QLatin1String("image") && type != QLatin1String("chart"))
            continue;
        for (int container : containers) {
            if (coverage(blocks.at(index), blocks.at(container)) >= 0.9) {
                covered.insert(index);
                break;
            }
        }
    }
    return covered;
}

bool eligibleForImageAnalysis(const ContentBlock& block) {
    const double width = block.xmax - block.xmin;
    const double height = block.ymax - block.ymin;
    return (width > 0.1 && height > 0.1) || width * height > 0.01;
}

void injectTableImageMetadata(const TableImageMetadata& metadata, QJsonArray* page) {
    if (!page)
        return;
    for (int tableIndex : metadata.tableOrder) {
        if (tableIndex < 0 || tableIndex >= page->size() || !page->at(tableIndex).isObject())
            continue;
        const QVector<TableImageToken> tokens = metadata.tokensByTable.value(tableIndex);
        if (tokens.isEmpty())
            continue;
        QJsonObject tokenMap;
        for (const TableImageToken& token : tokens)
            tokenMap.insert(token.token, token.dataUri);
        QJsonObject table = page->at(tableIndex).toObject();
        table.insert(kTableImageTokenMapKey, tokenMap);
        (*page)[tableIndex] = table;
    }
    for (int imageIndex : metadata.absorbedImageIndices) {
        if (imageIndex < 0 || imageIndex >= page->size() || !page->at(imageIndex).isObject())
            continue;
        QJsonObject image = page->at(imageIndex).toObject();
        image.insert(kTableImageAbsorbedKey, true);
        (*page)[imageIndex] = image;
    }
}

bool containsTableImagePrivateKeys(const QJsonArray& page) {
    for (const QJsonValue& value : page) {
        if (!value.isObject())
            continue;
        const QJsonObject object = value.toObject();
        if (object.contains(kTableImageTokenMapKey)
            || object.contains(kTableImageAbsorbedKey)) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool extractOfficialPageWithLayout(const QImage& page,
                                   QJsonArray* pageBlocks,
                                   bool imageAnalysis,
                                   const QStringList& notExtractList,
                                   int backendTokenLimit,
                                   const OfficialExtractModels* suppliedModels,
                                   QString* err,
                                   const std::function<bool()>& isCancelled,
                                   const OfficialExtractHooks* hooks) {
    auto fail = [&](const QString& message) {
        if (err)
            *err = message;
        return false;
    };
    if (!pageBlocks)
        return fail(QStringLiteral("page blocks are null"));
    if (page.isNull() || page.width() < 1 || page.height() < 1)
        return fail(QStringLiteral("page image is empty"));
    if (backendTokenLimit <= 0)
        return fail(QStringLiteral("backend token limit must be greater than zero"));

    QJsonArray workingPage = *pageBlocks;
    clearTableImagePrivateKeys(&workingPage);
    QVector<ContentBlock> blocks;
    if (!projectOfficialContentBlocks(workingPage, &blocks, err))
        return false;

    QVector<int> keptOriginalIndices;
    removeInternalImageCaptions(&blocks,
                                kOfficialTableImageCoverageThreshold,
                                nullptr,
                                &keptOriginalIndices);
    if (keptOriginalIndices.size() != workingPage.size()) {
        QJsonArray filtered;
        for (int originalIndex : keptOriginalIndices)
            filtered.append(workingPage.at(originalIndex));
        workingPage = filtered;
    }
    if (workingPage.size() != blocks.size())
        return fail(QStringLiteral("caption filtering desynchronized JSON and typed blocks"));

    QVector<int> tableIndices;
    for (int index = 0; index < blocks.size(); ++index) {
        if (blocks.at(index).type == QLatin1String("table")
            && !shouldSkipExtract(blocks.at(index).type, imageAnalysis, notExtractList)) {
            tableIndices.push_back(index);
        }
    }
    TableImageMetadata tableMetadata = buildTableImageMap(
        blocks, tableIndices, kOfficialTableImageCoverageThreshold);
    const QSet<int> coveredVisuals = coveredVisualIndices(blocks);

    QVector<PreparedExtract> prepared;
    const QImage rgb = page.convertToFormat(QImage::Format_RGB888);
    for (int index = 0; index < blocks.size(); ++index) {
        const ContentBlock& block = blocks.at(index);
        if (shouldSkipExtract(block.type, imageAnalysis, notExtractList))
            continue;
        if (block.type == QLatin1String("image")
            && isAbsorbedTableImage(tableMetadata, index)) {
            continue;
        }
        if ((block.type == QLatin1String("image") || block.type == QLatin1String("chart"))
            && (coveredVisuals.contains(index) || !eligibleForImageAnalysis(block))) {
            continue;
        }

        const int left = roundHalfToEven(block.xmin * double(rgb.width()));
        const int top = roundHalfToEven(block.ymin * double(rgb.height()));
        const int right = roundHalfToEven(block.xmax * double(rgb.width()));
        const int bottom = roundHalfToEven(block.ymax * double(rgb.height()));
        if (right <= left || bottom <= top)
            continue;
        QImage crop = rgb.copy(left, top, right - left, bottom - top);
        if (crop.isNull() || crop.width() < 1 || crop.height() < 1)
            continue;

        if (block.type == QLatin1String("table")) {
            QImage masked;
            if (!prepareTableImageForExtract(rgb,
                                             blocks,
                                             index,
                                             crop,
                                             &tableMetadata,
                                             &masked,
                                             err,
                                             hooks ? hooks->tableImage
                                                   : TableImageProcessorOptions{})) {
                return false;
            }
            crop = masked;
        } else if (block.angle
                   && (*block.angle == 90 || *block.angle == 180 || *block.angle == 270)) {
            crop = rotateOfficialPil(*block.angle, crop);
        }
        crop = resizeByNeed(crop);

        PreparedExtract job;
        job.crop = crop;
        job.prompt = officialPromptForType(block.type);
        job.sampling = officialSamplingForType(block.type);
        job.blockIndex = index;
        prepared.push_back(job);
    }

    if (!prepared.isEmpty() && hooks && hooks->infer) {
        for (const PreparedExtract& job : prepared) {
            QString text;
            if (!hooks->infer(job, &text, err))
                return false;
            if (job.blockIndex < 0 || job.blockIndex >= workingPage.size())
                return fail(QStringLiteral("prepared block index is out of range"));
            QJsonObject object = workingPage.at(job.blockIndex).toObject();
            object.insert(QStringLiteral("content"), text);
            workingPage[job.blockIndex] = object;
        }
    } else if (!prepared.isEmpty()) {
        OfficialTokenizer loadedTokenizer;
        const OfficialTokenizer* tokenizer = suppliedModels ? suppliedModels->tokenizer : nullptr;
        const Qwen2LanguageModel* language = suppliedModels ? suppliedModels->language : nullptr;
        const Qwen2VisionModel* vision = suppliedModels ? suppliedModels->vision : nullptr;
        Qwen2LanguageModel* loadedLanguage = nullptr;
        Qwen2VisionModel* loadedVision = nullptr;
        const VlmPaths paths = resolveOfficialVlmPaths();
        if (!tokenizer) {
            if (!loadOfficialTokenizer(paths.tokenizerJson, &loadedTokenizer, err))
                return false;
            tokenizer = &loadedTokenizer;
        }
        if (!language) {
            if (!officialLanguageModel(&loadedLanguage, err))
                return false;
            language = loadedLanguage;
        }
        if (!vision) {
            if (!officialVisionModel(&loadedVision, err))
                return false;
            vision = loadedVision;
        }

        VlmPreprocessorConfig preprocessor;
        if (!loadOfficialPreprocessorConfig(paths.preprocessorJson, &preprocessor, err))
            return false;
        for (const PreparedExtract& job : prepared) {
            OfficialImagePatches patches;
            if (!processOfficialImage(job.crop, preprocessor, &patches, err))
                return false;
            const QString chat = applyOfficialChatTemplate(
                officialSystemPrompt(), job.prompt, true, true);
            const QVector<int> promptIds = encodeOfficial(*tokenizer, chat);
            VlmGenerateResult generation;
#if defined(SCANENGINE_MLX)
            const VlmGenerationContract generationContract =
                VlmGenerationContract::explicitMaxNewTokens(backendTokenLimit, job.sampling);
#else
            const VlmGenerationContract generationContract =
                VlmGenerationContract::transformersMaxLength(backendTokenLimit, job.sampling);
#endif
            if (!generateOfficialVlmGreedy(*language,
                                           *vision,
                                           *tokenizer,
                                           promptIds,
                                           patches,
                                           generationContract,
                                           &generation,
                                           err,
                                           isCancelled)) {
                return false;
            }
            QString text = generation.decoded;
            if (text.endsWith(QLatin1String("<|im_end|>")))
                text.chop(QStringLiteral("<|im_end|>").size());
            if (text.endsWith(QLatin1String("<|endoftext|>")))
                text.chop(QStringLiteral("<|endoftext|>").size());
            if (job.blockIndex < 0 || job.blockIndex >= workingPage.size())
                return fail(QStringLiteral("prepared block index is out of range"));
            QJsonObject object = workingPage.at(job.blockIndex).toObject();
            object.insert(QStringLiteral("content"), text);
            workingPage[job.blockIndex] = object;
        }
    }

    injectTableImageMetadata(tableMetadata, &workingPage);
    OfficialPostProcessOptions postOptions;
    // MinerU's formal Hybrid ModelSingleton explicitly enables this even
    // though the reusable MinerUClient constructor defaults it to false.
    postOptions.enableTableFormulaEqWrap = true;
    workingPage = postProcessOfficialBlocks(workingPage, postOptions);
    if (containsTableImagePrivateKeys(workingPage))
        return fail(QStringLiteral("table image private metadata leaked after post-process"));

    *pageBlocks = workingPage;
    if (err)
        err->clear();
    return true;
}

QVector<ContentBlock> parseOfficialLayoutOutput(const QString& output) {
    static const QRegularExpression re(
        QStringLiteral(
            "<\\|box_start\\|>(\\d+)\\s+(\\d+)\\s+(\\d+)\\s+(\\d+)"
            "<\\|box_end\\|><\\|ref_start\\|>(\\w+?)<\\|ref_end\\|>"
            "(?:(<\\|rotate_(?:up|right|down|left)\\|>))?"
            "(.*?)(?=<\\|box_start\\|>|$)"),
        QRegularExpression::DotMatchesEverythingOption);
    QVector<ContentBlock> blocks;
    QRegularExpressionMatchIterator it = re.globalMatch(output);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        int x1 = m.captured(1).toInt();
        int y1 = m.captured(2).toInt();
        int x2 = m.captured(3).toInt();
        int y2 = m.captured(4).toInt();
        if (x1 < 0 || y1 < 0 || x2 < 0 || y2 < 0 || x1 > 1000 || y1 > 1000 || x2 > 1000 || y2 > 1000)
            continue;
        if (x2 < x1)
            std::swap(x1, x2);
        if (y2 < y1)
            std::swap(y1, y2);
        if (x1 == x2 || y1 == y2)
            continue;
        QString ref = m.captured(5).toLower();
        if (ref == QLatin1String("unknown"))
            ref = QStringLiteral("image");
        if (ref == QLatin1String("inline_formula"))
            continue;
        if (!officialBlockType(ref))
            continue;
        ContentBlock b;
        b.type = ref;
        b.xmin = double(x1) / 1000.0;
        b.ymin = double(y1) / 1000.0;
        b.xmax = double(x2) / 1000.0;
        b.ymax = double(y2) / 1000.0;
        const QString rot = m.captured(6);
        if (rot.contains(QLatin1String("rotate_up")))
            b.angle = 0;
        else if (rot.contains(QLatin1String("rotate_right")))
            b.angle = 90;
        else if (rot.contains(QLatin1String("rotate_down")))
            b.angle = 180;
        else if (rot.contains(QLatin1String("rotate_left")))
            b.angle = 270;
        if (ref == QLatin1String("text"))
            b.mergePrev = m.captured(7).contains(QLatin1String("txt_contd_tgt"));
        blocks.push_back(b);
    }
    QVector<int> drop;
    for (int i = 0; i < blocks.size(); ++i) {
        if (blocks[i].type != QLatin1String("text") && blocks[i].type != QLatin1String("equation")
            && blocks[i].type != QLatin1String("equation_block"))
            continue;
        for (int j = 0; j < blocks.size(); ++j) {
            if (blocks[j].type == QLatin1String("table") && coverage(blocks[i], blocks[j]) > 0.9f) {
                drop.push_back(i);
                break;
            }
        }
    }
    if (!drop.isEmpty()) {
        QVector<ContentBlock> kept;
        for (int i = 0; i < blocks.size(); ++i) {
            if (!drop.contains(i))
                kept.push_back(blocks[i]);
        }
        blocks.swap(kept);
    }
    return blocks;
}

bool twoStepExtract(const Qwen2LanguageModel& language,
                    const Qwen2VisionModel& vision,
                    const OfficialTokenizer& tok,
                    const QImage& page,
                    QVector<ContentBlock>* blocks,
                    bool imageAnalysis,
                    const QStringList& notExtractList,
                    int layoutMaxTokens,
                    int extractMaxTokens,
                    QString* rawLayout,
                    QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!blocks)
        return fail(QStringLiteral("blocks is null"));
    const VlmPaths paths = resolveOfficialVlmPaths();
    VlmPreprocessorConfig pre;
    if (!loadOfficialPreprocessorConfig(paths.preprocessorJson, &pre, err))
        return false;
    OfficialImagePatches layoutPatches;
    if (!processOfficialImage(prepareForLayout(page), pre, &layoutPatches, err))
        return false;
    const QString chat = applyOfficialChatTemplate(
        officialSystemPrompt(), officialPromptForType(QStringLiteral("[layout]")), true, true);
    VlmGenerateResult gen;
#if defined(SCANENGINE_MLX)
    const VlmGenerationContract layoutGenerationContract =
        VlmGenerationContract::explicitMaxNewTokens(
            layoutMaxTokens, officialSamplingForType(QStringLiteral("[layout]")));
#else
    const VlmGenerationContract layoutGenerationContract =
        VlmGenerationContract::transformersMaxLength(
            layoutMaxTokens, officialSamplingForType(QStringLiteral("[layout]")));
#endif
    if (!generateOfficialVlmGreedy(language, vision, tok, encodeOfficial(tok, chat), layoutPatches,
                                   layoutGenerationContract,
                                   &gen,
                                   err,
                                   {}))
        return false;
    QString text = gen.decoded;
    if (text.endsWith(QLatin1String("<|im_end|>")))
        text.chop(QStringLiteral("<|im_end|>").size());
    if (rawLayout)
        *rawLayout = text;
    *blocks = parseOfficialLayoutOutput(text);
    return extractWithLayout(language,
                            vision,
                            tok,
                            page,
                            blocks,
                            imageAnalysis,
                            notExtractList,
                            extractMaxTokens,
                            err,
                            {});
}

bool officialHybridReady() {
    return officialHybridMissingFiles().isEmpty();
}

QStringList officialHybridMissingFiles() {
    const VlmPaths vlm = resolveOfficialVlmPaths();
    const QString layoutDir = resolveOfficialLayoutDir();
    const QString ocrDir = resolveOfficialOcrDir();
    const QString magikaDir = resolveOfficialMagikaModelDir();
    const QStringList required = {
        vlm.configJson,
        vlm.tokenizerJson,
        vlm.weights,
        vlm.preprocessorJson,
        QDir(layoutDir).filePath(QStringLiteral("config.json")),
        QDir(layoutDir).filePath(QStringLiteral("model.safetensors")),
        QDir(ocrDir).filePath(QStringLiteral("ch_PP-OCRv6_small_det_infer.safetensors")),
        QDir(ocrDir).filePath(QStringLiteral("ch_PP-OCRv6_small_rec_infer.safetensors")),
        resolveOfficialOcrDict(),
        resolveOfficialFastTextModelPath(),
        QDir(magikaDir).filePath(QStringLiteral("model.onnx")),
        QDir(magikaDir).filePath(QStringLiteral("config.min.json")),
        resolveOfficialMagikaRuntimePath(),
    };
    QStringList missing;
    for (const QString& path : required) {
        const QFileInfo file(path);
        const bool splitWeights = path == vlm.weights
                                  && QFileInfo(path + QStringLiteral(".part1")).size() > 0
                                  && QFileInfo(path + QStringLiteral(".part2")).size() > 0;
        if ((!file.isFile() || file.size() <= 0) && !splitWeights)
            missing << path;
    }
    return missing;
}

bool loadOfficialLayoutJson(const QString& path,
                            QVector<LayoutDet>* dets,
                            int* pageWidth,
                            int* pageHeight,
                            QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!dets)
        return fail(QStringLiteral("dets is null"));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open layout json"));
    QJsonDocument doc;
    QString jsonError;
    if (!parseJsonDocumentStrict(f.readAll(), &doc, &jsonError))
        return fail(QStringLiteral("cannot parse layout json: %1").arg(jsonError));
    if (!doc.isObject())
        return fail(QStringLiteral("layout json must be an object"));
    const QJsonObject root = doc.object();
    const QJsonValue sizeValue = root.value(QStringLiteral("page_size"));
    if (!sizeValue.isArray() || sizeValue.toArray().size() != 2)
        return fail(QStringLiteral("layout page_size must contain width and height"));
    const QJsonArray size = sizeValue.toArray();
    for (const QJsonValue& value : size) {
        const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!value.isDouble() || !std::isfinite(number) || number <= 0.0
            || std::floor(number) != number
            || number > double(std::numeric_limits<int>::max())) {
            return fail(QStringLiteral("layout page_size must contain positive integers"));
        }
    }
    const int width = size.at(0).toInt();
    const int height = size.at(1).toInt();
    if (pageWidth)
        *pageWidth = width;
    if (pageHeight)
        *pageHeight = height;
    dets->clear();
    const QJsonValue detsValue = root.value(QStringLiteral("dets"));
    if (!detsValue.isArray())
        return fail(QStringLiteral("layout dets must be an array"));
    const QJsonArray arr = detsValue.toArray();
    for (int index = 0; index < arr.size(); ++index) {
        const QJsonValue v = arr.at(index);
        if (!v.isObject())
            return fail(QStringLiteral("layout det %1 must be an object").arg(index));
        const QJsonObject o = v.toObject();
        LayoutDet d;
        const QJsonValue labelValue = o.value(QStringLiteral("label"));
        if (!labelValue.isString() || labelValue.toString().isEmpty())
            return fail(QStringLiteral("layout det %1 has no label").arg(index));
        d.label = labelValue.toString();
        const QJsonValue bboxValue = o.value(QStringLiteral("bbox"));
        if (!bboxValue.isArray() || bboxValue.toArray().size() != 4)
            return fail(QStringLiteral("layout det %1 bbox must contain four numbers").arg(index));
        const QJsonArray bb = bboxValue.toArray();
        double coordinates[4] = {};
        for (int coordinate = 0; coordinate < 4; ++coordinate) {
            if (!bb.at(coordinate).isDouble())
                return fail(QStringLiteral("layout det %1 bbox contains a non-number").arg(index));
            coordinates[coordinate] = bb.at(coordinate).toDouble();
            if (!std::isfinite(coordinates[coordinate]))
                return fail(QStringLiteral("layout det %1 bbox is not finite").arg(index));
        }
        if (coordinates[0] < 0.0 || coordinates[1] < 0.0
            || coordinates[2] > width || coordinates[3] > height
            || coordinates[0] >= coordinates[2] || coordinates[1] >= coordinates[3]) {
            return fail(QStringLiteral("layout det %1 bbox is outside page_size or empty").arg(index));
        }
        d.xmin = float(coordinates[0]);
        d.ymin = float(coordinates[1]);
        d.xmax = float(coordinates[2]);
        d.ymax = float(coordinates[3]);
        const QJsonValue angleValue = o.value(QStringLiteral("angle"));
        if (!angleValue.isUndefined() && !angleValue.isNull()) {
            QString angle;
            if (angleValue.isString()) {
                angle = angleValue.toString();
            } else if (angleValue.isDouble()
                       && std::floor(angleValue.toDouble()) == angleValue.toDouble()) {
                angle = QString::number(angleValue.toInt());
            } else {
                return fail(QStringLiteral("layout det %1 angle has an invalid type").arg(index));
            }
            if (angle != QLatin1String("0") && angle != QLatin1String("90")
                && angle != QLatin1String("180") && angle != QLatin1String("270")) {
                return fail(QStringLiteral("layout det %1 angle is invalid").arg(index));
            }
            d.angle = angle;
        }
        dets->push_back(d);
    }
    if (err)
        err->clear();
    return true;
}

bool runOfficialLayoutSidecar(const QString& imagePath, const QString& outJson, QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    const QImage image = loadOfficialHybridPage(imagePath);
    if (image.isNull())
        return fail(QStringLiteral("cannot read layout image"));
    OfficialLayoutOutput result;
    if (!runOfficialLayout(image, &result, err))
        return false;
    if (!writeOfficialLayoutJson(result, outJson, err))
        return false;
    return true;
}

bool runOfficialTableOrientSidecar(const QString& imagePath,
                                   const QString& layoutJson,
                                   const QString& outJson,
                                   QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    const QImage image = loadOfficialHybridPage(imagePath);
    if (image.isNull())
        return fail(QStringLiteral("cannot read image for table orientation"));
    QVector<LayoutDet> dets;
    int w = image.width(), h = image.height();
    if (!loadOfficialLayoutJson(layoutJson, &dets, &w, &h, err))
        return false;
    const QVector<LayoutDet> originalDets = dets;
    if (!applyOfficialTableOrientation(image, &dets, w, h, err)) {
        // MinerU medium treats table-orientation classification as an optional
        // enhancement: a model/crop/prediction failure keeps the complete
        // original layout and continues with angle 0 semantics.  Our
        // classifier is sequential, so also discard any partial table updates.
        const QString warning = err ? *err : QString();
        dets = originalDets;
        fprintf(stderr, "table orientation warning: %s; using original table images\n",
                warning.toUtf8().constData());
        if (err)
            err->clear();
    }
    QFile lf(layoutJson);
    if (!lf.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot reread layout json"));
    QJsonDocument doc;
    QString jsonError;
    if (!parseJsonDocumentStrict(lf.readAll(), &doc, &jsonError))
        return fail(QStringLiteral("cannot parse layout json: %1").arg(jsonError));
    if (!doc.isObject())
        return fail(QStringLiteral("layout json is not an object"));
    QJsonObject root = doc.object();
    QJsonArray arr = root.value(QStringLiteral("dets")).toArray();
    int ti = 0;
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject o = arr.at(i).toObject();
        if (o.value(QStringLiteral("label")).toString() != QLatin1String("table"))
            continue;
        while (ti < dets.size() && dets[ti].label != QLatin1String("table"))
            ++ti;
        if (ti < dets.size()) {
            const QString angle = dets[ti++].angle;
            if (!angle.isEmpty())
                o.insert(QStringLiteral("angle"), angle);
        }
        arr[i] = o;
    }
    root.insert(QStringLiteral("dets"), arr);
    root.insert(QStringLiteral("orientation_engine"), QStringLiteral("pp-doclayout-table-orientation"));
    if (!QDir().mkpath(QFileInfo(outJson).absolutePath()))
        return fail(QStringLiteral("cannot create oriented layout directory"));
    QFile of(outJson);
    if (!of.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("cannot write oriented layout"));
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (of.write(bytes) != bytes.size() || !of.flush())
        return fail(QStringLiteral("cannot write oriented layout bytes"));
    if (err)
        err->clear();
    return true;
}

bool runOfficialMediumHintsSidecar(const QString& layoutJson,
                                   const QString& outJson,
                                   QString* err,
                                   const QString& imagePath,
                                   int pageWidth,
                                   int pageHeight) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (layoutJson.isEmpty() || outJson.isEmpty())
        return fail(QStringLiteral("hybrid-hints requires --layout and --out"));

    QFile file(layoutJson);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open layout json: %1").arg(layoutJson));
    QJsonDocument document;
    QString jsonError;
    if (!parseJsonDocumentStrict(file.readAll(), &document, &jsonError))
        return fail(QStringLiteral("cannot parse layout json: %1").arg(jsonError));

    int width = pageWidth;
    int height = pageHeight;
    QVector<LayoutDet> dets;
    if (document.isObject() && document.object().contains(QStringLiteral("dets"))
        && document.object().contains(QStringLiteral("page_size"))) {
        if (!loadOfficialLayoutJson(layoutJson, &dets, &width, &height, err))
            return false;
    } else {
        QJsonArray pageSize;
        if (!document.isObject() && !document.isArray())
            return fail(QStringLiteral("layout json is not an object or array"));
        QJsonValue value;
        if (document.isObject()) {
            const QJsonObject object = document.object();
            pageSize = object.value(QStringLiteral("page_size")).toArray();
            if (object.contains(QStringLiteral("payload")))
                value = object.value(QStringLiteral("payload"));
            else if (object.contains(QStringLiteral("dets")))
                value = object.value(QStringLiteral("dets"));
            else
                value = object.value(QStringLiteral("blocks"));
        } else {
            value = document.array();
        }
        QJsonArray array = value.toArray();
        while (array.size() == 1 && array.at(0).isArray())
            array = array.at(0).toArray();
        if (width <= 0 || height <= 0) {
            if (pageSize.size() == 2) {
                width = pageSize.at(0).toInt();
                height = pageSize.at(1).toInt();
            }
        }
        if ((width <= 0 || height <= 0) && !imagePath.isEmpty()) {
            const QImage image = loadOfficialHybridPage(imagePath);
            if (image.isNull())
                return fail(QStringLiteral("cannot read image for stage-30 page size"));
            width = image.width();
            height = image.height();
        }
        if (width <= 0 || height <= 0)
            return fail(QStringLiteral("hybrid-hints needs page size from sidecar, --image, or --page-w/--page-h"));
        for (int index = 0; index < array.size(); ++index) {
            if (!array.at(index).isObject())
                return fail(QStringLiteral("layout det %1 must be an object").arg(index));
            const QJsonObject object = array.at(index).toObject();
            LayoutDet det;
            det.label = object.value(QStringLiteral("label")).toString();
            if (det.label.isEmpty())
                return fail(QStringLiteral("layout det %1 has no label").arg(index));
            const QJsonArray bbox = object.value(QStringLiteral("bbox")).toArray();
            if (bbox.size() != 4)
                return fail(QStringLiteral("layout det %1 bbox must contain four numbers").arg(index));
            det.xmin = float(bbox.at(0).toDouble());
            det.ymin = float(bbox.at(1).toDouble());
            det.xmax = float(bbox.at(2).toDouble());
            det.ymax = float(bbox.at(3).toDouble());
            const QJsonValue angleValue = object.value(QStringLiteral("angle"));
            if (angleValue.isString())
                det.angle = angleValue.toString();
            else if (angleValue.isDouble()
                     && std::floor(angleValue.toDouble()) == angleValue.toDouble())
                det.angle = QString::number(angleValue.toInt());
            dets.push_back(det);
        }
    }

    const QVector<ContentBlock> blocks = buildMediumVlmLayoutBlocks(dets, width, height);
    if (!writeOfficialModelJson(outJson, blocks, err))
        return false;
    if (err)
        err->clear();
    return true;
}

bool runOfficialOcrSidecar(const QString& imagePath,
                           const QString& blocksJson,
                           const QString& layoutJson,
                           const QString& outJson,
                           QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    const QImage image = loadOfficialHybridPage(imagePath);
    if (image.isNull())
        return fail(QStringLiteral("cannot read image for ocr sidecar"));
    QFile bf(blocksJson);
    if (!bf.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open blocks json"));
    QJsonDocument bdoc;
    QString blocksParseError;
    if (!parseJsonDocumentStrict(bf.readAll(), &bdoc, &blocksParseError))
        return fail(QStringLiteral("cannot parse blocks json: %1").arg(blocksParseError));
    QVector<ContentBlock> blocks;
    QJsonArray barr;
    if (bdoc.isArray() && bdoc.array().size() == 1 && bdoc.array().at(0).isArray()) {
        barr = bdoc.array().at(0).toArray();
    } else if (bdoc.isArray()) {
        barr = bdoc.array();
    } else if (bdoc.isObject()) {
        const QJsonObject root = bdoc.object();
        const QJsonValue blocksValue = root.value(QStringLiteral("blocks"));
        const QJsonValue payloadValue = root.value(QStringLiteral("payload"));
        if (blocksValue.isArray()) {
            barr = blocksValue.toArray();
        } else if (payloadValue.isArray() && payloadValue.toArray().size() == 1
                   && payloadValue.toArray().at(0).isArray()) {
            barr = payloadValue.toArray().at(0).toArray();
        } else {
            return fail(QStringLiteral("blocks json has no single-page block array"));
        }
    } else {
        return fail(QStringLiteral("blocks json must be an array or object envelope"));
    }
    static const QSet<QString> blockTypes = {
        QStringLiteral("text"), QStringLiteral("title"), QStringLiteral("doc_title"),
        QStringLiteral("paragraph_title"), QStringLiteral("table"), QStringLiteral("equation"),
        QStringLiteral("formula_number"), QStringLiteral("code"), QStringLiteral("algorithm"),
        QStringLiteral("aside_text"), QStringLiteral("ref_text"), QStringLiteral("index"),
        QStringLiteral("phonetic"), QStringLiteral("list_item"), QStringLiteral("table_caption"),
        QStringLiteral("image_caption"), QStringLiteral("code_caption"), QStringLiteral("caption"),
        QStringLiteral("table_footnote"), QStringLiteral("image_footnote"),
        QStringLiteral("footnote"), QStringLiteral("header"), QStringLiteral("footer"),
        QStringLiteral("page_number"), QStringLiteral("page_footnote"), QStringLiteral("image"),
        QStringLiteral("chart"), QStringLiteral("list"), QStringLiteral("image_block"),
        QStringLiteral("equation_block"), QStringLiteral("unknown"),
    };
    for (int index = 0; index < barr.size(); ++index) {
        const QJsonValue v = barr.at(index);
        if (!v.isObject())
            return fail(QStringLiteral("block %1 must be an object").arg(index));
        const QJsonObject o = v.toObject();
        ContentBlock b;
        const QJsonValue typeValue = o.value(QStringLiteral("type"));
        if (!typeValue.isString() || !blockTypes.contains(typeValue.toString()))
            return fail(QStringLiteral("block %1 has unknown or missing type").arg(index));
        b.type = typeValue.toString();
        const QJsonValue bboxValue = o.value(QStringLiteral("bbox"));
        if (!bboxValue.isArray() || bboxValue.toArray().size() != 4)
            return fail(QStringLiteral("block %1 bbox must contain four numbers").arg(index));
        const QJsonArray bb = bboxValue.toArray();
        double coordinates[4] = {};
        for (int coordinate = 0; coordinate < 4; ++coordinate) {
            if (!bb.at(coordinate).isDouble())
                return fail(QStringLiteral("block %1 bbox contains a non-number").arg(index));
            coordinates[coordinate] = bb.at(coordinate).toDouble();
            if (!std::isfinite(coordinates[coordinate]) || coordinates[coordinate] < 0.0
                || coordinates[coordinate] > 1.0) {
                return fail(QStringLiteral("block %1 bbox must be finite and normalized").arg(index));
            }
        }
        if (coordinates[0] >= coordinates[2] || coordinates[1] >= coordinates[3])
            return fail(QStringLiteral("block %1 bbox must have positive area").arg(index));
        b.xmin = coordinates[0];
        b.ymin = coordinates[1];
        b.xmax = coordinates[2];
        b.ymax = coordinates[3];
        const QJsonValue contentValue = o.value(QStringLiteral("content"));
        if (!contentValue.isUndefined() && !contentValue.isNull() && !contentValue.isString())
            return fail(QStringLiteral("block %1 content must be null or a string").arg(index));
        b.contentNull = contentValue.isNull();
        if (contentValue.isString())
            b.content = contentValue.toString();
        blocks.push_back(b);
    }
    QVector<LayoutDet> dets;
    int w = image.width(), h = image.height();
    if (!loadOfficialLayoutJson(layoutJson, &dets, &w, &h, err))
        return false;
    return writeOfficialOcrSidecar(image, blocks, dets, outJson, err);
}

bool mergeOfficialOcrSidecarItems(const QJsonObject& sidecar, QJsonArray* page, QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!page)
        return fail(QStringLiteral("page is null"));
    const QJsonArray items = sidecar.value(QStringLiteral("items")).toArray();
    for (const QString& expectedType : {QStringLiteral("inline_formula"),
                                        QStringLiteral("ocr_text")}) {
        for (const QJsonValue& v : items) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("type")).toString() != expectedType
                || !o.contains(QStringLiteral("bbox"))) {
                continue;
            }
            page->append(o);
        }
    }
    if (err)
        err->clear();
    return true;
}

namespace {
QByteArray officialJsonBytes(const QJsonDocument& document, bool modelBboxesAreFloat = false);
}

bool readOfficialModelJson(const QString& path, QJsonArray* page, QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!page)
        return fail(QStringLiteral("page is null"));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open model json: %1").arg(path));
    QJsonDocument doc;
    QString jsonError;
    if (!parseJsonDocumentStrict(f.readAll(), &doc, &jsonError))
        return fail(QStringLiteral("cannot parse model json: %1 (%2)").arg(path, jsonError));
    if (!doc.isArray() || doc.array().size() != 1 || !doc.array().at(0).isArray())
        return fail(QStringLiteral("model json is not a single-page [[...]] envelope: %1")
                        .arg(path));
    *page = doc.array().at(0).toArray();
    if (err)
        err->clear();
    return true;
}

bool writeOfficialModelJson(const QString& path, const QJsonArray& page, QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(QStringLiteral("cannot create model json directory"));
    // MinerU normalizes every model bbox to the unit coordinate space and
    // rounds it to three decimal places before stage 40. Keep that public
    // stage/model contract even for callers that provide higher-precision
    // doubles through the typed compatibility API.
    QJsonArray canonicalPage = page;
    for (int i = 0; i < canonicalPage.size(); ++i) {
        if (!canonicalPage.at(i).isObject())
            continue;
        QJsonObject block = canonicalPage.at(i).toObject();
        const QJsonArray bbox = block.value(QStringLiteral("bbox")).toArray();
        if (bbox.size() != 4)
            continue;
        QJsonArray rounded;
        for (const QJsonValue& coordinate : bbox)
            rounded.append(roundToDecimalDigits(coordinate.toDouble(), 3));
        block.insert(QStringLiteral("bbox"), rounded);
        canonicalPage[i] = block;
    }
    QJsonArray root;
    root.append(canonicalPage);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("cannot write model json"));
    const QByteArray bytes = officialJsonBytes(QJsonDocument(root), true);
    if (f.write(bytes) != bytes.size() || !f.flush())
        return fail(QStringLiteral("cannot write model json bytes"));
    if (err)
        err->clear();
    return true;
}

bool writeOfficialModelJson(const QString& path, const QVector<ContentBlock>& blocks, QString* err) {
    return writeOfficialModelJson(path, officialContentBlocksJson(blocks), err);
}

QImage cropOfficialHybridPage(const QImage& page,
                              const QJsonArray& pdfBbox,
                              double renderScale) {
    if (page.isNull() || pdfBbox.size() != 4 || !std::isfinite(renderScale)
        || renderScale <= 0.0) {
        return QImage();
    }

    const double minInt = double(std::numeric_limits<int>::min());
    const double maxInt = double(std::numeric_limits<int>::max());
    double scaled[4];
    for (int i = 0; i < 4; ++i) {
        scaled[i] = pdfBbox.at(i).toDouble() * renderScale;
        if (!std::isfinite(scaled[i]) || scaled[i] < minInt || scaled[i] > maxInt)
            return QImage();
    }

    // normalize_to_int_bbox followed by PIL.Image.crop.
    const int x0 = int(std::floor(scaled[0]));
    const int y0 = int(std::floor(scaled[1]));
    const int x1 = int(std::ceil(scaled[2]));
    const int y1 = int(std::ceil(scaled[3]));
    if (x1 <= x0 || y1 <= y0)
        return QImage();

    const qint64 cropWidth = qint64(x1) - qint64(x0);
    const qint64 cropHeight = qint64(y1) - qint64(y0);
    if (cropWidth > std::numeric_limits<int>::max()
        || cropHeight > std::numeric_limits<int>::max()) {
        return QImage();
    }

    // Pillow preserves the requested rectangle and fills any part outside the
    // source image with black.  The formal corpus is in-bounds, but matching
    // this behavior keeps the helper exact for future fixtures too.
    const QImage rgb = page.convertToFormat(QImage::Format_RGB888);
    QImage crop(int(cropWidth), int(cropHeight), QImage::Format_RGB888);
    if (crop.isNull())
        return QImage();
    crop.fill(Qt::black);
    const int sourceX0 = std::max(0, x0);
    const int sourceY0 = std::max(0, y0);
    const int sourceX1 = std::min(rgb.width(), x1);
    const int sourceY1 = std::min(rgb.height(), y1);
    const int copyWidth = sourceX1 - sourceX0;
    if (copyWidth > 0 && sourceY1 > sourceY0) {
        const int targetX = sourceX0 - x0;
        const int targetY = sourceY0 - y0;
        for (int sourceY = sourceY0; sourceY < sourceY1; ++sourceY) {
            const uchar* source = rgb.constScanLine(sourceY) + sourceX0 * 3;
            uchar* target = crop.scanLine(targetY + sourceY - sourceY0) + targetX * 3;
            std::memcpy(target, source, size_t(copyWidth) * 3);
        }
    }
    return crop;
}

namespace {

QJsonArray pxBbox(double xmin, double ymin, double xmax, double ymax, int pw, int ph) {
    // MagicModel.cal_real_bbox
    int x0 = int(xmin * double(pw));
    int y0 = int(ymin * double(ph));
    int x1 = int(xmax * double(pw));
    int y1 = int(ymax * double(ph));
    if (x1 < x0)
        std::swap(x0, x1);
    if (y1 < y0)
        std::swap(y0, y1);
    return QJsonArray({x0, y0, x1, y1});
}

QJsonArray pxBbox(const QJsonArray& bbox, int pw, int ph) {
    if (bbox.size() != 4)
        return {};
    return pxBbox(bbox.at(0).toDouble(), bbox.at(1).toDouble(), bbox.at(2).toDouble(),
                  bbox.at(3).toDouble(), pw, ph);
}

QString pageRgbMd5(const QImage& input) {
    const QImage image = input.convertToFormat(QImage::Format_RGB888);
    QCryptographicHash hash(QCryptographicHash::Md5);
    const int rowBytes = image.width() * 3;
    for (int y = 0; y < image.height(); ++y)
        hash.addData(reinterpret_cast<const char*>(image.constScanLine(y)), rowBytes);
    return QString::fromLatin1(hash.result().toHex().toUpper());
}

bool visualImagePath(const QImage& renderedPage,
                     const QJsonArray& pdfBbox,
                     int pageWidth,
                     int pageHeight,
                     const QString& imageDir,
                     const QString& spanType,
                     QString* filenameOut,
                     QString* error) {
    if (filenameOut)
        filenameOut->clear();
    if (renderedPage.isNull() || imageDir.isEmpty())
        return true;
    if (pdfBbox.size() != 4 || pageWidth < 1 || pageHeight < 1) {
        if (error)
            *error = QStringLiteral("invalid visual crop geometry");
        return false;
    }
    const QString seed = QStringLiteral("%1/%2_0_%3_%4_%5_%6")
                             .arg(spanType)
                             .arg(pageRgbMd5(renderedPage))
                             .arg(pdfBbox.at(0).toInt())
                             .arg(pdfBbox.at(1).toInt())
                             .arg(pdfBbox.at(2).toInt())
                             .arg(pdfBbox.at(3).toInt());
    const QString filename =
        QString::fromLatin1(QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256)
                                .toHex())
        + QStringLiteral(".jpg");
    double scale = officialHybridPageRenderScale(renderedPage);
    if (scale <= 0.0) {
        // Compatibility fallback for callers that provide an arbitrary
        // QImage instead of loadOfficialHybridPage's tagged image.
        scale = std::min(double(renderedPage.width()) / double(pageWidth),
                         double(renderedPage.height()) / double(pageHeight));
    }
    const QImage crop = cropOfficialHybridPage(renderedPage, pdfBbox, scale);
    if (crop.isNull()) {
        if (error)
            *error = QStringLiteral("cannot crop %1 block").arg(spanType);
        return false;
    }
    if (!QDir().mkpath(imageDir)) {
        if (error)
            *error = QStringLiteral("cannot create image directory: %1").arg(imageDir);
        return false;
    }
    if (!writeOfficialHybridCropJpeg(crop, QDir(imageDir).filePath(filename), error))
        return false;
    if (filenameOut)
        *filenameOut = filename;
    return true;
}

double bboxArea(const QJsonArray& bbox) {
    if (bbox.size() != 4)
        return 0.0;
    return std::max(0.0, bbox.at(2).toDouble() - bbox.at(0).toDouble())
           * std::max(0.0, bbox.at(3).toDouble() - bbox.at(1).toDouble());
}

double spanOverlapRatio(const QJsonArray& span, const QJsonArray& block) {
    if (span.size() != 4 || block.size() != 4)
        return 0.0;
    const double x0 = std::max(span.at(0).toDouble(), block.at(0).toDouble());
    const double y0 = std::max(span.at(1).toDouble(), block.at(1).toDouble());
    const double x1 = std::min(span.at(2).toDouble(), block.at(2).toDouble());
    const double y1 = std::min(span.at(3).toDouble(), block.at(3).toDouble());
    const double intersection = std::max(0.0, x1 - x0) * std::max(0.0, y1 - y0);
    const double area = bboxArea(span);
    return area > 0.0 ? intersection / area : 0.0;
}

bool supportsOcrDetLines(const QString& type) {
    return type == QLatin1String("text") || type == QLatin1String("title")
           || type == QLatin1String("doc_title") || type == QLatin1String("paragraph_title");
}

QJsonArray lineBbox(const QJsonArray& spans) {
    if (spans.isEmpty())
        return {};
    double x0 = std::numeric_limits<double>::infinity();
    double y0 = std::numeric_limits<double>::infinity();
    double x1 = -std::numeric_limits<double>::infinity();
    double y1 = -std::numeric_limits<double>::infinity();
    for (const QJsonValue& v : spans) {
        const QJsonArray bb = v.toObject().value(QStringLiteral("bbox")).toArray();
        if (bb.size() != 4)
            continue;
        x0 = std::min(x0, bb.at(0).toDouble());
        y0 = std::min(y0, bb.at(1).toDouble());
        x1 = std::max(x1, bb.at(2).toDouble());
        y1 = std::max(y1, bb.at(3).toDouble());
    }
    if (!std::isfinite(x0))
        return {};
    return QJsonArray({int(x0), int(y0), int(x1), int(y1)});
}

double axisOverlapRatio(const QJsonArray& a, const QJsonArray& b, bool verticalAxis) {
    if (a.size() != 4 || b.size() != 4)
        return 0.0;
    const int lo = verticalAxis ? 1 : 0;
    const int hi = verticalAxis ? 3 : 2;
    const double overlap = std::max(0.0, std::min(a.at(hi).toDouble(), b.at(hi).toDouble())
                                            - std::max(a.at(lo).toDouble(), b.at(lo).toDouble()));
    const double length = std::min(a.at(hi).toDouble() - a.at(lo).toDouble(),
                                   b.at(hi).toDouble() - b.at(lo).toDouble());
    return length > 0.0 ? overlap / length : 0.0;
}

QJsonArray buildOcrDetLines(QJsonArray spans) {
    if (spans.isEmpty())
        return {};
    int valid = 0;
    int vertical = 0;
    for (const QJsonValue& v : spans) {
        const QJsonArray bb = v.toObject().value(QStringLiteral("bbox")).toArray();
        if (bb.size() != 4)
            continue;
        const double width = bb.at(2).toDouble() - bb.at(0).toDouble();
        const double height = bb.at(3).toDouble() - bb.at(1).toDouble();
        if (width <= 0.0 || height <= 0.0)
            continue;
        ++valid;
        if (height / width > 2.0)
            ++vertical;
    }
    const bool verticalText = valid > 0 && double(vertical) / double(valid) > 0.8;
    std::sort(spans.begin(), spans.end(), [verticalText](const QJsonValue& a, const QJsonValue& b) {
        const QJsonArray aa = a.toObject().value(QStringLiteral("bbox")).toArray();
        const QJsonArray bb = b.toObject().value(QStringLiteral("bbox")).toArray();
        return verticalText ? aa.at(2).toDouble() > bb.at(2).toDouble()
                            : aa.at(1).toDouble() < bb.at(1).toDouble();
    });

    QVector<QJsonArray> groups;
    for (const QJsonValue& span : spans) {
        if (groups.isEmpty()) {
            groups.push_back(QJsonArray({span}));
            continue;
        }
        // span_block_fix.merge_spans_to_{,vertical_}line compares the next
        // span to current_line[-1].  Using the union creates a bridge through
        // a tall/offset span and incorrectly collapses separate lines.
        const QJsonArray currentBbox =
            groups.last().last().toObject().value(QStringLiteral("bbox")).toArray();
        const QJsonArray spanBbox = span.toObject().value(QStringLiteral("bbox")).toArray();
        const bool sameLine = axisOverlapRatio(currentBbox, spanBbox, !verticalText) > 0.6;
        if (sameLine)
            groups.last().append(span);
        else
            groups.push_back(QJsonArray({span}));
    }

    QJsonArray lines;
    for (QJsonArray group : groups) {
        std::sort(group.begin(), group.end(), [verticalText](const QJsonValue& a, const QJsonValue& b) {
            const QJsonArray aa = a.toObject().value(QStringLiteral("bbox")).toArray();
            const QJsonArray bb = b.toObject().value(QStringLiteral("bbox")).toArray();
            return verticalText ? aa.at(1).toDouble() < bb.at(1).toDouble()
                                : aa.at(0).toDouble() < bb.at(0).toDouble();
        });
        QJsonObject line;
        line.insert(QStringLiteral("bbox"), lineBbox(group));
        line.insert(QStringLiteral("spans"), group);
        lines.append(line);
    }
    return lines;
}

int averageLineHeight(const QJsonArray& lines, const QJsonArray& fallback) {
    double sum = 0.0;
    int count = 0;
    for (const QJsonValue& v : lines) {
        const QJsonArray bb = v.toObject().value(QStringLiteral("bbox")).toArray();
        if (bb.size() == 4 && bb.at(3).toDouble() > bb.at(1).toDouble()) {
            sum += bb.at(3).toDouble() - bb.at(1).toDouble();
            ++count;
        }
    }
    if (count > 0)
        return roundHalfToEven(sum / double(count));
    if (fallback.size() == 4)
        return int(fallback.at(3).toDouble() - fallback.at(1).toDouble());
    return 0;
}

QString jsonBlockType(const QJsonObject& block) {
    return block.value(QStringLiteral("type")).toString();
}

int jsonBlockIndex(const QJsonObject& block) {
    return block.value(QStringLiteral("index")).toInt();
}

double bboxCoordinate(const QJsonArray& bbox, int index) {
    return bbox.size() == 4 ? bbox.at(index).toDouble() : 0.0;
}

double overlapAreaRatioInFirst(const QJsonArray& first, const QJsonArray& second) {
    if (first.size() != 4 || second.size() != 4)
        return 0.0;
    const double x0 = std::max(bboxCoordinate(first, 0), bboxCoordinate(second, 0));
    const double y0 = std::max(bboxCoordinate(first, 1), bboxCoordinate(second, 1));
    const double x1 = std::min(bboxCoordinate(first, 2), bboxCoordinate(second, 2));
    const double y1 = std::min(bboxCoordinate(first, 3), bboxCoordinate(second, 3));
    const double intersection = std::max(0.0, x1 - x0) * std::max(0.0, y1 - y0);
    const double area = bboxArea(first);
    return area > 0.0 ? intersection / area : 0.0;
}

bool isVisualMainType(const QString& type) {
    return type == QLatin1String("image_body") || type == QLatin1String("image_block_body")
           || type == QLatin1String("table_body") || type == QLatin1String("chart_body")
           || type == QLatin1String("code_body");
}

bool isGenericVisualChildType(const QString& type) {
    return type == QLatin1String("caption") || type == QLatin1String("footnote");
}

QString visualRootType(const QString& bodyType) {
    if (bodyType == QLatin1String("table_body"))
        return QStringLiteral("table");
    if (bodyType == QLatin1String("chart_body"))
        return QStringLiteral("chart");
    if (bodyType == QLatin1String("code_body"))
        return QStringLiteral("code");
    return QStringLiteral("image");
}

QString visualChildType(const QString& rootType, const QString& genericType) {
    return rootType + (genericType == QLatin1String("caption")
                           ? QStringLiteral("_caption")
                           : QStringLiteral("_footnote"));
}

bool jsonValueIsTruthy(const QJsonValue& value) {
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

bool bboxesOverlap(const QJsonArray& first, const QJsonArray& second) {
    if (first.size() != 4 || second.size() != 4)
        return false;
    return !(bboxCoordinate(first, 2) <= bboxCoordinate(second, 0)
             || bboxCoordinate(first, 0) >= bboxCoordinate(second, 2)
             || bboxCoordinate(first, 3) <= bboxCoordinate(second, 1)
             || bboxCoordinate(first, 1) >= bboxCoordinate(second, 3));
}

double bboxEdgeDistance(const QJsonArray& first, const QJsonArray& second) {
    const double ax0 = bboxCoordinate(first, 0);
    const double ay0 = bboxCoordinate(first, 1);
    const double ax1 = bboxCoordinate(first, 2);
    const double ay1 = bboxCoordinate(first, 3);
    const double bx0 = bboxCoordinate(second, 0);
    const double by0 = bboxCoordinate(second, 1);
    const double bx1 = bboxCoordinate(second, 2);
    const double by1 = bboxCoordinate(second, 3);
    const bool left = bx1 < ax0;
    const bool right = ax1 < bx0;
    const bool bottom = by1 < ay0;
    const bool top = ay1 < by0;
    auto pointDistance = [](double x0, double y0, double x1, double y1) {
        return std::hypot(x0 - x1, y0 - y1);
    };
    if (top && left)
        return pointDistance(ax0, ay1, bx1, by0);
    if (left && bottom)
        return pointDistance(ax0, ay0, bx1, by1);
    if (bottom && right)
        return pointDistance(ax1, ay0, bx0, by1);
    if (right && top)
        return pointDistance(ax1, ay1, bx0, by0);
    if (left)
        return ax0 - bx1;
    if (right)
        return bx0 - ax1;
    if (bottom)
        return ay0 - by1;
    if (top)
        return by0 - ay1;
    return 0.0;
}

double bboxCenterDistance(const QJsonArray& first, const QJsonArray& second) {
    const double ax = (bboxCoordinate(first, 0) + bboxCoordinate(first, 2)) / 2.0;
    const double ay = (bboxCoordinate(first, 1) + bboxCoordinate(first, 3)) / 2.0;
    const double bx = (bboxCoordinate(second, 0) + bboxCoordinate(second, 2)) / 2.0;
    const double by = (bboxCoordinate(second, 1) + bboxCoordinate(second, 3)) / 2.0;
    return std::hypot(ax - bx, ay - by);
}

bool isTransparentEmptyList(const QJsonObject& block) {
    if (jsonBlockType(block) != QLatin1String("list"))
        return false;
    if (!block.value(QStringLiteral("blocks")).toArray().isEmpty())
        return false;
    for (const QJsonValue& lineValue : block.value(QStringLiteral("lines")).toArray()) {
        for (const QJsonValue& spanValue :
             lineValue.toObject().value(QStringLiteral("spans")).toArray()) {
            if (!spanValue.toObject().value(QStringLiteral("content")).toString().trimmed().isEmpty())
                return false;
        }
    }
    return true;
}

bool isOutsideVisualGap(const QJsonObject& between,
                        const QJsonObject& child,
                        const QJsonObject& main) {
    const QJsonArray childBbox = child.value(QStringLiteral("bbox")).toArray();
    const QJsonArray mainBbox = main.value(QStringLiteral("bbox")).toArray();
    double gapTop = 0.0;
    double gapBottom = 0.0;
    if (bboxCoordinate(childBbox, 3) <= bboxCoordinate(mainBbox, 1)) {
        gapTop = bboxCoordinate(childBbox, 3);
        gapBottom = bboxCoordinate(mainBbox, 1);
    } else if (bboxCoordinate(mainBbox, 3) <= bboxCoordinate(childBbox, 1)) {
        gapTop = bboxCoordinate(mainBbox, 3);
        gapBottom = bboxCoordinate(childBbox, 1);
    } else {
        return false;
    }
    const QJsonArray bbox = between.value(QStringLiteral("bbox")).toArray();
    if (bboxesOverlap(bbox, childBbox) || bboxesOverlap(bbox, mainBbox))
        return false;
    return !(bboxCoordinate(bbox, 1) < gapBottom && bboxCoordinate(bbox, 3) > gapTop);
}

bool isVisualNeighbor(const QJsonObject& child,
                      const QJsonObject& main,
                      const QVector<QJsonObject>& ordered,
                      const QHash<int, int>& positionByIndex) {
    const QString childType = jsonBlockType(child);
    if (childType == QLatin1String("footnote") && jsonBlockIndex(child) < jsonBlockIndex(main))
        return false;
    const int childPos = positionByIndex.value(jsonBlockIndex(child), -1);
    const int mainPos = positionByIndex.value(jsonBlockIndex(main), -1);
    if (childPos < 0 || mainPos < 0)
        return false;
    const int start = std::min(childPos, mainPos) + 1;
    const int end = std::max(childPos, mainPos);
    for (int position = start; position < end; ++position) {
        const QJsonObject between = ordered.at(position);
        const QString betweenType = jsonBlockType(between);
        const bool allowed = childType == QLatin1String("caption")
                                 ? betweenType == QLatin1String("caption")
                                 : isGenericVisualChildType(betweenType);
        if (allowed || isOutsideVisualGap(between, child, main))
            continue;
        return false;
    }
    return true;
}

int effectiveVisualIndexDiff(const QJsonObject& child,
                             const QJsonObject& main,
                             const QVector<QJsonObject>& ordered,
                             const QHash<int, int>& positionByIndex) {
    const int childPos = positionByIndex.value(jsonBlockIndex(child));
    const int mainPos = positionByIndex.value(jsonBlockIndex(main));
    const int start = std::min(childPos, mainPos);
    const int end = std::max(childPos, mainPos);
    int skipped = 0;
    for (int position = start + 1; position < end; ++position) {
        if (jsonBlockType(ordered.at(position)) == jsonBlockType(child))
            ++skipped;
    }
    return end - start - skipped;
}

int findBestVisualParent(const QJsonObject& child,
                         const QVector<QJsonObject>& mains,
                         const QVector<QJsonObject>& ordered,
                         const QHash<int, int>& positionByIndex) {
    QVector<int> candidates;
    for (int index = 0; index < mains.size(); ++index) {
        if (isVisualNeighbor(child, mains.at(index), ordered, positionByIndex))
            candidates.push_back(index);
    }
    if (candidates.isEmpty())
        return -1;
    int minimumDiff = std::numeric_limits<int>::max();
    for (int candidate : candidates)
        minimumDiff = std::min(minimumDiff,
                               effectiveVisualIndexDiff(child, mains.at(candidate), ordered,
                                                        positionByIndex));
    QVector<int> closest;
    for (int candidate : candidates) {
        if (effectiveVisualIndexDiff(child, mains.at(candidate), ordered, positionByIndex)
            == minimumDiff) {
            closest.push_back(candidate);
        }
    }
    if (closest.size() == 1)
        return closest.first();
    double minimumEdge = std::numeric_limits<double>::infinity();
    double maximumEdge = -std::numeric_limits<double>::infinity();
    int nearestEdge = closest.first();
    for (int candidate : closest) {
        const double distance = bboxEdgeDistance(
            child.value(QStringLiteral("bbox")).toArray(),
            mains.at(candidate).value(QStringLiteral("bbox")).toArray());
        minimumEdge = std::min(minimumEdge, distance);
        maximumEdge = std::max(maximumEdge, distance);
        const double currentNearest = bboxEdgeDistance(
            child.value(QStringLiteral("bbox")).toArray(),
            mains.at(nearestEdge).value(QStringLiteral("bbox")).toArray());
        if (distance < currentNearest
            || (distance == currentNearest
                && jsonBlockIndex(mains.at(candidate)) < jsonBlockIndex(mains.at(nearestEdge)))) {
            nearestEdge = candidate;
        }
    }
    if (maximumEdge - minimumEdge > 2.0)
        return nearestEdge;
    if (jsonBlockType(child) == QLatin1String("caption")) {
        bool allTables = true;
        for (int candidate : closest)
            allTables = allTables && visualRootType(jsonBlockType(mains.at(candidate))) == QLatin1String("table");
        if (allTables) {
            return *std::max_element(closest.begin(), closest.end(), [&](int a, int b) {
                return jsonBlockIndex(mains.at(a)) < jsonBlockIndex(mains.at(b));
            });
        }
    }
    if (jsonBlockType(child) == QLatin1String("footnote")) {
        return *std::min_element(closest.begin(), closest.end(), [&](int a, int b) {
            return jsonBlockIndex(mains.at(a)) < jsonBlockIndex(mains.at(b));
        });
    }
    int nearestCenter = closest.first();
    for (int candidate : closest) {
        const double distance = bboxCenterDistance(
            child.value(QStringLiteral("bbox")).toArray(),
            mains.at(candidate).value(QStringLiteral("bbox")).toArray());
        const double nearestDistance = bboxCenterDistance(
            child.value(QStringLiteral("bbox")).toArray(),
            mains.at(nearestCenter).value(QStringLiteral("bbox")).toArray());
        if (distance < nearestDistance
            || (distance == nearestDistance
                && jsonBlockIndex(mains.at(candidate)) < jsonBlockIndex(mains.at(nearestCenter)))) {
            nearestCenter = candidate;
        }
    }
    return nearestCenter;
}

bool isVisualRelationIgnored(const QString& type) {
    return type == QLatin1String("header") || type == QLatin1String("footer")
           || type == QLatin1String("page_number") || type == QLatin1String("page_footnote")
           || type == QLatin1String("aside_text");
}

QString blockVisibleText(const QJsonObject& block) {
    QString result;
    for (const QJsonValue& lineValue : block.value(QStringLiteral("lines")).toArray()) {
        for (const QJsonValue& spanValue :
             lineValue.toObject().value(QStringLiteral("spans")).toArray()) {
            result += spanValue.toObject().value(QStringLiteral("content")).toString();
        }
    }
    return result;
}

bool isSingleLineCaptionFragment(const QJsonObject& block) {
    return block.value(QStringLiteral("lines")).toArray().size() <= 1;
}

bool isInlineCaptionFragmentType(const QString& type) {
    return type == QLatin1String("text") || type == QLatin1String("footnote");
}

bool isTableMainType(const QString& type) {
    return type == QLatin1String("table_body");
}

bool isHorizontallyNearTable(const QJsonObject& block, const QJsonObject& table) {
    const QJsonArray tableBbox = table.value(QStringLiteral("bbox")).toArray();
    const QJsonArray blockBbox = block.value(QStringLiteral("bbox")).toArray();
    const double tableWidth = std::max(bboxCoordinate(tableBbox, 2)
                                           - bboxCoordinate(tableBbox, 0),
                                       1.0);
    const double tolerance = std::max(12.0, tableWidth * 0.03);
    return !(bboxCoordinate(blockBbox, 2) < bboxCoordinate(tableBbox, 0) - tolerance
             || bboxCoordinate(blockBbox, 0) > bboxCoordinate(tableBbox, 2) + tolerance);
}

double stackedCaptionMaxGap(double blockHeight) {
    return std::max(12.0, blockHeight * 1.5);
}

bool inlineCaptionGeometryMatches(const QJsonObject& previousCaption,
                                  const QJsonObject& textBlock,
                                  const QJsonObject& nextVisual) {
    const QJsonArray captionBbox = previousCaption.value(QStringLiteral("bbox")).toArray();
    const QJsonArray textBbox = textBlock.value(QStringLiteral("bbox")).toArray();
    const QJsonArray visualBbox = nextVisual.value(QStringLiteral("bbox")).toArray();
    const double captionHeight = std::max(bboxCoordinate(captionBbox, 3)
                                              - bboxCoordinate(captionBbox, 1),
                                          1.0);
    const double textHeight = std::max(bboxCoordinate(textBbox, 3)
                                           - bboxCoordinate(textBbox, 1),
                                       1.0);
    const double minimumHeight = std::max(std::min(captionHeight, textHeight), 1.0);
    const double verticalOverlap = std::min(bboxCoordinate(captionBbox, 3),
                                            bboxCoordinate(textBbox, 3))
                                   - std::max(bboxCoordinate(captionBbox, 1),
                                              bboxCoordinate(textBbox, 1));
    const double captionCenter = (bboxCoordinate(captionBbox, 1)
                                  + bboxCoordinate(captionBbox, 3))
                                 / 2.0;
    const double textCenter = (bboxCoordinate(textBbox, 1) + bboxCoordinate(textBbox, 3)) / 2.0;
    const bool sameLine = verticalOverlap / minimumHeight >= 0.6
                          || std::abs(captionCenter - textCenter)
                                 <= std::max(captionHeight, textHeight) * 0.5;
    if (!sameLine)
        return false;
    const double gap = bboxCoordinate(visualBbox, 1)
                       - std::max(bboxCoordinate(captionBbox, 3),
                                  bboxCoordinate(textBbox, 3));
    const double maximumGap = std::max(12.0, std::max(captionHeight, textHeight) * 1.5);
    return gap >= 0.0 && gap <= maximumGap;
}

QString fullWidthToHalfWidth(QString text) {
    for (int index = 0; index < text.size(); ++index) {
        const ushort code = text.at(index).unicode();
        if (code == 0x3000)
            text[index] = QLatin1Char(' ');
        else if (code >= 0xff01 && code <= 0xff5e)
            text[index] = QChar(code - 0xfee0);
    }
    return text;
}

bool isTableContinuationText(const QString& rawText) {
    const QString text = fullWidthToHalfWidth(rawText.trimmed()).toLower();
    if (text.isEmpty())
        return false;
    static const QStringList endMarkers = {
        QString::fromUtf8("(续)"), QString::fromUtf8("(续表)"),
        QString::fromUtf8("(续上表)"), QStringLiteral("(continued)"),
        QStringLiteral("(cont.)"), QString::fromUtf8("(cont’d)"),
        QString::fromUtf8("(…continued)"), QStringLiteral("continued"),
        QString::fromUtf8("续表"),
    };
    for (const QString& marker : endMarkers) {
        if (!text.endsWith(marker))
            continue;
        if (marker != QLatin1String("continued"))
            return true;
        const int markerStart = text.size() - marker.size();
        if (markerStart == 0 || !text.at(markerStart - 1).isLetter())
            return true;
    }
    return text.contains(QStringLiteral("(continued)"));
}

QVector<int> orderedBlockPositions(const QVector<QJsonObject>& blocks) {
    QVector<int> positions;
    positions.reserve(blocks.size());
    for (int index = 0; index < blocks.size(); ++index)
        positions.push_back(index);
    std::stable_sort(positions.begin(), positions.end(), [&](int a, int b) {
        return jsonBlockIndex(blocks.at(a)) < jsonBlockIndex(blocks.at(b));
    });
    return positions;
}

void fallbackStackedTableCaptionFragments(QVector<QJsonObject>* blocks) {
    if (!blocks)
        return;
    for (int tablePosition = 0; tablePosition < blocks->size(); ++tablePosition) {
        const QJsonObject table = blocks->at(tablePosition);
        if (!isTableMainType(jsonBlockType(table)))
            continue;
        const QJsonArray tableBbox = table.value(QStringLiteral("bbox")).toArray();
        const double tableTop = bboxCoordinate(tableBbox, 1);
        QVector<int> candidates;
        for (int position = 0; position < blocks->size(); ++position) {
            if (position == tablePosition)
                continue;
            const QJsonObject candidate = blocks->at(position);
            const QString type = jsonBlockType(candidate);
            if (type != QLatin1String("caption") && type != QLatin1String("text")
                && type != QLatin1String("footnote")) {
                continue;
            }
            if (bboxCoordinate(candidate.value(QStringLiteral("bbox")).toArray(), 3) <= tableTop
                && isHorizontallyNearTable(candidate, table)) {
                candidates.push_back(position);
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(), [&](int a, int b) {
            const double ay = bboxCoordinate(
                blocks->at(a).value(QStringLiteral("bbox")).toArray(), 1);
            const double by = bboxCoordinate(
                blocks->at(b).value(QStringLiteral("bbox")).toArray(), 1);
            if (ay != by)
                return ay > by;
            return jsonBlockIndex(blocks->at(a)) > jsonBlockIndex(blocks->at(b));
        });
        QVector<int> cluster;
        double nextTop = tableTop;
        double maximumChildHeight = 1.0;
        for (int position : candidates) {
            const QJsonArray bbox = blocks->at(position).value(QStringLiteral("bbox")).toArray();
            const double height = std::max(bboxCoordinate(bbox, 3) - bboxCoordinate(bbox, 1), 1.0);
            const double gap = nextTop - bboxCoordinate(bbox, 3);
            if (gap < 0.0 || gap > stackedCaptionMaxGap(std::max(maximumChildHeight, height)))
                break;
            cluster.push_back(position);
            nextTop = bboxCoordinate(bbox, 1);
            maximumChildHeight = std::max(maximumChildHeight, height);
        }
        std::reverse(cluster.begin(), cluster.end());
        int lastCaption = -1;
        for (int index = cluster.size() - 1; index >= 0; --index) {
            if (jsonBlockType(blocks->at(cluster.at(index))) == QLatin1String("caption")) {
                lastCaption = index;
                break;
            }
        }
        if (lastCaption < 0)
            continue;
        for (int index = lastCaption + 1; index < cluster.size(); ++index) {
            QJsonObject fragment = blocks->at(cluster.at(index));
            if (isInlineCaptionFragmentType(jsonBlockType(fragment))
                && isSingleLineCaptionFragment(fragment)) {
                fragment.insert(QStringLiteral("type"), QStringLiteral("caption"));
                fragment.remove(QStringLiteral("merge_prev"));
                (*blocks)[cluster.at(index)] = fragment;
            }
        }
    }
}

void fallbackInlineCaptionFragments(QVector<QJsonObject>* blocks) {
    if (!blocks || blocks->size() < 3)
        return;
    const QVector<int> ordered = orderedBlockPositions(*blocks);
    for (int orderedPosition = 0; orderedPosition < ordered.size(); ++orderedPosition) {
        const int position = ordered.at(orderedPosition);
        QJsonObject fragment = blocks->at(position);
        if (!isInlineCaptionFragmentType(jsonBlockType(fragment)))
            continue;
        int previous = -1;
        for (int index = orderedPosition - 1; index >= 0; --index) {
            const int candidate = ordered.at(index);
            if (!isVisualRelationIgnored(jsonBlockType(blocks->at(candidate)))) {
                previous = candidate;
                break;
            }
        }
        int next = -1;
        for (int index = orderedPosition + 1; index < ordered.size(); ++index) {
            const int candidate = ordered.at(index);
            if (!isVisualRelationIgnored(jsonBlockType(blocks->at(candidate)))) {
                next = candidate;
                break;
            }
        }
        if (previous < 0 || next < 0
            || jsonBlockType(blocks->at(previous)) != QLatin1String("caption")
            || !isVisualMainType(jsonBlockType(blocks->at(next)))
            || !inlineCaptionGeometryMatches(blocks->at(previous), fragment, blocks->at(next))) {
            continue;
        }
        fragment.insert(QStringLiteral("type"), QStringLiteral("caption"));
        fragment.remove(QStringLiteral("merge_prev"));
        (*blocks)[position] = fragment;
    }
    fallbackStackedTableCaptionFragments(blocks);
}

void fallbackLeadingTableContinuationCaptions(QVector<QJsonObject>* blocks) {
    if (!blocks)
        return;
    const QVector<int> ordered = orderedBlockPositions(*blocks);
    QVector<int> effective;
    for (int position : ordered) {
        if (!isVisualRelationIgnored(jsonBlockType(blocks->at(position))))
            effective.push_back(position);
    }
    if (effective.size() < 2)
        return;
    QVector<int> leading;
    int cursor = 0;
    for (; cursor < effective.size(); ++cursor) {
        const QJsonObject candidate = blocks->at(effective.at(cursor));
        if (!isInlineCaptionFragmentType(jsonBlockType(candidate))
            || !isSingleLineCaptionFragment(candidate)
            || !isTableContinuationText(blockVisibleText(candidate))) {
            break;
        }
        leading.push_back(effective.at(cursor));
    }
    if (leading.isEmpty() || cursor >= effective.size())
        return;
    const QJsonObject table = blocks->at(effective.at(cursor));
    if (!isTableMainType(jsonBlockType(table)))
        return;
    double nextTop = bboxCoordinate(table.value(QStringLiteral("bbox")).toArray(), 1);
    double maximumChildHeight = 1.0;
    for (int index = leading.size() - 1; index >= 0; --index) {
        const QJsonObject candidate = blocks->at(leading.at(index));
        if (!isHorizontallyNearTable(candidate, table))
            return;
        const QJsonArray bbox = candidate.value(QStringLiteral("bbox")).toArray();
        const double height = std::max(bboxCoordinate(bbox, 3) - bboxCoordinate(bbox, 1), 1.0);
        const double gap = nextTop - bboxCoordinate(bbox, 3);
        if (gap > stackedCaptionMaxGap(std::max(maximumChildHeight, height))
            || gap < -std::max(2.0, height * 0.3)) {
            return;
        }
        nextTop = bboxCoordinate(bbox, 1);
        maximumChildHeight = std::max(maximumChildHeight, height);
    }
    for (int position : leading) {
        QJsonObject caption = blocks->at(position);
        caption.insert(QStringLiteral("type"), QStringLiteral("caption"));
        caption.remove(QStringLiteral("merge_prev"));
        (*blocks)[position] = caption;
    }
}

void regroupVisualBlocks(const QVector<QJsonObject>& blocks,
                         QVector<QJsonObject>* roots,
                         QVector<QJsonObject>* unmatchedChildren,
                         QSet<int>* consumedIndices) {
    if (!roots || !unmatchedChildren || !consumedIndices)
        return;
    QVector<QJsonObject> ordered = blocks;
    std::stable_sort(ordered.begin(), ordered.end(), [](const QJsonObject& a, const QJsonObject& b) {
        return jsonBlockIndex(a) < jsonBlockIndex(b);
    });

    // MinerU treats image_block_body as a composite visual.  Any image/chart
    // body covered by at least 90% is absorbed into the best composite parent
    // and recorded as a normalized sub_images entry instead of surviving as a
    // second top-level visual root.
    QVector<QJsonObject> imageBlockBodies;
    QVector<QJsonObject> imageBlockMembers;
    for (const QJsonObject& block : ordered) {
        const QString type = jsonBlockType(block);
        if (type == QLatin1String("image_block_body"))
            imageBlockBodies.push_back(block);
        else if (type == QLatin1String("image_body") || type == QLatin1String("chart_body"))
            imageBlockMembers.push_back(block);
    }
    QHash<int, int> imageBlockAssignment;
    for (const QJsonObject& member : imageBlockMembers) {
        bool found = false;
        double bestOverlap = 0.0;
        double bestParentArea = 0.0;
        int bestParentIndex = 0;
        for (const QJsonObject& parent : imageBlockBodies) {
            const double overlap = overlapAreaRatioInFirst(
                member.value(QStringLiteral("bbox")).toArray(),
                parent.value(QStringLiteral("bbox")).toArray());
            if (overlap < 0.9)
                continue;
            const double parentArea = bboxArea(parent.value(QStringLiteral("bbox")).toArray());
            const int parentIndex = jsonBlockIndex(parent);
            if (!found || overlap > bestOverlap
                || (overlap == bestOverlap && parentArea < bestParentArea)
                || (overlap == bestOverlap && parentArea == bestParentArea
                    && parentIndex < bestParentIndex)) {
                found = true;
                bestOverlap = overlap;
                bestParentArea = parentArea;
                bestParentIndex = parentIndex;
            }
        }
        if (found)
            imageBlockAssignment.insert(jsonBlockIndex(member), bestParentIndex);
    }
    QSet<int> absorbedMemberIndices;
    QHash<int, QJsonArray> subImagesByParent;
    for (const QJsonObject& parent : imageBlockBodies) {
        QVector<QJsonObject> members;
        const int parentIndex = jsonBlockIndex(parent);
        for (const QJsonObject& member : imageBlockMembers) {
            if (imageBlockAssignment.value(jsonBlockIndex(member), std::numeric_limits<int>::min())
                == parentIndex) {
                members.push_back(member);
            }
        }
        std::stable_sort(members.begin(), members.end(), [](const QJsonObject& a,
                                                            const QJsonObject& b) {
            return jsonBlockIndex(a) < jsonBlockIndex(b);
        });
        const QJsonArray parentBbox = parent.value(QStringLiteral("bbox")).toArray();
        const double parentX0 = bboxCoordinate(parentBbox, 0);
        const double parentY0 = bboxCoordinate(parentBbox, 1);
        const double parentWidth = std::max(bboxCoordinate(parentBbox, 2) - parentX0, 1.0);
        const double parentHeight = std::max(bboxCoordinate(parentBbox, 3) - parentY0, 1.0);
        QJsonArray subImages;
        for (const QJsonObject& member : members) {
            absorbedMemberIndices.insert(jsonBlockIndex(member));
            const QJsonArray memberBbox = member.value(QStringLiteral("bbox")).toArray();
            auto relative = [](double value) {
                return roundToDecimalDigits(std::min(std::max(value, 0.0), 1.0), 3);
            };
            QJsonObject subImage;
            subImage.insert(QStringLiteral("type"),
                            jsonBlockType(member) == QLatin1String("chart_body")
                                ? QStringLiteral("chart")
                                : QStringLiteral("image"));
            subImage.insert(
                QStringLiteral("bbox"),
                QJsonArray({relative((bboxCoordinate(memberBbox, 0) - parentX0) / parentWidth),
                            relative((bboxCoordinate(memberBbox, 1) - parentY0) / parentHeight),
                            relative((bboxCoordinate(memberBbox, 2) - parentX0) / parentWidth),
                            relative((bboxCoordinate(memberBbox, 3) - parentY0) / parentHeight)}));
            subImages.append(subImage);
        }
        if (!subImages.isEmpty())
            subImagesByParent.insert(parentIndex, subImages);
    }
    for (int index : absorbedMemberIndices)
        consumedIndices->insert(index);

    QVector<QJsonObject> relation;
    for (const QJsonObject& block : ordered) {
        if (!absorbedMemberIndices.contains(jsonBlockIndex(block))
            && !isTransparentEmptyList(block)) {
            relation.push_back(block);
        }
    }
    QHash<int, int> positionByIndex;
    QVector<QJsonObject> mains;
    QVector<QJsonObject> children;
    for (int position = 0; position < relation.size(); ++position) {
        positionByIndex.insert(jsonBlockIndex(relation.at(position)), position);
        if (isVisualMainType(jsonBlockType(relation.at(position)))) {
            QJsonObject main = relation.at(position);
            const int index = jsonBlockIndex(main);
            if (subImagesByParent.contains(index))
                main.insert(QStringLiteral("sub_images"), subImagesByParent.value(index));
            mains.push_back(main);
        }
        else if (isGenericVisualChildType(jsonBlockType(relation.at(position))))
            children.push_back(relation.at(position));
    }
    QHash<int, QJsonArray> captionsByMain;
    QHash<int, QJsonArray> footnotesByMain;
    for (const QJsonObject& main : mains)
        consumedIndices->insert(jsonBlockIndex(main));
    for (const QJsonObject& child : children) {
        const int parent = findBestVisualParent(child, mains, relation, positionByIndex);
        if (parent < 0) {
            unmatchedChildren->push_back(child);
            continue;
        }
        const int parentIndex = jsonBlockIndex(mains.at(parent));
        if (jsonBlockType(child) == QLatin1String("caption"))
            captionsByMain[parentIndex].append(child);
        else
            footnotesByMain[parentIndex].append(child);
        consumedIndices->insert(jsonBlockIndex(child));
    }
    for (QJsonObject main : mains) {
        const int index = jsonBlockIndex(main);
        const QString rootType = visualRootType(jsonBlockType(main));
        QJsonObject root;
        root.insert(QStringLiteral("type"), rootType);
        root.insert(QStringLiteral("bbox"), main.value(QStringLiteral("bbox")));
        root.insert(QStringLiteral("index"), index);
        if ((rootType == QLatin1String("image") || rootType == QLatin1String("chart"))
            && jsonValueIsTruthy(main.value(QStringLiteral("sub_type")))) {
            root.insert(QStringLiteral("sub_type"), main.value(QStringLiteral("sub_type")));
        }
        if (rootType == QLatin1String("image")
            && jsonValueIsTruthy(main.value(QStringLiteral("sub_images")))) {
            root.insert(QStringLiteral("sub_images"), main.value(QStringLiteral("sub_images")));
        }
        if (rootType == QLatin1String("table")
            && jsonValueIsTruthy(main.value(QStringLiteral("cell_merge"))))
            root.insert(QStringLiteral("cell_merge"), main.value(QStringLiteral("cell_merge")));
        if (rootType == QLatin1String("code")
            && main.contains(QStringLiteral("_code_sub_type"))) {
            root.insert(QStringLiteral("sub_type"),
                        main.value(QStringLiteral("_code_sub_type")).toString(QStringLiteral("code")));
            if (root.value(QStringLiteral("sub_type")).toString() == QLatin1String("code"))
                root.insert(QStringLiteral("guess_lang"),
                            main.value(QStringLiteral("_guess_lang")).toString(QStringLiteral("txt")));
        }
        main.remove(QStringLiteral("sub_type"));
        main.remove(QStringLiteral("sub_images"));
        main.remove(QStringLiteral("_code_sub_type"));
        main.remove(QStringLiteral("_guess_lang"));
        if (jsonBlockType(main) == QLatin1String("image_block_body"))
            main.insert(QStringLiteral("type"), QStringLiteral("image_body"));
        QJsonArray nested({main});
        auto appendChildren = [&](const QJsonArray& source) {
            for (const QJsonValue& value : source) {
                QJsonObject child = value.toObject();
                child.insert(QStringLiteral("type"),
                             visualChildType(rootType, jsonBlockType(child)));
                nested.append(child);
            }
        };
        appendChildren(captionsByMain.value(index));
        appendChildren(footnotesByMain.value(index));
        QVector<QJsonObject> sortedNested;
        for (const QJsonValue& value : nested)
            sortedNested.push_back(value.toObject());
        std::stable_sort(sortedNested.begin(), sortedNested.end(),
                         [](const QJsonObject& a, const QJsonObject& b) {
                             return jsonBlockIndex(a) < jsonBlockIndex(b);
                         });
        nested = QJsonArray();
        for (const QJsonObject& child : sortedNested)
            nested.append(child);
        root.insert(QStringLiteral("blocks"), nested);
        roots->push_back(root);
    }
}

void fixListBlocks(QVector<QJsonObject>* blocks,
                   QSet<int>* consumedTextIndices,
                   QSet<int>* retainedListIndices) {
    if (!blocks || !consumedTextIndices || !retainedListIndices)
        return;
    QSet<int> assigned;
    for (int listPosition = 0; listPosition < blocks->size(); ++listPosition) {
        QJsonObject list = blocks->at(listPosition);
        if (jsonBlockType(list) != QLatin1String("list"))
            continue;
        list.remove(QStringLiteral("lines"));
        QJsonArray children;
        int textCount = 0;
        int refCount = 0;
        // Official HybridMagicModel concatenates text_blocks + ref_text_blocks
        // before assigning list children.  Preserve that type-grouped order
        // even when a ref_text block has an earlier model index.
        for (const QString& expectedType : {QStringLiteral("text"),
                                            QStringLiteral("ref_text")}) {
            for (const QJsonObject& candidate : *blocks) {
                const QString type = jsonBlockType(candidate);
                const int candidateIndex = jsonBlockIndex(candidate);
                if (type != expectedType || assigned.contains(candidateIndex))
                    continue;
                if (overlapAreaRatioInFirst(candidate.value(QStringLiteral("bbox")).toArray(),
                                            list.value(QStringLiteral("bbox")).toArray())
                    >= 0.8) {
                    children.append(candidate);
                    assigned.insert(candidateIndex);
                    consumedTextIndices->insert(candidateIndex);
                    if (type == QLatin1String("ref_text"))
                        ++refCount;
                    else
                        ++textCount;
                }
            }
        }
        list.insert(QStringLiteral("blocks"), children);
        if (!children.isEmpty()) {
            list.insert(QStringLiteral("sub_type"), refCount > textCount
                                                       ? QStringLiteral("ref_text")
                                                       : QStringLiteral("text"));
            retainedListIndices->insert(jsonBlockIndex(list));
        }
        (*blocks)[listPosition] = list;
    }
}

bool blockHasSpans(const QJsonObject& block) {
    for (const QJsonValue& lineValue : block.value(QStringLiteral("lines")).toArray()) {
        if (!lineValue.toObject().value(QStringLiteral("spans")).toArray().isEmpty())
            return true;
    }
    return false;
}

QJsonArray metricLines(const QJsonObject& block) {
    const QJsonArray det = block.value(QStringLiteral("_ocr_det_lines")).toArray();
    return det.isEmpty() ? block.value(QStringLiteral("lines")).toArray() : det;
}

bool isVerticalLines(const QJsonArray& lines) {
    int valid = 0;
    int vertical = 0;
    for (const QJsonValue& lineValue : lines) {
        for (const QJsonValue& spanValue :
             lineValue.toObject().value(QStringLiteral("spans")).toArray()) {
            const QJsonArray bbox = spanValue.toObject().value(QStringLiteral("bbox")).toArray();
            const double width = bboxCoordinate(bbox, 2) - bboxCoordinate(bbox, 0);
            const double height = bboxCoordinate(bbox, 3) - bboxCoordinate(bbox, 1);
            if (width <= 0.0 || height <= 0.0)
                continue;
            ++valid;
            if (height / width > 2.0)
                ++vertical;
        }
    }
    return valid > 0 && double(vertical) / double(valid) > 0.8;
}

QJsonArray bboxFromLines(const QJsonArray& lines, const QJsonArray& fallback) {
    if (lines.isEmpty())
        return fallback;
    double x0 = std::numeric_limits<double>::infinity();
    double y0 = std::numeric_limits<double>::infinity();
    double x1 = -std::numeric_limits<double>::infinity();
    double y1 = -std::numeric_limits<double>::infinity();
    for (const QJsonValue& lineValue : lines) {
        const QJsonArray bbox = lineValue.toObject().value(QStringLiteral("bbox")).toArray();
        if (bbox.size() != 4)
            continue;
        x0 = std::min(x0, bboxCoordinate(bbox, 0));
        y0 = std::min(y0, bboxCoordinate(bbox, 1));
        x1 = std::max(x1, bboxCoordinate(bbox, 2));
        y1 = std::max(y1, bboxCoordinate(bbox, 3));
    }
    return std::isfinite(x0) ? QJsonArray({x0, y0, x1, y1}) : fallback;
}

QString firstNonEmptyContent(const QJsonArray& lines) {
    for (const QJsonValue& lineValue : lines) {
        for (const QJsonValue& spanValue :
             lineValue.toObject().value(QStringLiteral("spans")).toArray()) {
            const QString content = spanValue.toObject().value(QStringLiteral("content")).toString();
            if (!content.isEmpty())
                return content;
        }
    }
    return {};
}

QString lastNonEmptyContent(const QJsonArray& lines) {
    for (int lineIndex = lines.size() - 1; lineIndex >= 0; --lineIndex) {
        const QJsonArray spans =
            lines.at(lineIndex).toObject().value(QStringLiteral("spans")).toArray();
        for (int spanIndex = spans.size() - 1; spanIndex >= 0; --spanIndex) {
            const QString content =
                spans.at(spanIndex).toObject().value(QStringLiteral("content")).toString();
            if (!content.isEmpty())
                return content;
        }
    }
    return {};
}

bool endsWithLineStop(const QString& text) {
    static const QString stops = QString::fromUtf8(".!?。！？)）\"”:：;；");
    return !text.isEmpty() && stops.contains(text.back());
}

bool canAutoMergeTextBlocks(const QJsonObject& current,
                            const QJsonObject& previous,
                            bool allowSingleLine,
                            bool allowVertical) {
    const QJsonArray currentLines = current.value(QStringLiteral("lines")).toArray();
    const QJsonArray previousLines = previous.value(QStringLiteral("lines")).toArray();
    const QJsonArray currentMetric = metricLines(current);
    const QJsonArray previousMetric = metricLines(previous);
    if (currentLines.isEmpty() || previousLines.isEmpty() || currentMetric.isEmpty()
        || previousMetric.isEmpty()) {
        return false;
    }
    const bool vertical = allowVertical && isVerticalLines(currentMetric)
                          && isVerticalLines(previousMetric);
    const QJsonArray firstMetric = currentMetric.first().toObject()
                                       .value(QStringLiteral("bbox")).toArray();
    const QJsonArray lastMetric = previousMetric.last().toObject()
                                      .value(QStringLiteral("bbox")).toArray();
    const QJsonArray currentFs = bboxFromLines(
        currentMetric, current.value(QStringLiteral("bbox")).toArray());
    const QJsonArray previousFs = bboxFromLines(
        previousMetric, previous.value(QStringLiteral("bbox")).toArray());
    if (vertical) {
        const double firstWidth = bboxCoordinate(firstMetric, 2) - bboxCoordinate(firstMetric, 0);
        const double lastWidth = bboxCoordinate(lastMetric, 2) - bboxCoordinate(lastMetric, 0);
        if (firstWidth <= 0.0 || lastWidth <= 0.0
            || std::abs(bboxCoordinate(currentFs, 1) - bboxCoordinate(firstMetric, 1))
                   >= firstWidth / 2.0
            || std::abs(bboxCoordinate(previousFs, 3) - bboxCoordinate(lastMetric, 3))
                   >= lastWidth) {
            return false;
        }
        const double currentHeight = bboxCoordinate(currentFs, 3) - bboxCoordinate(currentFs, 1);
        const double previousHeight = bboxCoordinate(previousFs, 3) - bboxCoordinate(previousFs, 1);
        const double minimumHeight = std::min(currentHeight, previousHeight);
        if (minimumHeight <= 0.0 || std::abs(currentHeight - previousHeight) >= minimumHeight
            || bboxCoordinate(current.value(QStringLiteral("bbox")).toArray(), 2)
                   <= bboxCoordinate(previous.value(QStringLiteral("bbox")).toArray(), 0)) {
            return false;
        }
    } else {
        const double firstHeight = bboxCoordinate(firstMetric, 3) - bboxCoordinate(firstMetric, 1);
        const double lastHeight = bboxCoordinate(lastMetric, 3) - bboxCoordinate(lastMetric, 1);
        if (firstHeight <= 0.0 || lastHeight <= 0.0
            || std::abs(bboxCoordinate(currentFs, 0) - bboxCoordinate(firstMetric, 0))
                   >= firstHeight / 2.0
            || std::abs(bboxCoordinate(previousFs, 2) - bboxCoordinate(lastMetric, 2))
                   >= lastHeight) {
            return false;
        }
        const double currentWidth = bboxCoordinate(currentFs, 2) - bboxCoordinate(currentFs, 0);
        const double previousWidth = bboxCoordinate(previousFs, 2) - bboxCoordinate(previousFs, 0);
        const double minimumWidth = std::min(currentWidth, previousWidth);
        if (minimumWidth <= 0.0 || std::abs(currentWidth - previousWidth) >= minimumWidth
            || bboxCoordinate(current.value(QStringLiteral("bbox")).toArray(), 1)
                   >= bboxCoordinate(previous.value(QStringLiteral("bbox")).toArray(), 3)) {
            return false;
        }
    }
    const QString first = firstNonEmptyContent(currentLines);
    const QString last = lastNonEmptyContent(previousLines);
    if (first.isEmpty() || last.isEmpty() || endsWithLineStop(last)
        || first.front().isDigit() || first.front().isUpper()) {
        return false;
    }
    if (!vertical && !allowSingleLine && currentMetric.size() <= 1 && previousMetric.size() <= 1)
        return false;
    return true;
}

bool isTextMergeBarrier(const QString& type) {
    return type == QLatin1String("title") || type == QLatin1String("doc_title")
           || type == QLatin1String("paragraph_title")
           || type == QLatin1String("interline_equation") || type == QLatin1String("list");
}

bool isTextMergeTransparent(const QString& type) {
    return type == QLatin1String("image") || type == QLatin1String("table")
           || type == QLatin1String("chart") || type == QLatin1String("code");
}

int previousMergeableTextIndex(const QJsonArray& blocks, int currentIndex) {
    for (int index = currentIndex - 1; index >= 0; --index) {
        const QString type = jsonBlockType(blocks.at(index).toObject());
        if (isTextMergeBarrier(type))
            return -1;
        if (type == QLatin1String("text"))
            return index;
        if (!isTextMergeTransparent(type))
            return -1;
    }
    return -1;
}

void mergeParaTextBlocks(QJsonArray* blocks, bool allowVertical) {
    if (!blocks)
        return;
    for (int currentIndex = blocks->size() - 1; currentIndex >= 0; --currentIndex) {
        QJsonObject current = blocks->at(currentIndex).toObject();
        if (jsonBlockType(current) == QLatin1String("list")) {
            if (currentIndex < 1
                || current.value(QStringLiteral("sub_type")).toString()
                       != QLatin1String("ref_text")
                || current.value(QStringLiteral("blocks")).toArray().isEmpty()) {
                continue;
            }
            QJsonObject previous = blocks->at(currentIndex - 1).toObject();
            if (jsonBlockType(previous) != QLatin1String("list")
                || previous.value(QStringLiteral("sub_type")).toString()
                       != QLatin1String("ref_text")) {
                continue;
            }
            QJsonArray previousChildren = previous.value(QStringLiteral("blocks")).toArray();
            for (const QJsonValue& child : current.value(QStringLiteral("blocks")).toArray())
                previousChildren.append(child);
            previous.insert(QStringLiteral("blocks"), previousChildren);
            current.insert(QStringLiteral("blocks"), QJsonArray());
            current.insert(QStringLiteral("lines_deleted"), true);
            blocks->replace(currentIndex - 1, previous);
            blocks->replace(currentIndex, current);
            continue;
        }
        if (jsonBlockType(current) != QLatin1String("text") || !blockHasSpans(current))
            continue;
        int previousIndex = -1;
        if (current.value(QStringLiteral("merge_prev")).toBool()) {
            previousIndex = previousMergeableTextIndex(*blocks, currentIndex);
            if (previousIndex >= 0
                && !canAutoMergeTextBlocks(current, blocks->at(previousIndex).toObject(), true,
                                           false)) {
                previousIndex = -1;
            }
        }
        if (previousIndex < 0) {
            previousIndex = previousMergeableTextIndex(*blocks, currentIndex);
            if (previousIndex >= 0
                && !canAutoMergeTextBlocks(current, blocks->at(previousIndex).toObject(), false,
                                           allowVertical)) {
                previousIndex = -1;
            }
        }
        if (previousIndex < 0)
            continue;
        QJsonObject previous = blocks->at(previousIndex).toObject();
        QJsonArray previousLines = previous.value(QStringLiteral("lines")).toArray();
        for (const QJsonValue& line : current.value(QStringLiteral("lines")).toArray())
            previousLines.append(line);
        previous.insert(QStringLiteral("lines"), previousLines);
        const QJsonArray currentDet = current.value(QStringLiteral("_ocr_det_lines")).toArray();
        if (!currentDet.isEmpty()) {
            QJsonArray previousDet = previous.value(QStringLiteral("_ocr_det_lines")).toArray();
            for (const QJsonValue& line : currentDet)
                previousDet.append(line);
            previous.insert(QStringLiteral("_ocr_det_lines"), previousDet);
        }
        current.insert(QStringLiteral("lines"), QJsonArray());
        current.insert(QStringLiteral("_ocr_det_lines"), QJsonArray());
        current.insert(QStringLiteral("lines_deleted"), true);
        blocks->replace(previousIndex, previous);
        blocks->replace(currentIndex, current);
    }
}

void removeInternalMiddleMetadata(QJsonObject* block) {
    if (!block)
        return;
    block->remove(QStringLiteral("_ocr_det_lines"));
    block->remove(QStringLiteral("line_avg_height"));
    QJsonArray children = block->value(QStringLiteral("blocks")).toArray();
    for (int i = 0; i < children.size(); ++i) {
        QJsonObject child = children.at(i).toObject();
        removeInternalMiddleMetadata(&child);
        children[i] = child;
    }
    if (!children.isEmpty())
        block->insert(QStringLiteral("blocks"), children);
}

void finalizeMiddleBlocks(QJsonArray* blocks) {
    if (!blocks)
        return;
    for (int i = 0; i < blocks->size(); ++i) {
        QJsonObject block = blocks->at(i).toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("doc_title")) {
            block.insert(QStringLiteral("type"), QStringLiteral("title"));
            block.insert(QStringLiteral("level"), 1);
        } else if (type == QLatin1String("paragraph_title")) {
            block.insert(QStringLiteral("type"), QStringLiteral("title"));
            block.insert(QStringLiteral("level"), 2);
        }
        removeInternalMiddleMetadata(&block);
        (*blocks)[i] = block;
    }
}

QByteArray officialJsonBytes(const QJsonDocument& document, bool modelBboxesAreFloat) {
    QString text = QString::fromUtf8(document.toJson(QJsonDocument::Indented));
    // QJsonValue stores every JSON number as double but QJsonDocument prints an
    // integral double as `1`.  MinerU's sidecar confidence is a Python float,
    // so its canonical JSON is `1.0`; retain that public type contract.
    static const QRegularExpression integralScore(
        QStringLiteral("(\\\"score\\\"\\s*:\\s*)(-?\\d+)(?=\\s*[,}])"));
    text.replace(integralScore, QStringLiteral("\\1\\2.0"));
    if (modelBboxesAreFloat) {
        QStringList lines = text.split(QLatin1Char('\n'));
        bool inBbox = false;
        static const QRegularExpression integralArrayValue(
            QStringLiteral("^(\\s*)(-?\\d+)(,?)$"));
        for (QString& line : lines) {
            if (line.contains(QStringLiteral("\"bbox\""))
                && line.contains(QLatin1Char('['))) {
                inBbox = true;
                continue;
            }
            if (!inBbox)
                continue;
            if (line.trimmed().startsWith(QLatin1Char(']'))) {
                inBbox = false;
                continue;
            }
            line.replace(integralArrayValue, QStringLiteral("\\1\\2.0\\3"));
        }
        text = lines.join(QLatin1Char('\n'));
    }
    return text.toUtf8();
}

QString cleanBalancedDisplayDelimiters(QString content) {
    if (content.isEmpty() || content.count(QStringLiteral("\\[")) == 0
        || content.count(QStringLiteral("\\[")) != content.count(QStringLiteral("\\]"))) {
        return content;
    }
    // MinerU clean_content uses Python re.sub without re.DOTALL.
    const QRegularExpression expression(QStringLiteral("\\\\\\[(.*?)\\\\\\]"));
    QString result;
    int cursor = 0;
    QRegularExpressionMatchIterator matches = expression.globalMatch(content);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += content.mid(cursor, match.capturedStart() - cursor);
        result += QLatin1Char('[') + match.captured(1) + QLatin1Char(']');
        cursor = match.capturedEnd();
    }
    result += content.mid(cursor);
    return result;
}

QJsonObject contentSpan(const QJsonArray& bbox, const QString& type, const QString& content) {
    QJsonObject span;
    span.insert(QStringLiteral("bbox"), bbox);
    span.insert(QStringLiteral("type"), type);
    span.insert(QStringLiteral("content"), content);
    return span;
}

QJsonArray splitInlineFormulaSpans(QString content, const QJsonArray& bbox) {
    content = cleanBalancedDisplayDelimiters(content);
    if (content.count(QStringLiteral("\\(")) == 0
        || content.count(QStringLiteral("\\(")) != content.count(QStringLiteral("\\)"))) {
        return QJsonArray({contentSpan(bbox, QStringLiteral("text"), content)});
    }
    // Pinned MinerU calls Python re.finditer without re.DOTALL. A delimiter
    // pair separated by a newline therefore remains a single text span.
    const QRegularExpression expression(QStringLiteral("\\\\\\((.+?)\\\\\\)"));
    QJsonArray spans;
    int cursor = 0;
    QRegularExpressionMatchIterator matches = expression.globalMatch(content);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        if (match.capturedStart() > cursor) {
            const QString before = content.mid(cursor, match.capturedStart() - cursor);
            if (!before.trimmed().isEmpty())
                spans.append(contentSpan(bbox, QStringLiteral("text"), before));
        }
        spans.append(contentSpan(bbox, QStringLiteral("inline_equation"),
                                 match.captured(1).trimmed()));
        cursor = match.capturedEnd();
    }
    const QString after = content.mid(cursor);
    if (!after.trimmed().isEmpty())
        spans.append(contentSpan(bbox, QStringLiteral("text"), after));
    if (spans.isEmpty())
        spans.append(contentSpan(bbox, QStringLiteral("text"), content));
    return spans;
}

QString cleanIsolatedFormula(QString content) {
    if (content.startsWith(QStringLiteral("\\[")))
        content.remove(0, 2);
    if (content.endsWith(QStringLiteral("\\]")))
        content.chop(2);
    return content.trimmed();
}

QJsonObject textLikeBlock(const QString& type,
                          const QJsonArray& bb,
                          int angle,
                          const QString& content,
                          const QString& spanType,
                          int index) {
    QJsonObject line;
    line.insert(QStringLiteral("bbox"), bb);
    line.insert(QStringLiteral("spans"),
                spanType == QLatin1String("text")
                    ? splitInlineFormulaSpans(content, bb)
                    : QJsonArray({contentSpan(bb, spanType, content)}));
    QJsonObject o;
    o.insert(QStringLiteral("bbox"), bb);
    o.insert(QStringLiteral("type"), type);
    o.insert(QStringLiteral("angle"), angle);
    o.insert(QStringLiteral("lines"), QJsonArray({line}));
    o.insert(QStringLiteral("index"), index);
    return o;
}

QString normalizeVlmText(const QString& content) {
    QString result;
    result.reserve(content.size());
    for (QChar character : content) {
        const ushort code = character.unicode();
        if ((code >= 0xFF21 && code <= 0xFF3A) || (code >= 0xFF41 && code <= 0xFF5A)
            || (code >= 0xFF10 && code <= 0xFF19)) {
            character = QChar(ushort(code - 0xFEE0));
        }
        result.append(character);
    }
    return result;
}

QString escapeConservativeMarkdown(const QString& content) {
    QString escaped;
    escaped.reserve(content.size() * 2);
    int precedingBackslashes = 0;
    for (QChar character : content) {
        if (character == QLatin1Char('\\')) {
            escaped.append(character);
            ++precedingBackslashes;
            continue;
        }
        if ((character == QLatin1Char('*') || character == QLatin1Char('_')
             || character == QLatin1Char('`') || character == QLatin1Char('~')
             || character == QLatin1Char('$'))
            && precedingBackslashes % 2 == 0) {
            escaped.append(QLatin1Char('\\'));
        }
        escaped.append(character);
        precedingBackslashes = 0;
    }
    return escaped;
}

QString escapeTextBlockPrefix(const QString& content) {
    static const QRegularExpression prefix(
        QStringLiteral("^(?<indent>[ \\t]{0,3})(?<marker>#{1,6}|[+-])(?=[ \\t])"));
    const QRegularExpressionMatch match = prefix.match(content);
    if (!match.hasMatch())
        return content;
    QString result = content;
    result.insert(match.capturedStart(QStringLiteral("marker")), QLatin1Char('\\'));
    return result;
}

bool hasFollowingJoinableSpan(const QJsonObject& block, int lineIndex, int spanIndex) {
    const QJsonArray lines = block.value(QStringLiteral("lines")).toArray();
    for (int nextLine = lineIndex; nextLine < lines.size(); ++nextLine) {
        const QJsonArray spans =
            lines.at(nextLine).toObject().value(QStringLiteral("spans")).toArray();
        const int start = nextLine == lineIndex ? spanIndex + 1 : 0;
        for (int nextSpan = start; nextSpan < spans.size(); ++nextSpan) {
            const QJsonObject span = spans.at(nextSpan).toObject();
            const QString type = jsonBlockType(span);
            if (type == QLatin1String("text")
                && !normalizeVlmText(span.value(QStringLiteral("content")).toString())
                        .trimmed().isEmpty()) {
                return true;
            }
            if (type == QLatin1String("inline_equation")
                && !span.value(QStringLiteral("content")).toString().trimmed().isEmpty()) {
                return true;
            }
        }
    }
    return false;
}

bool isHyphenAtLineEnd(const QString& content) {
    static const QRegularExpression hyphen(
        QStringLiteral("[A-Za-z]+[-\\x{00AD}\\x{2010}\\x{2011}\\x{2043}]\\s*$"));
    return hyphen.match(content).hasMatch();
}

QString mergeParaWithText(const QJsonObject& block,
                          bool formulaEnabled = true,
                          const QString& imageBucket = QStringLiteral("images"),
                          bool escapePrefix = true) {
    QString plainText;
    const QJsonArray lines = block.value(QStringLiteral("lines")).toArray();
    for (const QJsonValue& lineValue : lines) {
        for (const QJsonValue& spanValue :
             lineValue.toObject().value(QStringLiteral("spans")).toArray()) {
            const QJsonObject span = spanValue.toObject();
            if (jsonBlockType(span) == QLatin1String("text"))
                plainText += normalizeVlmText(span.value(QStringLiteral("content")).toString());
        }
    }
    OfficialLanguageDetection detection;
    QString detectionError;
    if (!detectOfficialTextLanguage(plainText, &detection, &detectionError)) {
        throw std::runtime_error(detectionError.toUtf8().constData());
    }
    static const QSet<QString> cjkLanguages = {
        QStringLiteral("zh"), QStringLiteral("ja"), QStringLiteral("ko")};
    const bool cjk = cjkLanguages.contains(detection.language);
    const bool escapeText = jsonBlockType(block) != QLatin1String("code_body");
    QString result;
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const QJsonArray spans =
            lines.at(lineIndex).toObject().value(QStringLiteral("spans")).toArray();
        for (int spanIndex = 0; spanIndex < spans.size(); ++spanIndex) {
            const QJsonObject span = spans.at(spanIndex).toObject();
            const QString spanType = jsonBlockType(span);
            QString content;
            if (spanType == QLatin1String("text")) {
                content = normalizeVlmText(span.value(QStringLiteral("content")).toString());
                if (escapeText)
                    content = escapeConservativeMarkdown(content);
            } else if (spanType == QLatin1String("inline_equation")) {
                content = QLatin1Char('$')
                          + span.value(QStringLiteral("content")).toString() + QLatin1Char('$');
            } else if (spanType == QLatin1String("interline_equation")) {
                if (formulaEnabled) {
                    content = QStringLiteral("\n$$\n")
                              + span.value(QStringLiteral("content")).toString()
                              + QStringLiteral("\n$$\n");
                } else if (!span.value(QStringLiteral("image_path")).toString().isEmpty()) {
                    content = QStringLiteral("![](%1/%2)")
                                  .arg(imageBucket,
                                       span.value(QStringLiteral("image_path")).toString());
                }
            }
            content = content.trimmed();
            if (content.isEmpty())
                continue;
            if (spanType == QLatin1String("interline_equation")) {
                result += content;
                continue;
            }
            const bool isLastSpan = spanIndex == spans.size() - 1;
            const bool hasFollowing = hasFollowingJoinableSpan(block, lineIndex, spanIndex);
            if (cjk) {
                result += content;
                if (hasFollowing && (!isLastSpan || spanType == QLatin1String("inline_equation")))
                    result += QLatin1Char(' ');
                continue;
            }
            if ((spanType == QLatin1String("text")
                 || spanType == QLatin1String("inline_equation"))
                && isLastSpan && spanType == QLatin1String("text")
                && isHyphenAtLineEnd(content)) {
                const bool nextStartsLower = lineIndex + 1 < lines.size()
                                             && !lines.at(lineIndex + 1)
                                                     .toObject()
                                                     .value(QStringLiteral("spans"))
                                                     .toArray().isEmpty()
                                             && jsonBlockType(lines.at(lineIndex + 1)
                                                                  .toObject()
                                                                  .value(QStringLiteral("spans"))
                                                                  .toArray().first().toObject())
                                                    == QLatin1String("text")
                                             && !lines.at(lineIndex + 1)
                                                     .toObject()
                                                     .value(QStringLiteral("spans"))
                                                     .toArray().first().toObject()
                                                     .value(QStringLiteral("content"))
                                                     .toString().isEmpty()
                                             && lines.at(lineIndex + 1)
                                                    .toObject()
                                                    .value(QStringLiteral("spans"))
                                                    .toArray().first().toObject()
                                                    .value(QStringLiteral("content"))
                                                    .toString().front().isLower();
                if (nextStartsLower)
                    content.chop(1);
                result += content;
            } else {
                result += content;
                if (hasFollowing)
                    result += QLatin1Char(' ');
            }
        }
    }
    if (escapePrefix && jsonBlockType(block) == QLatin1String("text"))
        result = escapeTextBlockPrefix(result);
    return result;
}

QString mediaPath(const QString& bucket, const QString& imagePath) {
    if (imagePath.isEmpty())
        return {};
    return bucket.isEmpty() ? imagePath : bucket + QLatin1Char('/') + imagePath;
}

QString prefixTableImageSources(const QString& html, const QString& bucket) {
    if (html.isEmpty() || bucket.isEmpty())
        return html;
    static const QRegularExpression source(QStringLiteral("src=\"(?!data:)([^\"]+)\""));
    QString result;
    int cursor = 0;
    QRegularExpressionMatchIterator matches = source.globalMatch(html);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += html.mid(cursor, match.capturedStart() - cursor);
        result += QStringLiteral("src=\"") + bucket + QLatin1Char('/') + match.captured(1)
                  + QLatin1Char('"');
        cursor = match.capturedEnd();
    }
    result += html.mid(cursor);
    return result;
}

QString formattedTableHtml(const QString& html, const QString& bucket) {
    return formatOfficialEmbeddedTableHtml(prefixTableImageSources(html, bucket));
}

QString htmlEscapeWithoutQuotes(QString content) {
    content.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    content.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    content.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return content;
}

QString renderAlgorithmHtml(const QJsonObject& body) {
    QString htmlBody;
    QString previousType;
    for (const QJsonValue& lineValue : body.value(QStringLiteral("lines")).toArray()) {
        for (const QJsonValue& spanValue :
             lineValue.toObject().value(QStringLiteral("spans")).toArray()) {
            const QJsonObject span = spanValue.toObject();
            const QString type = jsonBlockType(span);
            QString content = span.value(QStringLiteral("content")).toString();
            if (type == QLatin1String("text")) {
                content = normalizeVlmText(content);
                htmlBody += htmlEscapeWithoutQuotes(content);
                if (!content.isEmpty())
                    previousType = type;
            } else if (type == QLatin1String("inline_equation") && !content.trimmed().isEmpty()) {
                if (previousType == QLatin1String("inline_equation") && !htmlBody.isEmpty()
                    && !htmlBody.endsWith(QLatin1Char(' ')) && !htmlBody.endsWith(QLatin1Char('\n'))
                    && !htmlBody.endsWith(QLatin1Char('\t'))) {
                    htmlBody += QLatin1Char(' ');
                }
                htmlBody += QLatin1Char('$') + htmlEscapeWithoutQuotes(content) + QLatin1Char('$');
                previousType = type;
            }
        }
    }
    if (htmlBody.trimmed().isEmpty())
        return {};
    return QStringLiteral("<div class=\"mineru-algorithm\" style=\"white-space: pre-wrap; font-family:monospace;\">\n")
           + htmlBody + QStringLiteral("\n</div>");
}

struct RenderedSegment {
    QString text;
    bool htmlBlock = false;
};

QVector<RenderedSegment> renderVisualChild(const QJsonObject& child,
                                           const QJsonObject& root,
                                           const QString& imageBucket) {
    QVector<RenderedSegment> segments;
    const QString type = jsonBlockType(child);
    if (type.endsWith(QStringLiteral("_caption"))
        || type.endsWith(QStringLiteral("_footnote"))) {
        const QString text = mergeParaWithText(child);
        if (!text.trimmed().isEmpty())
            segments.push_back({text, false});
        return segments;
    }
    if (type == QLatin1String("image_body") || type == QLatin1String("chart_body")) {
        const QString expectedSpanType = type == QLatin1String("image_body")
                                             ? QStringLiteral("image")
                                             : QStringLiteral("chart");
        for (const QJsonValue& lineValue : child.value(QStringLiteral("lines")).toArray()) {
            for (const QJsonValue& spanValue :
                 lineValue.toObject().value(QStringLiteral("spans")).toArray()) {
                const QJsonObject span = spanValue.toObject();
                if (jsonBlockType(span) != expectedSpanType)
                    continue;
                const QString path = mediaPath(
                    imageBucket, span.value(QStringLiteral("image_path")).toString());
                if (!path.isEmpty())
                    segments.push_back({QStringLiteral("![](%1)").arg(path), false});
                const QString content = span.value(QStringLiteral("content")).toString();
                if (!content.trimmed().isEmpty()) {
                    const QString defaultSummary = expectedSpanType == QLatin1String("chart")
                                                       ? QStringLiteral("chart content")
                                                       : QStringLiteral("image content");
                    const QString summary = root.value(QStringLiteral("sub_type")).toString(
                        defaultSummary);
                    segments.push_back(
                        {QStringLiteral("<details>\n<summary>%1</summary>\n\n%2\n</details>")
                             .arg(summary, content),
                         true});
                }
            }
        }
        return segments;
    }
    if (type == QLatin1String("table_body")) {
        for (const QJsonValue& lineValue : child.value(QStringLiteral("lines")).toArray()) {
            for (const QJsonValue& spanValue :
                 lineValue.toObject().value(QStringLiteral("spans")).toArray()) {
                const QJsonObject span = spanValue.toObject();
                if (jsonBlockType(span) != QLatin1String("table"))
                    continue;
                const QString html = formattedTableHtml(
                    span.value(QStringLiteral("html")).toString(), imageBucket);
                if (!html.isEmpty())
                    segments.push_back({html, true});
                else {
                    const QString path = mediaPath(
                        imageBucket, span.value(QStringLiteral("image_path")).toString());
                    if (!path.isEmpty())
                        segments.push_back({QStringLiteral("![](%1)").arg(path), false});
                }
            }
        }
        return segments;
    }
    if (type == QLatin1String("code_body")) {
        QString text;
        bool html = false;
        if (root.value(QStringLiteral("sub_type")).toString() == QLatin1String("algorithm")) {
            text = renderAlgorithmHtml(child);
            html = true;
        } else {
            text = QStringLiteral("```%1\n%2\n```")
                       .arg(root.value(QStringLiteral("guess_lang")).toString(QStringLiteral("txt")),
                            mergeParaWithText(child));
        }
        if (!text.trimmed().isEmpty())
            segments.push_back({text, html});
    }
    return segments;
}

QString renderVisualRoot(const QJsonObject& root,
                         const QString& imageBucket = QStringLiteral("images")) {
    QVector<QJsonObject> children;
    for (const QJsonValue& value : root.value(QStringLiteral("blocks")).toArray())
        children.push_back(value.toObject());
    std::stable_sort(children.begin(), children.end(), [](const QJsonObject& a, const QJsonObject& b) {
        return jsonBlockIndex(a) < jsonBlockIndex(b);
    });
    QVector<RenderedSegment> segments;
    for (const QJsonObject& child : children) {
        const QVector<RenderedSegment> rendered = renderVisualChild(child, root, imageBucket);
        for (const RenderedSegment& segment : rendered)
            segments.push_back(segment);
    }
    QString result;
    bool previousHtml = false;
    for (const RenderedSegment& segment : segments) {
        if (!result.isEmpty())
            result += previousHtml || segment.htmlBlock ? QStringLiteral("\n\n")
                                                        : QStringLiteral("  \n");
        result += segment.text;
        previousHtml = segment.htmlBlock;
    }
    return result;
}

QPair<QString, QString> visualBodyData(const QJsonObject& root) {
    for (const QJsonValue& childValue : root.value(QStringLiteral("blocks")).toArray()) {
        const QJsonObject child = childValue.toObject();
        const QString childType = jsonBlockType(child);
        if (childType != QLatin1String("image_body") && childType != QLatin1String("chart_body")
            && childType != QLatin1String("table_body")
            && childType != QLatin1String("code_body")) {
            continue;
        }
        for (const QJsonValue& lineValue : child.value(QStringLiteral("lines")).toArray()) {
            for (const QJsonValue& spanValue :
                 lineValue.toObject().value(QStringLiteral("spans")).toArray()) {
                const QJsonObject span = spanValue.toObject();
                const QString spanType = jsonBlockType(span);
                if (spanType == QLatin1String("table"))
                    return {span.value(QStringLiteral("image_path")).toString(),
                            span.value(QStringLiteral("html")).toString()};
                if (spanType == QLatin1String("image") || spanType == QLatin1String("chart"))
                    return {span.value(QStringLiteral("image_path")).toString(),
                            span.value(QStringLiteral("content")).toString()};
                if (spanType == QLatin1String("text"))
                    return {{}, span.value(QStringLiteral("content")).toString()};
            }
        }
    }
    return {};
}

QJsonArray visualChildTexts(const QJsonObject& root, const QString& childType) {
    QJsonArray result;
    for (const QJsonValue& value : root.value(QStringLiteral("blocks")).toArray()) {
        const QJsonObject child = value.toObject();
        if (jsonBlockType(child) == childType)
            result.append(mergeParaWithText(child));
    }
    return result;
}

}  // namespace

QByteArray officialHybridJsonBytes(const QJsonDocument& document,
                                   bool modelBboxesAreFloat) {
    return officialJsonBytes(document, modelBboxesAreFloat);
}

bool writeOfficialMiddleJson(const QJsonArray& pageModel,
                             int imageWidth,
                             int imageHeight,
                             const QString& path,
                             QString* err,
                             const QString& effort,
                             const QString& preprocPath,
                             const QImage* renderedPage,
                             const QString& imageDir) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (imageWidth < 1 || imageHeight < 1)
        return fail(QStringLiteral("invalid image size"));
    // Official middle.json: width, height = map(int, page.get_size())  [PDF points]
    const int pw = imageWidth;
    const int ph = imageHeight;

    QVector<QJsonObject> pageBlocks;
    QVector<QJsonObject> formulaSpans;
    QVector<QJsonObject> ocrSpans;
    for (const QJsonValue& value : pageModel) {
        const QJsonObject item = value.toObject();
        const QString type = item.value(QStringLiteral("type")).toString(
            item.value(QStringLiteral("label")).toString());
        if (type != QLatin1String("ocr_text") && type != QLatin1String("inline_formula")) {
            pageBlocks.push_back(item);
            continue;
        }
        const QJsonArray bb = pxBbox(item.value(QStringLiteral("bbox")).toArray(), pw, ph);
        if (bb.size() != 4 || bboxArea(bb) <= 0.0)
            continue;
        QJsonObject span;
        span.insert(QStringLiteral("bbox"), bb);
        span.insert(QStringLiteral("type"), type == QLatin1String("ocr_text")
                                                    ? QStringLiteral("text")
                                                    : QStringLiteral("inline_equation"));
        span.insert(QStringLiteral("content"), type == QLatin1String("ocr_text")
                                                       ? item.value(QStringLiteral("text")).toString()
                                                       : item.value(QStringLiteral("latex")).toString());
        if (item.contains(QStringLiteral("score")))
            span.insert(QStringLiteral("score"), item.value(QStringLiteral("score")));
        if (type == QLatin1String("inline_formula"))
            formulaSpans.push_back(span);
        else
            ocrSpans.push_back(span);
    }
    // MagicModel._split_page_model_list always builds this order, even when a
    // merged model.json happens to contain the sidecar records interleaved.
    QVector<QJsonObject> sidecarSpans = formulaSpans;
    for (const QJsonObject& span : ocrSpans)
        sidecarSpans.push_back(span);
    QVector<bool> usedSidecar(sidecarSpans.size(), false);

    QVector<QJsonObject> allBlocks;
    for (int index = 0; index < pageBlocks.size(); ++index) {
        const QJsonObject raw = pageBlocks.at(index);
        QString type = raw.value(QStringLiteral("type")).toString(
            raw.value(QStringLiteral("label")).toString());
        const QJsonArray bb = pxBbox(raw.value(QStringLiteral("bbox")).toArray(), pw, ph);
        if (type.isEmpty() || bb.size() != 4 || bboxArea(bb) <= 0.0)
            continue;
        const int angle = raw.value(QStringLiteral("angle")).toInt(0);
        QString content = raw.value(QStringLiteral("content")).toString();
        if (type == QLatin1String("title") || type == QLatin1String("doc_title")
            || type == QLatin1String("paragraph_title")) {
            content.replace(QRegularExpression(QStringLiteral("\\n\\s*")), QStringLiteral(" "));
            content = content.trimmed();
        }
        QString midType = type;
        QString spanType = QStringLiteral("text");
        if (type == QLatin1String("index"))
            midType = QStringLiteral("text");
        else if (type == QLatin1String("equation")) {
            midType = QStringLiteral("interline_equation");
            spanType = QStringLiteral("interline_equation");
            content = cleanIsolatedFormula(content);
        } else if (type == QLatin1String("image_caption")
                   || type == QLatin1String("table_caption")
                   || type == QLatin1String("chart_caption")
                   || type == QLatin1String("code_caption")) {
            midType = QStringLiteral("caption");
        } else if (type == QLatin1String("image_footnote")
                   || type == QLatin1String("table_footnote")
                   || type == QLatin1String("chart_footnote")
                   || type == QLatin1String("code_footnote")) {
            midType = QStringLiteral("footnote");
        } else if (type == QLatin1String("image")) {
            midType = QStringLiteral("image_body");
            spanType = QStringLiteral("image");
        } else if (type == QLatin1String("image_block")) {
            midType = QStringLiteral("image_block_body");
            spanType = QStringLiteral("image");
        } else if (type == QLatin1String("table")) {
            midType = QStringLiteral("table_body");
            spanType = QStringLiteral("table");
        } else if (type == QLatin1String("chart")) {
            midType = QStringLiteral("chart_body");
            spanType = QStringLiteral("chart");
        } else if (type == QLatin1String("code") || type == QLatin1String("algorithm")) {
            midType = QStringLiteral("code_body");
            content = cleanOfficialCodeContent(content);
        }

        QJsonObject o;
        if (spanType == QLatin1String("image") || spanType == QLatin1String("table")
            || spanType == QLatin1String("chart")) {
            QJsonObject span;
            span.insert(QStringLiteral("bbox"), bb);
            span.insert(QStringLiteral("type"), spanType);
            if (spanType == QLatin1String("table"))
                span.insert(QStringLiteral("html"), content);
            else if ((type == QLatin1String("image") || type == QLatin1String("chart"))
                     && raw.contains(QStringLiteral("content")))
                span.insert(QStringLiteral("content"), raw.value(QStringLiteral("content")));
            QString imagePath;
            if (renderedPage
                && !visualImagePath(*renderedPage, bb, pw, ph, imageDir, spanType,
                                    &imagePath, err)) {
                return false;
            }
            if (!imagePath.isEmpty())
                span.insert(QStringLiteral("image_path"), imagePath);
            QJsonObject line;
            line.insert(QStringLiteral("bbox"), bb);
            line.insert(QStringLiteral("spans"), QJsonArray({span}));
            o.insert(QStringLiteral("bbox"), bb);
            o.insert(QStringLiteral("type"), midType);
            o.insert(QStringLiteral("angle"), angle);
            o.insert(QStringLiteral("lines"), QJsonArray({line}));
            o.insert(QStringLiteral("index"), index);
            if ((type == QLatin1String("image") || type == QLatin1String("chart"))
                && jsonValueIsTruthy(raw.value(QStringLiteral("sub_type"))))
                o.insert(QStringLiteral("sub_type"), raw.value(QStringLiteral("sub_type")));
            if (type == QLatin1String("table") && raw.contains(QStringLiteral("cell_merge")))
                o.insert(QStringLiteral("cell_merge"), raw.value(QStringLiteral("cell_merge")));
        } else {
            o = textLikeBlock(midType, bb, angle, content, spanType, index);
            if (spanType == QLatin1String("interline_equation") && renderedPage) {
                QString imagePath;
                if (!visualImagePath(*renderedPage, bb, pw, ph, imageDir, spanType,
                                     &imagePath, err)) {
                    return false;
                }
                if (!imagePath.isEmpty()) {
                    QJsonArray lines = o.value(QStringLiteral("lines")).toArray();
                    QJsonObject line = lines.first().toObject();
                    QJsonArray spans = line.value(QStringLiteral("spans")).toArray();
                    QJsonObject span = spans.first().toObject();
                    span.insert(QStringLiteral("image_path"), imagePath);
                    spans.replace(0, span);
                    line.insert(QStringLiteral("spans"), spans);
                    lines.replace(0, line);
                    o.insert(QStringLiteral("lines"), lines);
                }
            }
            if (midType == QLatin1String("code_body")) {
                QString codeSubType = type;
                if (type == QLatin1String("code")
                    && content.count(QStringLiteral("\\(")) > 0
                    && content.count(QStringLiteral("\\("))
                           == content.count(QStringLiteral("\\)"))) {
                    codeSubType = QStringLiteral("algorithm");
                }
                o.insert(QStringLiteral("_code_sub_type"), codeSubType);
                OfficialCodeLanguageResult codeLanguage;
                if (!guessOfficialCodeLanguage(content, &codeLanguage, err))
                    return false;
                o.insert(QStringLiteral("_guess_lang"), codeLanguage.label);
            }
        }
        if (type == QLatin1String("text") && raw.contains(QStringLiteral("merge_prev")))
            o.insert(QStringLiteral("merge_prev"), raw.value(QStringLiteral("merge_prev")));

        if (supportsOcrDetLines(midType)) {
            QJsonArray matching;
            for (int si = 0; si < sidecarSpans.size(); ++si) {
                if (usedSidecar.at(si))
                    continue;
                const QJsonArray sidecarBbox =
                    sidecarSpans.at(si).value(QStringLiteral("bbox")).toArray();
                if (spanOverlapRatio(sidecarBbox, bb) > 0.5) {
                    matching.append(sidecarSpans.at(si));
                    usedSidecar[si] = true;
                }
            }
            const QJsonArray detLines = buildOcrDetLines(matching);
            if (!detLines.isEmpty())
                o.insert(QStringLiteral("_ocr_det_lines"), detLines);
        }
        if (midType == QLatin1String("title") || midType == QLatin1String("doc_title")
            || midType == QLatin1String("paragraph_title")) {
            QJsonArray metricLines = o.value(QStringLiteral("_ocr_det_lines")).toArray();
            if (metricLines.isEmpty())
                metricLines = o.value(QStringLiteral("lines")).toArray();
            o.insert(QStringLiteral("line_avg_height"), averageLineHeight(metricLines, bb));
        }
        allBlocks.push_back(o);
    }

    // These two fallbacks run on the converted flat blocks before list/visual
    // partitioning in MinerU's HybridMagicModel constructor.
    fallbackInlineCaptionFragments(&allBlocks);
    fallbackLeadingTableContinuationCaptions(&allBlocks);

    QSet<int> listChildIndices;
    QSet<int> retainedListIndices;
    fixListBlocks(&allBlocks, &listChildIndices, &retainedListIndices);

    QVector<QJsonObject> visualRoots;
    QVector<QJsonObject> orphanVisualChildren;
    QSet<int> visualConsumedIndices;
    regroupVisualBlocks(allBlocks, &visualRoots, &orphanVisualChildren,
                        &visualConsumedIndices);

    QVector<QJsonObject> preprocObjects = visualRoots;
    QVector<QJsonObject> discardedObjects;
    for (QJsonObject block : allBlocks) {
        const QString blockType = jsonBlockType(block);
        const int blockIndex = jsonBlockIndex(block);
        const bool isDiscarded = blockType == QLatin1String("header")
                                 || blockType == QLatin1String("footer")
                                 || blockType == QLatin1String("page_number")
                                 || blockType == QLatin1String("aside_text")
                                 || blockType == QLatin1String("page_footnote");
        if (isDiscarded) {
            discardedObjects.push_back(block);
            continue;
        }
        if (isVisualMainType(blockType) || visualConsumedIndices.contains(blockIndex)
            || listChildIndices.contains(blockIndex)) {
            continue;
        }
        if (blockType == QLatin1String("list")) {
            if (retainedListIndices.contains(blockIndex))
                preprocObjects.push_back(block);
            continue;
        }
        if (isGenericVisualChildType(blockType)) {
            // Only unmatched generic children survive regroup, and official
            // degrades those to ordinary text rather than losing content.
            block.insert(QStringLiteral("type"), QStringLiteral("text"));
        }
        preprocObjects.push_back(block);
    }
    std::stable_sort(preprocObjects.begin(), preprocObjects.end(),
                     [](const QJsonObject& a, const QJsonObject& b) {
                         return jsonBlockIndex(a) < jsonBlockIndex(b);
                     });
    std::stable_sort(discardedObjects.begin(), discardedObjects.end(),
                     [](const QJsonObject& a, const QJsonObject& b) {
                         return jsonBlockIndex(a) < jsonBlockIndex(b);
                     });
    QJsonArray preproc;
    QJsonArray discarded;
    for (const QJsonObject& block : preprocObjects)
        preproc.append(block);
    for (const QJsonObject& block : discardedObjects)
        discarded.append(block);

    QJsonObject preprocPage;
    preprocPage.insert(QStringLiteral("preproc_blocks"), preproc);
    preprocPage.insert(QStringLiteral("discarded_blocks"), discarded);
    preprocPage.insert(QStringLiteral("page_size"), QJsonArray({pw, ph}));
    preprocPage.insert(QStringLiteral("page_idx"), 0);
    QJsonObject preprocRoot;
    preprocRoot.insert(QStringLiteral("pdf_info"), QJsonArray({preprocPage}));
    preprocRoot.insert(QStringLiteral("_backend"), QStringLiteral("hybrid"));
    preprocRoot.insert(QStringLiteral("_effort"),
                       effort.isEmpty() ? QStringLiteral("medium") : effort);
    preprocRoot.insert(QStringLiteral("_ocr_enable"), true);
    preprocRoot.insert(QStringLiteral("_version_name"), QStringLiteral("3.4.4"));

    auto writeObject = [&](const QString& filePath, const QJsonObject& object) {
        if (!QDir().mkpath(QFileInfo(filePath).absolutePath()))
            return false;
        QFile f(filePath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        const QByteArray bytes = officialJsonBytes(QJsonDocument(object));
        return f.write(bytes) == bytes.size() && f.flush();
    };
    if (!preprocPath.isEmpty() && !writeObject(preprocPath, preprocRoot))
        return fail(QStringLiteral("cannot write preproc middle json"));

    QJsonArray finalizedPreproc = preproc;
    QJsonArray finalizedDiscarded = discarded;
    // build_para_blocks_from_preproc is a deep copy.  QJson values are
    // copy-on-write, so this has the same mutation isolation here.
    QJsonArray para = preproc;
    mergeParaTextBlocks(&para, effort == QLatin1String("medium"));
    finalizeMiddleBlocks(&finalizedPreproc);
    finalizeMiddleBlocks(&finalizedDiscarded);
    finalizeMiddleBlocks(&para);
    QJsonObject finalPage;
    finalPage.insert(QStringLiteral("preproc_blocks"), finalizedPreproc);
    finalPage.insert(QStringLiteral("discarded_blocks"), finalizedDiscarded);
    finalPage.insert(QStringLiteral("page_size"), QJsonArray({pw, ph}));
    finalPage.insert(QStringLiteral("page_idx"), 0);
    finalPage.insert(QStringLiteral("para_blocks"), para);
    QJsonObject finalRoot = preprocRoot;
    finalRoot.insert(QStringLiteral("pdf_info"), QJsonArray({finalPage}));
    if (!writeObject(path, finalRoot))
        return fail(QStringLiteral("cannot write middle json"));
    if (err)
        err->clear();
    return true;
}

bool runHybridSchemaFixture(const QString& mode,
                            const QJsonObject& input,
                            QJsonObject* output,
                            QString* err) {
    auto fail = [&](const QString& message) {
        if (err)
            *err = message;
        return false;
    };
    if (!output)
        return fail(QStringLiteral("schema fixture output is null"));
    const QJsonObject inputCases = input.value(QStringLiteral("cases")).toObject();
    if (inputCases.isEmpty())
        return fail(QStringLiteral("schema fixture has no cases"));
    QJsonObject outputCases;
    if (mode == QLatin1String("finalize")) {
        for (auto iterator = inputCases.constBegin(); iterator != inputCases.constEnd(); ++iterator) {
            QJsonObject testCase = iterator.value().toObject();
            QJsonArray pdfInfo = testCase.value(QStringLiteral("pdf_info")).toArray();
            const bool vertical = testCase.value(QStringLiteral("effort")).toString(
                                      QStringLiteral("medium"))
                                  == QLatin1String("medium");
            for (int pageIndex = 0; pageIndex < pdfInfo.size(); ++pageIndex) {
                QJsonObject page = pdfInfo.at(pageIndex).toObject();
                QJsonArray preproc = page.value(QStringLiteral("preproc_blocks")).toArray();
                QJsonArray discarded = page.value(QStringLiteral("discarded_blocks")).toArray();
                QJsonArray para = preproc;
                mergeParaTextBlocks(&para, vertical);
                finalizeMiddleBlocks(&preproc);
                finalizeMiddleBlocks(&discarded);
                finalizeMiddleBlocks(&para);
                page.insert(QStringLiteral("preproc_blocks"), preproc);
                page.insert(QStringLiteral("discarded_blocks"), discarded);
                page.insert(QStringLiteral("para_blocks"), para);
                pdfInfo.replace(pageIndex, page);
            }
            testCase.insert(QStringLiteral("pdf_info"), pdfInfo);
            outputCases.insert(iterator.key(), testCase);
        }
    } else if (mode == QLatin1String("visual")) {
        for (auto iterator = inputCases.constBegin(); iterator != inputCases.constEnd(); ++iterator) {
            QVector<QJsonObject> blocks;
            for (const QJsonValue& value :
                 iterator.value().toObject().value(QStringLiteral("blocks")).toArray()) {
                blocks.push_back(value.toObject());
            }
            fallbackInlineCaptionFragments(&blocks);
            fallbackLeadingTableContinuationCaptions(&blocks);
            QVector<QJsonObject> roots;
            QVector<QJsonObject> unmatched;
            QSet<int> consumed;
            regroupVisualBlocks(blocks, &roots, &unmatched, &consumed);
            QJsonObject grouped;
            grouped.insert(QStringLiteral("image"), QJsonArray());
            grouped.insert(QStringLiteral("table"), QJsonArray());
            grouped.insert(QStringLiteral("chart"), QJsonArray());
            grouped.insert(QStringLiteral("code"), QJsonArray());
            for (const QJsonObject& root : roots) {
                const QString type = jsonBlockType(root);
                QJsonArray values = grouped.value(type).toArray();
                values.append(root);
                grouped.insert(type, values);
            }
            QJsonArray unmatchedArray;
            for (const QJsonObject& child : unmatched)
                unmatchedArray.append(child);
            QJsonObject resultCase;
            resultCase.insert(QStringLiteral("grouped"), grouped);
            resultCase.insert(QStringLiteral("unmatched_children"), unmatchedArray);
            outputCases.insert(iterator.key(), resultCase);
        }
    } else if (mode == QLatin1String("list")) {
        for (auto iterator = inputCases.constBegin(); iterator != inputCases.constEnd(); ++iterator) {
            QVector<QJsonObject> blocks;
            for (const QJsonValue& value :
                 iterator.value().toObject().value(QStringLiteral("blocks")).toArray()) {
                blocks.push_back(value.toObject());
            }
            QSet<int> consumedTextIndices;
            QSet<int> retainedListIndices;
            fixListBlocks(&blocks, &consumedTextIndices, &retainedListIndices);
            QJsonArray listBlocks;
            QJsonArray textBlocks;
            QJsonArray refTextBlocks;
            for (const QJsonObject& block : blocks) {
                const QString type = jsonBlockType(block);
                const int index = jsonBlockIndex(block);
                if (type == QLatin1String("list") && retainedListIndices.contains(index))
                    listBlocks.append(block);
                else if (type == QLatin1String("text")
                         && !consumedTextIndices.contains(index))
                    textBlocks.append(block);
                else if (type == QLatin1String("ref_text")
                         && !consumedTextIndices.contains(index))
                    refTextBlocks.append(block);
            }
            QJsonObject resultCase;
            resultCase.insert(QStringLiteral("list_blocks"), listBlocks);
            resultCase.insert(QStringLiteral("text_blocks"), textBlocks);
            resultCase.insert(QStringLiteral("ref_text_blocks"), refTextBlocks);
            outputCases.insert(iterator.key(), resultCase);
        }
    } else if (mode == QLatin1String("sidecar")) {
        const QJsonObject order = inputCases.value(QStringLiteral("sidecar_append_order")).toObject();
        QJsonArray modelList = order.value(QStringLiteral("model_list")).toArray();
        if (modelList.isEmpty() || !modelList.first().isArray())
            return fail(QStringLiteral("sidecar fixture model_list is invalid"));
        QJsonArray page = modelList.first().toArray();
        const QJsonArray formulaPages = order.value(QStringLiteral("inline_formula_list")).toArray();
        if (!formulaPages.isEmpty()) {
            for (const QJsonValue& value : formulaPages.first().toArray()) {
                QJsonObject item = value.toObject();
                item.insert(QStringLiteral("type"), QStringLiteral("inline_formula"));
                page.append(item);
            }
        }
        const bool keepOcrText = order.value(QStringLiteral("keep_ocr_text")).toBool();
        const QJsonArray ocrPages = order.value(QStringLiteral("ocr_res_list")).toArray();
        if (!ocrPages.isEmpty()) {
            for (const QJsonValue& value : ocrPages.first().toArray()) {
                QJsonObject item = value.toObject();
                item.insert(QStringLiteral("type"), QStringLiteral("ocr_text"));
                if (!keepOcrText)
                    item.insert(QStringLiteral("text"), QString());
                page.append(item);
            }
        }
        QJsonObject orderResult;
        QJsonArray mergedModelList;
        mergedModelList.append(QJsonValue(page));
        orderResult.insert(QStringLiteral("model_list"), mergedModelList);
        outputCases.insert(QStringLiteral("sidecar_append_order"), orderResult);

        const QJsonObject bridge = inputCases.value(QStringLiteral("three_span_bridge")).toObject();
        QJsonObject block = bridge.value(QStringLiteral("block")).toObject();
        const QJsonArray spans = block.value(QStringLiteral("spans")).toArray();
        block.remove(QStringLiteral("spans"));
        block.insert(QStringLiteral("lines"), buildOcrDetLines(spans));
        QJsonObject bridgeResult;
        bridgeResult.insert(QStringLiteral("block"), block);
        bridgeResult.insert(QStringLiteral("rule"), bridge.value(QStringLiteral("rule")));
        outputCases.insert(QStringLiteral("three_span_bridge"), bridgeResult);
    } else {
        return fail(QStringLiteral("unknown schema fixture mode: %1").arg(mode));
    }
    QJsonObject result;
    result.insert(QStringLiteral("schema"), input.value(QStringLiteral("schema")));
    result.insert(QStringLiteral("cases"), outputCases);
    *output = result;
    if (err)
        err->clear();
    return true;
}

bool writeHybridPageArtifactsFromMiddle(const QString& outputDir,
                                        const QString& sourceStem,
                                        const QString& middleJsonPath,
                                        QString* markdownPath,
                                        QString* jsonPath,
                                        QString* err) {
    if (markdownPath)
        markdownPath->clear();
    if (jsonPath)
        jsonPath->clear();
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    QFile source(middleJsonPath);
    if (!source.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open middle json: %1").arg(middleJsonPath));
    QJsonDocument document;
    QString jsonError;
    if (!parseJsonDocumentStrict(source.readAll(), &document, &jsonError))
        return fail(QStringLiteral("cannot parse middle json: %1 (%2)")
                        .arg(middleJsonPath, jsonError));
    if (!document.isObject())
        return fail(QStringLiteral("middle json is not an object: %1").arg(middleJsonPath));
    const QJsonArray pdfInfo = document.object().value(QStringLiteral("pdf_info")).toArray();
    if (pdfInfo.size() != 1)
        return fail(QStringLiteral("single-image middle json must contain exactly one page"));
    const QJsonObject pageInfo = pdfInfo.at(0).toObject();
    const QJsonArray pageSize = pageInfo.value(QStringLiteral("page_size")).toArray();
    if (pageSize.size() != 2 || pageSize.at(0).toInt() < 1 || pageSize.at(1).toInt() < 1)
        return fail(QStringLiteral("middle json has invalid page_size"));
    const int pageWidth = pageSize.at(0).toInt();
    const int pageHeight = pageSize.at(1).toInt();

    try {

    auto contentBbox = [pageWidth, pageHeight](const QJsonArray& bbox) {
        if (bbox.size() != 4)
            return QJsonArray();
        return QJsonArray({int(bbox.at(0).toDouble() * 1000.0 / pageWidth),
                           int(bbox.at(1).toDouble() * 1000.0 / pageHeight),
                           int(bbox.at(2).toDouble() * 1000.0 / pageWidth),
                           int(bbox.at(3).toDouble() * 1000.0 / pageHeight)});
    };

    auto contentItem = [&](const QJsonObject& block) {
        QJsonObject item;
        const QString type = block.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text") || type == QLatin1String("ref_text")
            || type == QLatin1String("phonetic") || type == QLatin1String("header")
            || type == QLatin1String("footer") || type == QLatin1String("page_number")
            || type == QLatin1String("aside_text") || type == QLatin1String("page_footnote")) {
            item.insert(QStringLiteral("type"), type);
            item.insert(QStringLiteral("text"), mergeParaWithText(block));
        } else if (type == QLatin1String("list")) {
            item.insert(QStringLiteral("type"), QStringLiteral("list"));
            item.insert(QStringLiteral("sub_type"),
                        block.value(QStringLiteral("sub_type")).toString());
            QJsonArray listItems;
            for (const QJsonValue& childValue : block.value(QStringLiteral("blocks")).toArray()) {
                const QString text = mergeParaWithText(childValue.toObject(), true,
                                                       QStringLiteral("images"), false);
                if (!text.trimmed().isEmpty())
                    listItems.append(text);
            }
            item.insert(QStringLiteral("list_items"), listItems);
        } else if (type == QLatin1String("table")) {
            const QPair<QString, QString> data = visualBodyData(block);
            item.insert(QStringLiteral("type"), QStringLiteral("table"));
            item.insert(QStringLiteral("img_path"), mediaPath(QStringLiteral("images"), data.first));
            item.insert(QStringLiteral("table_caption"),
                        visualChildTexts(block, QStringLiteral("table_caption")));
            item.insert(QStringLiteral("table_footnote"),
                        visualChildTexts(block, QStringLiteral("table_footnote")));
            if (!data.second.isEmpty())
                item.insert(QStringLiteral("table_body"),
                            formattedTableHtml(data.second, QStringLiteral("images")));
        } else if (type == QLatin1String("title")) {
            item.insert(QStringLiteral("type"), QStringLiteral("text"));
            item.insert(QStringLiteral("text"), mergeParaWithText(block));
            int level = block.value(QStringLiteral("level")).toInt(1);
            level = level > 4 ? 4 : (level < 1 ? 0 : level);
            if (level != 0)
                item.insert(QStringLiteral("text_level"), level);
        } else if (type == QLatin1String("interline_equation")) {
            item.insert(QStringLiteral("type"), QStringLiteral("equation"));
            item.insert(QStringLiteral("text"), mergeParaWithText(block));
            item.insert(QStringLiteral("text_format"), QStringLiteral("latex"));
        } else if (type == QLatin1String("image") || type == QLatin1String("chart")) {
            const QPair<QString, QString> data = visualBodyData(block);
            item.insert(QStringLiteral("type"), type);
            item.insert(QStringLiteral("img_path"), mediaPath(QStringLiteral("images"), data.first));
            item.insert(QStringLiteral("content"), data.second);
            const QString captionType = type + QStringLiteral("_caption");
            const QString footnoteType = type + QStringLiteral("_footnote");
            item.insert(captionType, visualChildTexts(block, captionType));
            item.insert(footnoteType, visualChildTexts(block, footnoteType));
            if (!block.value(QStringLiteral("sub_type")).toString().isEmpty())
                item.insert(QStringLiteral("sub_type"), block.value(QStringLiteral("sub_type")));
        } else if (type == QLatin1String("code")) {
            item.insert(QStringLiteral("type"), QStringLiteral("code"));
            item.insert(QStringLiteral("sub_type"), block.value(QStringLiteral("sub_type")));
            item.insert(QStringLiteral("code_caption"),
                        visualChildTexts(block, QStringLiteral("code_caption")));
            for (const QJsonValue& childValue : block.value(QStringLiteral("blocks")).toArray()) {
                const QJsonObject child = childValue.toObject();
                if (jsonBlockType(child) != QLatin1String("code_body"))
                    continue;
                if (block.value(QStringLiteral("sub_type")).toString()
                    == QLatin1String("algorithm")) {
                    item.insert(QStringLiteral("code_body"), renderAlgorithmHtml(child));
                } else {
                    item.insert(QStringLiteral("code_body"),
                                QStringLiteral("```%1\n%2\n```")
                                    .arg(block.value(QStringLiteral("guess_lang"))
                                             .toString(QStringLiteral("txt")),
                                         mergeParaWithText(child)));
                }
                break;
            }
        }
        const QJsonArray bbox = contentBbox(block.value(QStringLiteral("bbox")).toArray());
        if (!bbox.isEmpty())
            item.insert(QStringLiteral("bbox"), bbox);
        item.insert(QStringLiteral("page_idx"), pageInfo.value(QStringLiteral("page_idx")).toInt(0));
        return item;
    };

    const QJsonArray paraBlocks = pageInfo.value(QStringLiteral("para_blocks")).toArray();
    const QJsonArray discardedBlocks = pageInfo.value(QStringLiteral("discarded_blocks")).toArray();
    QJsonArray contentList;
    QString markdown;
    for (const QJsonValue& blockValue : paraBlocks) {
        const QJsonObject block = blockValue.toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        contentList.append(contentItem(block));
        QString paragraph;
        if (type == QLatin1String("text") || type == QLatin1String("interline_equation")
            || type == QLatin1String("phonetic") || type == QLatin1String("ref_text")) {
            paragraph = mergeParaWithText(block);
        } else if (type == QLatin1String("list")) {
            for (const QJsonValue& childValue : block.value(QStringLiteral("blocks")).toArray()) {
                paragraph += mergeParaWithText(childValue.toObject(), true,
                                               QStringLiteral("images"), false);
                paragraph += QStringLiteral("  \n");
            }
        } else if (type == QLatin1String("title")) {
            int level = block.value(QStringLiteral("level")).toInt(1);
            level = level > 4 ? 4 : (level < 1 ? 0 : level);
            paragraph = QString(level, QLatin1Char('#')) + QLatin1Char(' ')
                        + mergeParaWithText(block);
        } else if (type == QLatin1String("image") || type == QLatin1String("table")
                   || type == QLatin1String("chart") || type == QLatin1String("code")) {
            paragraph = renderVisualRoot(block);
        }
        if (paragraph.trimmed().isEmpty())
            continue;
        if (!markdown.isEmpty())
            markdown += QStringLiteral("\n\n");
        markdown += paragraph.trimmed();
    }
    for (const QJsonValue& blockValue : discardedBlocks)
        contentList.append(contentItem(blockValue.toObject()));

    if (!QDir().mkpath(outputDir))
        return fail(QStringLiteral("cannot create output directory: %1").arg(outputDir));
    const QString mdPath = QDir(outputDir).filePath(sourceStem + QStringLiteral(".md"));
    const QString listPath =
        QDir(outputDir).filePath(sourceStem + QStringLiteral("_content_list.json"));

    // Prepare both files completely before making either target visible.
    // QSaveFile keeps an existing target intact on open/write/commit failure;
    // disabling direct-write fallback preserves that guarantee.
    QSaveFile markdownFile(mdPath);
    markdownFile.setDirectWriteFallback(false);
    if (!markdownFile.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("cannot open markdown temporary file: %1 (%2)")
                        .arg(mdPath, markdownFile.errorString()));
    }
    QSaveFile listFile(listPath);
    listFile.setDirectWriteFallback(false);
    if (!listFile.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("cannot open content_list temporary file: %1 (%2)")
                        .arg(listPath, listFile.errorString()));
    }

    const QByteArray markdownBytes = markdown.toUtf8();
    const QByteArray listBytes = QJsonDocument(contentList).toJson(QJsonDocument::Indented);
    if (markdownFile.write(markdownBytes) != markdownBytes.size()) {
        return fail(QStringLiteral("cannot write complete markdown temporary file: %1 (%2)")
                        .arg(mdPath, markdownFile.errorString()));
    }
    if (listFile.write(listBytes) != listBytes.size()) {
        return fail(QStringLiteral("cannot write complete content_list temporary file: %1 (%2)")
                        .arg(listPath, listFile.errorString()));
    }

    // There is no cross-file atomic rename. Commit the human-readable Markdown
    // first and the machine-consumed content_list last. If the first commit
    // fails, neither new target is published. If the second commit fails, the
    // new Markdown remains and the function reports an incomplete artifact
    // pair. On reruns neither filename alone is a reliable completion signal;
    // a job-level marker should be committed only after this function succeeds.
    if (!markdownFile.commit()) {
        return fail(QStringLiteral("cannot commit markdown atomically: %1 (%2); "
                                   "content_list was not committed")
                        .arg(mdPath, markdownFile.errorString()));
    }
    if (!listFile.commit()) {
        return fail(QStringLiteral("cannot commit content_list atomically: %1 (%2); "
                                   "markdown was committed first, artifact pair is incomplete")
                        .arg(listPath, listFile.errorString()));
    }
    if (markdownPath)
        *markdownPath = mdPath;
    if (jsonPath)
        *jsonPath = listPath;
    if (err)
        err->clear();
    return true;
    } catch (const std::exception& exception) {
        return fail(QStringLiteral("official language detection failed while rendering "
                                   "Hybrid artifacts: %1")
                        .arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        return fail(QStringLiteral("official language detection failed while rendering "
                                   "Hybrid artifacts: unknown error"));
    }
}

}  // namespace hybrid
}  // namespace scanengine

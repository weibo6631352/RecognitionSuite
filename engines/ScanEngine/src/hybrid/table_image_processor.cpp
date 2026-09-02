#include "scanengine/hybrid/table_image_processor.hpp"

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QGlyphRun>
#include <QImageWriter>
#include <QPainter>
#include <QRandomGenerator>
#include <QRawFont>
#include <QRegularExpression>
#include <QStringList>

#ifdef SCANENGINE_PINNED_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace scanengine {
namespace hybrid {
namespace {

double unitCoordinate(double value) {
    // The pinned post-processor consumes the bbox value it receives.  Parser
    // normalization, when present, belongs to the parser stage; this module
    // must preserve higher-precision callers and exact 0.9 coverage decisions.
    return value;
}

double blockArea(const ContentBlock& block) {
    return std::max(0.0, unitCoordinate(block.xmax) - unitCoordinate(block.xmin))
        * std::max(0.0, unitCoordinate(block.ymax) - unitCoordinate(block.ymin));
}

double intersectionArea(const ContentBlock& a, const ContentBlock& b) {
    const double left = std::max(unitCoordinate(a.xmin), unitCoordinate(b.xmin));
    const double top = std::max(unitCoordinate(a.ymin), unitCoordinate(b.ymin));
    const double right = std::min(unitCoordinate(a.xmax), unitCoordinate(b.xmax));
    const double bottom = std::min(unitCoordinate(a.ymax), unitCoordinate(b.ymax));
    if (right <= left || bottom <= top)
        return 0.0;
    return (right - left) * (bottom - top);
}

bool isCaptionContainer(const QString& type) {
    return type == QLatin1String("image") || type == QLatin1String("chart")
        || type == QLatin1String("image_block");
}

bool isValidTokenCode(const QString& code) {
    if (code.size() != kOfficialTableImageTokenLength)
        return false;
    const QString alphabet = QString::fromLatin1(kOfficialTableImageTokenChars);
    for (const QChar ch : code) {
        if (!alphabet.contains(ch))
            return false;
    }
    return true;
}

QString randomTokenCode() {
    const QByteArray alphabet(kOfficialTableImageTokenChars);
    QString code;
    code.reserve(kOfficialTableImageTokenLength);
    for (int index = 0; index < kOfficialTableImageTokenLength; ++index) {
        code.append(QChar::fromLatin1(
            alphabet.at(QRandomGenerator::global()->bounded(alphabet.size()))));
    }
    return code;
}

QString tokenCodeForOrdinal(quint32 ordinal) {
    const QByteArray alphabet(kOfficialTableImageTokenChars);
    QString code(kOfficialTableImageTokenLength, QLatin1Char(alphabet.at(0)));
    for (int position = kOfficialTableImageTokenLength - 1; position >= 0; --position) {
        code[position] = QChar::fromLatin1(alphabet.at(int(ordinal % quint32(alphabet.size()))));
        ordinal /= quint32(alphabet.size());
    }
    return code;
}

bool nextUniqueTokenCode(const TableImageProcessorOptions& options,
                         QSet<QString>* used,
                         QString* code) {
    if (!used || !code)
        return false;
    const quint32 alphabetSize = quint32(std::strlen(kOfficialTableImageTokenChars));
    quint32 maximumCount = 1;
    for (int index = 0; index < kOfficialTableImageTokenLength; ++index)
        maximumCount *= alphabetSize;
    if (quint32(used->size()) >= maximumCount)
        return false;

    // The normal path is the same random-retry rule as Python.  A bounded
    // deterministic scan guarantees progress if an injected test generator
    // keeps returning duplicates or invalid candidates.
    for (int attempt = 0; attempt < 1024; ++attempt) {
        const QString candidate = options.tokenCodeGenerator
            ? options.tokenCodeGenerator()
            : randomTokenCode();
        if (!isValidTokenCode(candidate) || used->contains(candidate))
            continue;
        used->insert(candidate);
        *code = candidate;
        return true;
    }
    for (quint32 ordinal = 0; ordinal < maximumCount; ++ordinal) {
        const QString candidate = tokenCodeForOrdinal(ordinal);
        if (used->contains(candidate))
            continue;
        used->insert(candidate);
        *code = candidate;
        return true;
    }
    return false;
}

QColor averageSurroundingColor(const QImage& image, const TableImagePixelBox& box) {
    if (image.isNull() || image.width() < 1 || image.height() < 1)
        return QColor(255, 255, 255);
    const int pad = 2;
    const int midX = (box.left + box.right) / 2;
    const int midY = (box.top + box.bottom) / 2;
    const int points[8][2] = {
        {box.left - pad, box.top - pad},
        {midX, box.top - pad},
        {box.right + pad, box.top - pad},
        {box.right + pad, midY},
        {box.right + pad, box.bottom + pad},
        {midX, box.bottom + pad},
        {box.left - pad, box.bottom + pad},
        {box.left - pad, midY},
    };
    int red = 0;
    int green = 0;
    int blue = 0;
    for (const auto& point : points) {
        const int x = std::max(0, std::min(point[0], image.width() - 1));
        const int y = std::max(0, std::min(point[1], image.height() - 1));
        const QColor color = image.pixelColor(x, y);
        red += color.red();
        green += color.green();
        blue += color.blue();
    }
    return QColor(red / 8, green / 8, blue / 8);
}

QColor contrastTextColor(const QColor& background) {
    const double luminance = 0.299 * background.red() + 0.587 * background.green()
        + 0.114 * background.blue();
    return luminance < 128.0 ? QColor(255, 255, 255) : QColor(0, 0, 0);
}

const char* tokenGlyph(QChar character) {
    switch (character.toLatin1()) {
    case '[': return "11100100001000010000100001000011100";
    case ']': return "00111000010000100001000010000100111";
    case 'A': return "01110100011000111111100011000110001";
    case 'C': return "01111100001000010000100001000001111";
    case 'D': return "11110100011000110001100011000111110";
    case 'G': return "01111100001000010111100011000101111";
    case 'H': return "10001100011000111111100011000110001";
    case 'K': return "10001100101010011000101001001010001";
    case 'T': return "11111001000010000100001000010000100";
    case 'W': return "10001100011000110101101011010101010";
    case 'X': return "10001100010101000100010101000110001";
    case 'Y': return "10001100010101000100001000010000100";
    case 'Z': return "11111000010001000100010001000011111";
    case '2': return "01110100010000100010001000100011111";
    case '3': return "11110000010000101110000010000111110";
    case '4': return "00010001100101010010111110001000010";
    case '5': return "11111100001000011110000010000111110";
    case '6': return "01110100001000011110100011000101110";
    case '7': return "11111000010001000100010000100001000";
    case '8': return "01110100011000101110100011000101110";
    default: return nullptr;
    }
}

QString officialTokenFontPath() {
    static const QString path = [] {
        const QStringList candidates{
            QStringLiteral("C:/Windows/Fonts/arial.ttf"),
            QStringLiteral("/System/Library/Fonts/Supplemental/Arial.ttf"),
            QStringLiteral("/Library/Fonts/Arial.ttf"),
            QStringLiteral("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
            QStringLiteral("/usr/share/fonts/dejavu/DejaVuSans.ttf"),
        };
        for (const QString& candidate : candidates) {
            if (QFileInfo(candidate).isFile())
                return candidate;
        }
        return QString();
    }();
    return path;
}

#ifdef SCANENGINE_PINNED_FREETYPE

int pillowPixel(FT_Pos value) {
    return value >= 0 ? int((value + 32) / 64) : -int((-value + 31) / 64);
}

int pillowMulDiv255(int first, int second) {
    const int temporary = first * second + 128;
    return ((temporary >> 8) + temporary) >> 8;
}

int pillowBlend(int alpha, int background, int foreground) {
    const int temporary = background * (255 - alpha) + foreground * alpha + 128;
    return ((temporary >> 8) + temporary) >> 8;
}

struct PillowFreeTypeContext {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    QString path;

    ~PillowFreeTypeContext() {
        if (face)
            FT_Done_Face(face);
        if (library)
            FT_Done_FreeType(library);
    }

    bool open(const QString& fontPath) {
        if (face && path == fontPath)
            return true;
        if (face) {
            FT_Done_Face(face);
            face = nullptr;
            path.clear();
        }
        if (!library && FT_Init_FreeType(&library) != 0)
            return false;
        const QByteArray encodedPath = QFileInfo(fontPath).absoluteFilePath().toLocal8Bit();
        if (FT_New_Face(library, encodedPath.constData(), 0, &face) != 0)
            return false;
        path = fontPath;
        return true;
    }

    bool requestPixelSize(int pixelSize) {
        if (!face || pixelSize < 1)
            return false;
        FT_Size_RequestRec request{};
        request.type = FT_SIZE_REQUEST_TYPE_NOMINAL;
        request.width = FT_Long(pixelSize) * 64;
        request.height = FT_Long(pixelSize) * 64;
        return FT_Request_Size(face, &request) == 0;
    }
};

PillowFreeTypeContext& pillowFreeTypeContext() {
    thread_local PillowFreeTypeContext context;
    return context;
}

struct PillowGlyphLayout {
    FT_UInt index = 0;
    FT_Pos advance = 0;
};

struct RawFontFit {
    int pixelSize = 0;
    QVector<PillowGlyphLayout> glyphs;
    int width = 0;
    int height = 0;
    int xOffset = 0;
    int yOffset = 0;
    bool valid = false;
};

RawFontFit layoutRawFont(const QString& text, int pixelSize) {
    RawFontFit fit;
    fit.pixelSize = pixelSize;
    PillowFreeTypeContext& context = pillowFreeTypeContext();
    const QString path = officialTokenFontPath();
    if (path.isEmpty() || !context.open(path) || !context.requestPixelSize(pixelSize))
        return fit;

    const bool hasKerning = FT_HAS_KERNING(context.face) != 0;
    FT_UInt previous = 0;
    fit.glyphs.reserve(text.size());
    for (const QChar character : text) {
        const FT_UInt glyphIndex = FT_Get_Char_Index(context.face, character.unicode());
        if (hasKerning && previous && glyphIndex) {
            FT_Vector delta{};
            if (FT_Get_Kerning(context.face,
                               previous,
                               glyphIndex,
                               FT_KERNING_DEFAULT,
                               &delta)
                == 0) {
                // This is the pinned Pillow BASIC-layout behavior: PIXEL()
                // returns an integer, which is then added directly to the
                // previous 26.6 advance.
                fit.glyphs.last().advance += pillowPixel(delta.x);
            }
        }
        if (FT_Load_Glyph(context.face, glyphIndex, FT_LOAD_DEFAULT) != 0)
            return {};
        fit.glyphs.push_back({glyphIndex, context.face->glyph->metrics.horiAdvance});
        previous = glyphIndex;
    }

    FT_Pos position = 0;
    int xMin = 0;
    int xMax = 0;
    int yMin = 0;
    int yMax = 0;
    for (const PillowGlyphLayout& glyphLayout : fit.glyphs) {
        const int x = pillowPixel(position);
        position += glyphLayout.advance;
        xMax = std::max(xMax, pillowPixel(position));
        if (FT_Load_Glyph(context.face, glyphLayout.index, FT_LOAD_DEFAULT) != 0)
            return {};
        FT_Glyph glyph = nullptr;
        if (FT_Get_Glyph(context.face->glyph, &glyph) != 0)
            return {};
        FT_BBox bounds{};
        FT_Glyph_Get_CBox(glyph, FT_GLYPH_BBOX_PIXELS, &bounds);
        FT_Done_Glyph(glyph);
        xMin = std::min(xMin, int(bounds.xMin) + x);
        xMax = std::max(xMax, int(bounds.xMax) + x);
        yMin = std::min(yMin, int(bounds.yMin));
        yMax = std::max(yMax, int(bounds.yMax));
    }

    fit.width = xMax - xMin;
    fit.height = yMax - yMin;
    fit.xOffset = xMin;
    fit.yOffset = pillowPixel(context.face->size->metrics.ascender) - yMax;
    fit.valid = fit.width > 0 && fit.height > 0;
    return fit;
}

RawFontFit rawFontAtSize(const QString& text, int pixelSize) {
    return layoutRawFont(text, pixelSize);
}

RawFontFit optimalRawFont(const QString& text, int boxWidth, int boxHeight) {
    int left = 4;
    int right = std::max(100, int(double(boxHeight) * 0.7));
    RawFontFit best = rawFontAtSize(text, left);
    if (!best.valid)
        return {};
    // The pinned helper initializes best_w/best_h to zero even though its
    // initial best_font is the 4px font.  Preserve that observable behavior
    // for boxes too small for any candidate in the binary search.
    best.width = 0;
    best.height = 0;
    for (int iteration = 0; iteration < 30 && left <= right; ++iteration) {
        const int middle = (left + right) / 2;
        const RawFontFit fit = rawFontAtSize(text, middle);
        if (!fit.valid)
            return {};
        if (double(fit.width) <= double(boxWidth) * 0.7
            && double(fit.height) <= double(boxHeight) * 0.7) {
            best = fit;
            left = middle + 1;
        } else {
            right = middle - 1;
        }
    }
    return best;
}

struct RawFontCacheEntry {
    int pixelSize = 0;
    int width = 0;
    int height = 0;
};

RawFontFit cachedOptimalRawFont(const QString& text,
                               int boxWidth,
                               int boxHeight,
                               QHash<quint64, RawFontCacheEntry>* cache) {
    const quint64 key = (quint64(quint32(boxHeight / 16)) << 32)
        | quint64(quint32(text.size()));
    if (cache && cache->contains(key)) {
        const RawFontCacheEntry entry = cache->value(key);
        if (entry.width <= boxWidth && entry.height <= boxHeight) {
            RawFontFit fit = layoutRawFont(text, entry.pixelSize);
            if (fit.valid) {
                fit.width = entry.width;
                fit.height = entry.height;
                return fit;
            }
        }
    }
    RawFontFit fit = optimalRawFont(text, boxWidth, boxHeight);
    if (cache && fit.valid)
        cache->insert(key, {fit.pixelSize, fit.width, fit.height});
    return fit;
}

struct PillowRenderedGlyph {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    QByteArray pixels;
};

bool paintRawFontToken(QImage* image,
                       const TableImagePixelBox& box,
                       const QString& token,
                       const QColor& color,
                       QHash<quint64, RawFontCacheEntry>* cache) {
    if (!image || image->isNull())
        return false;
    const RawFontFit fit = cachedOptimalRawFont(token, box.width(), box.height(), cache);
    if (!fit.valid)
        return false;
    PillowFreeTypeContext& context = pillowFreeTypeContext();
    if (!context.requestPixelSize(fit.pixelSize))
        return false;

    const double centeredX = box.left + (box.width() - fit.width) / 2.0;
    const double centeredY = box.top + (box.height() - fit.height) / 2.0;
    const int integerX = int(centeredX);
    const int integerY = int(centeredY);
    const double startX = centeredX - integerX;
    const double startY = centeredY - integerY;

    FT_Pos pen = 0;
    int renderXMin = 0;
    int renderYMax = 0;
    for (const PillowGlyphLayout& glyphLayout : fit.glyphs) {
        const int x = pillowPixel(pen);
        if (FT_Load_Glyph(context.face,
                          glyphLayout.index,
                          FT_LOAD_DEFAULT | FT_LOAD_RENDER)
            != 0) {
            return false;
        }
        renderXMin = std::min(renderXMin, x + context.face->glyph->bitmap_left);
        renderYMax = std::max(renderYMax, context.face->glyph->bitmap_top);
        pen += glyphLayout.advance;
    }

    FT_Pos renderX = FT_Pos(std::llround((-renderXMin + startX) * 64.0));
    const FT_Pos renderY = FT_Pos(std::llround((-renderYMax - startY) * 64.0));
    QVector<PillowRenderedGlyph> renderedGlyphs;
    renderedGlyphs.reserve(fit.glyphs.size());
    int unionLeft = image->width();
    int unionTop = image->height();
    int unionRight = 0;
    int unionBottom = 0;
    for (const PillowGlyphLayout& glyphLayout : fit.glyphs) {
        const int x = pillowPixel(renderX);
        const int y = pillowPixel(renderY);
        if (FT_Load_Glyph(context.face,
                          glyphLayout.index,
                          FT_LOAD_DEFAULT | FT_LOAD_RENDER)
            != 0) {
            return false;
        }
        const FT_Bitmap& bitmap = context.face->glyph->bitmap;
        if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.num_grays != 256)
            return false;
        PillowRenderedGlyph rendered;
        rendered.left = integerX + fit.xOffset + x + context.face->glyph->bitmap_left;
        rendered.top = integerY + fit.yOffset
            - (y + context.face->glyph->bitmap_top);
        rendered.width = int(bitmap.width);
        rendered.height = int(bitmap.rows);
        rendered.pixels.resize(rendered.width * rendered.height);
        for (int row = 0; row < rendered.height; ++row) {
            const uchar* source = bitmap.pitch >= 0
                ? bitmap.buffer + row * bitmap.pitch
                : bitmap.buffer + (rendered.height - 1 - row) * (-bitmap.pitch);
            std::memcpy(rendered.pixels.data() + row * rendered.width,
                        source,
                        size_t(rendered.width));
        }
        unionLeft = std::min(unionLeft, rendered.left);
        unionTop = std::min(unionTop, rendered.top);
        unionRight = std::max(unionRight, rendered.left + rendered.width);
        unionBottom = std::max(unionBottom, rendered.top + rendered.height);
        renderedGlyphs.push_back(std::move(rendered));
        renderX += glyphLayout.advance;
    }

    unionLeft = std::max(0, unionLeft);
    unionTop = std::max(0, unionTop);
    unionRight = std::min(image->width(), unionRight);
    unionBottom = std::min(image->height(), unionBottom);
    if (unionRight <= unionLeft || unionBottom <= unionTop)
        return true;
    const int maskWidth = unionRight - unionLeft;
    const int maskHeight = unionBottom - unionTop;
    QByteArray alpha(maskWidth * maskHeight, char(0));
    for (const PillowRenderedGlyph& glyph : renderedGlyphs) {
        for (int row = 0; row < glyph.height; ++row) {
            const int targetY = glyph.top + row;
            if (targetY < unionTop || targetY >= unionBottom)
                continue;
            const uchar* source = reinterpret_cast<const uchar*>(
                glyph.pixels.constData() + row * glyph.width);
            uchar* target = reinterpret_cast<uchar*>(alpha.data())
                + (targetY - unionTop) * maskWidth;
            for (int column = 0; column < glyph.width; ++column) {
                const int targetX = glyph.left + column;
                if (targetX < unionLeft || targetX >= unionRight || source[column] == 0)
                    continue;
                uchar& destination = target[targetX - unionLeft];
                destination = destination == 0
                    ? source[column]
                    : uchar(std::min(255,
                                     int(source[column])
                                         + pillowMulDiv255(destination,
                                                           255 - source[column])));
            }
        }
    }

    for (int row = 0; row < maskHeight; ++row) {
        uchar* imageLine = image->scanLine(unionTop + row);
        const uchar* alphaLine = reinterpret_cast<const uchar*>(alpha.constData())
            + row * maskWidth;
        for (int column = 0; column < maskWidth; ++column) {
            const int coverage = alphaLine[column];
            if (coverage == 0)
                continue;
            uchar* pixel = imageLine + 3 * (unionLeft + column);
            pixel[0] = uchar(pillowBlend(coverage, pixel[0], color.red()));
            pixel[1] = uchar(pillowBlend(coverage, pixel[1], color.green()));
            pixel[2] = uchar(pillowBlend(coverage, pixel[2], color.blue()));
        }
    }
    return true;
}

#else

struct RawFontFit {
    QRawFont font;
    QGlyphRun glyphRun;
    int width = 0;
    int height = 0;
    bool valid = false;
};

RawFontFit layoutRawFont(const QRawFont& font, const QString& text) {
    RawFontFit fit;
    fit.font = font;
    if (!fit.font.isValid())
        return fit;
    const QVector<quint32> glyphs = fit.font.glyphIndexesForString(text);
    if (glyphs.size() != text.size())
        return fit;
    const QVector<QPointF> advances = fit.font.advancesForGlyphIndexes(
        glyphs, QRawFont::UseDesignMetrics);
    const QVector<QPointF> kernedAdvances = fit.font.advancesForGlyphIndexes(
        glyphs, QRawFont::KernedAdvances | QRawFont::UseDesignMetrics);
    if (advances.size() != glyphs.size() || kernedAdvances.size() != glyphs.size())
        return fit;
    QVector<QPointF> positions;
    positions.reserve(glyphs.size());
    qreal cursor = 0.0;
    for (int index = 0; index < glyphs.size(); ++index) {
        positions.push_back(QPointF(cursor, 0.0));
        // Pillow BASIC stores hinted advances in 26.6 units, but adds its
        // integer-pixel FT_Get_Kerning result directly to that 26.6 value.
        // Reproduce both parts from Qt's design metrics: independently round
        // each glyph advance, then retain only 1/64 of the rounded pair shift.
        const qreal pairShift = kernedAdvances.at(index).x() - advances.at(index).x();
        // At the smallest token fonts QRawFont exposes some exact FreeType
        // half-pixel advances one 26.6 step low (Arial 9px W is reported as
        // 8.484375 while Pillow hints it to 9px).  Keep the correction local
        // to that range so it cannot perturb the binary-search fit at normal
        // token sizes.
        const qreal quantizationBias = fit.font.pixelSize() <= 9.0 ? 1.0 / 64.0 : 0.0;
        qint64 pairShift26Dot6 = qint64(std::llround(pairShift * 64.0));
        const qint64 pixelSize = qint64(std::llround(fit.font.pixelSize()));
        // FT_Get_Kerning(FT_KERNING_DEFAULT) attenuates kerning below
        // 25ppem, with a rounded fixed-point multiply/divide, before snapping
        // it to a whole pixel.  Pillow then (accidentally but observably) adds
        // that integer pixel count directly to a 26.6 advance.
        if (pixelSize < 25) {
            const bool negative = pairShift26Dot6 < 0;
            const qint64 magnitude = negative ? -pairShift26Dot6 : pairShift26Dot6;
            const qint64 scaledMagnitude = (magnitude * pixelSize + 12) / 25;
            pairShift26Dot6 = negative ? -scaledMagnitude : scaledMagnitude;
        }
        const qint64 pairShiftPixels = pairShift26Dot6 >= 0
            ? (pairShift26Dot6 + 32) / 64
            : -((-pairShift26Dot6 + 31) / 64);
        cursor += std::floor(advances.at(index).x() + 0.5 + quantizationBias)
            + qreal(pairShiftPixels) / 64.0;
    }
    fit.glyphRun.setRawFont(fit.font);
    fit.glyphRun.setGlyphIndexes(glyphs);
    fit.glyphRun.setPositions(positions);
    const QRectF bounds = fit.glyphRun.boundingRect();
    const int top = int(std::floor(fit.font.ascent() + bounds.top()));
    const int bottom = int(std::ceil(fit.font.ascent() + bounds.bottom()));
    fit.width = int(std::round(cursor));
    fit.height = std::max(0, bottom - top);
    fit.valid = fit.width > 0 && fit.height > 0;
    return fit;
}

RawFontFit rawFontAtSize(const QString& text, int pixelSize) {
    RawFontFit fit;
    // Qt 5.12's Windows QRawFont backend dereferences the GUI font database
    // even when loading an explicit file.  Guard it so the CLI's existing
    // QCoreApplication gets a declared fallback instead of crashing.
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        return fit;
    const QString path = officialTokenFontPath();
    if (path.isEmpty())
        return fit;
    return layoutRawFont(QRawFont(path, pixelSize, QFont::PreferDefaultHinting), text);
}

RawFontFit optimalRawFont(const QString& text, int boxWidth, int boxHeight) {
    int left = 4;
    int right = std::max(100, int(double(boxHeight) * 0.7));
    RawFontFit best = rawFontAtSize(text, left);
    if (!best.valid)
        return {};
    for (int iteration = 0; iteration < 30 && left <= right; ++iteration) {
        const int middle = (left + right) / 2;
        const RawFontFit fit = rawFontAtSize(text, middle);
        if (!fit.valid)
            return {};
        if (double(fit.width) <= double(boxWidth) * 0.7
            && double(fit.height) <= double(boxHeight) * 0.7) {
            best = fit;
            left = middle + 1;
        } else {
            right = middle - 1;
        }
    }
    return best;
}

struct RawFontCacheEntry {
    QRawFont font;
    int width = 0;
    int height = 0;
};

RawFontFit cachedOptimalRawFont(const QString& text,
                               int boxWidth,
                               int boxHeight,
                               QHash<quint64, RawFontCacheEntry>* cache) {
    const quint64 key = (quint64(quint32(boxHeight / 16)) << 32)
        | quint64(quint32(text.size()));
    if (cache && cache->contains(key)) {
        const RawFontCacheEntry entry = cache->value(key);
        if (entry.width <= boxWidth && entry.height <= boxHeight) {
            RawFontFit fit = layoutRawFont(entry.font, text);
            if (fit.valid) {
                // The pinned Python cache deliberately reuses the first
                // token's dimensions for every equal-length token in the same
                // height bucket, even when glyph widths differ.
                fit.width = entry.width;
                fit.height = entry.height;
                return fit;
            }
        }
    }
    RawFontFit fit = optimalRawFont(text, boxWidth, boxHeight);
    if (cache && fit.valid)
        cache->insert(key, {fit.font, fit.width, fit.height});
    return fit;
}

bool paintRawFontToken(QPainter* painter,
                       const TableImagePixelBox& box,
                       const QString& token,
                       const QColor& color,
                       QHash<quint64, RawFontCacheEntry>* cache) {
    if (!painter)
        return false;
    const RawFontFit fit = cachedOptimalRawFont(token,
                                               box.width(),
                                               box.height(),
                                               cache);
    if (!fit.valid)
        return false;
    // ImageDraw splits a fractional text coordinate into an integer bitmap
    // origin and a 26.6 `start` value.  FreeType then rounds start + the
    // accumulated pen before rasterizing every glyph.  Rebuild those integer
    // positions here instead of letting QPainter apply sub-pixel antialiasing.
    const qreal centeredX = box.left + (box.width() - fit.width) / 2.0;
    const qreal textX = std::floor(centeredX);
    const qreal startX = std::floor((centeredX - textX) * 64.0 + 0.5) / 64.0;
    QGlyphRun glyphRun = fit.glyphRun;
    QVector<QPointF> positions = glyphRun.positions();
    for (QPointF& position : positions)
        position.setX(std::floor(startX + position.x() + 0.5));
    glyphRun.setPositions(positions);

    // The vertical origin is truncated before the ascender is applied.
    const qreal textY = std::floor(box.top + (box.height() - fit.height) / 2.0);
    painter->setRenderHint(QPainter::TextAntialiasing, true);
    painter->setPen(color);
    painter->drawGlyphRun(QPointF(textX, textY + fit.font.ascent()), glyphRun);
    return true;
}

#endif  // SCANENGINE_PINNED_FREETYPE

void paintBitmapToken(QPainter* painter,
                      const TableImagePixelBox& box,
                      const QString& token,
                      const QColor& color) {
    if (!painter || token.isEmpty())
        return;
    constexpr int glyphWidth = 5;
    constexpr int glyphHeight = 7;
    constexpr int glyphGap = 1;
    const int unitsWide = token.size() * glyphWidth + (token.size() - 1) * glyphGap;
    const int horizontalScale = int(std::floor(double(box.width()) * 0.7 / unitsWide));
    const int verticalScale = int(std::floor(double(box.height()) * 0.7 / glyphHeight));
    const int scale = std::min(horizontalScale, verticalScale);
    if (scale < 1)
        return;
    const int paintedWidth = unitsWide * scale;
    const int paintedHeight = glyphHeight * scale;
    const int originX = box.left + (box.width() - paintedWidth) / 2;
    const int originY = box.top + (box.height() - paintedHeight) / 2;
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    for (int characterIndex = 0; characterIndex < token.size(); ++characterIndex) {
        const char* glyph = tokenGlyph(token.at(characterIndex));
        if (!glyph)
            continue;
        const int glyphX = originX + characterIndex * (glyphWidth + glyphGap) * scale;
        for (int row = 0; row < glyphHeight; ++row) {
            for (int column = 0; column < glyphWidth; ++column) {
                if (glyph[row * glyphWidth + column] != '1')
                    continue;
                painter->drawRect(glyphX + column * scale,
                                  originY + row * scale,
                                  scale,
                                  scale);
            }
        }
    }
}

bool jpegDataUri(const QImage& input, QString* dataUri) {
    if (!dataUri || input.isNull())
        return false;
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return false;
    QImageWriter writer(&buffer, QByteArrayLiteral("jpeg"));
    // Pillow Image.save(..., format="JPEG") uses quality 75 by default.
    writer.setQuality(75);
    if (!writer.write(input.convertToFormat(QImage::Format_RGB888)))
        return false;
    // Qt and Pillow use the same libjpeg defaults here, but Qt marks the JFIF
    // density as 100 dpi while Pillow writes aspect-only 1:1.  Normalizing the
    // five density bytes makes the Windows payload byte-identical without
    // changing decoded pixels.
    if (bytes.size() >= 18 && uchar(bytes.at(0)) == 0xff && uchar(bytes.at(1)) == 0xd8
        && uchar(bytes.at(2)) == 0xff && uchar(bytes.at(3)) == 0xe0
        && bytes.mid(6, 5) == QByteArray("JFIF\0", 5)) {
        bytes[13] = char(0);
        bytes[14] = char(0);
        bytes[15] = char(1);
        bytes[16] = char(0);
        bytes[17] = char(1);
    }
    *dataUri = QStringLiteral("data:image/jpeg;base64,")
        + QString::fromLatin1(bytes.toBase64());
    return true;
}

bool paintMaskAndToken(QImage* image,
                       const TableImagePixelBox& box,
                       const QString& token,
                       QHash<quint64, RawFontCacheEntry>* fontCache) {
    if (!image || image->isNull() || !box.isValid())
        return false;
    const QColor background = averageSurroundingColor(*image, box);
    QPainter painter(image);
    // PIL ImageDraw.rectangle includes both coordinate endpoints.
    painter.fillRect(box.left,
                     box.top,
                     box.width() + 1,
                     box.height() + 1,
                     background);

    const QColor textColor = contrastTextColor(background);
#ifdef SCANENGINE_PINNED_FREETYPE
    painter.end();
    const bool usedFallback = !paintRawFontToken(image,
                                                 box,
                                                 token,
                                                 textColor,
                                                 fontCache);
    if (usedFallback) {
        QPainter fallbackPainter(image);
        paintBitmapToken(&fallbackPainter, box, token, textColor);
        fallbackPainter.end();
    }
#else
    // QRawFont loads the same explicit Arial/DejaVu candidate files as the
    // pinned Pillow code, without consulting QGuiApplication's global font
    // database (ScanEngineTool intentionally uses QCoreApplication).  The 5x7
    // branch is only a last-resort equivalent of ImageFont.load_default when
    // none of those platform files is present.
    const bool usedFallback = !paintRawFontToken(&painter,
                                                 box,
                                                 token,
                                                 textColor,
                                                 fontCache);
    if (usedFallback)
        paintBitmapToken(&painter, box, token, textColor);
    painter.end();
#endif
    return usedFallback;
}

TableImageMetadata buildTableImageMapImpl(const QVector<ContentBlock>& blocks,
                                          const QVector<int>& requestedTableIndices,
                                          double threshold) {
    TableImageMetadata metadata;
    QSet<int> seenTables;
    for (int tableIndex : requestedTableIndices) {
        if (tableIndex < 0 || tableIndex >= blocks.size() || seenTables.contains(tableIndex))
            continue;
        seenTables.insert(tableIndex);
        metadata.tableOrder.push_back(tableIndex);
        metadata.tableToImages.insert(tableIndex, {});
    }
    if (metadata.tableOrder.isEmpty())
        return metadata;

    for (int imageIndex = 0; imageIndex < blocks.size(); ++imageIndex) {
        const ContentBlock& image = blocks.at(imageIndex);
        if (image.type != QLatin1String("image"))
            continue;
        bool found = false;
        int bestTableIndex = -1;
        double bestRatio = threshold;
        double bestArea = 0.0;
        for (int tableIndex : metadata.tableOrder) {
            const ContentBlock& table = blocks.at(tableIndex);
            const double ratio = tableImageCoverageRatio(image, table);
            if (ratio < threshold)
                continue;
            const double area = blockArea(table);
            if (!found || ratio > bestRatio
                || (ratio == bestRatio && area < bestArea)) {
                found = true;
                bestTableIndex = tableIndex;
                bestRatio = ratio;
                bestArea = area;
            }
        }
        if (found)
            metadata.tableToImages[bestTableIndex].push_back(imageIndex);
    }

    for (int tableIndex : metadata.tableOrder) {
        QVector<int>& imageIndices = metadata.tableToImages[tableIndex];
        std::stable_sort(imageIndices.begin(), imageIndices.end(), [&](int lhs, int rhs) {
            const ContentBlock& a = blocks.at(lhs);
            const ContentBlock& b = blocks.at(rhs);
            if (a.ymin != b.ymin)
                return a.ymin < b.ymin;
            return a.xmin < b.xmin;
        });
    }
    markAbsorbedTableImages(&metadata);
    return metadata;
}

}  // namespace

void TableImageMetadata::clear() {
    tableOrder.clear();
    tableToImages.clear();
    absorbedImageIndices.clear();
    tokensByTable.clear();
}

bool TableImageMetadata::isEmpty() const {
    return tableOrder.isEmpty() && tableToImages.isEmpty()
        && absorbedImageIndices.isEmpty() && tokensByTable.isEmpty();
}

double tableImageCoverageRatio(const ContentBlock& inner, const ContentBlock& outer) {
    const double area = blockArea(inner);
    if (area == 0.0)
        return 0.0;
    return intersectionArea(inner, outer) / area;
}

int removeInternalImageCaptions(QVector<ContentBlock>* blocks,
                                double threshold,
                                QVector<int>* removedOriginalIndices,
                                QVector<int>* keptOriginalIndices) {
    if (removedOriginalIndices)
        removedOriginalIndices->clear();
    if (keptOriginalIndices)
        keptOriginalIndices->clear();
    if (!blocks)
        return 0;
    if (blocks->isEmpty())
        return 0;
    QVector<int> containerIndices;
    for (int index = 0; index < blocks->size(); ++index) {
        if (isCaptionContainer(blocks->at(index).type))
            containerIndices.push_back(index);
    }
    if (containerIndices.isEmpty()) {
        if (keptOriginalIndices) {
            keptOriginalIndices->reserve(blocks->size());
            for (int index = 0; index < blocks->size(); ++index)
                keptOriginalIndices->push_back(index);
        }
        return 0;
    }

    QSet<int> covered;
    for (int index = 0; index < blocks->size(); ++index) {
        if (blocks->at(index).type != QLatin1String("image_caption"))
            continue;
        for (int containerIndex : containerIndices) {
            if (index == containerIndex)
                continue;
            if (tableImageCoverageRatio(blocks->at(index), blocks->at(containerIndex))
                >= threshold) {
                covered.insert(index);
                break;
            }
        }
    }
    if (covered.isEmpty()) {
        if (keptOriginalIndices) {
            keptOriginalIndices->reserve(blocks->size());
            for (int index = 0; index < blocks->size(); ++index)
                keptOriginalIndices->push_back(index);
        }
        return 0;
    }

    QVector<ContentBlock> kept;
    kept.reserve(blocks->size() - covered.size());
    for (int index = 0; index < blocks->size(); ++index) {
        if (!covered.contains(index)) {
            kept.push_back(blocks->at(index));
            if (keptOriginalIndices)
                keptOriginalIndices->push_back(index);
        } else if (removedOriginalIndices) {
            removedOriginalIndices->push_back(index);
        }
    }
    *blocks = kept;
    return covered.size();
}

TableImageMetadata buildTableImageMap(const QVector<ContentBlock>& blocks, double threshold) {
    QVector<int> tableIndices;
    for (int index = 0; index < blocks.size(); ++index) {
        if (blocks.at(index).type == QLatin1String("table"))
            tableIndices.push_back(index);
    }
    return buildTableImageMapImpl(blocks, tableIndices, threshold);
}

TableImageMetadata buildTableImageMap(const QVector<ContentBlock>& blocks,
                                      const QVector<int>& tableIndices,
                                      double threshold) {
    return buildTableImageMapImpl(blocks, tableIndices, threshold);
}

void markAbsorbedTableImages(TableImageMetadata* metadata) {
    if (!metadata)
        return;
    metadata->absorbedImageIndices.clear();
    for (int tableIndex : metadata->tableOrder) {
        const QVector<int> indices = metadata->tableToImages.value(tableIndex);
        for (int imageIndex : indices)
            metadata->absorbedImageIndices.insert(imageIndex);
    }
}

bool isAbsorbedTableImage(const TableImageMetadata& metadata, int blockIndex) {
    return metadata.absorbedImageIndices.contains(blockIndex);
}

int normalizeTableImageAngle(const std::optional<int>& angle) {
    if (!angle)
        return 0;
    return *angle == 90 || *angle == 180 || *angle == 270 ? *angle : 0;
}

TableImagePixelBox rotateTableImageBox(const TableImagePixelBox& box,
                                       const QSize& originalImageSize,
                                       int angle) {
    const int width = originalImageSize.width();
    const int height = originalImageSize.height();
    if (angle == 90)
        return {box.top, width - box.right, box.bottom, width - box.left};
    if (angle == 180) {
        return {width - box.right,
                height - box.bottom,
                width - box.left,
                height - box.top};
    }
    if (angle == 270)
        return {height - box.bottom, box.left, height - box.top, box.right};
    return box;
}

QImage rotateTableImage(const QImage& input, int angle) {
    const QImage source = input.convertToFormat(QImage::Format_RGB888);
    const int width = source.width();
    const int height = source.height();
    if (source.isNull() || width < 1 || height < 1)
        return source;
    if (angle == 180) {
        QImage output(width, height, QImage::Format_RGB888);
        for (int y = 0; y < height; ++y) {
            const uchar* sourceLine = source.constScanLine(height - 1 - y);
            uchar* outputLine = output.scanLine(y);
            for (int x = 0; x < width; ++x) {
                std::memcpy(outputLine + 3 * x,
                            sourceLine + 3 * (width - 1 - x),
                            3);
            }
        }
        return output;
    }
    if (angle == 90) {
        QImage output(height, width, QImage::Format_RGB888);
        for (int y = 0; y < height; ++y) {
            const uchar* sourceLine = source.constScanLine(y);
            for (int x = 0; x < width; ++x) {
                uchar* outputLine = output.scanLine(width - 1 - x);
                std::memcpy(outputLine + 3 * y, sourceLine + 3 * x, 3);
            }
        }
        return output;
    }
    if (angle == 270) {
        QImage output(height, width, QImage::Format_RGB888);
        for (int y = 0; y < height; ++y) {
            const uchar* sourceLine = source.constScanLine(y);
            for (int x = 0; x < width; ++x) {
                uchar* outputLine = output.scanLine(x);
                std::memcpy(outputLine + 3 * (height - 1 - y),
                            sourceLine + 3 * x,
                            3);
            }
        }
        return output;
    }
    return source;
}

bool maskAndEncodeTableImage(const QImage& pageImage,
                             const ContentBlock& tableBlock,
                             const QVector<TableImageEntry>& imageEntries,
                             const QImage& tableImage,
                             TableImageMaskedCrop* out,
                             QString* error,
                             const TableImageProcessorOptions& options) {
    auto fail = [&](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("table image output is null"));
    *out = {};
    if (pageImage.isNull() || pageImage.width() < 1 || pageImage.height() < 1)
        return fail(QStringLiteral("page image is empty"));
    if (tableImage.isNull() || tableImage.width() < 1 || tableImage.height() < 1)
        return fail(QStringLiteral("table crop is empty"));

    const QImage page = pageImage.convertToFormat(QImage::Format_RGB888);
    const QImage originalTable = tableImage.convertToFormat(QImage::Format_RGB888);
    const int angle = normalizeTableImageAngle(tableBlock.angle);
    out->image = rotateTableImage(originalTable, angle);

    const int pageWidth = page.width();
    const int pageHeight = page.height();
    const int tableAbsoluteLeft = int(unitCoordinate(tableBlock.xmin) * pageWidth);
    const int tableAbsoluteTop = int(unitCoordinate(tableBlock.ymin) * pageHeight);
    QSet<QString> usedCodes;
    QHash<quint64, RawFontCacheEntry> fontCache;

    for (const TableImageEntry& entry : imageEntries) {
        const ContentBlock& imageBlock = entry.block;
        const double absoluteLeft = unitCoordinate(imageBlock.xmin) * pageWidth;
        const double absoluteTop = unitCoordinate(imageBlock.ymin) * pageHeight;
        const double absoluteRight = unitCoordinate(imageBlock.xmax) * pageWidth;
        const double absoluteBottom = unitCoordinate(imageBlock.ymax) * pageHeight;

        const TableImagePixelBox relative{
            int(std::max(0.0, absoluteLeft - tableAbsoluteLeft)),
            int(std::max(0.0, absoluteTop - tableAbsoluteTop)),
            int(std::min(double(originalTable.width()), absoluteRight - tableAbsoluteLeft)),
            int(std::min(double(originalTable.height()), absoluteBottom - tableAbsoluteTop)),
        };
        if (!relative.isValid())
            continue;

        const int cropLeft = int(absoluteLeft);
        const int cropTop = int(absoluteTop);
        const int cropRight = int(absoluteRight);
        const int cropBottom = int(absoluteBottom);
        if (cropRight <= cropLeft || cropBottom <= cropTop)
            continue;
        const QImage embedded = page.copy(cropLeft,
                                          cropTop,
                                          cropRight - cropLeft,
                                          cropBottom - cropTop);
        if (embedded.isNull() || embedded.width() < 1 || embedded.height() < 1)
            continue;

        QString code;
        if (!nextUniqueTokenCode(options, &usedCodes, &code))
            return fail(QStringLiteral("exhausted table image token space"));
        TableImageToken token;
        token.imageBlockIndex = entry.blockIndex;
        token.code = code;
        token.token = QStringLiteral("[") + code + QStringLiteral("]");
        token.maskBox = rotateTableImageBox(relative, originalTable.size(), angle);
        const QImage rotatedEmbedded = rotateTableImage(embedded, angle);
        if (!jpegDataUri(rotatedEmbedded, &token.dataUri))
            return fail(QStringLiteral("failed to encode embedded table image as JPEG"));

        token.usedFallbackFontRaster = paintMaskAndToken(&out->image,
                                                         token.maskBox,
                                                         token.token,
                                                         &fontCache);
        if (token.usedFallbackFontRaster && !options.allowFallbackFontRaster) {
            return fail(QStringLiteral(
                "pinned FreeType table token raster is unavailable; install an "
                "Arial/DejaVu candidate or explicitly allow the diagnostic fallback"));
        }
        out->tokens.push_back(token);
    }
    if (error)
        error->clear();
    return true;
}

bool prepareTableImageForExtract(const QImage& pageImage,
                                 const QVector<ContentBlock>& blocks,
                                 int tableIndex,
                                 const QImage& tableImage,
                                 TableImageMetadata* metadata,
                                 QImage* maskedTableImage,
                                 QString* error,
                                 const TableImageProcessorOptions& options) {
    auto fail = [&](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!metadata)
        return fail(QStringLiteral("table image metadata is null"));
    if (!maskedTableImage)
        return fail(QStringLiteral("masked table image output is null"));
    if (tableIndex < 0 || tableIndex >= blocks.size())
        return fail(QStringLiteral("table block index is out of range"));

    QVector<TableImageEntry> entries;
    const QVector<int> imageIndices = metadata->tableToImages.value(tableIndex);
    entries.reserve(imageIndices.size());
    for (int imageIndex : imageIndices) {
        if (imageIndex < 0 || imageIndex >= blocks.size())
            return fail(QStringLiteral("embedded image block index is out of range"));
        entries.push_back({imageIndex, blocks.at(imageIndex)});
    }

    TableImageMaskedCrop result;
    if (!maskAndEncodeTableImage(pageImage,
                                 blocks.at(tableIndex),
                                 entries,
                                 tableImage,
                                 &result,
                                 error,
                                 options)) {
        return false;
    }
    *maskedTableImage = result.image;
    metadata->tokensByTable.insert(tableIndex, result.tokens);
    return true;
}

QString replaceTableImageTokens(const QString& content,
                                const QVector<TableImageToken>& tokens) {
    if (content.isEmpty() || tokens.isEmpty())
        return content;
    QString replaced = content;
    for (const TableImageToken& token : tokens) {
        const QString pattern = QStringLiteral("\\[\\s*")
            + QRegularExpression::escape(token.code)
            + QStringLiteral("\\s*\\]");
        replaced.replace(QRegularExpression(pattern),
                         QStringLiteral("<img src=\"") + token.dataUri
                             + QStringLiteral("\"/>"));
    }
    return replaced;
}

void applyTableImageTokens(QVector<ContentBlock>* blocks,
                           const TableImageMetadata& metadata) {
    if (!blocks)
        return;
    for (int tableIndex : metadata.tableOrder) {
        if (tableIndex < 0 || tableIndex >= blocks->size())
            continue;
        ContentBlock& block = (*blocks)[tableIndex];
        if (block.type != QLatin1String("table"))
            continue;
        block.content = replaceTableImageTokens(block.content,
                                                metadata.tokensByTable.value(tableIndex));
    }
}

void removeAbsorbedTableImages(QVector<ContentBlock>* blocks,
                               const TableImageMetadata& metadata) {
    if (!blocks || metadata.absorbedImageIndices.isEmpty())
        return;
    QVector<ContentBlock> kept;
    kept.reserve(std::max(0, blocks->size() - metadata.absorbedImageIndices.size()));
    for (int index = 0; index < blocks->size(); ++index) {
        const ContentBlock& block = blocks->at(index);
        if (block.type == QLatin1String("image")
            && metadata.absorbedImageIndices.contains(index)) {
            continue;
        }
        kept.push_back(block);
    }
    *blocks = kept;
}

void cleanupTableImageMetadata(TableImageMetadata* metadata) {
    if (metadata)
        metadata->clear();
}

}  // namespace hybrid
}  // namespace scanengine

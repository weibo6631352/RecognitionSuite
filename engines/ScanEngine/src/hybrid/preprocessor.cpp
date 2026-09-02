#include "scanengine/hybrid/preprocessor.hpp"

#include "scanengine/hybrid/rounding.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <vector>

namespace scanengine {
namespace hybrid {

bool loadOfficialPreprocessorConfig(const QString& path, VlmPreprocessorConfig* out, QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open ") + path);
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return fail(QStringLiteral("invalid preprocessor JSON"));

    const QJsonObject root = doc.object();
    VlmPreprocessorConfig c;
    c.minPixels = root.value(QStringLiteral("min_pixels")).toInt();
    c.maxPixels = root.value(QStringLiteral("max_pixels")).toInt();
    c.patchSize = root.value(QStringLiteral("patch_size")).toInt();
    c.temporalPatchSize = root.value(QStringLiteral("temporal_patch_size")).toInt();
    c.mergeSize = root.value(QStringLiteral("merge_size")).toInt();
    c.imageProcessorType = root.value(QStringLiteral("image_processor_type")).toString();
    const QJsonArray mean = root.value(QStringLiteral("image_mean")).toArray();
    const QJsonArray std = root.value(QStringLiteral("image_std")).toArray();
    if (mean.size() != 3 || std.size() != 3)
        return fail(QStringLiteral("image_mean / image_std must have 3 channels"));
    for (int i = 0; i < 3; ++i) {
        c.imageMean[i] = mean.at(i).toDouble();
        c.imageStd[i] = std.at(i).toDouble();
    }
    if (c.minPixels <= 0 || c.maxPixels <= 0 || c.patchSize <= 0)
        return fail(QStringLiteral("missing min_pixels / max_pixels / patch_size"));

    *out = c;
    if (err)
        err->clear();
    return true;
}

std::pair<int, int> officialSmartResize(int height, int width, int factor, int minPixels, int maxPixels) {
    // transformers qwen2_vl smart_resize.
    int hBar = roundHalfToEven(double(height) / factor) * factor;
    int wBar = roundHalfToEven(double(width) / factor) * factor;
    if (hBar * wBar > maxPixels) {
        const double beta = std::sqrt(double(height) * double(width) / double(maxPixels));
        hBar = std::max(factor, int(std::floor(height / beta / factor)) * factor);
        wBar = std::max(factor, int(std::floor(width / beta / factor)) * factor);
    } else if (hBar * wBar < minPixels) {
        const double beta = std::sqrt(double(minPixels) / (double(height) * double(width)));
        hBar = int(std::ceil(height * beta / factor)) * factor;
        wBar = int(std::ceil(width * beta / factor)) * factor;
    }
    return {hBar, wBar};
}

namespace {

constexpr int kPillowPrecisionBits = 22;
constexpr int kPillowPrecisionScale = 1 << kPillowPrecisionBits;
constexpr int kPillowPrecisionHalf = 1 << (kPillowPrecisionBits - 1);

double pillowBicubicFilter(double x) {
    constexpr double a = -0.5;
    if (x < 0.0)
        x = -x;
    if (x < 1.0)
        return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
    if (x < 2.0)
        return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
    return 0.0;
}

struct PillowBicubicKernel {
    int xmin = 0;
    int count = 0;
    std::vector<int> weights;
};

std::vector<PillowBicubicKernel> precomputePillowBicubic(int inSize, int outSize) {
    const double scale = double(inSize) / double(outSize);
    const double filterscale = scale < 1.0 ? 1.0 : scale;
    const double support = 2.0 * filterscale;
    const double invFilterscale = 1.0 / filterscale;
    const int ksize = int(std::ceil(support)) * 2 + 1;
    std::vector<PillowBicubicKernel> out;
    out.resize(size_t(outSize));
    for (int xx = 0; xx < outSize; ++xx) {
        const double center = (double(xx) + 0.5) * scale;
        int xmin = int(center - support + 0.5);
        if (xmin < 0)
            xmin = 0;
        int xmax = int(center + support + 0.5);
        if (xmax > inSize)
            xmax = inSize;
        xmax -= xmin;

        std::vector<double> coefficients(size_t(ksize), 0.0);
        double ww = 0.0;
        for (int x = 0; x < xmax; ++x) {
            const double w = pillowBicubicFilter(
                (double(x + xmin) - center + 0.5) * invFilterscale);
            coefficients[size_t(x)] = w;
            ww += w;
        }
        if (ww != 0.0) {
            for (int x = 0; x < xmax; ++x)
                coefficients[size_t(x)] /= ww;
        }

        PillowBicubicKernel kernel;
        kernel.xmin = xmin;
        kernel.count = xmax;
        kernel.weights.resize(size_t(xmax));
        for (int x = 0; x < xmax; ++x) {
            const double v = coefficients[size_t(x)] * double(kPillowPrecisionScale);
            kernel.weights[size_t(x)] = int(v < 0.0 ? v - 0.5 : v + 0.5);
        }
        out[size_t(xx)] = std::move(kernel);
    }
    return out;
}

uchar clipPillowU8(int value) {
    const int fixedPointValue = value - kPillowPrecisionHalf;
    value = roundHalfToEven(double(fixedPointValue) / double(kPillowPrecisionScale));
    // ImagingResample adds a half-unit before its fixed-point shift. Preserve
    // that exact positive-halfway result after using the shared quantizer.
    if (fixedPointValue >= 0
        && fixedPointValue % kPillowPrecisionScale == kPillowPrecisionHalf) {
        value = (fixedPointValue + kPillowPrecisionHalf) >> kPillowPrecisionBits;
    }
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return uchar(value);
}

}  // namespace

QImage resizeOfficialBicubic(const QImage& srcIn, int dstWidth, int dstHeight) {
    const QImage src = srcIn.convertToFormat(QImage::Format_RGB888);
    if (src.isNull() || dstWidth < 1 || dstHeight < 1)
        return src;
    if (src.width() == dstWidth && src.height() == dstHeight)
        return src;

    const int sw = src.width();
    const int sh = src.height();
    const auto kx = precomputePillowBicubic(sw, dstWidth);
    const auto ky = precomputePillowBicubic(sh, dstHeight);

    // Pillow ImagingResample performs two uint8 passes. The horizontal result
    // is quantized before it is consumed by the vertical pass.
    std::vector<uchar> tmp(size_t(sh) * size_t(dstWidth) * 3, 0);
    for (int y = 0; y < sh; ++y) {
        const uchar* row = src.constScanLine(y);
        for (int x = 0; x < dstWidth; ++x) {
            const PillowBicubicKernel& k = kx[size_t(x)];
            int r = kPillowPrecisionHalf;
            int g = r;
            int b = r;
            for (int j = 0; j < k.count; ++j) {
                const int xi = k.xmin + j;
                const uchar* p = row + xi * 3;
                const int w = k.weights[size_t(j)];
                r += int(p[0]) * w;
                g += int(p[1]) * w;
                b += int(p[2]) * w;
            }
            uchar* d = tmp.data() + (size_t(y) * size_t(dstWidth) + size_t(x)) * 3;
            d[0] = clipPillowU8(r);
            d[1] = clipPillowU8(g);
            d[2] = clipPillowU8(b);
        }
    }

    QImage out(dstWidth, dstHeight, QImage::Format_RGB888);
    for (int y = 0; y < dstHeight; ++y) {
        const PillowBicubicKernel& k = ky[size_t(y)];
        uchar* dst = out.scanLine(y);
        for (int x = 0; x < dstWidth; ++x) {
            int r = kPillowPrecisionHalf;
            int g = r;
            int b = r;
            for (int j = 0; j < k.count; ++j) {
                const int yi = k.xmin + j;
                const uchar* p = tmp.data() +
                    (size_t(yi) * size_t(dstWidth) + size_t(x)) * 3;
                const int w = k.weights[size_t(j)];
                r += int(p[0]) * w;
                g += int(p[1]) * w;
                b += int(p[2]) * w;
            }
            dst[x * 3] = clipPillowU8(r);
            dst[x * 3 + 1] = clipPillowU8(g);
            dst[x * 3 + 2] = clipPillowU8(b);
        }
    }
    return out;
}

bool processOfficialImage(const QImage& image,
                          const VlmPreprocessorConfig& cfg,
                          OfficialImagePatches* out,
                          QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));
    if (image.isNull())
        return fail(QStringLiteral("empty image"));

    const int factor = cfg.patchSize * cfg.mergeSize;
    const auto hw = officialSmartResize(image.height(), image.width(), factor, cfg.minPixels, cfg.maxPixels);
    const int rh = hw.first;
    const int rw = hw.second;
    QImage rgb = resizeOfficialBicubic(image.convertToFormat(QImage::Format_RGB888), rw, rh);

    const int p = cfg.patchSize;
    const int m = cfg.mergeSize;
    const int tp = cfg.temporalPatchSize;
    const int gh = rh / p;
    const int gw = rw / p;
    const int gt = 1;
    const int inner = 3 * tp * p * p;
    const int nPatch = gt * gh * gw;

    OfficialImagePatches r;
    r.gridT = gt;
    r.gridH = gh;
    r.gridW = gw;
    r.patchInner = inner;
    r.mergeSize = m;
    r.patches.resize(nPatch * inner);

    // Destination layout after official transpose(0,3,6,4,7,2,1,5,8):
    // (gt, gh/m, gw/m, m, m, C, tp, p, p)
    const int ghm = gh / m;
    const int gwm = gw / m;
    int outIdx = 0;
    for (int gti = 0; gti < gt; ++gti) {
        for (int hm = 0; hm < ghm; ++hm) {
            for (int wm = 0; wm < gwm; ++wm) {
                for (int mh = 0; mh < m; ++mh) {
                    for (int mw = 0; mw < m; ++mw) {
                        float* dst = r.patches.data() + outIdx * inner;
                        int d = 0;
                        for (int c = 0; c < 3; ++c) {
                            for (int t = 0; t < tp; ++t) {
                                for (int py = 0; py < p; ++py) {
                                    for (int px = 0; px < p; ++px) {
                                        const int y = (hm * m + mh) * p + py;
                                        const int x = (wm * m + mw) * p + px;
                                        const uchar* pix = rgb.constScanLine(y) + x * 3;
                                        // transformers.rescale computes in float64, casts to
                                        // float32, then normalize subtracts/divides in float32.
                                        const float v = float(double(pix[c]) * (1.0 / 255.0));
                                        const float mean = float(cfg.imageMean[c]);
                                        const float stddev = float(cfg.imageStd[c]);
                                        dst[d++] = (v - mean) / stddev;
                                    }
                                }
                            }
                        }
                        ++outIdx;
                    }
                }
            }
        }
    }

    *out = r;
    if (err)
        err->clear();
    return true;
}

}  // namespace hybrid
}  // namespace scanengine

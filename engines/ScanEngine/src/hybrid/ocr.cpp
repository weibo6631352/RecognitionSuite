#include "scanengine/hybrid/ocr.hpp"

#include "scanengine/config.hpp"
#include "scanengine/hybrid/extract.hpp"
#include "scanengine/hybrid/layout.hpp"
#include "scanengine/hybrid/ocr_ppv6.hpp"
#include "scanengine/hybrid/rounding.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QRect>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numeric>
#include <optional>
#include <vector>



namespace scanengine {
namespace hybrid {
namespace {

constexpr float kDetThresh = 0.3f;
constexpr int kDetLimit = 960;
constexpr int kMaxCandidates = 1000;
constexpr int kRecH = 48;
constexpr int kRecWBase = 320;
constexpr int kRecMinW = 16;
constexpr int kRecMaxW = 2560;
constexpr int kMinBox = 3;
constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};

struct Pt {
    float x = 0;
    float y = 0;
};

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

int clampi(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}

QJsonArray normalizeSidecarBboxToUnit(double x0, double y0, double x1, double y1,
                                       int pageWidth, int pageHeight) {
    // MinerU hybrid_analyze.normalize_bbox_to_unit accepts both normalized and
    // pixel inputs, then clamps and applies Python's round(..., 3) semantics.
    const bool alreadyUnit = x0 >= 0.0 && x0 <= 1.0 && y0 >= 0.0 && y0 <= 1.0
                             && x1 >= 0.0 && x1 <= 1.0 && y1 >= 0.0 && y1 <= 1.0;
    if (!alreadyUnit) {
        x0 /= double(pageWidth);
        y0 /= double(pageHeight);
        x1 /= double(pageWidth);
        y1 /= double(pageHeight);
    }
    const auto clampRound3 = [](double value) {
        value = std::min(std::max(value, 0.0), 1.0);
        return roundToDecimalDigits(value, 3);
    };
    return QJsonArray({clampRound3(x0), clampRound3(y0), clampRound3(x1), clampRound3(y1)});
}

QByteArray officialSidecarJsonBytes(const QJsonObject& root) {
    QString text = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
    // Python keeps confidence scores and normalized bbox coordinates as float
    // values even when they are integral.  QJsonDocument prints those doubles
    // as integers, so restore the public JSON number type used by MinerU.
    static const QRegularExpression integralScore(
        QStringLiteral("(\\\"score\\\"\\s*:\\s*)(-?\\d+)(?=\\s*[,}])"));
    text.replace(integralScore, QStringLiteral("\\1\\2.0"));
    QStringList lines = text.split(QLatin1Char('\n'));
    bool inBbox = false;
    static const QRegularExpression integralArrayValue(
        QStringLiteral("^(\\s*)(-?\\d+)(,?)$"));
    for (QString& line : lines) {
        if (line.contains(QStringLiteral("\"bbox\"")) && line.contains(QLatin1Char('['))) {
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
    return lines.join(QLatin1Char('\n')).toUtf8();
}

QImage toBgr888(const QImage& in) {
    QImage rgb = in.convertToFormat(QImage::Format_RGB888);
    QImage bgr(rgb.size(), QImage::Format_RGB888);
    for (int y = 0; y < rgb.height(); ++y) {
        const uchar* s = rgb.constScanLine(y);
        uchar* d = bgr.scanLine(y);
        for (int x = 0; x < rgb.width(); ++x) {
            d[x * 3 + 0] = s[x * 3 + 2];
            d[x * 3 + 1] = s[x * 3 + 1];
            d[x * 3 + 2] = s[x * 3 + 0];
        }
    }
    return bgr;
}

QImage resizeBilinear(const QImage& srcIn, int dw, int dh) {
    const QImage src = srcIn.convertToFormat(QImage::Format_RGB888);
    if (src.width() == dw && src.height() == dh)
        return src;
    QImage dst(dw, dh, QImage::Format_RGB888);
    const int sw = src.width();
    const int sh = src.height();
    const float sx = float(sw) / float(dw);
    const float sy = float(sh) / float(dh);
    for (int y = 0; y < dh; ++y) {
        const float fy = (float(y) + 0.5f) * sy - 0.5f;
        const int y0 = clampi(int(std::floor(fy)), 0, sh - 1);
        const int y1 = clampi(y0 + 1, 0, sh - 1);
        const float ty = fy - float(y0);
        const uchar* r0 = src.constScanLine(y0);
        const uchar* r1 = src.constScanLine(y1);
        uchar* out = dst.scanLine(y);
        for (int x = 0; x < dw; ++x) {
            const float fx = (float(x) + 0.5f) * sx - 0.5f;
            const int x0 = clampi(int(std::floor(fx)), 0, sw - 1);
            const int x1 = clampi(x0 + 1, 0, sw - 1);
            const float tx = fx - float(x0);
            for (int c = 0; c < 3; ++c) {
                const float v00 = r0[x0 * 3 + c];
                const float v10 = r0[x1 * 3 + c];
                const float v01 = r1[x0 * 3 + c];
                const float v11 = r1[x1 * 3 + c];
                const float v = (1 - ty) * ((1 - tx) * v00 + tx * v10) + ty * ((1 - tx) * v01 + tx * v11);
                out[x * 3 + c] = uchar(clampf(std::round(v), 0, 255));
            }
        }
    }
    return dst;
}

void detResize(int srcH, int srcW, int* dstH, int* dstW) {
    float ratio = 1.f;
    if (std::max(srcH, srcW) > kDetLimit) {
        ratio = srcH > srcW ? float(kDetLimit) / float(srcH) : float(kDetLimit) / float(srcW);
    }
    int rh = int(srcH * ratio);
    int rw = int(srcW * ratio);
    if (std::max(rh, rw) > 4000) {
        const float r2 = 4000.f / float(std::max(rh, rw));
        rh = int(rh * r2);
        rw = int(rw * r2);
    }
    rh = std::max(int(std::round(rh / 32.0)) * 32, 32);
    rw = std::max(int(std::round(rw / 32.0)) * 32, 32);
    *dstH = rh;
    *dstW = rw;
}

std::vector<float> imageToDetChw(const QImage& bgr, int dw, int dh) {
    const QImage resized = resizeBilinear(bgr, dw, dh);
    std::vector<float> chw(size_t(3 * dw * dh));
    for (int y = 0; y < dh; ++y) {
        const uchar* row = resized.constScanLine(y);
        for (int x = 0; x < dw; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float v = float(row[x * 3 + c]) / 255.0f;
                chw[size_t(c * dh * dw + y * dw + x)] = (v - kMean[c]) / kStd[c];
            }
        }
    }
    return chw;
}

std::vector<float> imageToRecChw(const QImage& bgr, int dstW, int* validW) {
    const int h = bgr.height();
    const int w = bgr.width();
    const float ratio = (h > 0) ? float(w) / float(h) : 1.f;
    const int resizedW = std::min(dstW, std::max(kRecMinW, int(std::ceil(kRecH * ratio))));
    const QImage resized = resizeBilinear(bgr, resizedW, kRecH);
    *validW = resizedW;
    std::vector<float> chw(size_t(3 * kRecH * dstW), 0.f);
    for (int y = 0; y < kRecH; ++y) {
        const uchar* row = resized.constScanLine(y);
        for (int x = 0; x < resizedW; ++x) {
            for (int c = 0; c < 3; ++c)
                chw[size_t(c * kRecH * dstW + y * dstW + x)] = float(row[x * 3 + c]) / 127.5f - 1.f;
        }
    }
    return chw;
}

float polyArea(const std::vector<Pt>& p) {
    double a = 0;
    const int n = int(p.size());
    for (int i = 0; i < n; ++i) {
        const Pt& u = p[size_t(i)];
        const Pt& v = p[size_t((i + 1) % n)];
        a += double(u.x) * double(v.y) - double(v.x) * double(u.y);
    }
    return float(std::fabs(a) * 0.5);
}

float polyLen(const std::vector<Pt>& p) {
    double l = 0;
    const int n = int(p.size());
    for (int i = 0; i < n; ++i) {
        const Pt& u = p[size_t(i)];
        const Pt& v = p[size_t((i + 1) % n)];
        l += std::hypot(double(v.x - u.x), double(v.y - u.y));
    }
    return float(l);
}

std::vector<Pt> convexHull(std::vector<Pt> pts) {
    if (pts.size() < 3)
        return pts;
    std::sort(pts.begin(), pts.end(), [](const Pt& a, const Pt& b) {
        if (a.x != b.x)
            return a.x < b.x;
        return a.y < b.y;
    });
    auto cross = [](const Pt& o, const Pt& a, const Pt& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    std::vector<Pt> lo, up;
    for (const Pt& p : pts) {
        while (lo.size() >= 2 && cross(lo[lo.size() - 2], lo.back(), p) <= 0)
            lo.pop_back();
        lo.push_back(p);
    }
    for (int i = int(pts.size()) - 1; i >= 0; --i) {
        const Pt& p = pts[size_t(i)];
        while (up.size() >= 2 && cross(up[up.size() - 2], up.back(), p) <= 0)
            up.pop_back();
        up.push_back(p);
    }
    lo.pop_back();
    up.pop_back();
    lo.insert(lo.end(), up.begin(), up.end());
    return lo;
}

bool minAreaRect(const std::vector<Pt>& pts, std::array<Pt, 4>* box, float* minSide) {
    const std::vector<Pt> hull = convexHull(pts);
    if (hull.size() < 3)
        return false;
    float best = 1e30f;
    std::array<Pt, 4> bestBox{};
    float bestMin = 0;
    const int n = int(hull.size());
    for (int i = 0; i < n; ++i) {
        const Pt a = hull[size_t(i)];
        const Pt b = hull[size_t((i + 1) % n)];
        const float ex = b.x - a.x;
        const float ey = b.y - a.y;
        const float el = std::hypot(ex, ey);
        if (el < 1e-6f)
            continue;
        const float ux = ex / el;
        const float uy = ey / el;
        const float vx = -uy;
        const float vy = ux;
        float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
        for (const Pt& p : hull) {
            const float du = (p.x - a.x) * ux + (p.y - a.y) * uy;
            const float dv = (p.x - a.x) * vx + (p.y - a.y) * vy;
            minU = std::min(minU, du);
            maxU = std::max(maxU, du);
            minV = std::min(minV, dv);
            maxV = std::max(maxV, dv);
        }
        const float w = maxU - minU;
        const float h = maxV - minV;
        const float area = w * h;
        if (area < best) {
            best = area;
            bestMin = std::min(w, h);
            const Pt o{a.x + ux * minU + vx * minV, a.y + uy * minU + vy * minV};
            bestBox[0] = o;
            bestBox[1] = Pt{o.x + ux * w, o.y + uy * w};
            bestBox[2] = Pt{o.x + ux * w + vx * h, o.y + uy * w + vy * h};
            bestBox[3] = Pt{o.x + vx * h, o.y + vy * h};
        }
    }
    *box = bestBox;
    *minSide = bestMin;
    return best < 1e29f;
}

std::array<Pt, 4> orderClockwise(std::array<Pt, 4> p) {
    std::sort(p.begin(), p.end(), [](const Pt& a, const Pt& b) { return a.x < b.x; });
    Pt l0 = p[0], l1 = p[1], r0 = p[2], r1 = p[3];
    if (l0.y > l1.y)
        std::swap(l0, l1);
    if (r0.y > r1.y)
        std::swap(r0, r1);
    return {l0, r0, r1, l1};  // tl tr br bl
}

std::vector<Pt> unclipQuad(const std::array<Pt, 4>& box, float ratio) {
    std::vector<Pt> poly(box.begin(), box.end());
    const float area = polyArea(poly);
    const float len = polyLen(poly);
    if (len < 1e-6f)
        return poly;
    const float dist = area * ratio / len;
    std::vector<Pt> out(4);
    for (int i = 0; i < 4; ++i) {
        const Pt prev = box[size_t((i + 3) % 4)];
        const Pt cur = box[size_t(i)];
        const Pt next = box[size_t((i + 1) % 4)];
        Pt e1{cur.x - prev.x, cur.y - prev.y};
        Pt e2{next.x - cur.x, next.y - cur.y};
        const float l1 = std::hypot(e1.x, e1.y);
        const float l2 = std::hypot(e2.x, e2.y);
        if (l1 < 1e-6f || l2 < 1e-6f) {
            out[size_t(i)] = cur;
            continue;
        }
        e1.x /= l1;
        e1.y /= l1;
        e2.x /= l2;
        e2.y /= l2;
        // outward normal for CCW/CW: use left normal of incoming and outgoing
        Pt n1{e1.y, -e1.x};
        Pt n2{e2.y, -e2.x};
        // pick consistent outward (away from centroid)
        float cx = 0, cy = 0;
        for (const Pt& q : box) {
            cx += q.x;
            cy += q.y;
        }
        cx *= 0.25f;
        cy *= 0.25f;
        if ((cur.x + n1.x - cx) * (cur.x - cx) + (cur.y + n1.y - cy) * (cur.y - cy) < 0) {
            n1.x = -n1.x;
            n1.y = -n1.y;
        }
        if ((cur.x + n2.x - cx) * (cur.x - cx) + (cur.y + n2.y - cy) * (cur.y - cy) < 0) {
            n2.x = -n2.x;
            n2.y = -n2.y;
        }
        Pt n{n1.x + n2.x, n1.y + n2.y};
        const float nl = std::hypot(n.x, n.y);
        if (nl < 1e-6f) {
            out[size_t(i)] = Pt{cur.x + n1.x * dist, cur.y + n1.y * dist};
            continue;
        }
        n.x /= nl;
        n.y /= nl;
        const float cosA = clampf(n.x * n1.x + n.y * n1.y, 0.2f, 1.f);
        out[size_t(i)] = Pt{cur.x + n.x * dist / cosA, cur.y + n.y * dist / cosA};
    }
    return out;
}

float boxScore(const float* pred, int ph, int pw, const std::array<Pt, 4>& box) {
    // Official DBPostProcess.box_score_fast: AABB crop + fillPoly mask mean.
    float xmin = box[0].x, xmax = box[0].x, ymin = box[0].y, ymax = box[0].y;
    for (const Pt& p : box) {
        xmin = std::min(xmin, p.x);
        xmax = std::max(xmax, p.x);
        ymin = std::min(ymin, p.y);
        ymax = std::max(ymax, p.y);
    }
    const int x0 = clampi(int(std::floor(xmin)), 0, pw - 1);
    const int x1 = clampi(int(std::ceil(xmax)), 0, pw - 1);
    const int y0 = clampi(int(std::floor(ymin)), 0, ph - 1);
    const int y1 = clampi(int(std::ceil(ymax)), 0, ph - 1);
    auto edge = [](const Pt& a, const Pt& b, float x, float y) {
        return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
    };
    double sum = 0;
    int n = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float px = float(x);
            const float py = float(y);
            int pos = 0;
            int neg = 0;
            bool inside = true;
            for (int i = 0; i < 4; ++i) {
                const float c = edge(box[size_t(i)], box[size_t((i + 1) % 4)], px, py);
                if (c > 0.f)
                    ++pos;
                else if (c < 0.f)
                    ++neg;
                if (pos && neg) {
                    inside = false;
                    break;
                }
            }
            if (!inside)
                continue;
            sum += pred[y * pw + x];
            ++n;
        }
    }
    return n ? float(sum / n) : 0;
}

void floodCollect(const std::vector<char>& mask, int h, int w, int sx, int sy, std::vector<char>* seen,
                  std::vector<Pt>* pts) {
    std::vector<int> st;
    st.push_back(sy * w + sx);
    (*seen)[size_t(sy * w + sx)] = 1;
    while (!st.empty()) {
        const int id = st.back();
        st.pop_back();
        const int y = id / w;
        const int x = id % w;
        pts->push_back(Pt{float(x), float(y)});
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy)
                    continue;
                const int nx = x + dx;
                const int ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                    continue;
                const int nid = ny * w + nx;
                if ((*seen)[size_t(nid)] || !mask[size_t(nid)])
                    continue;
                (*seen)[size_t(nid)] = 1;
                st.push_back(nid);
            }
        }
    }
}

QVector<OcrQuad> dbPostprocess(const float* pred, int ph, int pw, int srcH, int srcW, float boxThresh,
                               float unclipRatio) {
    std::vector<char> mask(size_t(ph * pw), 0);
    for (int i = 0; i < ph * pw; ++i)
        mask[size_t(i)] = char(pred[i] > kDetThresh);
    std::vector<char> seen(size_t(ph * pw), 0);
    QVector<OcrQuad> out;
    int found = 0;
    for (int y = 0; y < ph && found < kMaxCandidates; ++y) {
        for (int x = 0; x < pw && found < kMaxCandidates; ++x) {
            const int id = y * pw + x;
            if (!mask[size_t(id)] || seen[size_t(id)])
                continue;
            std::vector<Pt> pts;
            floodCollect(mask, ph, pw, x, y, &seen, &pts);
            if (pts.size() < 4)
                continue;
            std::array<Pt, 4> rect{};
            float side = 0;
            if (!minAreaRect(pts, &rect, &side) || side < kMinBox)
                continue;
            if (boxScore(pred, ph, pw, rect) < boxThresh)
                continue;
            std::vector<Pt> exp = unclipQuad(rect, unclipRatio);
            if (exp.size() < 4)
                continue;
            if (!minAreaRect(exp, &rect, &side) || side < kMinBox + 2)
                continue;
            rect = orderClockwise(rect);
            OcrQuad q;
            for (int i = 0; i < 4; ++i) {
                const float px = clampf(std::round(rect[size_t(i)].x / float(pw) * float(srcW)), 0, float(srcW - 1));
                const float py = clampf(std::round(rect[size_t(i)].y / float(ph) * float(srcH)), 0, float(srcH - 1));
                q.xy[size_t(i * 2)] = px;
                q.xy[size_t(i * 2 + 1)] = py;
            }
            const float rw = std::hypot(q.xy[2] - q.xy[0], q.xy[3] - q.xy[1]);
            const float rh = std::hypot(q.xy[6] - q.xy[0], q.xy[7] - q.xy[1]);
            if (int(rw) <= 3 || int(rh) <= 3)
                continue;
            out.push_back(q);
            ++found;
        }
    }
    return out;
}

bool isAngleBox(const OcrQuad& q) {
    const float h = ((q.xy[7] - q.xy[1]) + (q.xy[5] - q.xy[3])) * 0.5f;
    const float d = q.xy[5] - q.xy[1];
    return !(0.8f * h <= d && d <= 1.2f * h);
}

QVector<OcrQuad> mergeDetBoxes(QVector<OcrQuad> boxes) {
    struct Span {
        float x0, y0, x1, y1;
        bool angled;
        OcrQuad raw;
    };
    QVector<Span> spans;
    QVector<OcrQuad> angled;
    for (const OcrQuad& q : boxes) {
        if (isAngleBox(q)) {
            angled.push_back(q);
            continue;
        }
        Span s;
        s.x0 = q.xy[0];
        s.y0 = q.xy[1];
        s.x1 = q.xy[4];
        s.y1 = q.xy[5];
        s.raw = q;
        spans.push_back(s);
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.y0 < b.y0; });
    auto yOverlap = [](const Span& a, const Span& b) {
        const float ov = std::max(0.f, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
        const float mh = std::min(a.y1 - a.y0, b.y1 - b.y0);
        return mh > 0 && ov / mh > 0.6f;
    };
    QVector<QVector<Span>> lines;
    for (const Span& s : spans) {
        if (lines.isEmpty() || !yOverlap(lines.back().back(), s))
            lines.push_back({s});
        else
            lines.back().push_back(s);
    }
    QVector<OcrQuad> out;
    for (QVector<Span>& line : lines) {
        float minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
        for (const Span& s : line) {
            minX = std::min(minX, s.x0);
            maxX = std::max(maxX, s.x1);
            minY = std::min(minY, s.y0);
            maxY = std::max(maxY, s.y1);
        }
        if ((maxX - minX) > (maxY - minY) * 4.f) {
            std::sort(line.begin(), line.end(), [](const Span& a, const Span& b) { return a.x0 < b.x0; });
            QVector<Span> merged;
            for (const Span& s : line) {
                if (merged.isEmpty() || merged.back().x1 < s.x0)
                    merged.push_back(s);
                else {
                    merged.back().x0 = std::min(merged.back().x0, s.x0);
                    merged.back().y0 = std::min(merged.back().y0, s.y0);
                    merged.back().x1 = std::max(merged.back().x1, s.x1);
                    merged.back().y1 = std::max(merged.back().y1, s.y1);
                }
            }
            for (const Span& s : merged) {
                OcrQuad q;
                q.xy = {s.x0, s.y0, s.x1, s.y0, s.x1, s.y1, s.x0, s.y1};
                out.push_back(q);
            }
        } else {
            for (const Span& s : line)
                out.push_back(s.raw);
        }
    }
    out += angled;
    return out;
}

QVector<OcrQuad> sortBoxes(QVector<OcrQuad> boxes) {
    std::sort(boxes.begin(), boxes.end(), [](const OcrQuad& a, const OcrQuad& b) {
        if (a.xy[1] != b.xy[1])
            return a.xy[1] < b.xy[1];
        return a.xy[0] < b.xy[0];
    });
    for (int i = 0; i + 1 < boxes.size(); ++i) {
        for (int j = i; j >= 0; --j) {
            if (std::fabs(boxes[j + 1].xy[1] - boxes[j].xy[1]) < 10.f && boxes[j + 1].xy[0] < boxes[j].xy[0])
                std::swap(boxes[j], boxes[j + 1]);
            else
                break;
        }
    }
    return boxes;
}

QImage cropTable(const QImage& page, const LayoutDet& d, int pw, int ph) {
    float x0 = d.xmin, y0 = d.ymin, x1 = d.xmax, y1 = d.ymax;
    if (x0 >= 0 && x0 <= 1 && y0 >= 0 && y0 <= 1 && x1 >= 0 && x1 <= 1 && y1 >= 0 && y1 <= 1) {
        x0 *= float(pw);
        x1 *= float(pw);
        y0 *= float(ph);
        y1 *= float(ph);
    }
    const int ix0 = clampi(int(std::floor(x0)), 0, pw);
    const int iy0 = clampi(int(std::floor(y0)), 0, ph);
    const int ix1 = clampi(int(std::ceil(x1)), 0, pw);
    const int iy1 = clampi(int(std::ceil(y1)), 0, ph);
    if (ix1 <= ix0 || iy1 <= iy0)
        return QImage();
    return page.copy(ix0, iy0, ix1 - ix0, iy1 - iy0);
}

QImage cropPad(const QImage& page, float xmin, float ymin, float xmax, float ymax, int pad) {
    const int pw = page.width();
    const int ph = page.height();
    int x0 = int(std::floor(xmin));
    int y0 = int(std::floor(ymin));
    int x1 = int(std::ceil(xmax));
    int y1 = int(std::ceil(ymax));
    if (xmin >= 0 && xmin <= 1 && xmax >= 0 && xmax <= 1) {
        x0 = int(std::floor(xmin * pw));
        y0 = int(std::floor(ymin * ph));
        x1 = int(std::ceil(xmax * pw));
        y1 = int(std::ceil(ymax * ph));
    }
    x0 = clampi(x0, 0, pw);
    y0 = clampi(y0, 0, ph);
    x1 = clampi(x1, 0, pw);
    y1 = clampi(y1, 0, ph);
    QImage out(x1 - x0 + pad * 2, y1 - y0 + pad * 2, QImage::Format_RGB888);
    out.fill(qRgb(255, 255, 255));
    if (x1 > x0 && y1 > y0) {
        const QImage crop = page.copy(x0, y0, x1 - x0, y1 - y0).convertToFormat(QImage::Format_RGB888);
        for (int y = 0; y < crop.height(); ++y)
            memcpy(out.scanLine(y + pad) + pad * 3, crop.constScanLine(y), size_t(crop.width() * 3));
    }
    return out;
}

QImage rotateLabel(const QImage& img, const QString& label) {
    // Official: cv2.ROTATE_90_COUNTERCLOCKWISE for "90", CLOCKWISE for "270".
    if (label == QLatin1String("90"))
        return rotateOfficialPil(90, img);
    if (label == QLatin1String("270"))
        return rotateOfficialPil(270, img);
    return img;
}

int countVertical(const QVector<OcrQuad>& boxes) {
    int n = 0;
    for (const OcrQuad& q : boxes) {
        const float w = q.xy[4] - q.xy[0];
        const float h = q.xy[5] - q.xy[1];
        const float ar = h > 0 ? w / h : 1.f;
        if (ar < 0.8f)
            ++n;
    }
    return n;
}

bool isRotationCandidate(const QVector<OcrQuad>& boxes) {
    if (boxes.isEmpty())
        return false;
    const int v = countVertical(boxes);
    return v >= 3 && float(v) >= float(boxes.size()) * 0.28f;
}

QVector<OcrQuad> sampleBoxes(const QVector<OcrQuad>& boxes) {
    if (boxes.size() <= 18)
        return boxes;
    QVector<OcrQuad> out;
    QSet<int> used;
    for (int i = 0; i < 18; ++i) {
        const int idx = int(std::round(double(i) * (boxes.size() - 1) / 17.0));
        if (used.contains(idx))
            continue;
        used.insert(idx);
        out.push_back(boxes[idx]);
    }
    return out;
}

QImage cropQuadAabb(const QImage& img, const OcrQuad& q) {
    float xmin = q.xy[0], xmax = q.xy[0], ymin = q.xy[1], ymax = q.xy[1];
    for (int i = 0; i < 4; ++i) {
        xmin = std::min(xmin, q.xy[size_t(i * 2)]);
        xmax = std::max(xmax, q.xy[size_t(i * 2)]);
        ymin = std::min(ymin, q.xy[size_t(i * 2 + 1)]);
        ymax = std::max(ymax, q.xy[size_t(i * 2 + 1)]);
    }
    const int x0 = clampi(int(std::floor(xmin)), 0, img.width());
    const int y0 = clampi(int(std::floor(ymin)), 0, img.height());
    const int x1 = clampi(int(std::ceil(xmax)), 0, img.width());
    const int y1 = clampi(int(std::ceil(ymax)), 0, img.height());
    if (x1 <= x0 || y1 <= y0)
        return QImage();
    return img.copy(x0, y0, x1 - x0, y1 - y0);
}

struct OrientScore {
    float score = 0;
    int valid = 0;
    int chars = 0;
};

bool betterOrient(const OrientScore& a, const OrientScore& b) {
    // Official: max(labels, key=lambda l: (score, valid_count, char_count))
    if (a.score != b.score)
        return a.score > b.score;
    if (a.valid != b.valid)
        return a.valid > b.valid;
    return a.chars > b.chars;
}

OrientScore scoreRec(const QVector<QPair<QString, float>>& recs) {
    QVector<float> valid;
    int chars = 0;
    for (const auto& r : recs) {
        if (r.first.trimmed().isEmpty())
            continue;
        valid.push_back(r.second);
        chars += r.first.size();
    }
    OrientScore o;
    o.valid = valid.size();
    o.chars = chars;
    if (valid.size() < 5)
        return o;
    float s = 0;
    for (float v : valid)
        s += v;
    o.score = s / float(valid.size());
    return o;
}

QString pickAngle(const QMap<QString, OrientScore>& scores) {
    const OrientScore z = scores.value(QStringLiteral("0"));
    if (z.score >= 0.9f)
        return QStringLiteral("0");
    QString best = QStringLiteral("0");
    OrientScore bests = z;
    for (const QString& k : {QStringLiteral("0"), QStringLiteral("90"), QStringLiteral("270")}) {
        const OrientScore s = scores.value(k);
        if (betterOrient(s, bests)) {
            bests = s;
            best = k;
        }
    }
    if (best != QLatin1String("0") && bests.score - z.score < 0.08f)
        return QStringLiteral("0");
    return best;
}

struct OcrRuntime {
    QStringList chars;
    explicit OcrRuntime(const QString& dictPath) {
        chars << QStringLiteral("blank");
        QFile f(dictPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts.setCodec("UTF-8");
            while (!ts.atEnd())
                chars << ts.readLine();
        }
        chars << QStringLiteral(" ");
    }
};

OcrRuntime* cachedOcr(QString* err) {
    static std::mutex mu;
    static OcrRuntime* rt = nullptr;
    std::lock_guard<std::mutex> lock(mu);
    if (rt)
        return rt;
    const QString dir = resolveOfficialOcrDir();
    const QString detW = QDir(dir).filePath(QStringLiteral("ch_PP-OCRv6_small_det_infer.safetensors"));
    const QString recW = QDir(dir).filePath(QStringLiteral("ch_PP-OCRv6_small_rec_infer.safetensors"));
    const QString dictPath = resolveOfficialOcrDict();
    if (!QFileInfo::exists(detW) || !QFileInfo::exists(recW) || !QFileInfo::exists(dictPath)) {
        if (err)
            *err = QStringLiteral("missing official PP-OCRv6 safetensors/dict");
        return nullptr;
    }
    rt = new OcrRuntime(dictPath);
    return rt;
}

bool runDet(OcrRuntime*, const QImage& image, const OcrDetConfig& cfg,
            QVector<OcrQuad>* boxes, QString* err) {
    if (!boxes) {
        if (err)
            *err = QStringLiteral("OCR detector output is null");
        return false;
    }
    const QImage bgr = toBgr888(image);
    int dw = 0, dh = 0;
    detResize(bgr.height(), bgr.width(), &dh, &dw);
    std::vector<float> chw = imageToDetChw(bgr, dw, dh);
    QVector<float> maps;
    int mh = 0, mw = 0;
    if (!officialOcrDetForward(chw.data(), dh, dw, &maps, &mh, &mw, err))
        return false;
    QVector<OcrQuad> detected = dbPostprocess(maps.constData(), mh, mw, bgr.height(), bgr.width(),
                                              cfg.boxThresh, cfg.unclipRatio);
    detected = sortBoxes(std::move(detected));
    if (cfg.mergeBoxes)
        detected = mergeDetBoxes(std::move(detected));
    *boxes = std::move(detected);
    return true;
}

bool runRec(OcrRuntime* rt, const QVector<QImage>& crops,
            QVector<QPair<QString, float>>* out, QString* err) {
    if (!out) {
        if (err)
            *err = QStringLiteral("OCR recognizer output is null");
        return false;
    }
    *out = QVector<QPair<QString, float>>(crops.size(), {QString(), 0.f});
    if (crops.isEmpty())
        return true;
    QVector<int> order(crops.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const float ra = crops[a].height() > 0 ? float(crops[a].width()) / float(crops[a].height()) : 1.f;
        const float rb = crops[b].height() > 0 ? float(crops[b].width()) / float(crops[b].height()) : 1.f;
        return ra < rb;
    });
    const int batch = 6;
    for (int beg = 0; beg < order.size(); beg += batch) {
        const int end = std::min(beg + batch, order.size());
        float maxRatio = 0;
        for (int i = beg; i < end; ++i) {
            const QImage& im = crops[order[i]];
            const float r = im.height() > 0 ? float(im.width()) / float(im.height()) : 1.f;
            maxRatio = std::max(maxRatio, r);
        }
        int imgW = int(kRecH * std::max(maxRatio, float(kRecWBase) / float(kRecH)));
        imgW = std::max(kRecMinW, std::min(kRecMaxW, imgW));
        const int n = end - beg;
        std::vector<float> batchChw(size_t(n * 3 * kRecH * imgW));
        for (int i = 0; i < n; ++i) {
            int valid = 0;
            const std::vector<float> one = imageToRecChw(toBgr888(crops[order[beg + i]]), imgW, &valid);
            std::memcpy(batchChw.data() + size_t(i) * one.size(), one.data(), one.size() * sizeof(float));
        }
        QVector<float> logitsV;
        int t = 0, c = 0;
        if (!officialOcrRecForward(batchChw.data(), n, kRecH, imgW, &logitsV, &t, &c, err))
            return false;
        const float* logits = logitsV.constData();
        for (int i = 0; i < n; ++i) {
            QString text;
            float probSum = 0;
            int keep = 0;
            int prev = -1;
            for (int s = 0; s < t; ++s) {
                const float* row = logits + (size_t(i) * t + s) * c;
                int best = 0;
                float bestv = row[0];
                for (int k = 1; k < c; ++k) {
                    if (row[k] > bestv) {
                        bestv = row[k];
                        best = k;
                    }
                }
                double lse = 0;
                for (int k = 0; k < c; ++k)
                    lse += std::exp(double(row[k]) - double(bestv));
                const float p = float(1.0 / lse);
                if (best == 0 || best == prev)
                    continue;
                prev = best;
                if (best >= 0 && best < rt->chars.size()) {
                    text += rt->chars[best];
                    probSum += p;
                    ++keep;
                }
            }
            (*out)[order[beg + i]] = {text, keep ? probSum / float(keep) : 0.f};
        }
    }
    return true;
}

}  // namespace

QString resolveOfficialOcrDir() {
    QFile f(modelManifestPath());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject dirs = QJsonDocument::fromJson(f.readAll())
                                     .object()
                                     .value(QStringLiteral("models-dir"))
                                     .toObject();
        const QString ocr = resolveRepoPath(dirs.value(QStringLiteral("ocr")).toString());
        if (!ocr.isEmpty())
            return ocr;
        const QString pipeline = resolveRepoPath(dirs.value(QStringLiteral("pipeline")).toString());
        if (!pipeline.isEmpty())
            return QDir(pipeline).filePath(QStringLiteral("models/OCR/paddleocr_torch"));
    }
    return QDir(defaultModelsDir()).filePath(QStringLiteral("ocr"));
}

QString resolveOfficialOcrDict() {
    return QDir(defaultModelsDir()).filePath(QStringLiteral("dict/ppocrv6_dict.txt"));
}

bool detectTextBoxes(const QImage& image, const OcrDetConfig& cfg, QVector<OcrQuad>* boxes, QString* err) {
    if (!boxes)
        return false;
    OcrRuntime* rt = cachedOcr(err);
    if (!rt)
        return false;
    try {
        return runDet(rt, image, cfg, boxes, err);
    } catch (const std::exception& ex) {
        if (err)
            *err = QString::fromUtf8(ex.what());
        return false;
    }
}

bool recognizeCrops(const QVector<QImage>& crops, QVector<QPair<QString, float>>* out, QString* err) {
    if (!out)
        return false;
    OcrRuntime* rt = cachedOcr(err);
    if (!rt)
        return false;
    try {
        return runRec(rt, crops, out, err);
    } catch (const std::exception& ex) {
        if (err)
            *err = QString::fromUtf8(ex.what());
        return false;
    }
}

bool applyOfficialTableOrientation(const QImage& page, QVector<LayoutDet>* dets, int pageWidth, int pageHeight,
                                   QString* err) {
    if (!dets)
        return false;
    OcrRuntime* rt = cachedOcr(err);
    if (!rt)
        return false;
    try {
        const OcrDetConfig cfg = orientOcrConfig();
        for (LayoutDet& d : *dets) {
            if (d.label != QLatin1String("table"))
                continue;
            const QImage crop = cropTable(page, d, pageWidth, pageHeight);
            if (crop.isNull()) {
                // MinerU keeps the model-provided orientation when the crop
                // itself is unavailable.  Replacing it with zero loses useful
                // information and makes a transient crop failure semantic.
                continue;
            }
            QVector<OcrQuad> boxes;
            if (!runDet(rt, crop, cfg, &boxes, err))
                return false;
            if (!isRotationCandidate(boxes)) {
                d.angle = QStringLiteral("0");
                continue;
            }
            QMap<QString, OrientScore> scores;
            for (const QString& lab : {QStringLiteral("0"), QStringLiteral("90"), QStringLiteral("270")}) {
                const QImage rot = rotateLabel(crop, lab);
                QVector<OcrQuad> rb;
                if (lab == QLatin1String("0")) {
                    rb = boxes;
                } else if (!runDet(rt, rot, cfg, &rb, err)) {
                    return false;
                }
                const QVector<OcrQuad> sampled = sampleBoxes(rb);
                QVector<QImage> crops;
                for (const OcrQuad& q : sampled) {
                    const QImage c = cropQuadAabb(rot, q);
                    if (!c.isNull())
                        crops.push_back(c);
                }
                if (crops.size() < 5) {
                    scores.insert(lab, OrientScore{});
                    continue;
                }
                QVector<QPair<QString, float>> recs;
                if (!runRec(rt, crops, &recs, err))
                    return false;
                scores.insert(lab, scoreRec(recs));
            }
            d.angle = pickAngle(scores);
            fprintf(stderr,
                    "orient: 0=%.4f/%d/%d 90=%.4f/%d/%d 270=%.4f/%d/%d -> %s\n",
                    scores.value(QStringLiteral("0")).score, scores.value(QStringLiteral("0")).valid,
                    scores.value(QStringLiteral("0")).chars,
                    scores.value(QStringLiteral("90")).score, scores.value(QStringLiteral("90")).valid,
                    scores.value(QStringLiteral("90")).chars,
                    scores.value(QStringLiteral("270")).score, scores.value(QStringLiteral("270")).valid,
                    scores.value(QStringLiteral("270")).chars, qPrintable(d.angle));
        }
        if (err)
            err->clear();
        return true;
    } catch (const std::exception& ex) {
        if (err)
            *err = QString::fromUtf8(ex.what());
        return false;
    }
}

bool writeOfficialOcrSidecar(const QImage& page, const QVector<ContentBlock>& blocks,
                             const QVector<LayoutDet>& layout, const QString& path, QString* err) {
    auto fail = [&](const QString& m) {
        if (err)
            *err = m;
        return false;
    };
    OcrRuntime* rt = cachedOcr(err);
    if (!rt)
        return false;
    try {
        QJsonArray ocrItems;
        QJsonArray formulaItems;
        const OcrDetConfig cfg = sidecarOcrConfig();
        const int pw = page.width();
        const int ph = page.height();
        auto isCand = [](const QString& t) {
            return t == QLatin1String("text") || t == QLatin1String("title") || t == QLatin1String("doc_title")
                   || t == QLatin1String("paragraph_title");
        };
        for (const ContentBlock& b : blocks) {
            if (!isCand(b.type))
                continue;
            const QImage crop = cropPad(page, b.xmin, b.ymin, b.xmax, b.ymax, 50);
            QVector<OcrQuad> boxes;
            if (!runDet(rt, crop, cfg, &boxes, err))
                return false;
            const int x0 = (b.xmin <= 1) ? int(std::floor(b.xmin * pw)) : int(std::floor(b.xmin));
            const int y0 = (b.ymin <= 1) ? int(std::floor(b.ymin * ph)) : int(std::floor(b.ymin));
            for (const OcrQuad& q : boxes) {
                if ((q.xy[4] - q.xy[0]) < 3)
                    continue;
                const int bx0 = clampi(int(std::floor(q.xy[0] - 50 + x0)), 0, pw);
                const int by0 = clampi(int(std::floor(q.xy[1] - 50 + y0)), 0, ph);
                const int bx1 = clampi(int(std::ceil(q.xy[4] - 50 + x0)), 0, pw);
                const int by1 = clampi(int(std::ceil(q.xy[5] - 50 + y0)), 0, ph);
                if (bx1 <= bx0 || by1 <= by0)
                    continue;
                QJsonObject o;
                o.insert(QStringLiteral("type"), QStringLiteral("ocr_text"));
                o.insert(QStringLiteral("bbox"),
                         normalizeSidecarBboxToUnit(bx0, by0, bx1, by1, pw, ph));
                o.insert(QStringLiteral("text"), QString());
                o.insert(QStringLiteral("score"), 1);
                ocrItems.append(o);
            }
        }
        QVector<QRect> containers;
        for (const LayoutDet& d : layout) {
            if (d.label == QLatin1String("table") || d.label == QLatin1String("image")
                || d.label == QLatin1String("chart") || d.label == QLatin1String("display_formula")) {
                containers.push_back(QRect(int(d.xmin), int(d.ymin), int(d.xmax - d.xmin), int(d.ymax - d.ymin)));
            }
        }
        for (const LayoutDet& d : layout) {
            if (d.label != QLatin1String("inline_formula"))
                continue;
            QRect r(int(d.xmin), int(d.ymin), int(d.xmax - d.xmin), int(d.ymax - d.ymin));
            bool inside = false;
            for (const QRect& c : containers) {
                const QRect inter = r.intersected(c);
                if (inter.width() * inter.height() > r.width() * r.height() * 0.7)
                    inside = true;
            }
            if (inside)
                continue;
            QJsonObject o;
            o.insert(QStringLiteral("type"), QStringLiteral("inline_formula"));
            o.insert(QStringLiteral("bbox"),
                     normalizeSidecarBboxToUnit(d.xmin, d.ymin, d.xmax, d.ymax, pw, ph));
            o.insert(QStringLiteral("latex"), QString());
            o.insert(QStringLiteral("score"), 0);
            formulaItems.append(o);
        }
        // HybridMagicModel builds the matcher as inline_formula followed by
        // ocr_text, regardless of the order in which the producers ran.
        QJsonArray items = formulaItems;
        for (const QJsonValue& value : ocrItems)
            items.append(value);
        QJsonObject root;
        root.insert(QStringLiteral("engine"), QStringLiteral("official-ocr-det"));
        root.insert(QStringLiteral("items"), items);
        root.insert(QStringLiteral("page"), items);
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return fail(QStringLiteral("cannot create ocr sidecar directory"));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(QStringLiteral("cannot write ocr sidecar"));
        const QByteArray bytes = officialSidecarJsonBytes(root);
        if (f.write(bytes) != bytes.size() || !f.flush())
            return fail(QStringLiteral("cannot write ocr sidecar bytes"));
        if (err)
            err->clear();
        return true;
    } catch (const std::exception& ex) {
        return fail(QString::fromUtf8(ex.what()));
    }
}

}  // namespace hybrid
}  // namespace scanengine

#include "scanengine/hybrid/extract.hpp"
#include "scanengine/config.hpp"

#include "fpdfview_min.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QVector>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef SCANENGINE_LIBJPEG
#include <stdio.h>
#ifdef _WIN32
#ifndef HAVE_BOOLEAN
typedef unsigned char boolean;
#define HAVE_BOOLEAN
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#endif
#include <jpeglib.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace scanengine {
namespace hybrid {
namespace {

// images_bytes_to_pdf_bytes: save(PDF, resolution=200, quality=95)
constexpr double kOfficialPdfDpi = 200.0;
constexpr double kOfficialPdfPoint = 72.0;
constexpr int kOfficialMaxPageEdge = 3500;
const QString kOfficialRenderScaleKey = QStringLiteral("scanengine.official-render-scale");

#ifdef _WIN32
struct PdfiumApi {
    HMODULE module = nullptr;
    decltype(&::FPDF_InitLibrary) initLibrary = nullptr;
    decltype(&::FPDF_DestroyLibrary) destroyLibrary = nullptr;
    decltype(&::FPDF_LoadMemDocument) loadMemDocument = nullptr;
    decltype(&::FPDF_CloseDocument) closeDocument = nullptr;
    decltype(&::FPDF_LoadPage) loadPage = nullptr;
    decltype(&::FPDF_ClosePage) closePage = nullptr;
    decltype(&::FPDF_GetPageWidth) getPageWidth = nullptr;
    decltype(&::FPDF_GetPageHeight) getPageHeight = nullptr;
    decltype(&::FPDF_RenderPageBitmap) renderPageBitmap = nullptr;
    decltype(&::FPDFBitmap_CreateEx) bitmapCreateEx = nullptr;
    decltype(&::FPDFBitmap_FillRect) bitmapFillRect = nullptr;
    decltype(&::FPDFBitmap_GetBuffer) bitmapGetBuffer = nullptr;
    decltype(&::FPDFBitmap_GetStride) bitmapGetStride = nullptr;
    decltype(&::FPDFBitmap_Destroy) bitmapDestroy = nullptr;

    template <typename T>
    bool resolve(T& function, const char* name) {
        function = reinterpret_cast<T>(GetProcAddress(module, name));
        if (function)
            return true;
        fprintf(stderr,
                "official_page: pdfium.dll is missing export %s (Windows error %lu)\n",
                name,
                static_cast<unsigned long>(GetLastError()));
        return false;
    }

    bool load() {
        const QString path = QDir(repoRoot()).filePath(
            QStringLiteral("pdfium.dll"));
        module = LoadLibraryW(reinterpret_cast<LPCWSTR>(path.utf16()));
        if (!module) {
            fprintf(stderr,
                    "official_page: failed to load pdfium.dll from the runtime root "
                    "(Windows error %lu)\n",
                    static_cast<unsigned long>(GetLastError()));
            return false;
        }
        return resolve(initLibrary, "FPDF_InitLibrary") &&
               resolve(destroyLibrary, "FPDF_DestroyLibrary") &&
               resolve(loadMemDocument, "FPDF_LoadMemDocument") &&
               resolve(closeDocument, "FPDF_CloseDocument") &&
               resolve(loadPage, "FPDF_LoadPage") &&
               resolve(closePage, "FPDF_ClosePage") &&
               resolve(getPageWidth, "FPDF_GetPageWidth") &&
               resolve(getPageHeight, "FPDF_GetPageHeight") &&
               resolve(renderPageBitmap, "FPDF_RenderPageBitmap") &&
               resolve(bitmapCreateEx, "FPDFBitmap_CreateEx") &&
               resolve(bitmapFillRect, "FPDFBitmap_FillRect") &&
               resolve(bitmapGetBuffer, "FPDFBitmap_GetBuffer") &&
               resolve(bitmapGetStride, "FPDFBitmap_GetStride") &&
               resolve(bitmapDestroy, "FPDFBitmap_Destroy");
    }
};

PdfiumApi& pdfiumApi() {
    static PdfiumApi api;
    return api;
}
#endif

bool ensurePdfium() {
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, [] {
#ifdef _WIN32
        PdfiumApi& api = pdfiumApi();
        if (!api.load())
            return;
        api.initLibrary();
#else
        FPDF_InitLibrary();
#endif
        initialized = true;
    });
    return initialized;
}

QByteArray encodePillowDefaultCropJpegQt(const QImage& rgb) {
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(75);
    if (!writer.write(rgb.convertToFormat(QImage::Format_RGB888)))
        return QByteArray();

    // Qt and Pillow produce identical entropy-coded bytes with the deployed
    // JPEG codec and defaults.  Their sole header difference is density: Qt
    // writes 100 dpi while Pillow writes unitless 1x1.  Patch the JFIF APP0
    // fields without touching tables or scan data.
    if (jpeg.size() >= 18 && uchar(jpeg.at(0)) == 0xFF && uchar(jpeg.at(1)) == 0xD8
        && uchar(jpeg.at(2)) == 0xFF && uchar(jpeg.at(3)) == 0xE0
        && jpeg.mid(6, 5) == QByteArray("JFIF\0", 5)) {
        jpeg[13] = char(0);
        jpeg[14] = char(0);
        jpeg[15] = char(1);
        jpeg[16] = char(0);
        jpeg[17] = char(1);
    }
    return jpeg;
}

#ifdef SCANENGINE_LIBJPEG
struct JpegErr {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

void jpegFail(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErr*>(cinfo->err);
    longjmp(err->jump, 1);
}

QByteArray encodeJpeg(const QImage& rgbIn, int quality, bool force444) {
    const QImage rgb = rgbIn.convertToFormat(QImage::Format_RGB888);
    QByteArray out;
    jpeg_compress_struct cinfo;
    JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpegFail;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_compress(&cinfo);
        return QByteArray();
    }
    jpeg_create_compress(&cinfo);
    unsigned char* heap = nullptr;
    size_t heapLen = 0;
    jpeg_mem_dest(&cinfo, &heap, &heapLen);
    cinfo.image_width = JDIMENSION(rgb.width());
    cinfo.image_height = JDIMENSION(rgb.height());
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    if (force444) {
        cinfo.comp_info[0].h_samp_factor = 1;
        cinfo.comp_info[0].v_samp_factor = 1;
        cinfo.comp_info[1].h_samp_factor = 1;
        cinfo.comp_info[1].v_samp_factor = 1;
        cinfo.comp_info[2].h_samp_factor = 1;
        cinfo.comp_info[2].v_samp_factor = 1;
    }
    // Pillow's default JFIF header has no physical-density unit and uses 1x1
    // aspect density.  QImageWriter instead emits 100 dpi, which changes three
    // header bytes even when the decoded pixels are identical.
    cinfo.density_unit = 0;
    cinfo.X_density = 1;
    cinfo.Y_density = 1;
    jpeg_start_compress(&cinfo, TRUE);
    QByteArray rowBuf(rgb.width() * 3, 0);
    while (cinfo.next_scanline < cinfo.image_height) {
        const uchar* src = rgb.constScanLine(int(cinfo.next_scanline));
        std::memcpy(rowBuf.data(), src, size_t(rgb.width() * 3));
        JSAMPROW row = reinterpret_cast<JSAMPROW>(rowBuf.data());
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    if (heap && heapLen)
        out = QByteArray(reinterpret_cast<const char*>(heap), int(heapLen));
    jpeg_destroy_compress(&cinfo);
    if (heap)
        free(heap);
    return out;
}

// Official images_bytes_to_pdf_bytes: quality=95, subsampling=0 (4:4:4).
QByteArray encodeOfficialJpeg(const QImage& rgbIn) {
    return encodeJpeg(rgbIn, 95, true);
}

QByteArray encodeOfficialCropJpeg(const QImage& rgbIn) {
    return encodePillowDefaultCropJpegQt(rgbIn);
}
#else
QByteArray encodeOfficialJpeg(const QImage& rgb) {
    QByteArray jpeg;
    QBuffer buf(&jpeg);
    buf.open(QIODevice::WriteOnly);
    QImageWriter writer(&buf, "jpeg");
    writer.setQuality(95);
    writer.write(rgb);
    return jpeg;
}

QByteArray encodeOfficialCropJpeg(const QImage& rgb) {
    return encodePillowDefaultCropJpegQt(rgb);
}
#endif

// Pillow PdfImagePlugin RGB: DCTDecode JPEG + MediaBox = px * 72 / resolution
QByteArray officialJpegPdfBytes(const QImage& rgb) {
    const QByteArray jpeg = encodeOfficialJpeg(rgb);
    if (jpeg.isEmpty())
        return QByteArray();
    const double pageW = double(rgb.width()) * kOfficialPdfPoint / kOfficialPdfDpi;
    const double pageH = double(rgb.height()) * kOfficialPdfPoint / kOfficialPdfDpi;
    const QByteArray contents =
        QByteArray("q ") + QByteArray::number(pageW, 'f', 6) + " 0 0 " +
        QByteArray::number(pageH, 'f', 6) + " 0 0 cm /image Do Q\n";

    auto obj = [](int id, const QByteArray& body) {
        return QByteArray::number(id) + " 0 obj\n" + body + "\nendobj\n";
    };

    QByteArray out;
    QVector<int> xref(6, 0);
    out += "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    xref[1] = out.size();
    out += obj(1, "<< /Type /Catalog /Pages 2 0 R >>");
    xref[2] = out.size();
    out += obj(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    xref[3] = out.size();
    out += obj(3,
               "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " +
                   QByteArray::number(pageW, 'f', 6) + " " +
                   QByteArray::number(pageH, 'f', 6) +
                   "] /Resources << /ProcSet [/PDF /ImageC] /XObject << /image 4 0 R >> >> "
                   "/Contents 5 0 R >>");
    xref[4] = out.size();
    out += QByteArray::number(4) + " 0 obj\n<< /Type /XObject /Subtype /Image /Width " +
           QByteArray::number(rgb.width()) + " /Height " + QByteArray::number(rgb.height()) +
           " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length " +
           QByteArray::number(jpeg.size()) + " >>\nstream\n";
    out += jpeg;
    out += "\nendstream\nendobj\n";
    xref[5] = out.size();
    out += obj(5, "<< /Length " + QByteArray::number(contents.size()) + " >>\nstream\n" + contents +
                      "endstream");
    const int startxref = out.size();
    out += "xref\n0 6\n0000000000 65535 f \n";
    for (int i = 1; i <= 5; ++i) {
        char line[32];
        std::snprintf(line, sizeof(line), "%010d 00000 n \n", xref[i]);
        out += line;
    }
    out += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n";
    out += QByteArray::number(startxref);
    out += "\n%%EOF\n";
    return out;
}

}  // namespace

QImage loadOfficialHybridPage(const QString& path,
                              int* pdfWidth,
                              int* pdfHeight,
                              double* renderScale) {
    if (renderScale)
        *renderScale = 0.0;
    // mineru.cli.common.read_fn → Image.open + ImageOps.exif_transpose
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage src = reader.read();
    if (src.isNull())
        src = QImage(path);
    if (src.isNull()) {
        fprintf(stderr, "official_page: cannot open %s\n", path.toUtf8().constData());
        return src;
    }
    src = src.convertToFormat(QImage::Format_RGB888);

    const QByteArray pdf = officialJpegPdfBytes(src);
    if (pdf.size() < 32) {
        fprintf(stderr, "official_page: empty pdf bytes=%d jpeg_src=%dx%d\n",
                pdf.size(), src.width(), src.height());
        return QImage();
    }
    if (!ensurePdfium()) {
        fprintf(stderr, "official_page: pdfium initialization failed\n");
        return QImage();
    }
#ifdef _WIN32
    PdfiumApi& pdfium = pdfiumApi();
    FPDF_DOCUMENT doc = pdfium.loadMemDocument(pdf.constData(), pdf.size(), nullptr);
#else
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(pdf.constData(), pdf.size(), nullptr);
#endif
    if (!doc) {
        fprintf(stderr, "official_page: FPDF_LoadMemDocument failed bytes=%d\n", pdf.size());
        return QImage();
    }
#ifdef _WIN32
    FPDF_PAGE page = pdfium.loadPage(doc, 0);
#else
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
#endif
    if (!page) {
        fprintf(stderr, "official_page: FPDF_LoadPage failed\n");
#ifdef _WIN32
        pdfium.closeDocument(doc);
#else
        FPDF_CloseDocument(doc);
#endif
        return QImage();
    }

#ifdef _WIN32
    const double ptsW = pdfium.getPageWidth(page);
    const double ptsH = pdfium.getPageHeight(page);
#else
    const double ptsW = FPDF_GetPageWidth(page);
    const double ptsH = FPDF_GetPageHeight(page);
#endif
    if (pdfWidth)
        *pdfWidth = int(ptsW);
    if (pdfHeight)
        *pdfHeight = int(ptsH);

    // mineru.utils.pdf_reader.page_to_image
    const double longPts = std::max(ptsW, ptsH);
    double scale = kOfficialPdfDpi / kOfficialPdfPoint;
    if (longPts * scale > double(kOfficialMaxPageEdge))
        scale = double(kOfficialMaxPageEdge) / longPts;
    if (renderScale)
        *renderScale = scale;
    // Match the reference PDFium renderer: ceil(page size * scale).
    const int outW = std::max(1, int(std::ceil(ptsW * scale)));
    const int outH = std::max(1, int(std::ceil(ptsH * scale)));

    const int stride = outW * 3;
    QByteArray buf(stride * outH, 0);
#ifdef _WIN32
    FPDF_BITMAP bmp =
        pdfium.bitmapCreateEx(outW, outH, FPDFBitmap_BGR, buf.data(), stride);
#else
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(outW, outH, FPDFBitmap_BGR, buf.data(), stride);
#endif
    if (!bmp) {
        fprintf(stderr, "official_page: FPDFBitmap_CreateEx failed %dx%d\n", outW, outH);
#ifdef _WIN32
        pdfium.closePage(page);
        pdfium.closeDocument(doc);
#else
        FPDF_ClosePage(page);
        FPDF_CloseDocument(doc);
#endif
        return QImage();
    }
#ifdef _WIN32
    pdfium.bitmapFillRect(bmp, 0, 0, outW, outH, 0xFFFFFFFFu);
    pdfium.renderPageBitmap(bmp, page, 0, 0, outW, outH, 0, FPDF_ANNOT);
    pdfium.bitmapDestroy(bmp);
    pdfium.closePage(page);
    pdfium.closeDocument(doc);
#else
    FPDFBitmap_FillRect(bmp, 0, 0, outW, outH, 0xFFFFFFFFu);
    FPDF_RenderPageBitmap(bmp, page, 0, 0, outW, outH, 0, FPDF_ANNOT);
    FPDFBitmap_Destroy(bmp);
    FPDF_ClosePage(page);
    FPDF_CloseDocument(doc);
#endif

    QImage rgb(outW, outH, QImage::Format_RGB888);
    for (int y = 0; y < outH; ++y) {
        const uchar* s = reinterpret_cast<const uchar*>(buf.constData()) + y * stride;
        uchar* d = rgb.scanLine(y);
        for (int x = 0; x < outW; ++x) {
            d[3 * x + 0] = s[3 * x + 2];
            d[3 * x + 1] = s[3 * x + 1];
            d[3 * x + 2] = s[3 * x + 0];
        }
    }
    rgb.setText(kOfficialRenderScaleKey, QString::number(scale, 'g', 17));
    return rgb;
}

double officialHybridPageRenderScale(const QImage& page) {
    bool ok = false;
    const double scale = page.text(kOfficialRenderScaleKey).toDouble(&ok);
    return ok && std::isfinite(scale) && scale > 0.0 ? scale : 0.0;
}

bool writeOfficialHybridCropJpeg(const QImage& crop, const QString& path, QString* err) {
    auto fail = [&](const QString& message) {
        if (err)
            *err = message;
        return false;
    };
    if (crop.isNull())
        return fail(QStringLiteral("cannot encode an empty official crop"));
    const QByteArray jpeg = encodeOfficialCropJpeg(crop);
    if (jpeg.isEmpty())
        return fail(QStringLiteral("official JPEG crop encoder returned no bytes"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("cannot open official crop output: %1").arg(path));
    if (file.write(jpeg) != jpeg.size())
        return fail(QStringLiteral("short write for official crop output: %1").arg(path));
    if (err)
        err->clear();
    return true;
}

}  // namespace hybrid
}  // namespace scanengine

#include "scanengine/hybrid/device_policy.hpp"
#include "scanengine/parser_service.hpp"

#include "scanengine/config.hpp"
#include "scanengine/excel_export.hpp"
#include "scanengine/hybrid/canonical_model.hpp"
#include "scanengine/hybrid/extract.hpp"
#include "scanengine/hybrid/orchestrator.hpp"
#include "scanengine/hybrid/qwen2_language.hpp"
#include "scanengine/hybrid/qwen2_vision.hpp"
#include "scanengine/hybrid/tokenizer.hpp"
#include "scanengine/hybrid/vlm_config.hpp"
#include "scanengine/hybrid/vlm_paths.hpp"
#include "scanengine/output_layout.hpp"
#include "scanengine/preprocess.hpp"
#include "scanengine/strict_json.hpp"

#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <new>

namespace scanengine {
namespace {

bool optionalMissing(const std::optional<QString>& value) {
    return !value.has_value() || value->isEmpty();
}

bool copyFileChecked(const QString& sourcePath, const QString& destinationPath, QString* error) {
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QString::fromUtf8("无法读取待复制文件: %1").arg(sourcePath);
        return false;
    }
    if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())) {
        if (error)
            *error = QString::fromUtf8("无法创建复制目标目录: %1")
                         .arg(QFileInfo(destinationPath).absolutePath());
        return false;
    }
    QSaveFile destination(destinationPath);
    if (!destination.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QString::fromUtf8("无法打开复制目标: %1").arg(destinationPath);
        return false;
    }
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            if (error)
                *error = QString::fromUtf8("读取待复制文件失败: %1").arg(sourcePath);
            return false;
        }
        if (destination.write(chunk) != chunk.size()) {
            if (error)
                *error = QString::fromUtf8("写入复制目标失败: %1").arg(destinationPath);
            return false;
        }
    }
    if (!destination.commit()) {
        if (error)
            *error = QString::fromUtf8("提交复制目标失败: %1").arg(destinationPath);
        return false;
    }
    return true;
}

}  // namespace

ParserService::ParserService(QObject* parent)
    : QObject(parent) {}

bool ParserService::isRunning() const {
    return running_.load(std::memory_order_acquire);
}

void ParserService::cancel() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (running_.load(std::memory_order_relaxed))
        cancelled_.store(true, std::memory_order_release);
}

ParseResult ParserService::run(ParseOptions options, LogCallback onLog, StatusCallback onStatus) {
    ParseResult rejection;
    bool acquired = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (running_.load(std::memory_order_relaxed)) {
            rejection.success = false;
            rejection.status = TaskStatus::Failed;
            rejection.outputDir = options.outputDir;
            rejection.message = QString::fromUtf8("已有解析任务在运行");
        } else {
            cancelled_.store(false, std::memory_order_relaxed);
            running_.store(true, std::memory_order_release);
            acquired = true;
        }
    }
    if (!acquired) {
        if (onStatus)
            onStatus(TaskStatus::Failed, rejection.message);
        return rejection;
    }

    bool ownsRunning = true;
    auto resetRunning = [this, &ownsRunning](void*) {
        if (!ownsRunning)
            return;
        std::lock_guard<std::mutex> lock(stateMutex_);
        running_.store(false, std::memory_order_release);
    };
    std::unique_ptr<void, decltype(resetRunning)> runningGuard(this, resetRunning);

    auto finish = [&](ParseResult result) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (cancelled_.load(std::memory_order_acquire)
                || result.status == TaskStatus::Cancelled) {
                result.success = false;
                result.status = TaskStatus::Cancelled;
                result.message = QString::fromUtf8("任务已取消");
            } else if (result.success) {
                result.status = TaskStatus::Done;
                if (result.message.isEmpty())
                    result.message = QString::fromUtf8("解析完成");
            } else {
                result.status = TaskStatus::Failed;
                if (result.message.isEmpty())
                    result.message = QString::fromUtf8("解析失败");
            }
            running_.store(false, std::memory_order_release);
            ownsRunning = false;
        }
        hybrid::releaseHybridRuntimeCache();
        if (onStatus)
            onStatus(result.status, result.message);
        return result;
    };

    auto exceptionFailure = [&](const QString& detail) {
        ParseResult failure;
        failure.success = false;
        failure.status = TaskStatus::Failed;
        failure.outputDir = options.outputDir;
        failure.message = QString::fromUtf8("解析异常: %1").arg(detail);
        return finish(failure);
    };

    try {
    if (optionalMissing(options.excelPath) || optionalMissing(options.jobDir)) {
        const JobPaths job = allocateJobPaths(options.outputDir, options.inputPath);
        if (!job.success) {
            ParseResult failure;
            failure.success = false;
            failure.status = TaskStatus::Failed;
            failure.outputDir = job.workDir.isEmpty() ? options.outputDir : job.workDir;
            failure.message = job.error.isEmpty()
                ? QString::fromUtf8("初始化作业目录失败")
                : job.error;
            return finish(failure);
        }
        options.outputDir = job.intermediateDir;
        options.excelPath = job.excelPath;
        options.jobDir = job.workDir;
        if (onLog) {
            onLog(QString::fromUtf8("输出目录: %1/  原图=%2  Excel=%3  过程=work/")
                      .arg(QFileInfo(job.workDir).fileName(),
                           QFileInfo(job.sourceCopy).fileName(),
                           QFileInfo(job.excelPath).fileName()));
        }
    }
    if (!QDir().mkpath(options.outputDir)) {
        ParseResult failure;
        failure.success = false;
        failure.status = TaskStatus::Failed;
        failure.outputDir = options.outputDir;
        failure.message = QString::fromUtf8("无法创建输出目录: %1").arg(options.outputDir);
        return finish(failure);
    }

    if (onStatus)
        onStatus(TaskStatus::Prepare, QString::fromUtf8("准备解析任务…"));

    ParseResult result = runOnce(options, onLog, onStatus);
    if (!result.success)
        return finish(result);
    if (cancelled_.load(std::memory_order_acquire))
        return finish(result);

    if (onStatus)
        onStatus(TaskStatus::Outputs, QString::fromUtf8("整理输出…"));
    if (cancelled_.load(std::memory_order_acquire))
        return finish(result);
    result = attachExcel(result, options, onLog);
    if (cancelled_.load(std::memory_order_acquire))
        return finish(result);
    if (result.success) {
        const int finalChars = mdChars(result.markdownPath);
        if (finalChars < minChars_ && result.excelPath.isEmpty()) {
            result.success = true;
            result.status = TaskStatus::Done;
            result.message = QString::fromUtf8("解析完成，但文本较少(%1字)，请检查原图方向/清晰度")
                                 .arg(finalChars);
        }
    }
    return finish(result);
    } catch (const std::bad_alloc&) {
        return exceptionFailure(QString::fromUtf8("内存不足"));
    } catch (const std::exception& ex) {
        return exceptionFailure(QString::fromUtf8(ex.what()));
    } catch (...) {
        return exceptionFailure(QString::fromUtf8("未知 C++ 异常"));
    }
}

ParseResult ParserService::runOnce(ParseOptions options, LogCallback onLog, StatusCallback onStatus) {
    options.inputPath = QFileInfo(options.inputPath).absoluteFilePath();
    options.outputDir = QFileInfo(options.outputDir).absoluteFilePath();
    if (cancelled_.load(std::memory_order_acquire)) {
        ParseResult r;
        r.success = false;
        r.status = TaskStatus::Cancelled;
        r.outputDir = options.outputDir;
        r.message = QString::fromUtf8("任务已取消");
        return r;
    }
    if (!QDir().mkpath(options.outputDir)) {
        ParseResult r;
        r.success = false;
        r.status = TaskStatus::Failed;
        r.outputDir = options.outputDir;
        r.message = QString::fromUtf8("无法创建输出目录: %1").arg(options.outputDir);
        return r;
    }

    if (options.effort != QLatin1String("medium")) {
        ParseResult r;
        r.success = false;
        r.status = TaskStatus::Failed;
        r.outputDir = options.outputDir;
        r.message = QString::fromUtf8("ScanEngine 只接受官方 hybrid medium");
        return r;
    }

    bool deviceOk = false;
    const hybrid::DeviceMode deviceMode =
        hybrid::deviceModeFromName(options.device, &deviceOk);
    if (!deviceOk) {
        ParseResult r;
        r.success = false;
        r.status = TaskStatus::Failed;
        r.outputDir = options.outputDir;
        r.message = QString::fromUtf8("未知推理设备: %1").arg(options.device);
        return r;
    }
    int deviceFallbackCount = 0;
    QString deviceError;
    if (!hybrid::applyHybridDeviceMode(deviceMode, &deviceFallbackCount, &deviceError)) {
        ParseResult r;
        r.success = false;
        r.status = TaskStatus::Failed;
        r.outputDir = options.outputDir;
        r.message = deviceError;
        r.fallbackCount = 0;
        return r;
    }

    if (isImageFile(options.inputPath) && hybrid::officialHybridReady()) {
        ParseResult r;
        r.outputDir = options.outputDir;
        auto cancelResult = [&]() {
            r.success = false;
            r.status = TaskStatus::Cancelled;
            r.message = QString::fromUtf8("任务已取消");
            return r;
        };
        auto isCancelled = [&]() {
            return cancelled_.load(std::memory_order_acquire);
        };
        if (onStatus)
            onStatus(TaskStatus::Process, QString::fromUtf8("官方 hybrid 逐步解析…"));
        if (isCancelled())
            return cancelResult();
        int pdfW = 0;
        int pdfH = 0;
        QImage page = hybrid::loadOfficialHybridPage(options.inputPath, &pdfW, &pdfH);
        if (isCancelled())
            return cancelResult();
        if (page.isNull()) {
            r.success = false;
            r.status = TaskStatus::Failed;
            r.message = QString::fromUtf8("无法打开图片");
            return r;
        }
        QString err;
        const int backendTokenLimit = hybrid::officialVlmTokenLimit();
        if (backendTokenLimit <= 0) {
            r.success = false;
            r.status = TaskStatus::Failed;
            r.message = QStringLiteral("missing text_config.max_position_embeddings in official VLM config");
            if (onLog)
                onLog(r.message);
            return r;
        }
        QVector<hybrid::ContentBlock> blocks;
        const QString steps = QDir(options.outputDir).filePath(QStringLiteral("steps"));
        if (!QDir().mkpath(steps)) {
            r.success = false;
            r.status = TaskStatus::Failed;
            r.message = QString::fromUtf8("无法创建过程目录: %1").arg(steps);
            return r;
        }
        QString layoutForOcr;
            if (isCancelled())
                return cancelResult();
            if (onLog)
                onLog(QString::fromUtf8("step1 medium: C++ PP-DocLayoutV2"));
            const auto tStage = std::chrono::steady_clock::now();
            const QString layoutJson = QDir(steps).filePath(QStringLiteral("01_layout.json"));
            if (!hybrid::runOfficialLayoutSidecar(options.inputPath, layoutJson, &err)) {
                r.success = false;
                r.status = TaskStatus::Failed;
                r.message = err;
                return r;
            }
            if (isCancelled())
                return cancelResult();
            const QString orientJson = QDir(steps).filePath(QStringLiteral("01b_layout_oriented.json"));
            fprintf(stderr, "prof: layout=%.0fms\n",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count());
            if (onLog)
                onLog(QString::fromUtf8("step1b medium: C++ TableOrientationCls"));
            const auto tOri = std::chrono::steady_clock::now();
            if (!hybrid::runOfficialTableOrientSidecar(options.inputPath, layoutJson, orientJson, &err)) {
                r.success = false;
                r.status = TaskStatus::Failed;
                r.message = err;
                return r;
            }
            if (isCancelled())
                return cancelResult();
            layoutForOcr = orientJson;
            QVector<hybrid::LayoutDet> dets;
            int lw = page.width(), lh = page.height();
            if (!hybrid::loadOfficialLayoutJson(orientJson, &dets, &lw, &lh, &err)) {
                r.success = false;
                r.status = TaskStatus::Failed;
                r.message = err;
                return r;
            }
            fprintf(stderr, "prof: orient=%.0fms dets=%d\n",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tOri).count(),
                    dets.size());
            if (onLog)
                onLog(QString::fromUtf8("step1 完成: %1 个 layout 框").arg(dets.size()));
            blocks = hybrid::buildMediumVlmLayoutBlocks(dets, lw, lh);
            {
                const QString stagePath =
                    QDir(steps).filePath(QStringLiteral("02_vlm_blocks.json"));
                if (!hybrid::writeOfficialModelJson(stagePath, blocks, &err)) {
                    r.success = false;
                    r.status = TaskStatus::Failed;
                    r.message = err;
                    return r;
                }
            }
            if (isCancelled())
                return cancelResult();
            if (onLog)
                onLog(QString::fromUtf8("step2 完成: %1 个 VLM 块").arg(blocks.size()));
            QJsonArray extractedPage = hybrid::officialContentBlocksJson(blocks);
            if (onLog)
                onLog(QString::fromUtf8("step3: 官方 VLM extract_with_layout"));
            const auto tExtract = std::chrono::steady_clock::now();
            // Windows Transformers treats this as max_length; the macOS MLX
            // backend treats the same config value as max new tokens.
            if (!hybrid::extractOfficialPageWithLayout(page,
                                                       &extractedPage,
                                                       false,
                                                       {},
                                                       backendTokenLimit,
                                                       nullptr,
                                                       &err,
                                                       isCancelled,
                                                       nullptr)) {
                r.success = false;
                r.status = TaskStatus::Failed;
                r.message = err;
                return r;
            }
            if (isCancelled())
                return cancelResult();
            fprintf(stderr, "prof: extract=%.0fms\n",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tExtract)
                        .count());
        {
            const QString stagePath = QDir(steps).filePath(QStringLiteral("03_extract.json"));
            if (!hybrid::writeOfficialModelJson(stagePath, extractedPage, &err)) {
                r.success = false;
                r.status = TaskStatus::Failed;
                r.message = err;
                return r;
            }
            if (isCancelled())
                return cancelResult();
        }
        const QString stem = QFileInfo(options.inputPath).completeBaseName();
        const QString rawModelJson = QDir(steps).filePath(QStringLiteral("04_model_raw.json"));
        if (onLog)
            onLog(QString::fromUtf8("step4: VLM blocks（官方 stage 40）"));
        if (!hybrid::writeOfficialModelJson(rawModelJson, extractedPage, &err)) {
            r.success = false;
            r.status = TaskStatus::Failed;
            r.message = err;
            return r;
        }

        QJsonArray canonicalPage = extractedPage;
        if (onLog)
            onLog(QString::fromUtf8("step4a: 公式编号规范化（官方 stage 45）"));
        hybrid::optimizeHybridFormulaNumberBlocks(canonicalPage);
        const QString formulaModelJson =
            QDir(steps).filePath(QStringLiteral("04a_formula_number.json"));
        if (!hybrid::writeOfficialModelJson(formulaModelJson, canonicalPage, &err)) {
            r.success = false;
            r.status = TaskStatus::Failed;
            r.message = err;
            return r;
        }
        if (isCancelled())
            return cancelResult();

        if (!layoutForOcr.isEmpty()) {
            const QString ocrJson = QDir(steps).filePath(QStringLiteral("04b_ocr.json"));
            if (onLog)
                onLog(QString::fromUtf8("step4b: 官方 OCR det sidecar"));
            if (!hybrid::runOfficialOcrSidecar(options.inputPath, formulaModelJson, layoutForOcr,
                                               ocrJson, &err)) {
                r.success = false;
                r.status = TaskStatus::Failed;
                r.message = err;
                return r;
            }
            const QString modelOcrJson = QDir(steps).filePath(QStringLiteral("04c_model_ocr.json"));
            QFile ocrFile(ocrJson);
            if (!ocrFile.open(QIODevice::ReadOnly)) {
                r.success = false;
                r.status = TaskStatus::Failed;
                r.message = QString::fromUtf8("cannot open OCR sidecar");
                return r;
            }
            QJsonDocument ocrDocument;
            if (!parseJsonDocumentStrict(ocrFile.readAll(), &ocrDocument, &err)
                || !ocrDocument.isObject()) {
                r.success = false;
                r.status = TaskStatus::Failed;
                r.message = err.isEmpty() ? QString::fromUtf8("OCR sidecar is not an object") : err;
                return r;
            }
            if (!hybrid::mergeOfficialOcrSidecarItems(ocrDocument.object(), &canonicalPage, &err)
                || !hybrid::writeOfficialModelJson(modelOcrJson, canonicalPage, &err)) {
                r.success = false;
                r.status = TaskStatus::Failed;
                r.message = err.isEmpty() ? QString::fromUtf8("cannot merge OCR sidecar") : err;
                return r;
            }
        }
        if (isCancelled())
            return cancelResult();

        if (!layoutForOcr.isEmpty()) {
            QFile layoutFile(layoutForOcr);
            if (!layoutFile.open(QIODevice::ReadOnly)) {
                r.success = false;
                r.status = TaskStatus::Failed;
                r.message = QString::fromUtf8("cannot open layout for title split");
                return r;
            }
            QJsonDocument layoutDocument;
            if (!parseJsonDocumentStrict(layoutFile.readAll(), &layoutDocument, &err)) {
                r.success = false;
                r.status = TaskStatus::Failed;
                r.message = err;
                return r;
            }
            const QJsonObject layoutObject = layoutDocument.object();
            const QJsonArray layoutSize = layoutObject.value(QStringLiteral("page_size")).toArray();
            if (!layoutDocument.isObject() || layoutSize.size() != 2) {
                r.success = false;
                r.status = TaskStatus::Failed;
                r.message = QString::fromUtf8("invalid layout for title split");
                return r;
            }
            if (onLog)
                onLog(QString::fromUtf8("step4d: layout 标题拆分（官方 stage 60）"));
            hybrid::applyLayoutTitleSplit(canonicalPage,
                                          layoutObject.value(QStringLiteral("dets")).toArray(),
                                          layoutSize.at(0).toDouble(),
                                          layoutSize.at(1).toDouble());
        }
        if (isCancelled())
            return cancelResult();
        const QString finalModelJson =
            QDir(steps).filePath(QStringLiteral("04d_title_split_model.json"));
        if (!hybrid::writeOfficialModelJson(finalModelJson, canonicalPage, &err)) {
            r.success = false;
            r.status = TaskStatus::Failed;
            r.message = err;
            return r;
        }
        if (isCancelled())
            return cancelResult();

        const QString middlePreprocJson =
            QDir(steps).filePath(QStringLiteral("05a_middle_preproc.json"));
        const QString middleJson = QDir(steps).filePath(QStringLiteral("05_middle.json"));
        if (onLog)
            onLog(QString::fromUtf8("step5: canonical middle stage 70 → 80"));
        if (!hybrid::writeOfficialMiddleJson(canonicalPage,
                                              pdfW > 0 ? pdfW : page.width(),
                                              pdfH > 0 ? pdfH : page.height(), middleJson, &err,
                                              QStringLiteral("medium"), middlePreprocJson, &page,
                                              QDir(options.outputDir).filePath(QStringLiteral("images")))) {
            r.success = false;
            r.status = TaskStatus::Failed;
            r.message = err;
            return r;
        }
        if (isCancelled())
            return cancelResult();
        const QString exportedModelJson =
            QDir(options.outputDir).filePath(stem + QStringLiteral("_model.json"));
        const QString exportedMiddleJson =
            QDir(options.outputDir).filePath(stem + QStringLiteral("_middle.json"));
        if (isCancelled())
            return cancelResult();
        if (!copyFileChecked(finalModelJson, exportedModelJson, &err)
            || !copyFileChecked(middleJson, exportedMiddleJson, &err)) {
            r.success = false;
            r.status = TaskStatus::Failed;
            r.message = err;
            return r;
        }
        if (!hybrid::writeHybridPageArtifactsFromMiddle(options.outputDir, stem, middleJson,
                                                        &r.markdownPath, &r.jsonPath, &err)) {
            r.success = false;
            r.status = TaskStatus::Failed;
            r.message = err;
            return r;
        }
        if (isCancelled())
            return cancelResult();
        r.success = true;
        r.status = TaskStatus::Done;
        r.message = QString::fromUtf8("hybrid 逐步完成（%1 块）").arg(canonicalPage.size());
        r.artifacts << r.markdownPath << r.jsonPath;
        r.fallbackCount = deviceFallbackCount;
        return r;
    }

    ParseResult r;
    r.success = false;
    r.status = TaskStatus::Failed;
    r.outputDir = options.outputDir;
    if (!isImageFile(options.inputPath))
        r.message = QString::fromUtf8("C++ hybrid 只支持图片输入");
    else {
        const QStringList missing = hybrid::officialHybridMissingFiles();
        r.message = QString::fromUtf8("C++ hybrid 缺少官方模型文件: %1")
                        .arg(missing.join(QStringLiteral(", ")));
    }
    return r;
}

ParseResult ParserService::attachExcel(ParseResult result,
                                       const ParseOptions& options,
                                       LogCallback onLog) {
    if (result.outputDir.isEmpty())
        return result;

    const QString stem = !result.markdownPath.isEmpty()
        ? QFileInfo(result.markdownPath).completeBaseName()
        : QFileInfo(result.outputDir).fileName();
    const QString xlsxPath = (options.excelPath && !options.excelPath->isEmpty())
        ? *options.excelPath
        : QDir(result.outputDir).filePath(stem + QStringLiteral(".xlsx"));

    const ExcelExportResult exp =
        exportExcelFromParseDir(result.outputDir, xlsxPath, stem);
    if (onLog) {
        onLog(QString::fromUtf8("Excel 导出: success=%1 tables=%2 path=%3 (%4)")
                  .arg(exp.success ? QStringLiteral("True") : QStringLiteral("False"))
                  .arg(exp.tableCount)
                  .arg(exp.path)
                  .arg(exp.message));
    }
    if (!exp.success || exp.path.isEmpty()) {
        result.success = false;
        result.status = TaskStatus::Failed;
        result.message = QString::fromUtf8("Excel 导出失败: %1")
                             .arg(exp.message.isEmpty()
                                      ? QString::fromUtf8("未生成输出文件")
                                      : exp.message);
        result.excelPath.clear();
        return result;
    }

    QString msg = result.message.isEmpty() ? QString::fromUtf8("解析完成") : result.message;
    if (exp.tableCount > 0) {
        msg = QString::fromUtf8("解析完成 → Excel（%1 表）: %2")
                  .arg(exp.tableCount)
                  .arg(QFileInfo(exp.path).fileName());
    } else {
        msg = QString::fromUtf8("解析完成 → Excel（文本）: %1")
                  .arg(QFileInfo(exp.path).fileName());
    }

    const QString jobDir = (options.jobDir && !options.jobDir->isEmpty())
        ? *options.jobDir
        : result.outputDir;

    const QString excelAbs = QFileInfo(exp.path).absoluteFilePath();
    QStringList artifacts;
    artifacts.append(exp.path);
    for (const QString& p : result.artifacts) {
        if (QFileInfo(p).absoluteFilePath() != excelAbs)
            artifacts.append(p);
    }

    result.success = true;
    result.status = TaskStatus::Done;
    result.message = msg;
    result.outputDir = jobDir;
    result.excelPath = exp.path;
    result.artifacts = artifacts;
    return result;
}

int ParserService::mdChars(const QString& path) {
    if (path.isEmpty())
        return 0;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return 0;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    return QString::fromUtf8(f.readAll()).trimmed().size();
}

}  // namespace scanengine

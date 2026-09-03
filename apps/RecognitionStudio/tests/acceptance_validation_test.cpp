#include "acceptance_validation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <iostream>

namespace {

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool require(bool condition, const char* message) {
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    using namespace speechdoc::acceptance;

    Dataset dataset;
    dataset.removeWhitespace = true;
    const Score score = compare(QStringLiteral("甲 乙丙"),
                                QStringLiteral("甲乙丁"), dataset);
    if (!require(score.referenceCharacters == 3, "reference normalization failed")
        || !require(score.editDistance == 1, "edit distance failed"))
        return 1;

    QTemporaryDir directory;
    if (!require(directory.isValid(), "temporary directory failed"))
        return 1;
    const QString inputPath = QDir(directory.path()).filePath(QStringLiteral("input.wav"));
    const QString referencePath = QDir(directory.path()).filePath(QStringLiteral("truth.txt"));
    const QString manifestPath = QDir(directory.path()).filePath(QStringLiteral("manifest.json"));
    if (!writeFile(inputPath, "audio") || !writeFile(referencePath, QStringLiteral("测试文本").toUtf8()))
        return 1;
    QJsonObject manifest;
    manifest.insert(QStringLiteral("schema"), QStringLiteral("RecognitionStudio-Acceptance/1"));
    manifest.insert(QStringLiteral("dataset_id"), QStringLiteral("unit-test"));
    manifest.insert(QStringLiteral("task_type"), QStringLiteral("voice"));
    manifest.insert(QStringLiteral("threshold"), 0.98);
    QJsonObject sample;
    sample.insert(QStringLiteral("id"), QStringLiteral("one"));
    sample.insert(QStringLiteral("input"), QStringLiteral("input.wav"));
    sample.insert(QStringLiteral("reference_path"), QStringLiteral("truth.txt"));
    sample.insert(QStringLiteral("input_sha256"), sha256File(inputPath));
    sample.insert(QStringLiteral("reference_sha256"), sha256File(referencePath));
    manifest.insert(QStringLiteral("samples"), QJsonArray({sample}));
    if (!writeFile(manifestPath, QJsonDocument(manifest).toJson()))
        return 1;
    Dataset loaded;
    QString error;
    if (!require(loadDataset(manifestPath, &loaded, &error),
                 qPrintable(QStringLiteral("manifest load failed: %1").arg(error))))
        return 1;
    if (!require(loaded.samples.size() == 1, "manifest sample count failed")
        || !require(loaded.samples.first().reference == QStringLiteral("测试文本"),
                    "reference read failed"))
        return 1;

    manifest.insert(QStringLiteral("threshold"), 0.90);
    const QString weakManifestPath =
        QDir(directory.path()).filePath(QStringLiteral("weak-manifest.json"));
    if (!writeFile(weakManifestPath, QJsonDocument(manifest).toJson()))
        return 1;
    if (!require(!loadDataset(weakManifestPath, &loaded, &error),
                 "threshold below 98 percent was accepted"))
        return 1;

    const QString contentPath =
        QDir(directory.path()).filePath(QStringLiteral("page_content_list.json"));
    QJsonObject textItem;
    textItem.insert(QStringLiteral("type"), QStringLiteral("text"));
    textItem.insert(QStringLiteral("text"), QStringLiteral("第一行"));
    QJsonObject tableItem;
    tableItem.insert(QStringLiteral("type"), QStringLiteral("table"));
    tableItem.insert(QStringLiteral("table_body"),
                     QStringLiteral("<table><tr><td>表格值</td></tr></table>"));
    if (!writeFile(contentPath,
                   QJsonDocument(QJsonArray({textItem, tableItem})).toJson()))
        return 1;
    const QString extracted = extractScanText(contentPath, &error);
    if (!require(error.isEmpty(), "scan result extraction failed")
        || !require(extracted.contains(QStringLiteral("第一行")),
                    "scan text was not extracted")
        || !require(extracted.contains(QStringLiteral("表格值")),
                    "scan table was not extracted"))
        return 1;

    const QByteArray voiceJson =
        R"({"segments":[{"confidence":{"value":0.8}},{"confidence":{"value":1.0}}]})";
    const ConfidenceSummary confidence = extractVoiceConfidence(voiceJson);
    if (!require(confidence.count == 2, "voice confidence count failed")
        || !require(confidence.mean > 0.899 && confidence.mean < 0.901,
                    "voice confidence mean failed"))
        return 1;

    const QByteArray exactBytes =
        QByteArray::fromHex("007b2261223a317d0d0aff");
    const QString exactPath =
        QDir(directory.path()).filePath(QStringLiteral("exact.bin"));
    if (!require(writeBytes(exactPath, exactBytes, &error),
                 "exact evidence write failed"))
        return 1;
    QFile exactFile(exactPath);
    if (!require(exactFile.open(QIODevice::ReadOnly)
                     && exactFile.readAll() == exactBytes,
                 "exact evidence bytes changed"))
        return 1;

    QJsonObject report;
    report.insert(QStringLiteral("verdict"), QStringLiteral("passed"));
    const QString reportPath =
        QDir(directory.path()).filePath(QStringLiteral("report.json"));
    const QString checksumPath =
        QDir(directory.path()).filePath(QStringLiteral("SHA256SUMS.txt"));
    QString reportSha256;
    QString checksumSha256;
    if (!require(writeChecksummedJson(
                     reportPath, checksumPath, report,
                     &reportSha256, &checksumSha256, &error),
                 "checksummed report write failed")
        || !require(reportSha256 == sha256File(reportPath),
                    "report checksum mismatch")
        || !require(checksumSha256 == sha256File(checksumPath),
                    "checksum file hash mismatch"))
        return 1;
    const QString missingReport =
        QDir(directory.path()).filePath(
            QStringLiteral("missing/report.json"));
    if (!require(!writeChecksummedJson(
                     missingReport, checksumPath, report,
                     &reportSha256, &checksumSha256, &error),
                 "invalid report destination was accepted"))
        return 1;

    const QString runtimeRoot =
        QDir(directory.path()).filePath(QStringLiteral("runtime"));
    const QString runtimeModel =
        QDir(runtimeRoot).filePath(QStringLiteral("models/model.bin"));
    if (!QDir().mkpath(QFileInfo(runtimeModel).absolutePath())
        || !writeFile(runtimeModel, QByteArrayLiteral("published-model"))) {
        return 1;
    }
    const QString sdkChecksums =
        QDir(runtimeRoot).filePath(QStringLiteral("SDK_SHA256SUMS.txt"));
    const QByteArray sdkChecksumBytes =
        sha256File(runtimeModel).toLatin1()
        + QByteArrayLiteral("  bin/models/model.bin\n");
    if (!writeFile(sdkChecksums, sdkChecksumBytes))
        return 1;
    QMap<QString, QString> sdkEntries;
    if (!require(loadSha256Manifest(sdkChecksums, &sdkEntries, &error),
                 "SDK checksum manifest load failed"))
        return 1;
    RuntimeFileExpectation runtimeModelExpectation;
    runtimeModelExpectation.manifestPath = QStringLiteral(
        "bin/models/model.bin");
    runtimeModelExpectation.runtimeRelativePath = QStringLiteral(
        "models/model.bin");
    runtimeModelExpectation.expectedSha256 = sdkEntries.value(
        runtimeModelExpectation.manifestPath);
    const QVector<RuntimeFileExpectation> runtimeExpectations = {
        runtimeModelExpectation};
    const RuntimePayloadVerification validPayload = verifyRuntimePayload(
        runtimeRoot, sdkChecksums, sha256File(sdkChecksums),
        runtimeExpectations);
    if (!require(validPayload.valid, "published runtime payload was rejected")
        || !require(validPayload.evidence
                        .value(QStringLiteral("checked_file_count")).toInt() == 1,
                    "runtime payload evidence count failed")
        || !require(validPayload.evidence
                        .value(QStringLiteral("exact_file_set_match")).toBool(),
                    "runtime exact file-set evidence failed")) {
        return 1;
    }
    const QString unexpectedRuntimeFile =
        QDir(runtimeRoot).filePath(QStringLiteral("unexpected.bin"));
    if (!writeFile(unexpectedRuntimeFile, QByteArrayLiteral("unexpected")))
        return 1;
    const RuntimePayloadVerification extraPayload = verifyRuntimePayload(
        runtimeRoot, sdkChecksums, sha256File(sdkChecksums),
        runtimeExpectations);
    if (!require(!extraPayload.valid
                     && extraPayload.evidence
                            .value(QStringLiteral("unexpected_file_count"))
                            .toInt() == 1,
                 "unexpected runtime file was accepted")
        || !QFile::remove(unexpectedRuntimeFile)) {
        return 1;
    }
    std::atomic_bool cancelledVerification{true};
    const RuntimePayloadVerification cancelledPayload = verifyRuntimePayload(
        runtimeRoot, sdkChecksums, sha256File(sdkChecksums),
        runtimeExpectations, &cancelledVerification);
    if (!require(!cancelledPayload.valid
                     && cancelledPayload.evidence
                            .value(QStringLiteral("cancelled")).toBool(),
                 "cancelled runtime verification did not stop")) {
        return 1;
    }
    if (!writeFile(runtimeModel, QByteArrayLiteral("tampered-model")))
        return 1;
    const RuntimePayloadVerification tamperedPayload = verifyRuntimePayload(
        runtimeRoot, sdkChecksums, sha256File(sdkChecksums),
        runtimeExpectations);
    if (!require(!tamperedPayload.valid,
                 "tampered runtime payload was accepted"))
        return 1;
    if (!writeFile(runtimeModel, QByteArrayLiteral("published-model")))
        return 1;
    const RuntimePayloadVerification wrongAnchor = verifyRuntimePayload(
        runtimeRoot, sdkChecksums, QString(64, QLatin1Char('0')),
        runtimeExpectations);
    if (!require(!wrongAnchor.valid,
                 "wrong build checksum anchor was accepted"))
        return 1;
    const QString unsafeChecksums =
        QDir(runtimeRoot).filePath(QStringLiteral("unsafe-SHA256SUMS.txt"));
    if (!writeFile(unsafeChecksums,
                   QByteArray(64, 'a') + QByteArrayLiteral("  ../escape.bin\n"))
        || !require(!loadSha256Manifest(
                         unsafeChecksums, &sdkEntries, &error),
                    "unsafe checksum path was accepted")) {
        return 1;
    }

    const QDir repository(QStringLiteral(RECOGNITION_SUITE_SOURCE_DIR));
    const QString frozenRoot = repository.filePath(
        QStringLiteral("testdata/acceptance/codex-independent-2026-09"));
    const QStringList frozenManifests = {
        QDir(frozenRoot).filePath(QStringLiteral("scan/manifest.json")),
        QDir(frozenRoot).filePath(QStringLiteral("voice/manifest.json"))};
    for (const QString& frozenManifest : frozenManifests) {
        Dataset frozen;
        if (!require(loadDataset(frozenManifest, &frozen, &error),
                     qPrintable(QStringLiteral("frozen manifest failed: %1 (%2)")
                                    .arg(frozenManifest, error)))
            || !require(frozen.samples.size() == 8,
                        "frozen manifest sample count failed")
            || !require(frozen.threshold == 0.98,
                        "frozen manifest threshold failed"))
            return 1;
    }
    return 0;
}

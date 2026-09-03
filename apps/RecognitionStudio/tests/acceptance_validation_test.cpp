#include "acceptance_validation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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

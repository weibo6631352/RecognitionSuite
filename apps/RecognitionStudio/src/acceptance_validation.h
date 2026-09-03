#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

#include <atomic>

namespace speechdoc::acceptance {

enum class Kind { Scan, Voice };

struct Sample {
    QString id;
    QString inputPath;
    QString inputSha256;
    QString reference;
    QString referenceSha256;
};

struct Dataset {
    QString id;
    QString manifestPath;
    QString manifestSha256;
    Kind kind = Kind::Scan;
    double threshold = 0.98;
    bool removeWhitespace = false;
    bool ignorePunctuation = false;
    bool caseSensitive = true;
    QVector<Sample> samples;
};

struct Score {
    qint64 referenceCharacters = 0;
    qint64 hypothesisCharacters = 0;
    qint64 editDistance = 0;

    double accuracy() const;
};

struct ConfidenceSummary {
    int count = 0;
    double mean = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    QString source;
};

struct RuntimeFileExpectation {
    QString manifestPath;
    QString runtimeRelativePath;
    QString expectedSha256;
};

struct RuntimePayloadVerification {
    bool valid = false;
    QString error;
    QJsonObject evidence;
};

bool loadDataset(const QString& manifestPath, Dataset* dataset, QString* error);
QString normalizeText(const QString& text, const Dataset& dataset);
Score compare(const QString& reference, const QString& hypothesis,
              const Dataset& dataset);
QJsonObject describeFirstDifference(const QString& reference,
                                    const QString& hypothesis,
                                    const Dataset& dataset);
QString extractScanText(const QString& contentListPath, QString* error);
ConfidenceSummary extractScanConfidence(const QString& contentListPath);
ConfidenceSummary extractVoiceConfidence(const QByteArray& resultJson);
QString sha256File(const QString& path,
                   const std::atomic_bool* cancelled = nullptr);
QString sha256Bytes(const QByteArray& value);
bool loadSha256Manifest(const QString& path,
                        QMap<QString, QString>* entries,
                        QString* error);
RuntimePayloadVerification verifyRuntimePayload(
    const QString& runtimeRoot,
    const QString& checksumManifestPath,
    const QString& expectedChecksumManifestSha256,
    const QVector<RuntimeFileExpectation>& expectations,
    const std::atomic_bool* cancelled = nullptr);
bool writeJson(const QString& path, const QJsonObject& value, QString* error);
bool writeChecksummedJson(const QString& jsonPath,
                          const QString& checksumPath,
                          const QJsonObject& value,
                          QString* jsonSha256,
                          QString* checksumSha256,
                          QString* error);
bool writeBytes(const QString& path, const QByteArray& value, QString* error);
bool writeUtf8(const QString& path, const QString& value, QString* error);

}  // namespace speechdoc::acceptance

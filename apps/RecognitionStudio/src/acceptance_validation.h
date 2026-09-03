#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

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

bool loadDataset(const QString& manifestPath, Dataset* dataset, QString* error);
QString normalizeText(const QString& text, const Dataset& dataset);
Score compare(const QString& reference, const QString& hypothesis,
              const Dataset& dataset);
QString extractScanText(const QString& contentListPath, QString* error);
ConfidenceSummary extractScanConfidence(const QString& contentListPath);
ConfidenceSummary extractVoiceConfidence(const QByteArray& resultJson);
QString sha256File(const QString& path);
bool writeJson(const QString& path, const QJsonObject& value, QString* error);
bool writeUtf8(const QString& path, const QString& value, QString* error);

}  // namespace speechdoc::acceptance

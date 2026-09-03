#include "acceptance_validation.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextDocument>

#include <algorithm>
#include <limits>

namespace speechdoc::acceptance {
namespace {

QString readUtf8(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取文件：%1（%2）")
                         .arg(path, file.errorString());
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString resolvedPath(const QDir& root, const QString& path) {
    if (QDir::isAbsolutePath(path))
        return QDir::cleanPath(path);
    return QDir::cleanPath(root.absoluteFilePath(path));
}

QString htmlText(const QString& html) {
    QTextDocument document;
    document.setHtml(html);
    return document.toPlainText();
}

void appendContentItem(const QJsonObject& item, QStringList* parts) {
    const QStringList scalarFields = {
        QStringLiteral("text"), QStringLiteral("content"),
        QStringLiteral("code_body")};
    for (const QString& field : scalarFields) {
        const QString value = item.value(field).toString().trimmed();
        if (!value.isEmpty())
            parts->append(field == QLatin1String("code_body")
                              ? htmlText(value) : value);
    }
    const QString table = item.value(QStringLiteral("table_body")).toString();
    if (!table.trimmed().isEmpty())
        parts->append(htmlText(table));
    const QStringList arrayFields = {
        QStringLiteral("list_items"), QStringLiteral("table_caption"),
        QStringLiteral("table_footnote"), QStringLiteral("image_caption"),
        QStringLiteral("image_footnote"), QStringLiteral("chart_caption"),
        QStringLiteral("chart_footnote"), QStringLiteral("code_caption")};
    for (const QString& field : arrayFields) {
        for (const QJsonValue& value : item.value(field).toArray()) {
            const QString text = value.toString().trimmed();
            if (!text.isEmpty())
                parts->append(text);
        }
    }
}

void collectScores(const QJsonValue& value, QVector<double>* scores) {
    if (value.isArray()) {
        for (const QJsonValue& child : value.toArray())
            collectScores(child, scores);
        return;
    }
    if (!value.isObject())
        return;
    const QJsonObject object = value.toObject();
    for (auto iterator = object.constBegin(); iterator != object.constEnd();
         ++iterator) {
        if ((iterator.key() == QLatin1String("score")
             || iterator.key() == QLatin1String("confidence"))
            && iterator.value().isDouble()) {
            const double score = iterator.value().toDouble();
            if (score >= 0.0 && score <= 1.0)
                scores->append(score);
        }
        collectScores(iterator.value(), scores);
    }
}

ConfidenceSummary summarize(const QVector<double>& scores,
                            const QString& source) {
    ConfidenceSummary result;
    result.source = source;
    if (scores.isEmpty())
        return result;
    result.count = scores.size();
    result.minimum = std::numeric_limits<double>::max();
    result.maximum = std::numeric_limits<double>::lowest();
    double total = 0.0;
    for (double score : scores) {
        total += score;
        result.minimum = std::min(result.minimum, score);
        result.maximum = std::max(result.maximum, score);
    }
    result.mean = total / static_cast<double>(scores.size());
    return result;
}

}  // namespace

double Score::accuracy() const {
    if (referenceCharacters <= 0)
        return 0.0;
    return 1.0 - static_cast<double>(editDistance)
                     / static_cast<double>(referenceCharacters);
}

bool loadDataset(const QString& manifestPath, Dataset* dataset, QString* error) {
    if (!dataset) {
        if (error)
            *error = QStringLiteral("数据集输出参数为空");
        return false;
    }
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取验收清单：%1").arg(file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (!document.isObject()) {
        if (error)
            *error = QStringLiteral("验收清单不是有效 JSON：%1")
                         .arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema")).toString()
        != QLatin1String("RecognitionStudio-Acceptance/1")) {
        if (error)
            *error = QStringLiteral("不支持的验收清单 schema");
        return false;
    }
    Dataset loaded;
    loaded.id = root.value(QStringLiteral("dataset_id")).toString().trimmed();
    loaded.manifestPath = QFileInfo(manifestPath).absoluteFilePath();
    loaded.manifestSha256 = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QString kind = root.value(QStringLiteral("task_type")).toString();
    if (kind == QLatin1String("scan"))
        loaded.kind = Kind::Scan;
    else if (kind == QLatin1String("voice"))
        loaded.kind = Kind::Voice;
    else {
        if (error)
            *error = QStringLiteral("task_type 必须是 scan 或 voice");
        return false;
    }
    loaded.threshold = root.value(QStringLiteral("threshold")).toDouble(0.98);
    if (loaded.threshold < 0.98 || loaded.threshold > 1.0) {
        if (error)
            *error = QStringLiteral("正式验收阈值不得低于 0.98");
        return false;
    }
    const QJsonObject normalization =
        root.value(QStringLiteral("normalization")).toObject();
    loaded.removeWhitespace =
        normalization.value(QStringLiteral("remove_whitespace")).toBool(false);
    loaded.ignorePunctuation =
        normalization.value(QStringLiteral("ignore_punctuation")).toBool(false);
    loaded.caseSensitive =
        normalization.value(QStringLiteral("case_sensitive")).toBool(true);
    const QDir rootDirectory = QFileInfo(manifestPath).absoluteDir();
    const QJsonArray samples = root.value(QStringLiteral("samples")).toArray();
    for (int index = 0; index < samples.size(); ++index) {
        const QJsonObject object = samples.at(index).toObject();
        Sample sample;
        sample.id = object.value(QStringLiteral("id")).toString().trimmed();
        if (sample.id.isEmpty())
            sample.id = QStringLiteral("sample-%1").arg(index + 1, 4, 10, QLatin1Char('0'));
        sample.inputPath = resolvedPath(
            rootDirectory, object.value(QStringLiteral("input")).toString());
        if (!QFileInfo::exists(sample.inputPath)) {
            if (error)
                *error = QStringLiteral("样本输入不存在：%1").arg(sample.inputPath);
            return false;
        }
        QString actualReferenceHash;
        if (object.contains(QStringLiteral("reference_text"))) {
            sample.reference = object.value(QStringLiteral("reference_text")).toString();
            actualReferenceHash = QString::fromLatin1(
                QCryptographicHash::hash(sample.reference.toUtf8(),
                                         QCryptographicHash::Sha256).toHex());
        } else {
            const QString referencePath = resolvedPath(
                rootDirectory,
                object.value(QStringLiteral("reference_path")).toString());
            QString readError;
            sample.reference = readUtf8(referencePath, &readError);
            if (!readError.isEmpty()) {
                if (error)
                    *error = readError;
                return false;
            }
            actualReferenceHash = sha256File(referencePath);
        }
        if (sample.reference.trimmed().isEmpty()) {
            if (error)
                *error = QStringLiteral("样本真值为空：%1").arg(sample.id);
            return false;
        }
        sample.inputSha256 =
            object.value(QStringLiteral("input_sha256")).toString().toLower();
        sample.referenceSha256 =
            object.value(QStringLiteral("reference_sha256")).toString().toLower();
        const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));
        if (!hashPattern.match(sample.inputSha256).hasMatch()
            || !hashPattern.match(sample.referenceSha256).hasMatch()) {
            if (error)
                *error = QStringLiteral("样本缺少有效 SHA-256：%1").arg(sample.id);
            return false;
        }
        const QString actualInputHash = sha256File(sample.inputPath);
        if (actualInputHash != sample.inputSha256
            || actualReferenceHash != sample.referenceSha256) {
            if (error)
                *error = QStringLiteral("冻结样本 SHA-256 不匹配：%1").arg(sample.id);
            return false;
        }
        loaded.samples.append(sample);
    }
    if (loaded.id.isEmpty() || loaded.samples.isEmpty()) {
        if (error)
            *error = QStringLiteral("验收清单必须包含 dataset_id 和 samples");
        return false;
    }
    *dataset = std::move(loaded);
    if (error)
        error->clear();
    return true;
}

QString normalizeText(const QString& text, const Dataset& dataset) {
    QString value = text.normalized(QString::NormalizationForm_C);
    value.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    value.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    value = value.trimmed();
    if (!dataset.caseSensitive)
        value = value.toCaseFolded();
    if (dataset.removeWhitespace)
        value.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (dataset.ignorePunctuation) {
        value.remove(QRegularExpression(
            QStringLiteral("[\\p{P}\\p{S}]+"),
            QRegularExpression::UseUnicodePropertiesOption));
    }
    return value;
}

Score compare(const QString& reference, const QString& hypothesis,
              const Dataset& dataset) {
    const QVector<uint> expected = normalizeText(reference, dataset).toUcs4();
    const QVector<uint> actual = normalizeText(hypothesis, dataset).toUcs4();
    QVector<qint64> previous(actual.size() + 1);
    QVector<qint64> current(actual.size() + 1);
    for (int column = 0; column <= actual.size(); ++column)
        previous[column] = column;
    for (int row = 1; row <= expected.size(); ++row) {
        current[0] = row;
        for (int column = 1; column <= actual.size(); ++column) {
            const qint64 substitution = previous[column - 1]
                + (expected[row - 1] == actual[column - 1] ? 0 : 1);
            current[column] = std::min(
                {previous[column] + 1, current[column - 1] + 1,
                 substitution});
        }
        previous.swap(current);
    }
    return {expected.size(), actual.size(), previous.last()};
}

QString extractScanText(const QString& contentListPath, QString* error) {
    QFile file(contentListPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取扫描结果 JSON：%1")
                         .arg(file.errorString());
        return {};
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isArray()) {
        if (error)
            *error = QStringLiteral("扫描结果 JSON 无效：%1")
                         .arg(parseError.errorString());
        return {};
    }
    QStringList parts;
    for (const QJsonValue& value : document.array())
        appendContentItem(value.toObject(), &parts);
    if (error)
        error->clear();
    return parts.join(QLatin1Char('\n'));
}

ConfidenceSummary extractScanConfidence(const QString& contentListPath) {
    const QFileInfo contentInfo(contentListPath);
    QString stem = contentInfo.completeBaseName();
    stem.remove(QRegularExpression(QStringLiteral("_content_list$")));
    const QStringList candidates = {
        contentInfo.dir().filePath(stem + QStringLiteral("_middle.json")),
        contentInfo.dir().filePath(stem + QStringLiteral("_model.json"))};
    QVector<double> scores;
    for (const QString& candidate : candidates) {
        QFile file(candidate);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        collectScores(document.isArray() ? QJsonValue(document.array())
                                         : QJsonValue(document.object()),
                      &scores);
    }
    return summarize(scores, QStringLiteral("scan_intermediate_score"));
}

ConfidenceSummary extractVoiceConfidence(const QByteArray& resultJson) {
    const QJsonObject root = QJsonDocument::fromJson(resultJson).object();
    QVector<double> scores;
    for (const QJsonValue& value : root.value(QStringLiteral("segments")).toArray()) {
        const QJsonValue confidence =
            value.toObject().value(QStringLiteral("confidence"));
        const double score = confidence.isObject()
            ? confidence.toObject().value(QStringLiteral("value")).toDouble(-1.0)
            : confidence.toDouble(-1.0);
        if (score >= 0.0 && score <= 1.0)
            scores.append(score);
    }
    return summarize(scores, QStringLiteral("decoder_token_geometric_mean"));
}

QString sha256File(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

bool writeJson(const QString& path, const QJsonObject& value, QString* error) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool writeUtf8(const QString& path, const QString& value, QString* error) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = value.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

}  // namespace speechdoc::acceptance

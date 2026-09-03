#include "acceptance_validation.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
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

QJsonObject describeFirstDifference(const QString& reference,
                                    const QString& hypothesis,
                                    const Dataset& dataset) {
    const QVector<uint> expected = normalizeText(reference, dataset).toUcs4();
    const QVector<uint> actual = normalizeText(hypothesis, dataset).toUcs4();
    int index = 0;
    while (index < expected.size() && index < actual.size()
           && expected.at(index) == actual.at(index)) {
        ++index;
    }
    if (index == expected.size() && index == actual.size())
        return {};

    const auto character = [](const QVector<uint>& text, int position) {
        if (position >= text.size())
            return QStringLiteral("<文本结束>");
        const uint codePoint = text.at(position);
        if (codePoint == static_cast<uint>(' '))
            return QStringLiteral("␠");
        if (codePoint == static_cast<uint>('\n'))
            return QStringLiteral("↵");
        if (codePoint == static_cast<uint>('\t'))
            return QStringLiteral("⇥");
        return QString::fromUcs4(&codePoint, 1);
    };
    const auto codePoint = [](const QVector<uint>& text, int position) {
        return position < text.size()
            ? QStringLiteral("U+%1").arg(
                  text.at(position), 4, 16, QLatin1Char('0')).toUpper()
            : QString();
    };
    const auto context = [index](const QVector<uint>& text) {
        if (text.isEmpty())
            return QString();
        const int begin = qMax(0, index - 8);
        const int count = qMin(text.size() - begin, 17);
        return QString::fromUcs4(text.constData() + begin, count);
    };

    QJsonObject difference;
    difference.insert(QStringLiteral("position"), index + 1);
    difference.insert(QStringLiteral("reference_character"),
                      character(expected, index));
    difference.insert(QStringLiteral("reference_code_point"),
                      codePoint(expected, index));
    difference.insert(QStringLiteral("hypothesis_character"),
                      character(actual, index));
    difference.insert(QStringLiteral("hypothesis_code_point"),
                      codePoint(actual, index));
    difference.insert(QStringLiteral("reference_context"), context(expected));
    difference.insert(QStringLiteral("hypothesis_context"), context(actual));
    difference.insert(
        QStringLiteral("comparison_basis"),
        QStringLiteral("normalized text used by the accuracy metric"));
    return difference;
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

QString sha256File(const QString& path, const std::atomic_bool* cancelled) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer;
    buffer.resize(8 * 1024 * 1024);
    for (;;) {
        if (cancelled && cancelled->load())
            return {};
        const qint64 count = file.read(buffer.data(), buffer.size());
        if (count < 0)
            return {};
        if (count == 0)
            break;
        hash.addData(buffer.constData(), static_cast<int>(count));
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString sha256Bytes(const QByteArray& value) {
    return QString::fromLatin1(
        QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex());
}

bool loadSha256Manifest(const QString& path,
                        QMap<QString, QString>* entries,
                        QString* error) {
    if (!entries) {
        if (error)
            *error = QStringLiteral("校验清单输出参数为空");
        return false;
    }
    entries->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("无法读取 SHA-256 清单：%1（%2）")
                         .arg(path, file.errorString());
        }
        return false;
    }
    const QRegularExpression linePattern(
        QStringLiteral("^([0-9a-fA-F]{64})[\\t ]+(.+?)\\r?$"));
    int lineNumber = 0;
    while (!file.atEnd()) {
        ++lineNumber;
        const QString line = QString::fromUtf8(file.readLine());
        if (line.trimmed().isEmpty())
            continue;
        const QRegularExpressionMatch match = linePattern.match(line);
        if (!match.hasMatch()) {
            if (error) {
                *error = QStringLiteral("SHA-256 清单第 %1 行格式无效：%2")
                             .arg(lineNumber)
                             .arg(path);
            }
            entries->clear();
            return false;
        }
        QString relativePath = QDir::fromNativeSeparators(
            match.captured(2).trimmed());
        relativePath = QDir::cleanPath(relativePath);
        const QStringList components = relativePath.split(
            QLatin1Char('/'), QString::SkipEmptyParts);
        if (relativePath.isEmpty() || relativePath == QLatin1String(".")
            || QDir::isAbsolutePath(relativePath)
            || components.contains(QStringLiteral(".."))) {
            if (error) {
                *error = QStringLiteral("SHA-256 清单包含不安全路径：%1")
                             .arg(relativePath);
            }
            entries->clear();
            return false;
        }
        if (entries->contains(relativePath)) {
            if (error) {
                *error = QStringLiteral("SHA-256 清单包含重复路径：%1")
                             .arg(relativePath);
            }
            entries->clear();
            return false;
        }
        entries->insert(relativePath, match.captured(1).toLower());
    }
    if (entries->isEmpty()) {
        if (error)
            *error = QStringLiteral("SHA-256 清单不包含任何条目：%1").arg(path);
        return false;
    }
    if (error)
        error->clear();
    return true;
}

RuntimePayloadVerification verifyRuntimePayload(
    const QString& runtimeRoot,
    const QString& checksumManifestPath,
    const QString& expectedChecksumManifestSha256,
    const QVector<RuntimeFileExpectation>& expectations,
    const std::atomic_bool* cancelled) {
    RuntimePayloadVerification verification;
    QJsonObject evidence;
    const QString expectedManifestHash =
        expectedChecksumManifestSha256.trimmed().toLower();
    const QString actualManifestHash = sha256File(
        checksumManifestPath, cancelled);
    const QDir runtimeDirectory(runtimeRoot);
    QString checksumRelativePath = QDir::fromNativeSeparators(
        runtimeDirectory.relativeFilePath(
            QFileInfo(checksumManifestPath).absoluteFilePath()));
    checksumRelativePath = QDir::cleanPath(checksumRelativePath);
    const QStringList checksumComponents = checksumRelativePath.split(
        QLatin1Char('/'), QString::SkipEmptyParts);
    const bool checksumInsideRuntime = !checksumRelativePath.isEmpty()
        && checksumRelativePath != QLatin1String(".")
        && !QDir::isAbsolutePath(checksumRelativePath)
        && !checksumComponents.contains(QStringLiteral(".."));
    const bool manifestHashValid = checksumInsideRuntime
        && QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
            .match(expectedManifestHash).hasMatch()
        && actualManifestHash == expectedManifestHash;
    evidence.insert(QStringLiteral("runtime_root"),
                    QDir(runtimeRoot).absolutePath());
    evidence.insert(QStringLiteral("checksum_manifest_path"),
                    checksumManifestPath);
    evidence.insert(QStringLiteral("expected_checksum_manifest_sha256"),
                    expectedManifestHash);
    evidence.insert(QStringLiteral("actual_checksum_manifest_sha256"),
                    actualManifestHash);
    evidence.insert(QStringLiteral("checksum_manifest_matches_build_anchor"),
                    manifestHashValid);
    evidence.insert(QStringLiteral("checksum_manifest_inside_runtime"),
                    checksumInsideRuntime);

    bool filesValid = !expectations.isEmpty();
    bool exactFileSetValid = checksumInsideRuntime;
    qint64 checkedBytes = 0;
    QJsonArray files;
    QStringList errors;
    QSet<QString> expectedRuntimePaths;
    if (checksumInsideRuntime)
        expectedRuntimePaths.insert(checksumRelativePath);
    if (!manifestHashValid)
        errors.append(QStringLiteral("SDK 校验清单与构建时锚点不匹配"));
    if (expectations.isEmpty())
        errors.append(QStringLiteral("没有需要校验的运行时文件"));
    const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));
    for (const RuntimeFileExpectation& expectation : expectations) {
        if (cancelled && cancelled->load()) {
            filesValid = false;
            errors.append(QStringLiteral("运行时文件校验已取消"));
            evidence.insert(QStringLiteral("cancelled"), true);
            break;
        }
        QString relativePath = QDir::fromNativeSeparators(
            expectation.runtimeRelativePath.trimmed());
        relativePath = QDir::cleanPath(relativePath);
        const QStringList components = relativePath.split(
            QLatin1Char('/'), QString::SkipEmptyParts);
        const bool safePath = !relativePath.isEmpty()
            && relativePath != QLatin1String(".")
            && !QDir::isAbsolutePath(relativePath)
            && !components.contains(QStringLiteral(".."));
        if (safePath) {
            if (expectedRuntimePaths.contains(relativePath)) {
                filesValid = false;
                exactFileSetValid = false;
                if (errors.size() < 8) {
                    errors.append(QStringLiteral("运行时校验条目路径重复：%1")
                                      .arg(relativePath));
                }
            }
            expectedRuntimePaths.insert(relativePath);
        }
        const QString expectedHash = expectation.expectedSha256.toLower();
        const bool expectedHashValid =
            hashPattern.match(expectedHash).hasMatch();
        const QString runtimePath = safePath
            ? QDir(runtimeRoot).absoluteFilePath(relativePath) : QString();
        const QString actualHash = safePath
            ? sha256File(runtimePath, cancelled) : QString();
        const QFileInfo info(runtimePath);
        const bool matched = safePath && expectedHashValid
            && !actualHash.isEmpty() && actualHash == expectedHash;
        if (info.isFile())
            checkedBytes += info.size();
        filesValid = filesValid && matched;

        QJsonObject fileEvidence;
        fileEvidence.insert(QStringLiteral("manifest_path"),
                            expectation.manifestPath);
        fileEvidence.insert(QStringLiteral("runtime_path"), runtimePath);
        fileEvidence.insert(QStringLiteral("expected_sha256"), expectedHash);
        fileEvidence.insert(QStringLiteral("actual_sha256"), actualHash);
        fileEvidence.insert(QStringLiteral("bytes"),
                            static_cast<double>(info.isFile() ? info.size() : 0));
        fileEvidence.insert(QStringLiteral("matched"), matched);
        files.append(fileEvidence);
        if (!matched && errors.size() < 8) {
            errors.append(QStringLiteral("运行时文件校验失败：%1")
                              .arg(expectation.manifestPath));
        }
    }
    QJsonArray unexpectedFiles;
    bool enumerationCancelled = false;
    QSet<QString> actualRuntimePaths;
    QDirIterator iterator(
        runtimeDirectory.absolutePath(),
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        if (cancelled && cancelled->load()) {
            enumerationCancelled = true;
            exactFileSetValid = false;
            filesValid = false;
            if (!evidence.value(QStringLiteral("cancelled")).toBool()) {
                errors.append(QStringLiteral("运行时文件校验已取消"));
                evidence.insert(QStringLiteral("cancelled"), true);
            }
            break;
        }
        const QString absolutePath = iterator.next();
        const QString relativePath = QDir::cleanPath(
            QDir::fromNativeSeparators(
                runtimeDirectory.relativeFilePath(absolutePath)));
        actualRuntimePaths.insert(relativePath);
        if (!expectedRuntimePaths.contains(relativePath)) {
            exactFileSetValid = false;
            unexpectedFiles.append(relativePath);
        }
    }
    if (!enumerationCancelled) {
        for (const QString& expectedPath : expectedRuntimePaths) {
            if (!actualRuntimePaths.contains(expectedPath)) {
                exactFileSetValid = false;
                break;
            }
        }
    }
    if (!unexpectedFiles.isEmpty()) {
        filesValid = false;
        errors.append(QStringLiteral("运行时目录包含 %1 个发布基线之外的文件")
                          .arg(unexpectedFiles.size()));
    }
    evidence.insert(QStringLiteral("expected_file_count"),
                    expectedRuntimePaths.size());
    evidence.insert(QStringLiteral("actual_file_count"),
                    actualRuntimePaths.size());
    evidence.insert(QStringLiteral("unexpected_file_count"),
                    unexpectedFiles.size());
    evidence.insert(QStringLiteral("unexpected_files"), unexpectedFiles);
    evidence.insert(QStringLiteral("exact_file_set_match"),
                    exactFileSetValid);
    verification.valid = manifestHashValid && filesValid
        && exactFileSetValid;
    verification.error = errors.join(QStringLiteral("；"));
    evidence.insert(QStringLiteral("checked_file_count"), files.size());
    evidence.insert(QStringLiteral("checked_bytes"),
                    static_cast<double>(checkedBytes));
    evidence.insert(QStringLiteral("files"), files);
    evidence.insert(QStringLiteral("matched"), verification.valid);
    if (!verification.error.isEmpty())
        evidence.insert(QStringLiteral("error"), verification.error);
    verification.evidence = evidence;
    return verification;
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

bool writeChecksummedJson(const QString& jsonPath,
                          const QString& checksumPath,
                          const QJsonObject& value,
                          QString* jsonSha256,
                          QString* checksumSha256,
                          QString* error) {
    if (jsonSha256)
        jsonSha256->clear();
    if (checksumSha256)
        checksumSha256->clear();
    if (error)
        error->clear();
    QString localError;
    if (!writeJson(jsonPath, value, &localError)) {
        if (error)
            *error = localError;
        return false;
    }
    const QString reportHash = sha256File(jsonPath);
    if (reportHash.isEmpty()) {
        if (error)
            *error = QStringLiteral("无法计算 JSON 文件 SHA-256");
        return false;
    }
    const QByteArray checksumBytes =
        reportHash.toLatin1() + QByteArrayLiteral("  ")
        + QFileInfo(jsonPath).fileName().toUtf8()
        + QByteArrayLiteral("\n");
    if (!writeBytes(checksumPath, checksumBytes, &localError)) {
        if (error)
            *error = localError;
        return false;
    }
    QFile checksumFile(checksumPath);
    if (!checksumFile.open(QIODevice::ReadOnly)
        || checksumFile.readAll() != checksumBytes) {
        if (error)
            *error = QStringLiteral("SHA256SUMS.txt 读回校验失败");
        return false;
    }
    const QString checksumHash = sha256File(checksumPath);
    if (checksumHash.isEmpty()) {
        if (error)
            *error = QStringLiteral("无法计算 SHA256SUMS.txt SHA-256");
        return false;
    }
    if (jsonSha256)
        *jsonSha256 = reportHash;
    if (checksumSha256)
        *checksumSha256 = checksumHash;
    return true;
}

bool writeBytes(const QString& path, const QByteArray& value, QString* error) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (file.write(value) != value.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool writeUtf8(const QString& path, const QString& value, QString* error) {
    return writeBytes(path, value.toUtf8(), error);
}

}  // namespace speechdoc::acceptance

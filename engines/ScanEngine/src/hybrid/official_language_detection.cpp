#include "scanengine/hybrid/official_language_detection.hpp"

#include "scanengine/config.hpp"

#include "fasttext.h"

#include <QByteArray>
#include <QChar>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace scanengine {
namespace hybrid {
namespace {

constexpr char kFastTextModelSha256[] =
    "8f3472cfe8738a7b6099e8e999c3cbfae0dcd15696aac7d7738a8039db603e83";
constexpr std::int32_t kFastTextFileMagic = 793712314;
constexpr std::int32_t kFastTextMaximumVersion = 12;

// FastText::loadModel(std::istream&) starts after the file signature because
// the filename overload calls its protected checkModel() first.  Keep loading
// from memory (for Unicode Windows paths) while preserving that exact boundary.
class LockedFastText final : public fasttext::FastText {
 public:
    void loadLockedStream(std::istream& stream) {
        std::int32_t magic = 0;
        std::int32_t fileVersion = 0;
        stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        stream.read(reinterpret_cast<char*>(&fileVersion), sizeof(fileVersion));
        if (!stream || magic != kFastTextFileMagic
            || fileVersion > kFastTextMaximumVersion) {
            throw std::invalid_argument("locked fastText model has wrong file format");
        }
        version = fileVersion;
        fasttext::FastText::loadModel(stream);
    }
};

struct ModelCache {
    std::mutex mutex;
    QString path;
    std::shared_ptr<fasttext::FastText> model;
};

ModelCache& modelCache() {
    static ModelCache cache;
    return cache;
}

QString absoluteOverridePath(const QString& path) {
    const QFileInfo info(path);
    if (info.isAbsolute())
        return QDir::cleanPath(path);
    return QDir::cleanPath(QDir::current().absoluteFilePath(path));
}

QString configuredFastTextRoot() {
    QFile manifest(modelManifestPath());
    if (!manifest.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(manifest.readAll());
    if (!document.isObject())
        return {};
    const QString configured = document.object()
                                   .value(QStringLiteral("models-dir"))
                                   .toObject()
                                   .value(QStringLiteral("fasttext"))
                                   .toString();
    return configured.isEmpty() ? QString() : resolveRepoPath(configured);
}

bool loadLockedModel(const QString& path,
                     std::shared_ptr<fasttext::FastText>* model,
                     QString* err) {
    auto fail = [&](const QString& message) {
        if (err)
            *err = message;
        return false;
    };

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("cannot open locked fastText language model: %1 (%2)")
                        .arg(path, file.errorString()));
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return fail(QStringLiteral("cannot read locked fastText language model: %1 (%2)")
                        .arg(path, file.errorString()));
    }
    const QByteArray digest =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    if (digest != QByteArray(kFastTextModelSha256)) {
        return fail(QStringLiteral("locked fastText language model SHA-256 mismatch: %1 "
                                   "(expected %2, actual %3)")
                        .arg(path, QString::fromLatin1(kFastTextModelSha256),
                             QString::fromLatin1(digest)));
    }

    try {
        const std::string payload(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        std::istringstream stream(payload, std::ios::in | std::ios::binary);
        auto candidate = std::make_shared<LockedFastText>();
        candidate->loadLockedStream(stream);
        *model = std::move(candidate);
    } catch (const std::exception& exception) {
        return fail(QStringLiteral("cannot load locked fastText language model: %1 (%2)")
                        .arg(path, QString::fromUtf8(exception.what())));
    } catch (...) {
        return fail(QStringLiteral("cannot load locked fastText language model: %1 "
                                   "(unknown error)")
                        .arg(path));
    }
    if (err)
        err->clear();
    return true;
}

bool acquireModel(std::shared_ptr<fasttext::FastText>* model, QString* err) {
    const QString path = resolveOfficialFastTextModelPath();
    ModelCache& cache = modelCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.model && cache.path == path) {
        *model = cache.model;
        if (err)
            err->clear();
        return true;
    }

    std::shared_ptr<fasttext::FastText> loaded;
    if (!loadLockedModel(path, &loaded, err))
        return false;
    cache.path = path;
    cache.model = loaded;
    *model = std::move(loaded);
    return true;
}

QString removeLfAndLoneSurrogates(const QString& text) {
    QString sanitized;
    sanitized.reserve(text.size());
    for (int index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (character == QLatin1Char('\n'))
            continue;
        if (character.isHighSurrogate()) {
            if (index + 1 < text.size() && text.at(index + 1).isLowSurrogate()) {
                sanitized.append(character);
                sanitized.append(text.at(++index));
            }
            continue;
        }
        if (character.isLowSurrogate())
            continue;
        sanitized.append(character);
    }
    return sanitized;
}

bool isUnicodeCategoryC(QChar::Category category) {
    switch (category) {
    case QChar::Other_Control:
    case QChar::Other_Format:
    case QChar::Other_Surrogate:
    case QChar::Other_PrivateUse:
    case QChar::Other_NotAssigned:
        return true;
    default:
        return false;
    }
}

QString removeUnicodeCategoryC(const QString& text) {
    QString filtered;
    filtered.reserve(text.size());
    for (int index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        uint codePoint = character.unicode();
        int width = 1;
        if (character.isHighSurrogate() && index + 1 < text.size()
            && text.at(index + 1).isLowSurrogate()) {
            codePoint = QChar::surrogateToUcs4(character, text.at(index + 1));
            width = 2;
        }
        if (!isUnicodeCategoryC(QChar::category(codePoint))) {
            filtered.append(character);
            if (width == 2)
                filtered.append(text.at(index + 1));
        }
        index += width - 1;
    }
    return filtered;
}

bool containsStrictKana(const QString& text) {
    for (const QChar character : text) {
        const ushort code = character.unicode();
        if (code > 0x3040 && code < 0x30FF)
            return true;
    }
    return false;
}

OfficialLanguageDetection predict(const fasttext::FastText& model,
                                  const QString& text) {
    QByteArray utf8 = text.toUtf8();
    utf8.append('\n');  // fasttext-predict 0.9.2.4 Python wrapper behavior.
    const std::string line(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    std::istringstream stream(line, std::ios::in | std::ios::binary);
    std::vector<std::pair<fasttext::real, std::string>> predictions;
    if (!model.predictLine(stream, predictions, 1, 0.0f) || predictions.empty())
        throw std::runtime_error("fastText returned no language prediction");

    QString language = QString::fromUtf8(predictions.front().second.c_str());
    language.replace(QStringLiteral("__label__"), QString());
    language = language.toUpper();
    if (language == QLatin1String("JA") && !containsStrictKana(text))
        language = QStringLiteral("ZH");

    OfficialLanguageDetection result;
    result.language = language.toLower();
    result.score = std::min(float(predictions.front().first), 1.0f);
    return result;
}

}  // namespace

QString resolveOfficialFastTextModelPath() {
    const QString overridePath = qEnvironmentVariable("SCANENGINE_FASTTEXT_MODEL");
    if (!overridePath.isEmpty())
        return absoluteOverridePath(overridePath);

    // The deployed manifest is immutable for a running process.  Cache its
    // resolution so the ~10-microsecond prediction path never performs JSON
    // file I/O; the explicit environment override remains dynamic for tests.
    static const QString configuredPath = [] {
        QString root = configuredFastTextRoot();
        if (root.isEmpty())
            root = QDir(defaultModelsDir()).filePath(QStringLiteral("fasttext"));
        return QDir(root).filePath(QStringLiteral("lid.176.ftz"));
    }();
    return configuredPath;
}

bool ensureOfficialLanguageModel(QString* err) {
    std::shared_ptr<fasttext::FastText> model;
    return acquireModel(&model, err);
}

bool detectOfficialTextLanguage(const QString& text,
                                OfficialLanguageDetection* result,
                                QString* err) {
    if (!result) {
        if (err)
            *err = QStringLiteral("official language detection result is null");
        return false;
    }
    *result = {};
    if (text.isEmpty()) {
        if (err)
            err->clear();
        return true;
    }

    std::shared_ptr<fasttext::FastText> model;
    if (!acquireModel(&model, err))
        return false;

    const QString sanitized = removeLfAndLoneSurrogates(text);
    try {
        *result = predict(*model, sanitized);
    } catch (const std::exception&) {
        try {
            *result = predict(*model, removeUnicodeCategoryC(sanitized));
        } catch (const std::exception& exception) {
            if (err) {
                *err = QStringLiteral("fastText language prediction failed after MinerU "
                                      "category-C fallback: %1")
                           .arg(QString::fromUtf8(exception.what()));
            }
            return false;
        } catch (...) {
            if (err)
                *err = QStringLiteral("fastText language prediction failed after MinerU "
                                      "category-C fallback: unknown error");
            return false;
        }
    } catch (...) {
        try {
            *result = predict(*model, removeUnicodeCategoryC(sanitized));
        } catch (...) {
            if (err)
                *err = QStringLiteral("fastText language prediction failed after MinerU "
                                      "category-C fallback: unknown error");
            return false;
        }
    }
    if (err)
        err->clear();
    return true;
}

}  // namespace hybrid
}  // namespace scanengine

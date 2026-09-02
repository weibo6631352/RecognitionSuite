#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <functional>

namespace scanengine {
namespace hybrid {

// Exact batch=1 input contract of Magika 1.0.3 standard_v3_3.
struct OfficialMagikaFeatures {
    QVector<std::int32_t> bytes;
    bool requiresInference = false;
    QString immediateLabel = QStringLiteral("txt");
};

struct OfficialCodeLanguageResult {
    QString label = QStringLiteral("txt");
    QString rawLabel;
    QString diagnostic;
    float score = 1.0f;
    bool usedInference = false;
};

// Narrow test/diagnostic seam. Production passes no hook and always invokes
// the locked ONNX Runtime session. A hook still runs only after identity and
// session loading has succeeded, so failure-fallback semantics are testable.
struct OfficialCodeLanguageHooks {
    std::function<bool(const QVector<std::int32_t>&,
                       QVector<float>*,
                       QString*)> infer;
};

// mineru.utils.visual_magic_model_utils.code_content_clean. Python splitlines()
// semantics are preserved for all eleven line boundaries before joining with LF.
QString cleanOfficialCodeContent(const QString& content);

// mineru.utils.guess_suffix_or_lang._normalize_text_for_language_guess followed
// by UTF-8 encode(errors="replace"). Lone UTF-16 surrogates are discarded and
// valid surrogate pairs are retained as their scalar UTF-8 encoding.
QByteArray normalizeOfficialCodeUtf8(const QString& code);

// Magika feature extraction v2: first/last 4096 bytes, ASCII bytes
// lstrip/rstrip, 1024+1024 int32 tokens, padding token 256. Content with fewer
// than eight meaningful beginning bytes returns the official immediate result.
bool prepareOfficialMagikaFeatures(const QByteArray& content,
                                   OfficialMagikaFeatures* features,
                                   QString* err = nullptr);

// Exact first-argmax (strict >), overwrite_map, per-label >= threshold and
// MinerU unknown-to-txt post-processing. Exposed as a pure contract seam.
bool finalizeOfficialMagikaPrediction(const QStringList& labels,
                                      const QHash<QString, double>& thresholds,
                                      double mediumConfidenceThreshold,
                                      const QVector<float>& scores,
                                      OfficialCodeLanguageResult* result,
                                      QString* err = nullptr);

// Resolve models/magika/standard_v3_3 from SCANENGINE_MAGIKA_MODEL_DIR, then
// scanengine.json models-dir.magika, then the default deployed models root.
QString resolveOfficialMagikaModelDir();

// Resolve the byte-locked app-private ONNX Runtime without loading it or
// creating a session. Used by the Hybrid readiness gate and deployment tests.
QString resolveOfficialMagikaRuntimePath();

// Lazily create and cache one batch=1 CPUExecutionProvider session for each
// resolved, byte-locked model/runtime identity.
bool ensureOfficialMagikaModel(QString* err = nullptr);

// Official narrow MinerU code-language boundary. Identity/config/session load
// errors return false and block artifact publication. Once the locked session
// has loaded, a single Run failure returns true with label=txt and a diagnostic.
bool guessOfficialCodeLanguage(const QString& cleanedCode,
                               OfficialCodeLanguageResult* result,
                               QString* err = nullptr,
                               const OfficialCodeLanguageHooks* hooks = nullptr);

// Raw batch=1 logits seam used by the locked Python-wheel vs product-SDK
// differential. scoreBits are compared as all 214 float32 bit patterns.
bool inferOfficialMagikaScores(const QString& cleanedCode,
                               QVector<float>* scores,
                               bool* requiresInference,
                               QString* err = nullptr);

// Diagnostics/test observability; reading this counter never triggers loading.
quint64 officialMagikaSuccessfulSessionLoadCount();

}  // namespace hybrid
}  // namespace scanengine

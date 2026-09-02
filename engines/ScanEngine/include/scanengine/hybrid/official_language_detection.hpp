#pragma once

#include <QString>

namespace scanengine {
namespace hybrid {

struct OfficialLanguageDetection {
    QString language;
    float score = 0.0f;
};

// Resolve models/fasttext/lid.176.ftz from SCANENGINE_FASTTEXT_MODEL, then
// scanengine.json models-dir.fasttext, then the default deployed models root.
QString resolveOfficialFastTextModelPath();

// Load and validate the locked low-memory language model without predicting.
// The model is cached by resolved path after a successful load.
bool ensureOfficialLanguageModel(QString* err);

// MinerU 0dfc9460 + fast-langdetect 0.2.5 compatible detect_lang boundary:
// remove LF, remove lone UTF-16 surrogates, predict top-1 at threshold 0, map
// kana-free JA to ZH, then return a lowercase language code.  An originally
// empty QString returns an empty language with score zero.
bool detectOfficialTextLanguage(const QString& text,
                                OfficialLanguageDetection* result,
                                QString* err);

}  // namespace hybrid
}  // namespace scanengine

#pragma once

#include <QHash>
#include <QString>
#include <QVector>

namespace scanengine {
namespace hybrid {

// Official HF tokenizer.json (Qwen2 BPE + MinerU added specials).
struct OfficialTokenizer {
    QHash<QString, int> vocab;
    QHash<QString, int> mergeRank;
    QHash<QString, int> specials;  // content -> id
    QHash<int, QString> idToToken;
    int unkId = -1;
};

bool loadOfficialTokenizer(const QString& tokenizerJsonPath, OfficialTokenizer* out, QString* err);

QVector<int> encodeOfficial(const OfficialTokenizer& tok, const QString& text);
QString decodeOfficial(const OfficialTokenizer& tok, const QVector<int>& ids, bool skipSpecials = false);

}  // namespace hybrid
}  // namespace scanengine

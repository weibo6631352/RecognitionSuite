#include "scanengine/hybrid/tokenizer.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVector>
#include <algorithm>
#include <limits>

namespace scanengine {
namespace hybrid {

namespace {

struct ByteMaps {
    QChar toUnicode[256];
    QHash<uint, uchar> toByte;
};

ByteMaps makeByteMaps() {
    // GPT-2 / Qwen2 bytes_to_unicode
    QVector<int> bs;
    for (int i = int('!'); i <= int('~'); ++i)
        bs.push_back(i);
    for (int i = 161; i <= 172; ++i)
        bs.push_back(i);
    for (int i = 174; i <= 255; ++i)
        bs.push_back(i);
    QVector<int> cs = bs;
    int n = 0;
    for (int b = 0; b < 256; ++b) {
        if (!bs.contains(b)) {
            bs.push_back(b);
            cs.push_back(256 + n);
            ++n;
        }
    }
    ByteMaps m;
    for (int i = 0; i < bs.size(); ++i) {
        const int b = bs.at(i);
        const QChar ch = QChar(ushort(cs.at(i)));
        m.toUnicode[b] = ch;
        m.toByte.insert(uint(ch.unicode()), uchar(b));
    }
    return m;
}

const ByteMaps& byteMaps() {
    static const ByteMaps m = makeByteMaps();
    return m;
}

QString mergeKey(const QString& a, const QString& b) {
    return a + QChar(0x1f) + b;
}

QString byteLevelEncode(const QString& piece) {
    const QByteArray utf8 = piece.toUtf8();
    const ByteMaps& m = byteMaps();
    QString out;
    out.reserve(utf8.size());
    for (char c : utf8)
        out.append(m.toUnicode[uchar(c)]);
    return out;
}

QVector<QString> bpe(const OfficialTokenizer& tok, const QString& token) {
    QVector<QString> word;
    word.reserve(token.size());
    for (int i = 0; i < token.size(); ++i)
        word.push_back(token.mid(i, 1));
    if (word.size() <= 1)
        return word;

    while (word.size() >= 2) {
        int bestRank = std::numeric_limits<int>::max();
        int bestI = -1;
        for (int i = 0; i + 1 < word.size(); ++i) {
            const auto it = tok.mergeRank.constFind(mergeKey(word[i], word[i + 1]));
            if (it != tok.mergeRank.constEnd() && it.value() < bestRank) {
                bestRank = it.value();
                bestI = i;
            }
        }
        if (bestI < 0)
            break;
        const QString a = word[bestI];
        const QString b = word[bestI + 1];
        QVector<QString> next;
        next.reserve(word.size() - 1);
        for (int i = 0; i < word.size();) {
            if (i + 1 < word.size() && word[i] == a && word[i + 1] == b) {
                next.push_back(a + b);
                i += 2;
            } else {
                next.push_back(word[i]);
                ++i;
            }
        }
        word.swap(next);
    }
    return word;
}

const QRegularExpression& splitRe() {
    static const QRegularExpression re(
        QString::fromUtf8(
            R"((?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+)"),
        QRegularExpression::UseUnicodePropertiesOption);
    return re;
}

struct Special {
    QString content;
    int id = 0;
};

QVector<Special> specialsByLen(const OfficialTokenizer& tok) {
    QVector<Special> s;
    s.reserve(tok.specials.size());
    for (auto it = tok.specials.constBegin(); it != tok.specials.constEnd(); ++it) {
        Special e;
        e.content = it.key();
        e.id = it.value();
        s.push_back(e);
    }
    std::sort(s.begin(), s.end(), [](const Special& a, const Special& b) {
        return a.content.size() > b.content.size();
    });
    return s;
}

void encodePlain(const OfficialTokenizer& tok, const QString& plain, QVector<int>* ids) {
    if (plain.isEmpty())
        return;
    const QString nfc = plain.normalized(QString::NormalizationForm_C);
    QRegularExpressionMatchIterator it = splitRe().globalMatch(nfc);
    while (it.hasNext()) {
        const QString piece = it.next().captured();
        if (piece.isEmpty())
            continue;
        const QString mapped = byteLevelEncode(piece);
        const QVector<QString> parts = bpe(tok, mapped);
        for (const QString& p : parts) {
            const auto vit = tok.vocab.constFind(p);
            if (vit != tok.vocab.constEnd())
                ids->push_back(vit.value());
            else if (tok.unkId >= 0)
                ids->push_back(tok.unkId);
        }
    }
}

}  // namespace

bool loadOfficialTokenizer(const QString& tokenizerJsonPath, OfficialTokenizer* out, QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));

    QFile f(tokenizerJsonPath);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open ") + tokenizerJsonPath);
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return fail(QStringLiteral("invalid tokenizer.json"));

    const QJsonObject root = doc.object();
    const QJsonObject model = root.value(QStringLiteral("model")).toObject();
    if (model.value(QStringLiteral("type")).toString() != QLatin1String("BPE"))
        return fail(QStringLiteral("tokenizer model is not BPE"));

    OfficialTokenizer tok;
    const QJsonObject vocab = model.value(QStringLiteral("vocab")).toObject();
    tok.vocab.reserve(vocab.size());
    tok.idToToken.reserve(vocab.size() + 32);
    for (auto it = vocab.begin(); it != vocab.end(); ++it) {
        const int id = it.value().toInt();
        tok.vocab.insert(it.key(), id);
        tok.idToToken.insert(id, it.key());
    }

    const QJsonArray merges = model.value(QStringLiteral("merges")).toArray();
    tok.mergeRank.reserve(merges.size());
    for (int i = 0; i < merges.size(); ++i) {
        const QJsonValue v = merges.at(i);
        QString a, b;
        if (v.isArray()) {
            const QJsonArray pair = v.toArray();
            if (pair.size() < 2)
                continue;
            a = pair.at(0).toString();
            b = pair.at(1).toString();
        } else {
            const QString s = v.toString();
            const int sp = s.indexOf(QLatin1Char(' '));
            if (sp <= 0)
                continue;
            a = s.left(sp);
            b = s.mid(sp + 1);
        }
        tok.mergeRank.insert(mergeKey(a, b), i);
    }

    const QJsonArray added = root.value(QStringLiteral("added_tokens")).toArray();
    for (const QJsonValue& v : added) {
        const QJsonObject o = v.toObject();
        const QString content = o.value(QStringLiteral("content")).toString();
        const int id = o.value(QStringLiteral("id")).toInt();
        if (content.isEmpty())
            continue;
        tok.idToToken.insert(id, content);
        if (o.value(QStringLiteral("special")).toBool(true))
            tok.specials.insert(content, id);
        else
            tok.vocab.insert(content, id);
    }

    if (tok.vocab.isEmpty() || tok.mergeRank.isEmpty())
        return fail(QStringLiteral("empty vocab or merges"));

    *out = tok;
    if (err)
        err->clear();
    return true;
}

QVector<int> encodeOfficial(const OfficialTokenizer& tok, const QString& text) {
    QVector<int> ids;
    if (text.isEmpty())
        return ids;

    const QVector<Special> specials = specialsByLen(tok);
    const int n = text.size();
    int i = 0;
    int plainStart = 0;
    while (i < n) {
        const Special* hit = nullptr;
        for (const Special& s : specials) {
            const int len = s.content.size();
            if (len > 0 && i + len <= n && text.midRef(i, len) == s.content) {
                hit = &s;
                break;  // already longest-first
            }
        }
        if (hit) {
            if (i > plainStart)
                encodePlain(tok, text.mid(plainStart, i - plainStart), &ids);
            ids.push_back(hit->id);
            i += hit->content.size();
            plainStart = i;
        } else {
            ++i;
        }
    }
    if (plainStart < n)
        encodePlain(tok, text.mid(plainStart), &ids);
    return ids;
}

QString decodeOfficial(const OfficialTokenizer& tok, const QVector<int>& ids, bool skipSpecials) {
    QString joined;
    for (int id : ids) {
        const auto sit = tok.idToToken.constFind(id);
        if (sit == tok.idToToken.constEnd())
            continue;
        bool isSpecial = false;
        for (auto it = tok.specials.constBegin(); it != tok.specials.constEnd(); ++it) {
            if (it.value() == id) {
                isSpecial = true;
                break;
            }
        }
        if (isSpecial && skipSpecials)
            continue;
        joined += sit.value();
    }
    const ByteMaps& m = byteMaps();
    QByteArray bytes;
    bytes.reserve(joined.size());
    for (int i = 0; i < joined.size(); ++i) {
        const auto it = m.toByte.constFind(uint(joined.at(i).unicode()));
        if (it != m.toByte.constEnd())
            bytes.append(char(it.value()));
    }
    return QString::fromUtf8(bytes);
}

}  // namespace hybrid
}  // namespace scanengine

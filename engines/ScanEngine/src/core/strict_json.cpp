#include "scanengine/strict_json.hpp"

#include <QJsonParseError>
#include <QSet>

#include <cstring>

namespace scanengine {
namespace {

class JsonPreflight final {
public:
    explicit JsonPreflight(const QByteArray& bytes)
        : bytes_(bytes) {}

    bool validate(QString* error) {
        skipWhitespace();
        if (!parseValue(0)) {
            if (error)
                *error = error_;
            return false;
        }
        skipWhitespace();
        if (position_ != bytes_.size()) {
            fail(QStringLiteral("unexpected trailing JSON data"));
            if (error)
                *error = error_;
            return false;
        }
        if (error)
            error->clear();
        return true;
    }

private:
    static constexpr int kMaximumDepth = 1024;

    bool fail(const QString& message, int offset = -1) {
        if (error_.isEmpty()) {
            error_ = QStringLiteral("%1 at byte %2")
                         .arg(message)
                         .arg(offset >= 0 ? offset : position_);
        }
        return false;
    }

    void skipWhitespace() {
        while (position_ < bytes_.size()) {
            const char c = bytes_.at(position_);
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                break;
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ >= bytes_.size() || bytes_.at(position_) != expected)
            return false;
        ++position_;
        return true;
    }

    static bool isDigit(char c) {
        return c >= '0' && c <= '9';
    }

    static int hexDigit(char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    static QString quotedKey(const QString& key) {
        QString escaped;
        const int limit = qMin(key.size(), 80);
        for (int index = 0; index < limit; ++index) {
            const ushort codeUnit = key.at(index).unicode();
            if (codeUnit == ushort('"')) {
                escaped += QStringLiteral("\\\"");
            } else if (codeUnit == ushort('\\')) {
                escaped += QStringLiteral("\\\\");
            } else if (codeUnit < 0x20) {
                escaped += QStringLiteral("\\u%1")
                               .arg(codeUnit, 4, 16, QLatin1Char('0'));
            } else {
                escaped += key.at(index);
            }
        }
        if (key.size() > limit)
            escaped += QStringLiteral("…");
        return QStringLiteral("\"%1\"").arg(escaped);
    }

    bool parseValue(int depth) {
        if (depth > kMaximumDepth)
            return fail(QStringLiteral("JSON nesting is too deep"));
        if (position_ >= bytes_.size())
            return fail(QStringLiteral("expected a JSON value"));

        const char c = bytes_.at(position_);
        if (c == '{')
            return parseObject(depth + 1);
        if (c == '[')
            return parseArray(depth + 1);
        if (c == '"')
            return parseString(nullptr);
        if (c == 't')
            return parseLiteral("true", 4);
        if (c == 'f')
            return parseLiteral("false", 5);
        if (c == 'n')
            return parseLiteral("null", 4);
        if (c == '-' || isDigit(c))
            return parseNumber();
        return fail(QStringLiteral("unexpected token while reading a JSON value"));
    }

    bool parseObject(int depth) {
        if (depth > kMaximumDepth)
            return fail(QStringLiteral("JSON nesting is too deep"));
        consume('{');
        skipWhitespace();
        if (consume('}'))
            return true;

        QSet<QString> keys;
        while (true) {
            if (position_ >= bytes_.size() || bytes_.at(position_) != '"')
                return fail(QStringLiteral("expected an object member name"));
            const int keyOffset = position_;
            QString key;
            if (!parseString(&key))
                return false;
            if (keys.contains(key)) {
                return fail(QStringLiteral("duplicate object key %1").arg(quotedKey(key)),
                            keyOffset);
            }
            keys.insert(key);

            skipWhitespace();
            if (!consume(':'))
                return fail(QStringLiteral("expected ':' after an object member name"));
            skipWhitespace();
            if (!parseValue(depth))
                return false;
            skipWhitespace();
            if (consume('}'))
                return true;
            if (!consume(','))
                return fail(QStringLiteral("expected ',' or '}' in an object"));
            skipWhitespace();
        }
    }

    bool parseArray(int depth) {
        if (depth > kMaximumDepth)
            return fail(QStringLiteral("JSON nesting is too deep"));
        consume('[');
        skipWhitespace();
        if (consume(']'))
            return true;

        while (true) {
            if (!parseValue(depth))
                return false;
            skipWhitespace();
            if (consume(']'))
                return true;
            if (!consume(','))
                return fail(QStringLiteral("expected ',' or ']' in an array"));
            skipWhitespace();
        }
    }

    bool parseString(QString* decoded) {
        if (!consume('"'))
            return fail(QStringLiteral("expected a JSON string"));
        while (position_ < bytes_.size()) {
            const unsigned char c = static_cast<unsigned char>(bytes_.at(position_));
            if (c == '"') {
                ++position_;
                return true;
            }
            if (c == '\\') {
                const int escapeOffset = position_++;
                if (position_ >= bytes_.size())
                    return fail(QStringLiteral("unterminated JSON escape"), escapeOffset);
                const char escaped = bytes_.at(position_++);
                switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    if (decoded)
                        decoded->append(QLatin1Char(escaped));
                    break;
                case 'b':
                    if (decoded)
                        decoded->append(QChar(0x0008));
                    break;
                case 'f':
                    if (decoded)
                        decoded->append(QChar(0x000c));
                    break;
                case 'n':
                    if (decoded)
                        decoded->append(QChar(0x000a));
                    break;
                case 'r':
                    if (decoded)
                        decoded->append(QChar(0x000d));
                    break;
                case 't':
                    if (decoded)
                        decoded->append(QChar(0x0009));
                    break;
                case 'u':
                    if (!parseUnicodeEscape(decoded))
                        return false;
                    break;
                default:
                    return fail(QStringLiteral("invalid JSON escape"), escapeOffset);
                }
                continue;
            }
            if (c < 0x20)
                return fail(QStringLiteral("unescaped control character in JSON string"));
            if (c < 0x80) {
                ++position_;
                if (decoded)
                    decoded->append(QChar(c));
                continue;
            }
            if (!parseUtf8Character(decoded))
                return false;
        }
        return fail(QStringLiteral("unterminated JSON string"));
    }

    bool parseUnicodeEscape(QString* decoded) {
        const int escapeOffset = position_ - 2;
        if (bytes_.size() - position_ < 4)
            return fail(QStringLiteral("truncated JSON unicode escape"), escapeOffset);
        uint value = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = hexDigit(bytes_.at(position_++));
            if (digit < 0)
                return fail(QStringLiteral("invalid JSON unicode escape"), position_ - 1);
            value = (value << 4) | uint(digit);
        }
        if (decoded)
            decoded->append(QChar(ushort(value)));
        return true;
    }

    bool parseUtf8Character(QString* decoded) {
        const int start = position_;
        const unsigned char first = static_cast<unsigned char>(bytes_.at(position_));
        int length = 0;
        uint codePoint = 0;
        uint minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
            codePoint = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
            codePoint = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
            codePoint = first & 0x07;
            minimum = 0x10000;
        } else {
            return fail(QStringLiteral("invalid UTF-8 in JSON string"), start);
        }
        if (bytes_.size() - position_ < length)
            return fail(QStringLiteral("truncated UTF-8 in JSON string"), start);
        for (int index = 1; index < length; ++index) {
            const unsigned char continuation =
                static_cast<unsigned char>(bytes_.at(position_ + index));
            if ((continuation & 0xc0) != 0x80)
                return fail(QStringLiteral("invalid UTF-8 in JSON string"), start);
            codePoint = (codePoint << 6) | uint(continuation & 0x3f);
        }
        if (codePoint < minimum || codePoint > 0x10ffff
            || (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
            return fail(QStringLiteral("invalid UTF-8 code point in JSON string"), start);
        }
        position_ += length;
        if (decoded) {
            if (codePoint <= 0xffff) {
                decoded->append(QChar(ushort(codePoint)));
            } else {
                const uint surrogate = codePoint - 0x10000;
                decoded->append(QChar(ushort(0xd800 + (surrogate >> 10))));
                decoded->append(QChar(ushort(0xdc00 + (surrogate & 0x3ff))));
            }
        }
        return true;
    }

    bool parseLiteral(const char* literal, int length) {
        if (bytes_.size() - position_ < length
            || std::memcmp(bytes_.constData() + position_, literal, size_t(length)) != 0) {
            return fail(QStringLiteral("invalid JSON literal"));
        }
        position_ += length;
        return true;
    }

    bool parseNumber() {
        const int start = position_;
        if (consume('-') && position_ >= bytes_.size())
            return fail(QStringLiteral("incomplete JSON number"), start);

        if (consume('0')) {
            if (position_ < bytes_.size() && isDigit(bytes_.at(position_)))
                return fail(QStringLiteral("leading zero in JSON number"), start);
        } else {
            if (position_ >= bytes_.size() || bytes_.at(position_) < '1'
                || bytes_.at(position_) > '9') {
                return fail(QStringLiteral("invalid JSON number"), start);
            }
            while (position_ < bytes_.size() && isDigit(bytes_.at(position_)))
                ++position_;
        }

        if (consume('.')) {
            if (position_ >= bytes_.size() || !isDigit(bytes_.at(position_)))
                return fail(QStringLiteral("missing fraction digits in JSON number"), start);
            while (position_ < bytes_.size() && isDigit(bytes_.at(position_)))
                ++position_;
        }

        if (position_ < bytes_.size()
            && (bytes_.at(position_) == 'e' || bytes_.at(position_) == 'E')) {
            ++position_;
            if (position_ < bytes_.size()
                && (bytes_.at(position_) == '+' || bytes_.at(position_) == '-')) {
                ++position_;
            }
            if (position_ >= bytes_.size() || !isDigit(bytes_.at(position_)))
                return fail(QStringLiteral("missing exponent digits in JSON number"), start);
            while (position_ < bytes_.size() && isDigit(bytes_.at(position_)))
                ++position_;
        }
        return true;
    }

    const QByteArray& bytes_;
    int position_ = 0;
    QString error_;
};

}  // namespace

bool parseJsonDocumentStrict(const QByteArray& bytes,
                             QJsonDocument* document,
                             QString* error) {
    if (!document) {
        if (error)
            *error = QStringLiteral("JSON output document is null");
        return false;
    }
    *document = QJsonDocument();

    QString preflightError;
    JsonPreflight preflight(bytes);
    if (!preflight.validate(&preflightError)) {
        if (error)
            *error = preflightError;
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) {
            *error = QStringLiteral("invalid JSON at byte %1: %2")
                         .arg(parseError.offset)
                         .arg(parseError.errorString());
        }
        return false;
    }

    *document = parsed;
    if (error)
        error->clear();
    return true;
}

}  // namespace scanengine

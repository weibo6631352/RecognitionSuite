#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QString>

namespace scanengine {

// Parses a JSON document only after a full grammar preflight has verified that
// every object has unique, JSON-decoded member names.  QJsonDocument otherwise
// silently keeps one value when an input object repeats a key.
bool parseJsonDocumentStrict(const QByteArray& bytes,
                             QJsonDocument* document,
                             QString* error = nullptr);

}  // namespace scanengine

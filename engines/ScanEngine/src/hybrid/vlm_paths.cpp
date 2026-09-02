#include "scanengine/hybrid/vlm_paths.hpp"

#include "scanengine/config.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace scanengine {
namespace hybrid {

VlmPaths resolveOfficialVlmPaths() {
    VlmPaths p;
    const QString jsonPath = modelManifestPath();
    QString root;
    QFile f(jsonPath);
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        const QJsonObject models = doc.object().value(QStringLiteral("models-dir")).toObject();
        root = resolveRepoPath(models.value(QStringLiteral("vlm")).toString());
    }
    if (root.isEmpty())
        root = QDir(defaultModelsDir()).filePath(QStringLiteral("vlm"));
    p.snapshotDir = root;
    p.configJson = QDir(root).filePath(QStringLiteral("config.json"));
    p.weights = QDir(root).filePath(QStringLiteral("model.safetensors"));
    p.tokenizerJson = QDir(root).filePath(QStringLiteral("tokenizer.json"));
    p.preprocessorJson = QDir(root).filePath(QStringLiteral("preprocessor_config.json"));
    p.chatTemplate = QDir(root).filePath(QStringLiteral("chat_template.jinja"));
    return p;
}

QString vlmPathsMissing(const VlmPaths& p) {
    QStringList miss;
    const QString* files[] = {&p.configJson, &p.weights, &p.tokenizerJson, &p.preprocessorJson};
    for (const QString* f : files) {
        const bool exists = QFileInfo::exists(*f)
                            || (f == &p.weights && QFileInfo::exists(*f + QStringLiteral(".part1"))
                                && QFileInfo::exists(*f + QStringLiteral(".part2")));
        if (!exists)
            miss << *f;
    }
    return miss.join(QStringLiteral(", "));
}

}  // namespace hybrid
}  // namespace scanengine

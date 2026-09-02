#include "scanengine/config.hpp"

#include "scanengine/ui_options.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QMutex>
#include <QMutexLocker>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace scanengine {

namespace {
QMutex gRuntimeRootMutex;
QString gRuntimeRoot;
}

void setRuntimeRoot(const QString& root) {
    QMutexLocker lock(&gRuntimeRootMutex);
    gRuntimeRoot = root.isEmpty() ? QString() : QDir(root).absolutePath();
}

QString repoRoot() {
    {
        QMutexLocker lock(&gRuntimeRootMutex);
        if (!gRuntimeRoot.isEmpty())
            return gRuntimeRoot;
    }
    const QString dir = QCoreApplication::applicationDirPath();
    if (dir.isEmpty())
        return QDir::currentPath();
    return QDir(dir).absolutePath();
}

QString resolveRepoPath(const QString& path) {
    if (path.isEmpty())
        return QString();
    const QFileInfo fi(path);
    if (fi.isAbsolute())
        return QDir::cleanPath(path);
    return QDir(repoRoot()).absoluteFilePath(path);
}

QString defaultOutputDir() {
    return QDir(repoRoot()).filePath(QStringLiteral("output"));
}

QString defaultModelsDir() {
    return QDir(repoRoot()).filePath(QStringLiteral("models"));
}

QString configPath() {
    return QDir(repoRoot()).filePath(QStringLiteral("config.local.json"));
}

QString modelManifestPath() {
    return QDir(repoRoot()).filePath(QStringLiteral("scanengine.json"));
}

void ensureRuntimeDirs() {
    QDir().mkpath(defaultOutputDir());
    QDir().mkpath(defaultModelsDir());
}

QJsonObject defaultConfig() {
    QJsonObject o;
    o.insert(QStringLiteral("output_dir"), defaultOutputDir());
    o.insert(QStringLiteral("window_geometry"), QJsonValue());
    return o;
}

QJsonObject loadConfig() {
    QJsonObject defaults = defaultConfig();
    QFile f(configPath());
    if (!f.open(QIODevice::ReadOnly))
        return defaults;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return defaults;
    const QJsonObject data = doc.object();
    for (const QString& key : {QStringLiteral("output_dir"),
                               QStringLiteral("window_geometry")}) {
        if (data.contains(key))
            defaults.insert(key, data.value(key));
    }
    return defaults;
}

void saveConfig(const QJsonObject& patch) {
    QJsonObject merged = loadConfig();
    for (auto it = patch.begin(); it != patch.end(); ++it)
        merged.insert(it.key(), it.value());
    QFile f(configPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    f.write(QJsonDocument(merged).toJson(QJsonDocument::Indented));
}

void openLocalPath(const QString& path) {
#ifdef Q_OS_MACOS
    QProcess::startDetached(QStringLiteral("open"), QStringList() << path);
#elif defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer"), QStringList() << QDir::toNativeSeparators(path));
#else
    QProcess::startDetached(QStringLiteral("xdg-open"), QStringList() << path);
#endif
}

}  // namespace scanengine

#pragma once

#include <QJsonObject>
#include <QString>

namespace scanengine {

QString repoRoot();
void setRuntimeRoot(const QString& root);
QString resolveRepoPath(const QString& path);
QString defaultOutputDir();
QString defaultModelsDir();
QString configPath();
QString modelManifestPath();

void ensureRuntimeDirs();

QJsonObject defaultConfig();
QJsonObject loadConfig();
void saveConfig(const QJsonObject& patch);

void openLocalPath(const QString& path);

}  // namespace scanengine

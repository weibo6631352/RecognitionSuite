#include "scanengine/ui_options.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace scanengine {
namespace {

const QStringList kImageSuffixes = {
    QStringLiteral(".png"), QStringLiteral(".jpg"), QStringLiteral(".jpeg"),
    QStringLiteral(".bmp"), QStringLiteral(".tif"), QStringLiteral(".tiff"),
    QStringLiteral(".webp")
};

}  // namespace

bool isImagePath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return imageSuffixes().contains(QLatin1Char('.') + suffix);
}

bool isSupportedPath(const QString& path) {
    return isImagePath(path);
}

const QStringList& imageSuffixes() {
    return kImageSuffixes;
}

QStringList expandInputPaths(const QStringList& paths) {
    QStringList files;
    QSet<QString> seen;
    for (const QString& raw : paths) {
        const QFileInfo info(raw);
        const QString resolved = info.canonicalFilePath().isEmpty()
            ? info.absoluteFilePath()
            : info.canonicalFilePath();
        if (seen.contains(resolved))
            continue;
        if (info.isDir()) {
            QStringList pendingDirs{resolved};
            QStringList collected;
            while (!pendingDirs.isEmpty()) {
                QDir dir(pendingDirs.takeFirst());
                const QFileInfoList entries = dir.entryInfoList(
                    QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
                for (const QFileInfo& entry : entries) {
                    if (entry.isDir())
                        pendingDirs.append(entry.absoluteFilePath());
                    else if (entry.isFile() && isSupportedPath(entry.absoluteFilePath()))
                        collected.append(entry.absoluteFilePath());
                }
            }
            collected.sort();
            for (const QString& child : collected) {
                const QString canonical = QFileInfo(child).canonicalFilePath();
                const QString key = canonical.isEmpty() ? child : canonical;
                if (!seen.contains(key)) {
                    seen.insert(key);
                    files.append(child);
                }
            }
        } else if (info.isFile() && isSupportedPath(resolved)) {
            seen.insert(resolved);
            files.append(resolved);
        }
    }
    return files;
}

}  // namespace scanengine

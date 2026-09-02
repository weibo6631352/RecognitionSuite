#include "scanengine/preprocess.hpp"

#include "scanengine/ui_options.hpp"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QTransform>

namespace scanengine {

bool isImageFile(const QString& path) {
    return QFileInfo::exists(path) && QFileInfo(path).isFile() && isImagePath(path);
}

QString rotateImageClockwise(const QString& src, const QString& dest, int clockwiseDegrees) {
    QDir().mkpath(QFileInfo(dest).absolutePath());
    QImage img(src);
    if (img.isNull())
        return dest;
    // Qt QTransform::rotate is clockwise with the default y-down system.
    const QImage rotated = img.transformed(QTransform().rotate(clockwiseDegrees));
    rotated.save(dest);
    return dest;
}

std::vector<std::pair<int, QString>> candidateRotations(const QString& src, const QString& workDir) {
    std::vector<std::pair<int, QString>> result;
    result.emplace_back(0, src);
    if (!isImageFile(src))
        return result;
    QDir().mkpath(workDir);
    const QFileInfo info(src);
    const int angles[] = {90, 180, 270};
    for (int angle : angles) {
        const QString dest = QDir(workDir).filePath(
            info.completeBaseName() + QStringLiteral("_rot") + QString::number(angle)
            + QLatin1Char('.') + info.suffix().toLower());
        rotateImageClockwise(src, dest, angle);
        result.emplace_back(angle, dest);
    }
    return result;
}

}  // namespace scanengine

#pragma once

#include <QString>
#include <utility>
#include <vector>

namespace scanengine {

bool isImageFile(const QString& path);
QString rotateImageClockwise(const QString& src, const QString& dest, int clockwiseDegrees);
std::vector<std::pair<int, QString>> candidateRotations(const QString& src, const QString& workDir);

}  // namespace scanengine

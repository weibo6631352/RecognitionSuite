#pragma once

#include <QString>
#include <QStringList>

namespace scanengine {

bool isImagePath(const QString& path);
bool isSupportedPath(const QString& path);
const QStringList& imageSuffixes();
QStringList expandInputPaths(const QStringList& paths);

inline constexpr auto kHeaderTitle = "ScanEngine";
inline constexpr auto kHeaderSubtitle = "本地图片结构化与表格提取";
inline constexpr auto kStatusIdleTitle = "等待任务";
inline constexpr auto kStatusIdleHint = "选择、拖放或从剪贴板导入图片。";

}  // namespace scanengine

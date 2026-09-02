#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <optional>

namespace scanengine {

enum class TaskStatus {
    Idle,
    Prepare,
    Process,
    Outputs,
    Done,
    Failed,
    Cancelled,
};

QString taskStatusValue(TaskStatus status);
TaskStatus taskStatusFromValue(const QString& value);

struct ParseOptions {
    QString inputPath;
    QString outputDir;
    QString effort = QStringLiteral("medium");
    QString device = QStringLiteral("gpu-required");
    std::optional<QString> excelPath;
    std::optional<QString> jobDir;
};

struct ParseResult {
    bool success = false;
    TaskStatus status = TaskStatus::Idle;
    QString message;
    QString outputDir;
    QString markdownPath;
    QString jsonPath;
    QString excelPath;
    QString logTail;
    QStringList artifacts;
    int fallbackCount = 0;
};

}  // namespace scanengine

Q_DECLARE_METATYPE(scanengine::ParseResult)
Q_DECLARE_METATYPE(scanengine::ParseOptions)

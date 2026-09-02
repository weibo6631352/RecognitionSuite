#include "scanengine/models.hpp"

namespace scanengine {

QString taskStatusValue(TaskStatus status) {
    switch (status) {
    case TaskStatus::Idle:
        return QStringLiteral("idle");
    case TaskStatus::Prepare:
        return QStringLiteral("prepare");
    case TaskStatus::Process:
        return QStringLiteral("process");
    case TaskStatus::Outputs:
        return QStringLiteral("outputs");
    case TaskStatus::Done:
        return QStringLiteral("done");
    case TaskStatus::Failed:
        return QStringLiteral("failed");
    case TaskStatus::Cancelled:
        return QStringLiteral("cancelled");
    }
    return QStringLiteral("idle");
}

TaskStatus taskStatusFromValue(const QString& value) {
    if (value == QLatin1String("prepare"))
        return TaskStatus::Prepare;
    if (value == QLatin1String("process"))
        return TaskStatus::Process;
    if (value == QLatin1String("outputs"))
        return TaskStatus::Outputs;
    if (value == QLatin1String("done"))
        return TaskStatus::Done;
    if (value == QLatin1String("failed"))
        return TaskStatus::Failed;
    if (value == QLatin1String("cancelled"))
        return TaskStatus::Cancelled;
    return TaskStatus::Idle;
}

}  // namespace scanengine

#include "scanengine/output_layout.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace scanengine {

QString safeFilename(QString name) {
    static const QRegularExpression illegal(QStringLiteral("[<>:\"/\\\\|?*\\n\\r\\t]"));
    name.replace(illegal, QStringLiteral("_"));
    while (name.startsWith(QLatin1Char(' ')) || name.startsWith(QLatin1Char('.'))
           || name.startsWith(QLatin1Char('_'))) {
        name.remove(0, 1);
    }
    while (name.endsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char('.'))
           || name.endsWith(QLatin1Char('_'))) {
        name.chop(1);
    }
    if (name.isEmpty())
        name = QStringLiteral("doc");
    if (name.size() > 120)
        name = name.left(120);
    return name;
}

JobPaths allocateJobPaths(const QString& outputRoot,
                          const QString& sourcePath,
                          QDateTime now) {
    JobPaths job;
    if (!now.isValid())
        now = QDateTime::currentDateTime();

    if (outputRoot.isEmpty()) {
        job.error = QString::fromUtf8("输出根目录为空");
        return job;
    }
    if (!QDir().mkpath(outputRoot)) {
        job.error = QString::fromUtf8("无法创建输出根目录: %1").arg(outputRoot);
        return job;
    }

    const QString stamp = now.toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QFileInfo src(sourcePath);
    if (!src.exists() || !src.isFile()) {
        job.error = QString::fromUtf8("源文件不存在或不是普通文件: %1").arg(sourcePath);
        return job;
    }
    const QString originalName = safeFilename(src.fileName());
    const QString stem = safeFilename(src.completeBaseName());
    const QString base = stamp + QLatin1Char('_') + originalName;
    QString jobId = base;
    int seq = 2;
    while (QDir(outputRoot + QLatin1Char('/') + jobId).exists()) {
        jobId = base + QLatin1Char('_') + QString::number(seq);
        ++seq;
    }

    job.jobId = jobId;
    job.root = outputRoot;
    job.workDir = QDir(outputRoot).filePath(jobId);
    job.intermediateDir = QDir(job.workDir).filePath(QStringLiteral("work"));
    job.sourceCopy = QDir(job.workDir).filePath(originalName);
    job.excelPath = QDir(job.workDir).filePath(stem + QStringLiteral(".xlsx"));

    if (!QDir().mkpath(job.workDir) || !QDir().mkpath(job.intermediateDir)) {
        job.error = QString::fromUtf8("无法创建作业目录: %1").arg(job.intermediateDir);
        return job;
    }
    if (!QFile::copy(src.absoluteFilePath(), job.sourceCopy)) {
        job.error = QString::fromUtf8("无法复制源文件: %1 -> %2")
                        .arg(src.absoluteFilePath(), job.sourceCopy);
        return job;
    }
    job.success = true;
    return job;
}

}  // namespace scanengine

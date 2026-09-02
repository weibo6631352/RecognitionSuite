#pragma once

#include <QDateTime>
#include <QString>

namespace scanengine {

QString safeFilename(QString name);

struct JobPaths {
    bool success = false;
    QString error;
    QString jobId;
    QString root;
    QString workDir;           // 作业根：时间戳_源文件名
    QString intermediateDir;   // work/
    QString sourceCopy;
    QString excelPath;
};

JobPaths allocateJobPaths(const QString& outputRoot,
                          const QString& sourcePath,
                          QDateTime now = QDateTime());

}  // namespace scanengine

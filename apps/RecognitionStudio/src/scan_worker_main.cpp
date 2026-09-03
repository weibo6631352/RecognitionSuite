#include <scanengine/scanengine.hpp>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QThread>

#include <iostream>

namespace {

const auto kProtocolSchema = "RecognitionStudio-ScanWorker/1";

std::wstring toWidePath(const QString& path) {
    return QDir::toNativeSeparators(path).toStdWString();
}

QString fromWidePath(const std::wstring& path) {
    return QString::fromWCharArray(path.c_str(),
                                   static_cast<int>(path.size()));
}

void emitEvent(const QJsonObject& event) {
    QJsonObject envelope = event;
    envelope.insert(QStringLiteral("schema"),
                    QString::fromLatin1(kProtocolSchema));
    const QByteArray line =
        QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    std::cout.write(line.constData(), line.size());
    std::cout << '\n' << std::flush;
}

int fail(const QString& message, int exitCode = 1) {
    QJsonObject event;
    event.insert(QStringLiteral("event"), QStringLiteral("error"));
    event.insert(QStringLiteral("message"), message);
    emitEvent(event);
    return exitCode;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral(
        "RecognitionStudioScanWorker"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "RecognitionStudio isolated ScanEngine worker"));
    parser.addHelpOption();
    const QCommandLineOption runtimeOption(
        QStringLiteral("runtime"), QStringLiteral("ScanEngine runtime root"),
        QStringLiteral("path"));
    const QCommandLineOption inputOption(
        QStringLiteral("input"), QStringLiteral("Input image path"),
        QStringLiteral("path"));
    const QCommandLineOption outputOption(
        QStringLiteral("output"), QStringLiteral("Output directory"),
        QStringLiteral("path"));
    const QCommandLineOption checkOption(
        QStringLiteral("check"),
        QStringLiteral("Check the isolated ScanEngine runtime only"));
    parser.addOptions(
        {runtimeOption, inputOption, outputOption, checkOption});
    parser.process(application);

    const QString runtime = parser.value(runtimeOption).trimmed();
    const QString input = parser.value(inputOption).trimmed();
    const QString output = parser.value(outputOption).trimmed();
    if (runtime.isEmpty())
        return fail(QStringLiteral("--runtime 为必填参数"), 2);

    try {
        auto context = scanengine::Context::open(toWidePath(runtime));
        const auto runtimeInfo = context.runtime_info();
        if (parser.isSet(checkOption)) {
            QJsonObject event;
            event.insert(QStringLiteral("event"), QStringLiteral("check"));
            event.insert(QStringLiteral("gpu_available"),
                         runtimeInfo.gpu_available);
            event.insert(QStringLiteral("backend"),
                         QString::fromUtf8(runtimeInfo.backend.c_str()));
            emitEvent(event);
            return runtimeInfo.gpu_available ? 0 : 1;
        }
        if (!runtimeInfo.gpu_available)
            return fail(QStringLiteral("ScanEngine GPU 后端不可用"));
        if (input.isEmpty() || output.isEmpty())
            return fail(QStringLiteral(
                "--input 和 --output 均为必填参数"), 2);
        auto task = context.submit_file(
            toWidePath(input), toWidePath(output), SE_EFFORT_MEDIUM);
        int lastProgress = -1;
        QString lastMessage;
        for (;;) {
            const auto status = task.poll();
            const int progress = qBound(
                0, static_cast<int>(status.progress * 100.0f), 100);
            const QString message = QString::fromUtf8(status.message.c_str());
            if (progress != lastProgress || message != lastMessage) {
                QJsonObject event;
                event.insert(QStringLiteral("event"), QStringLiteral("progress"));
                event.insert(QStringLiteral("progress"), progress);
                event.insert(QStringLiteral("message"), message);
                emitEvent(event);
                lastProgress = progress;
                lastMessage = message;
            }
            if (!status.finished()) {
                QThread::msleep(80);
                continue;
            }
            if (status.state != SE_TASK_DONE) {
                return fail(
                    message.isEmpty()
                        ? QStringLiteral("扫描任务失败（状态 %1，错误码 %2）")
                              .arg(static_cast<int>(status.state))
                              .arg(static_cast<int>(status.error))
                        : message,
                    status.state == SE_TASK_CANCELLED ? 3 : 1);
            }

            const auto result = task.result();
            QJsonObject event;
            event.insert(QStringLiteral("event"), QStringLiteral("result"));
            event.insert(QStringLiteral("task_id"),
                         static_cast<double>(result.task_id));
            event.insert(QStringLiteral("message"),
                         QString::fromUtf8(result.message.c_str()));
            event.insert(QStringLiteral("output_dir"),
                         fromWidePath(result.output_dir));
            event.insert(QStringLiteral("excel_path"),
                         fromWidePath(result.excel_path));
            event.insert(QStringLiteral("markdown_path"),
                         fromWidePath(result.markdown_path));
            event.insert(QStringLiteral("json_path"),
                         fromWidePath(result.json_path));
            event.insert(QStringLiteral("fallback_count"),
                         result.fallback_count);
            emitEvent(event);
            return 0;
        }
    } catch (const std::exception& error) {
        return fail(QString::fromUtf8(error.what()));
    }
}

#include "parse_controller.h"

#include "scanengine/parser_service.hpp"

#include <QMetaType>
#include <QThread>
#include <stdexcept>

namespace scanengine {

class ParseControllerWorker : public QObject {
    Q_OBJECT
public:
    ParseControllerWorker(ParserService* service, ParseOptions options, QObject* parent = nullptr)
        : QObject(parent)
        , service_(service)
        , options_(std::move(options)) {}

public slots:
    void run() {
        const ParseResult result = service_->run(
            options_,
            [this](const QString& line) { emit logLine(line); },
            [this](TaskStatus status, const QString& message) {
                emit statusChanged(taskStatusValue(status), message);
            });
        emit finished(result);
    }

signals:
    void logLine(const QString& line);
    void statusChanged(const QString& status, const QString& message);
    void finished(const ParseResult& result);

private:
    ParserService* service_ = nullptr;
    ParseOptions options_;
};

ParseController::ParseController(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<scanengine::ParseResult>("scanengine::ParseResult");
}

ParseController::~ParseController() {
    cancel();
    if (thread_) {
        disconnect(thread_, nullptr, this, nullptr);
        thread_->quit();
        // ParserService cancellation is currently guaranteed at stage
        // boundaries.  Keep the QThread alive until the active synchronous
        // model call returns; destroying a running QThread is process-fatal.
        thread_->wait();
    }
}

bool ParseController::isRunning() const {
    return thread_ != nullptr && thread_->isRunning();
}

void ParseController::start(const ParseOptions& options) {
    if (isRunning()) {
        ParseResult r;
        r.success = false;
        r.status = TaskStatus::Failed;
        r.message = QString::fromUtf8("已有解析任务在运行");
        emit finished(r);
        throw std::runtime_error("已有解析任务在运行");
    }

    thread_ = new QThread(this);
    service_ = new ParserService();
    worker_ = new ParseControllerWorker(service_, options);
    service_->moveToThread(thread_);
    worker_->moveToThread(thread_);

    connect(worker_, &ParseControllerWorker::logLine,
            this, &ParseController::logLine, Qt::QueuedConnection);
    connect(worker_, &ParseControllerWorker::statusChanged,
            this, &ParseController::statusChanged, Qt::QueuedConnection);
    connect(worker_, &ParseControllerWorker::finished,
            this, &ParseController::onWorkerFinished, Qt::QueuedConnection);
    connect(thread_, &QThread::started,
            worker_, &ParseControllerWorker::run, Qt::QueuedConnection);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(thread_, &QThread::finished, service_, &QObject::deleteLater);
    connect(thread_, &QThread::finished,
            this, &ParseController::onThreadFinished, Qt::QueuedConnection);

    thread_->start();
}

void ParseController::cancel() {
    if (service_)
        service_->cancel();
}

void ParseController::onWorkerFinished(const ParseResult& result) {
    emit finished(result);
    if (thread_)
        thread_->quit();
}

void ParseController::onThreadFinished() {
    if (thread_)
        thread_->deleteLater();
    thread_ = nullptr;
    service_ = nullptr;
    worker_ = nullptr;
}

}  // namespace scanengine

#include "parse_controller.moc"

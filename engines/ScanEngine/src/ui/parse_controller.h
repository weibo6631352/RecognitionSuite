#pragma once

#include "scanengine/models.hpp"

#include <QObject>

class QThread;

namespace scanengine {

class ParserService;
class ParseControllerWorker;

// Runs ParserService on a worker thread and forwards progress to the UI.
class ParseController : public QObject {
    Q_OBJECT
public:
    explicit ParseController(QObject* parent = nullptr);
    ~ParseController() override;

    bool isRunning() const;
    void start(const ParseOptions& options);
    void cancel();

signals:
    void logLine(const QString& line);
    void statusChanged(const QString& status, const QString& message);
    void finished(const ParseResult& result);

private slots:
    void onWorkerFinished(const ParseResult& result);
    void onThreadFinished();

private:
    QThread* thread_ = nullptr;
    ParserService* service_ = nullptr;
    ParseControllerWorker* worker_ = nullptr;
};

}  // namespace scanengine

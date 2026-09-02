#pragma once

#include "scanengine/models.hpp"

#include <QObject>
#include <atomic>
#include <functional>
#include <mutex>

namespace scanengine {

class ParserService : public QObject {
    Q_OBJECT
public:
    using LogCallback = std::function<void(const QString&)>;
    using StatusCallback = std::function<void(TaskStatus, const QString&)>;

    static constexpr int kDefaultMinChars = 80;

    explicit ParserService(QObject* parent = nullptr);

    bool isRunning() const;
    void cancel();

    ParseResult run(ParseOptions options,
                    LogCallback onLog = {},
                    StatusCallback onStatus = {});

    void setMinChars(int n) { minChars_ = n; }

private:
    ParseResult runOnce(ParseOptions options, LogCallback onLog, StatusCallback onStatus);
    ParseResult attachExcel(ParseResult result, const ParseOptions& options, LogCallback onLog);
    static int mdChars(const QString& path);

    mutable std::mutex stateMutex_;
    std::atomic_bool running_{false};
    std::atomic_bool cancelled_{false};
    int minChars_ = kDefaultMinChars;
};

}  // namespace scanengine

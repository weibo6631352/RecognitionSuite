#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <thread>

namespace speechdoc {

class MicCapture final : public QObject {
    Q_OBJECT
public:
    explicit MicCapture(QObject* parent = nullptr);
    ~MicCapture() override;

    bool start();
    void stop();
    bool running() const noexcept { return running_.load(); }

signals:
    void pcmReady(const QByteArray& pcm16le, int sampleRate);
    void error(const QString& message);

private:
    void captureLoop();

    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace speechdoc

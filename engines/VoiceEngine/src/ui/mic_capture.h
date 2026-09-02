#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <thread>

namespace voiceengine {

class MicCapture : public QObject {
    Q_OBJECT
public:
    explicit MicCapture(QObject* parent = nullptr);
    ~MicCapture() override;
    bool start();
    void stop();
    bool running() const { return m_running.load(); }

signals:
    void pcmReady(const QByteArray& pcm16le, int sampleRate);
    void error(const QString& message);

private:
    void loop();

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    int m_sr = 16000;
};

}  // namespace voiceengine

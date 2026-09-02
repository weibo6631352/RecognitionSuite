#pragma once

#include "voiceengine/voiceengine_api.h"

#include <QList>
#include <QMainWindow>
#include <QStringList>
#include <cstdint>

class QCloseEvent;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTimer;

namespace voiceengine {

class DropZone;
class MicCapture;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void pickFiles();
    void onDropped(const QStringList& paths);
    void enqueue(const QStringList& paths);
    void loadModel();
    void startNext();
    void cancelCurrent();
    void poll();
    void copyText();
    void saveText();
    void toggleMic();
    void onMicPcm(const QByteArray& pcm16, int sampleRate);
    void onModelLoadFinished(int status, const QString& err, const QString& device);

private:
    void setStatus(const QString& s);
    void showError(const QString& s);
    void watchTask(uint64_t id, const QString& status);
    void finishActive();
    void stopMicStream(bool cancel);
    void pollMicStream();
    QString modelDir() const;

    ve_context* m_ctx = nullptr;
    DropZone* m_drop = nullptr;
    QListWidget* m_queue = nullptr;
    QLabel* m_modelStatus = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QPlainTextEdit* m_interim = nullptr;
    QPlainTextEdit* m_final = nullptr;
    QPushButton* m_load = nullptr;
    QPushButton* m_start = nullptr;
    QPushButton* m_cancel = nullptr;
    QPushButton* m_copy = nullptr;
    QPushButton* m_save = nullptr;
    QPushButton* m_mic = nullptr;
    QTimer* m_timer = nullptr;
    MicCapture* m_micCap = nullptr;
    ve_stream* m_stream = nullptr;
    QStringList m_pending;
    QList<quint64> m_watch;
    uint64_t m_active = 0;
    bool m_busy = false;
    bool m_loading = false;
};

}  // namespace voiceengine

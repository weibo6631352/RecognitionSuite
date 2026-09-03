#pragma once

#include "acceptance_validation.h"
#include "mic_capture.h"

#include <voiceengine/voiceengine.hpp>

#include <QMainWindow>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>

#include <atomic>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QProcess;
class QPushButton;
class QTableWidget;
class QTimer;

namespace speechdoc {

class AcceptanceEvidencePanel;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool sdksReady() const noexcept { return speechReady_ && documentReady_; }
    bool publishedRuntimeMatches(QString* error = nullptr) const;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void initializeSdks();
    void updateControls();
    bool operationActive() const;
    void pollSdkTasks();

    void chooseAudio();
    void loadSpeechModel();
    void recognizeAudio();
    void toggleMicrophone();
    void cancelSpeech();
    void onMicrophonePcm(const QByteArray& pcm16le, int sampleRate);
    void drainMicrophoneAudio();
    void pollSpeechStream();
    void appendSpeechFinal(const QString& text);

    void chooseDocument();
    void chooseOutputDirectory();
    void parseDocument();
    void cancelDocument();
    void openDocumentOutput();
    void startDocumentWorker(const QString& input, const QString& output);
    void readDocumentWorkerOutput();
    void readDocumentWorkerErrors();
    void processDocumentWorkerLine(const QByteArray& line);
    void finishDocumentWorker(int exitCode, bool normalExit);
    void failDocumentWorkerStart();

    void chooseAcceptanceManifest();
    void chooseAcceptanceOutput();
    void startAcceptance();
    void verifyAcceptanceRuntimeStart();
    void verifyAcceptanceRuntimeEnd(const QString& requestedState);
    void cancelAcceptance();
    void openAcceptanceOutput();
    void startNextAcceptanceSample();
    void completeAcceptanceVoice(const voiceengine::Result& result);
    void completeAcceptanceDocument(const QJsonObject& result);
    void failAcceptanceSample(const QString& message,
                              const QJsonObject& evidence = {},
                              const QByteArray& rawResult = {});
    void recordAcceptanceSample(const QString& hypothesis,
                                const acceptance::ConfidenceSummary& confidence,
                                const QString& status,
                                const QJsonObject& evidence = {},
                                const QByteArray& rawResult = {});
    void finishAcceptance(const QString& state = QString());
    void updateAcceptanceSummary();
    void showSelectedAcceptanceEvidence();

    void setRuntimeStatus(QLabel* label, bool ready, const QString& text);
    void appendLog(QPlainTextEdit* output, const QString& text);
    void showOperationError(const QString& title, const QString& text);
    QString componentRoot(const QString& name) const;

    QString speechRuntimeRoot_;
    QString documentRuntimeRoot_;
    bool speechReady_ = false;
    bool documentReady_ = false;
    bool speechModelLoading_ = false;
    bool pendingSpeechRecognition_ = false;
    bool pendingMicrophoneStart_ = false;
    bool pendingVoiceAcceptance_ = false;
    std::atomic<bool> closing_{false};

    std::unique_ptr<voiceengine::Context> speechContext_;
    std::unique_ptr<voiceengine::Task> speechTask_;
    std::unique_ptr<voiceengine::Stream> speechStream_;
    std::deque<std::vector<float>> pendingMicAudio_;
    bool microphoneInputClosed_ = false;
    bool speechFinalHasText_ = false;
    std::thread speechModelThread_;

    MicCapture* microphone_ = nullptr;
    QTimer* pollTimer_ = nullptr;

    QLabel* speechRuntimeStatus_ = nullptr;
    QLabel* documentRuntimeStatus_ = nullptr;
    QLineEdit* audioPath_ = nullptr;
    QPushButton* loadModelButton_ = nullptr;
    QPushButton* recognizeButton_ = nullptr;
    QPushButton* microphoneButton_ = nullptr;
    QPushButton* cancelSpeechButton_ = nullptr;
    QProgressBar* speechProgress_ = nullptr;
    QPlainTextEdit* speechInterim_ = nullptr;
    QPlainTextEdit* speechResult_ = nullptr;

    QLineEdit* documentPath_ = nullptr;
    QLineEdit* outputPath_ = nullptr;
    QPushButton* parseButton_ = nullptr;
    QPushButton* cancelDocumentButton_ = nullptr;
    QPushButton* openOutputButton_ = nullptr;
    QProgressBar* documentProgress_ = nullptr;
    QPlainTextEdit* documentResult_ = nullptr;
    QString lastDocumentOutput_;
    QProcess* documentProcess_ = nullptr;
    QByteArray documentWorkerStdout_;
    QByteArray documentWorkerTranscript_;
    QByteArray documentWorkerStderr_;
    QJsonObject documentWorkerResult_;
    QString documentWorkerError_;
    bool documentWorkerCancelled_ = false;

    acceptance::Dataset acceptanceDataset_;
    bool acceptanceDatasetLoaded_ = false;
    bool acceptanceRunning_ = false;
    bool acceptanceCancelRequested_ = false;
    int acceptanceIndex_ = 0;
    int acceptanceFailures_ = 0;
    qint64 acceptanceReferenceCharacters_ = 0;
    qint64 acceptanceHypothesisCharacters_ = 0;
    qint64 acceptanceEditDistance_ = 0;
    int acceptanceConfidenceCount_ = 0;
    double acceptanceConfidenceTotal_ = 0.0;
    QString acceptanceRunDirectory_;
    QJsonArray acceptanceResults_;
    QJsonObject acceptanceReport_;
    QJsonObject acceptanceRuntimeStartEvidence_;
    bool acceptanceRuntimeStartValid_ = false;
    QString acceptanceRuntimeStartError_;
    QJsonObject acceptanceRuntimeEndEvidence_;
    bool acceptanceRuntimeEndValid_ = false;
    bool acceptanceRuntimeEndVerified_ = false;
    QString acceptanceRuntimeEndError_;
    bool acceptanceRuntimeVerifying_ = false;
    std::atomic_bool acceptanceRuntimeAbort_{false};
    QString pendingAcceptanceFinishState_;
    std::thread acceptanceRuntimeThread_;
    QVector<QString> acceptanceHypotheses_;

    QLineEdit* acceptanceManifestPath_ = nullptr;
    QLineEdit* acceptanceOutputPath_ = nullptr;
    QPushButton* acceptanceStartButton_ = nullptr;
    QPushButton* acceptanceCancelButton_ = nullptr;
    QPushButton* acceptanceOpenButton_ = nullptr;
    QPushButton* acceptanceEvidenceButton_ = nullptr;
    QProgressBar* acceptanceProgress_ = nullptr;
    QLabel* acceptanceDatasetStatus_ = nullptr;
    QLabel* acceptanceAccuracy_ = nullptr;
    QLabel* acceptanceConfidence_ = nullptr;
    QLabel* acceptanceVerdict_ = nullptr;
    QTableWidget* acceptanceTable_ = nullptr;
    AcceptanceEvidencePanel* acceptanceEvidencePanel_ = nullptr;
};

}  // namespace speechdoc

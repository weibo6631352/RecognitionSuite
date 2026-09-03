#include "main_window.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

#include <cstdint>
#include <exception>
#include <functional>
#include <utility>
#include <vector>

namespace speechdoc {
namespace {

std::wstring toWidePath(const QString& path) {
    return QDir::toNativeSeparators(path).toStdWString();
}

QString fromWidePath(const std::wstring& path) {
    return QString::fromWCharArray(path.c_str(),
                                   static_cast<int>(path.size()));
}

bool isSupportedGpuArchitecture(const voiceengine::CudaInfo& info) noexcept {
    return (info.compute_major == 8 && info.compute_minor == 9)
        || (info.compute_major == 12 && info.compute_minor == 0);
}

QString cudaArchitecture(const voiceengine::CudaInfo& info) {
    return QStringLiteral("sm_%1%2")
        .arg(info.compute_major)
        .arg(info.compute_minor);
}

QString safeFileName(QString value) {
    value.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\r\\n\\t]")),
                  QStringLiteral("_"));
    value = value.trimmed();
    while (value.endsWith(QLatin1Char('.')))
        value.chop(1);
    return value.isEmpty() ? QStringLiteral("dataset") : value.left(100);
}

QString percent(double value) {
    return QStringLiteral("%1%").arg(value * 100.0, 0, 'f', 2);
}

QFrame* makeRuntimeCard(const QString& name, QLabel** status,
                        QWidget* parent) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("runtimeCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(3);
    auto* title = new QLabel(name, card);
    title->setObjectName(QStringLiteral("runtimeName"));
    *status = new QLabel(QStringLiteral("正在检查…"), card);
    (*status)->setObjectName(QStringLiteral("runtimeStatus"));
    layout->addWidget(title);
    layout->addWidget(*status);
    return card;
}

QHBoxLayout* makePathRow(QLineEdit** edit, const QString& placeholder,
                         const QString& buttonText, QWidget* parent,
                         const std::function<void()>& action) {
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    *edit = new QLineEdit(parent);
    (*edit)->setPlaceholderText(placeholder);
    auto* button = new QPushButton(buttonText, parent);
    QObject::connect(button, &QPushButton::clicked, parent, action);
    row->addWidget(*edit, 1);
    row->addWidget(button);
    return row;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("纸质扫描与语音识别系统"));
    resize(1180, 800);
    setMinimumSize(960, 680);
    buildUi();

    microphone_ = new MicCapture(this);
    connect(microphone_, &MicCapture::pcmReady,
            this, &MainWindow::onMicrophonePcm);
    connect(microphone_, &MicCapture::error, this,
            [this](const QString& message) {
                showOperationError(QStringLiteral("麦克风"), message);
                updateControls();
            });

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(80);
    connect(pollTimer_, &QTimer::timeout,
            this, &MainWindow::pollSdkTasks);
    pollTimer_->start();

    initializeSdks();
    updateControls();
}

MainWindow::~MainWindow() {
    closing_.store(true);
    if (microphone_)
        microphone_->stop();
    if (speechStream_) {
        try {
            speechStream_->cancel();
        } catch (...) {
        }
        speechStream_.reset();
    }
    if (speechTask_) {
        try {
            speechTask_->cancel();
        } catch (...) {
        }
        speechTask_.reset();
    }
    if (documentTask_) {
        try {
            documentTask_->cancel();
        } catch (...) {
        }
        documentTask_.reset();
    }
    if (speechModelThread_.joinable())
        speechModelThread_.join();
}

void MainWindow::buildUi() {
    auto* rootWidget = new QWidget(this);
    rootWidget->setObjectName(QStringLiteral("centralRoot"));
    setCentralWidget(rootWidget);

    auto* root = new QVBoxLayout(rootWidget);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("纸质扫描与语音识别系统"), rootWidget);
    title->setObjectName(QStringLiteral("headerTitle"));
    auto* subtitle = new QLabel(
        QStringLiteral("聚焦两项核心能力 · 纸质扫描识别与语音识别 · 验收目标 98%"),
        rootWidget);
    subtitle->setObjectName(QStringLiteral("headerSubtitle"));
    root->addWidget(title);
    root->addWidget(subtitle);

    auto* runtimeCards = new QHBoxLayout;
    runtimeCards->setSpacing(10);
    runtimeCards->addWidget(makeRuntimeCard(
        QStringLiteral("VoiceEngine · 流式语音识别"),
        &speechRuntimeStatus_, rootWidget), 1);
    runtimeCards->addWidget(makeRuntimeCard(
        QStringLiteral("ScanEngine · 纸质扫描识别"),
        &documentRuntimeStatus_, rootWidget), 1);
    root->addLayout(runtimeCards);

    auto* tabs = new QTabWidget(rootWidget);

    auto* speechPage = new QWidget(tabs);
    auto* speechLayout = new QVBoxLayout(speechPage);
    speechLayout->setContentsMargins(16, 16, 16, 16);
    speechLayout->setSpacing(10);
    auto* speechTitle = new QLabel(QStringLiteral("语音识别"), speechPage);
    speechTitle->setObjectName(QStringLiteral("sectionTitle"));
    auto* speechHint = new QLabel(
        QStringLiteral("选择音频文件，或使用系统默认麦克风进行流式识别。识别率验收目标：98%。"),
        speechPage);
    speechHint->setObjectName(QStringLiteral("hintText"));
    speechLayout->addWidget(speechTitle);
    speechLayout->addWidget(speechHint);
    speechLayout->addLayout(makePathRow(
        &audioPath_, QStringLiteral("选择 WAV / MP3 / M4A / FLAC 等音频"),
        QStringLiteral("选择音频"), speechPage,
        [this] { chooseAudio(); }));

    auto* speechButtons = new QHBoxLayout;
    loadModelButton_ = new QPushButton(QStringLiteral("加载语音模型"), speechPage);
    recognizeButton_ = new QPushButton(QStringLiteral("识别文件"), speechPage);
    recognizeButton_->setObjectName(QStringLiteral("primaryButton"));
    microphoneButton_ = new QPushButton(QStringLiteral("开始麦克风"), speechPage);
    microphoneButton_->setObjectName(QStringLiteral("recordButton"));
    cancelSpeechButton_ = new QPushButton(QStringLiteral("取消"), speechPage);
    speechButtons->addWidget(loadModelButton_);
    speechButtons->addWidget(recognizeButton_);
    speechButtons->addWidget(microphoneButton_);
    speechButtons->addWidget(cancelSpeechButton_);
    speechButtons->addStretch(1);
    speechLayout->addLayout(speechButtons);

    speechProgress_ = new QProgressBar(speechPage);
    speechProgress_->setRange(0, 100);
    speechProgress_->setValue(0);
    speechProgress_->setTextVisible(false);
    speechLayout->addWidget(speechProgress_);

    auto* speechText = new QHBoxLayout;
    auto* interimBox = new QVBoxLayout;
    interimBox->addWidget(new QLabel(QStringLiteral("实时文本"), speechPage));
    speechInterim_ = new QPlainTextEdit(speechPage);
    speechInterim_->setReadOnly(true);
    speechInterim_->setPlaceholderText(QStringLiteral("麦克风临时结果将在这里更新"));
    interimBox->addWidget(speechInterim_, 1);
    auto* resultBox = new QVBoxLayout;
    resultBox->addWidget(new QLabel(QStringLiteral("最终文本"), speechPage));
    speechResult_ = new QPlainTextEdit(speechPage);
    speechResult_->setReadOnly(true);
    speechResult_->setPlaceholderText(QStringLiteral("识别完成后的文本"));
    resultBox->addWidget(speechResult_, 1);
    speechText->addLayout(interimBox, 2);
    speechText->addLayout(resultBox, 3);
    speechLayout->addLayout(speechText, 1);

    connect(loadModelButton_, &QPushButton::clicked,
            this, &MainWindow::loadSpeechModel);
    connect(recognizeButton_, &QPushButton::clicked,
            this, &MainWindow::recognizeAudio);
    connect(microphoneButton_, &QPushButton::clicked,
            this, &MainWindow::toggleMicrophone);
    connect(cancelSpeechButton_, &QPushButton::clicked,
            this, &MainWindow::cancelSpeech);
    connect(audioPath_, &QLineEdit::textChanged,
            this, [this] { updateControls(); });

    auto* documentPage = new QWidget(tabs);
    auto* documentLayout = new QVBoxLayout(documentPage);
    documentLayout->setContentsMargins(16, 16, 16, 16);
    documentLayout->setSpacing(10);
    auto* documentTitle = new QLabel(QStringLiteral("纸质扫描识别"), documentPage);
    documentTitle->setObjectName(QStringLiteral("sectionTitle"));
    auto* documentHint = new QLabel(
        QStringLiteral("选择纸质资料的扫描图片，识别文字与表格并生成 Excel。识别率验收目标：98%。"),
        documentPage);
    documentHint->setObjectName(QStringLiteral("hintText"));
    documentLayout->addWidget(documentTitle);
    documentLayout->addWidget(documentHint);
    documentLayout->addLayout(makePathRow(
        &documentPath_, QStringLiteral("选择纸质扫描图片：PNG / JPEG / BMP / TIFF / WebP"),
        QStringLiteral("选择扫描件"), documentPage,
        [this] { chooseDocument(); }));
    documentLayout->addLayout(makePathRow(
        &outputPath_, QStringLiteral("结果输出目录"),
        QStringLiteral("选择目录"), documentPage,
        [this] { chooseOutputDirectory(); }));
    outputPath_->setText(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("output")));

    auto* documentButtons = new QHBoxLayout;
    parseButton_ = new QPushButton(QStringLiteral("开始识别"), documentPage);
    parseButton_->setObjectName(QStringLiteral("primaryButton"));
    cancelDocumentButton_ = new QPushButton(QStringLiteral("取消"), documentPage);
    openOutputButton_ = new QPushButton(QStringLiteral("打开结果目录"), documentPage);
    documentButtons->addWidget(parseButton_);
    documentButtons->addWidget(cancelDocumentButton_);
    documentButtons->addWidget(openOutputButton_);
    documentButtons->addStretch(1);
    documentLayout->addLayout(documentButtons);

    documentProgress_ = new QProgressBar(documentPage);
    documentProgress_->setRange(0, 100);
    documentProgress_->setValue(0);
    documentProgress_->setTextVisible(false);
    documentLayout->addWidget(documentProgress_);
    documentResult_ = new QPlainTextEdit(documentPage);
    documentResult_->setReadOnly(true);
    documentResult_->setPlaceholderText(
        QStringLiteral("识别状态、Excel 路径和输出目录将在这里显示"));
    documentLayout->addWidget(documentResult_, 1);

    connect(parseButton_, &QPushButton::clicked,
            this, &MainWindow::parseDocument);
    connect(cancelDocumentButton_, &QPushButton::clicked,
            this, &MainWindow::cancelDocument);
    connect(openOutputButton_, &QPushButton::clicked,
            this, &MainWindow::openDocumentOutput);
    connect(documentPath_, &QLineEdit::textChanged,
            this, [this] { updateControls(); });

    tabs->addTab(speechPage, QStringLiteral("语音识别"));
    tabs->addTab(documentPage, QStringLiteral("纸质扫描识别"));

    auto* acceptancePage = new QWidget(tabs);
    auto* acceptanceLayout = new QVBoxLayout(acceptancePage);
    acceptanceLayout->setContentsMargins(16, 16, 16, 16);
    acceptanceLayout->setSpacing(10);
    auto* acceptanceTitle = new QLabel(QStringLiteral("冻结测试集验收验证"), acceptancePage);
    acceptanceTitle->setObjectName(QStringLiteral("sectionTitle"));
    auto* acceptanceHint = new QLabel(
        QStringLiteral("在界面内批量调用已发布 SDK，以统一字符编辑距离验证 98% 指标；模型置信度仅作辅助诊断。"),
        acceptancePage);
    acceptanceHint->setObjectName(QStringLiteral("hintText"));
    acceptanceHint->setWordWrap(true);
    acceptanceLayout->addWidget(acceptanceTitle);
    acceptanceLayout->addWidget(acceptanceHint);
    acceptanceLayout->addLayout(makePathRow(
        &acceptanceManifestPath_, QStringLiteral("选择 RecognitionStudio-Acceptance/1 清单"),
        QStringLiteral("选择清单"), acceptancePage,
        [this] { chooseAcceptanceManifest(); }));
    acceptanceLayout->addLayout(makePathRow(
        &acceptanceOutputPath_, QStringLiteral("验收证据输出目录"),
        QStringLiteral("选择目录"), acceptancePage,
        [this] { chooseAcceptanceOutput(); }));
    acceptanceOutputPath_->setText(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("output/acceptance")));

    acceptanceDatasetStatus_ = new QLabel(QStringLiteral("尚未加载冻结测试集"), acceptancePage);
    acceptanceDatasetStatus_->setWordWrap(true);
    acceptanceLayout->addWidget(acceptanceDatasetStatus_);

    auto* acceptanceMetrics = new QGridLayout;
    acceptanceMetrics->addWidget(new QLabel(QStringLiteral("实际准确率"), acceptancePage), 0, 0);
    acceptanceMetrics->addWidget(new QLabel(QStringLiteral("模型置信度"), acceptancePage), 0, 1);
    acceptanceMetrics->addWidget(new QLabel(QStringLiteral("验收结论"), acceptancePage), 0, 2);
    acceptanceAccuracy_ = new QLabel(QStringLiteral("—"), acceptancePage);
    acceptanceConfidence_ = new QLabel(QStringLiteral("—"), acceptancePage);
    acceptanceVerdict_ = new QLabel(QStringLiteral("未验证"), acceptancePage);
    acceptanceAccuracy_->setObjectName(QStringLiteral("metricValue"));
    acceptanceConfidence_->setObjectName(QStringLiteral("metricValue"));
    acceptanceVerdict_->setObjectName(QStringLiteral("verdictValue"));
    acceptanceMetrics->addWidget(acceptanceAccuracy_, 1, 0);
    acceptanceMetrics->addWidget(acceptanceConfidence_, 1, 1);
    acceptanceMetrics->addWidget(acceptanceVerdict_, 1, 2);
    acceptanceLayout->addLayout(acceptanceMetrics);

    acceptanceProgress_ = new QProgressBar(acceptancePage);
    acceptanceProgress_->setRange(0, 100);
    acceptanceProgress_->setValue(0);
    acceptanceProgress_->setTextVisible(false);
    acceptanceLayout->addWidget(acceptanceProgress_);

    auto* acceptanceButtons = new QHBoxLayout;
    acceptanceStartButton_ = new QPushButton(QStringLiteral("开始正式验证"), acceptancePage);
    acceptanceStartButton_->setObjectName(QStringLiteral("primaryButton"));
    acceptanceCancelButton_ = new QPushButton(QStringLiteral("取消验证"), acceptancePage);
    acceptanceOpenButton_ = new QPushButton(QStringLiteral("打开证据目录"), acceptancePage);
    acceptanceButtons->addWidget(acceptanceStartButton_);
    acceptanceButtons->addWidget(acceptanceCancelButton_);
    acceptanceButtons->addWidget(acceptanceOpenButton_);
    acceptanceButtons->addStretch(1);
    acceptanceLayout->addLayout(acceptanceButtons);

    acceptanceTable_ = new QTableWidget(acceptancePage);
    acceptanceTable_->setColumnCount(6);
    acceptanceTable_->setHorizontalHeaderLabels({
        QStringLiteral("样本"), QStringLiteral("状态"),
        QStringLiteral("真值字符"), QStringLiteral("编辑距离"),
        QStringLiteral("实际准确率"), QStringLiteral("置信度")});
    acceptanceTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < acceptanceTable_->columnCount(); ++column)
        acceptanceTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    acceptanceTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    acceptanceTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    acceptanceLayout->addWidget(acceptanceTable_, 1);

    connect(acceptanceStartButton_, &QPushButton::clicked,
            this, &MainWindow::startAcceptance);
    connect(acceptanceCancelButton_, &QPushButton::clicked,
            this, &MainWindow::cancelAcceptance);
    connect(acceptanceOpenButton_, &QPushButton::clicked,
            this, &MainWindow::openAcceptanceOutput);
    tabs->addTab(acceptancePage, QStringLiteral("验收验证"));
    root->addWidget(tabs, 1);
    statusBar()->showMessage(QStringLiteral("正在检查两套 SDK…"));
}

QString MainWindow::componentRoot(const QString& name) const {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("components/") + name);
}

void MainWindow::initializeSdks() {
    speechRuntimeRoot_ = componentRoot(QStringLiteral("voiceengine"));
    documentRuntimeRoot_ = componentRoot(QStringLiteral("scanengine"));
    bool gpuArchitectureKnown = false;
    bool gpuArchitectureSupported = false;

    try {
        auto context = voiceengine::Context::open(toWidePath(speechRuntimeRoot_));
        const auto info = context.cuda_info();
        gpuArchitectureKnown = info.available;
        gpuArchitectureSupported = info.available
            && isSupportedGpuArchitecture(info);
        speechReady_ = gpuArchitectureSupported;
        if (speechReady_) {
            speechContext_ = std::make_unique<voiceengine::Context>(
                std::move(context));
            setRuntimeStatus(
                speechRuntimeStatus_, true,
                QStringLiteral("就绪 · %1 · %2 · %3 MiB")
                    .arg(QString::fromUtf8(info.name.c_str()))
                    .arg(cudaArchitecture(info))
                    .arg(info.vram_mib));
        } else if (info.available) {
            setRuntimeStatus(
                speechRuntimeStatus_, false,
                QStringLiteral("不支持 · %1 · %2；需要 RTX 40 (sm_89) 或 RTX 50 (sm_120)")
                    .arg(QString::fromUtf8(info.name.c_str()))
                    .arg(cudaArchitecture(info)));
        } else {
            setRuntimeStatus(speechRuntimeStatus_, false,
                             QStringLiteral("CUDA GPU 不可用"));
        }
    } catch (const std::exception& error) {
        speechReady_ = false;
        setRuntimeStatus(speechRuntimeStatus_, false,
                         QString::fromUtf8(error.what()));
    }

    try {
        auto context = scanengine::Context::open(toWidePath(documentRuntimeRoot_));
        const auto info = context.runtime_info();
        documentReady_ = info.gpu_available
            && (!gpuArchitectureKnown || gpuArchitectureSupported);
        if (documentReady_) {
            documentContext_ = std::make_unique<scanengine::Context>(
                std::move(context));
            setRuntimeStatus(
                documentRuntimeStatus_, true,
                QStringLiteral("就绪 · %1")
                    .arg(QString::fromUtf8(info.backend.c_str())));
        } else if (info.gpu_available && gpuArchitectureKnown) {
            setRuntimeStatus(
                documentRuntimeStatus_, false,
                QStringLiteral("GPU 架构不受支持；需要 RTX 40 (sm_89) 或 RTX 50 (sm_120)"));
        } else {
            setRuntimeStatus(documentRuntimeStatus_, false,
                             QStringLiteral("GPU 后端不可用"));
        }
    } catch (const std::exception& error) {
        documentReady_ = false;
        setRuntimeStatus(documentRuntimeStatus_, false,
                         QString::fromUtf8(error.what()));
    }

    statusBar()->showMessage(sdksReady()
        ? QStringLiteral("两套 SDK 均已就绪")
        : QStringLiteral("部分 SDK 未就绪，请检查上方状态"));
}

void MainWindow::setRuntimeStatus(QLabel* label, bool ready,
                                  const QString& text) {
    label->setText(text);
    label->setProperty("ready", ready);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

bool MainWindow::operationActive() const {
    return speechModelLoading_ || speechTask_ || documentTask_
        || speechStream_ || acceptanceRunning_
        || (microphone_ && microphone_->running());
}

void MainWindow::updateControls() {
    const bool micRunning = microphone_ && microphone_->running();
    const bool speechBusy = speechModelLoading_ || speechTask_ || speechStream_
        || micRunning;
    const bool documentBusy = static_cast<bool>(documentTask_);
    const bool otherBusy = speechBusy || documentBusy;

    loadModelButton_->setEnabled(speechReady_ && !otherBusy);
    recognizeButton_->setEnabled(
        speechReady_ && speechContext_ && speechContext_->model_loaded()
        && !otherBusy && !audioPath_->text().trimmed().isEmpty());
    microphoneButton_->setEnabled(
        speechReady_ && speechContext_
        && (micRunning || speechStream_
            || (speechContext_->model_loaded() && !otherBusy)));
    microphoneButton_->setText(micRunning
        ? QStringLiteral("停止麦克风") : QStringLiteral("开始麦克风"));
    microphoneButton_->setProperty("recording", micRunning);
    microphoneButton_->style()->unpolish(microphoneButton_);
    microphoneButton_->style()->polish(microphoneButton_);
    cancelSpeechButton_->setEnabled(speechBusy);

    parseButton_->setEnabled(
        documentReady_ && !otherBusy
        && !documentPath_->text().trimmed().isEmpty());
    cancelDocumentButton_->setEnabled(documentBusy);
    openOutputButton_->setEnabled(!lastDocumentOutput_.isEmpty());

    const bool acceptanceRuntimeReady = acceptanceDatasetLoaded_
        && (acceptanceDataset_.kind == acceptance::Kind::Scan
                ? documentReady_
                : speechReady_ && speechContext_
                      && speechContext_->model_loaded());
    acceptanceStartButton_->setEnabled(
        acceptanceRuntimeReady && !operationActive());
    acceptanceCancelButton_->setEnabled(acceptanceRunning_);
    acceptanceOpenButton_->setEnabled(!acceptanceRunDirectory_.isEmpty());
}

void MainWindow::chooseAudio() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择音频"), QString(),
        QStringLiteral("音频 (*.wav *.mp3 *.m4a *.aac *.flac *.opus *.mp4 *.mkv);;所有文件 (*.*)"));
    if (!path.isEmpty())
        audioPath_->setText(QDir::toNativeSeparators(path));
    updateControls();
}

void MainWindow::loadSpeechModel() {
    if (!speechContext_ || operationActive())
        return;
    if (speechContext_->model_loaded()) {
        statusBar()->showMessage(QStringLiteral("语音模型已经加载"));
        return;
    }
    if (speechModelThread_.joinable())
        speechModelThread_.join();

    speechModelLoading_ = true;
    speechProgress_->setRange(0, 0);
    appendLog(speechResult_, QStringLiteral("正在加载 Qwen3-ASR 模型…"));
    updateControls();
    speechModelThread_ = std::thread([this] {
        QString message;
        bool ok = false;
        try {
            speechContext_->load_model();
            ok = true;
            message = QStringLiteral("语音模型加载完成");
        } catch (const std::exception& error) {
            message = QString::fromUtf8(error.what());
        }
        if (closing_.load())
            return;
        QMetaObject::invokeMethod(this, [this, ok, message] {
            speechModelLoading_ = false;
            speechProgress_->setRange(0, 100);
            speechProgress_->setValue(ok ? 100 : 0);
            appendLog(speechResult_, message);
            statusBar()->showMessage(message);
            updateControls();
        }, Qt::QueuedConnection);
    });
}

void MainWindow::recognizeAudio() {
    if (!speechContext_ || operationActive())
        return;
    const QString path = audioPath_->text().trimmed();
    if (!QFileInfo::exists(path)) {
        showOperationError(QStringLiteral("语音识别"),
                           QStringLiteral("音频文件不存在"));
        return;
    }
    if (!speechContext_->model_loaded()) {
        showOperationError(QStringLiteral("语音识别"),
                           QStringLiteral("请先加载语音模型"));
        return;
    }
    try {
        auto task = speechContext_->submit_file(toWidePath(path));
        speechTask_ = std::make_unique<voiceengine::Task>(std::move(task));
        speechProgress_->setValue(0);
        speechInterim_->clear();
        appendLog(speechResult_, QStringLiteral("开始识别：%1").arg(path));
        statusBar()->showMessage(QStringLiteral("正在识别音频"));
    } catch (const std::exception& error) {
        showOperationError(QStringLiteral("语音识别"),
                           QString::fromUtf8(error.what()));
    }
    updateControls();
}

void MainWindow::toggleMicrophone() {
    if (!speechContext_)
        return;
    if (microphone_->running()) {
        microphone_->stop();
        if (speechStream_) {
            microphoneInputClosed_ = true;
            drainMicrophoneAudio();
            statusBar()->showMessage(
                QStringLiteral("麦克风已停止，正在收尾识别"));
        }
        updateControls();
        return;
    }
    if (operationActive() || !speechContext_->model_loaded()) {
        showOperationError(QStringLiteral("麦克风"),
                           QStringLiteral("请先加载模型，并等待当前任务结束"));
        return;
    }
    speechInterim_->clear();
    pendingMicAudio_.clear();
    microphoneInputClosed_ = false;
    speechFinalHasText_ = false;
    microphone_->start();
    appendLog(speechResult_, QStringLiteral("开始流式麦克风识别"));
    statusBar()->showMessage(QStringLiteral("正在监听系统默认麦克风"));
    updateControls();
}

void MainWindow::onMicrophonePcm(const QByteArray& pcm16le, int sampleRate) {
    if (!speechContext_ || !microphone_->running() || pcm16le.size() < 2)
        return;
    try {
        if (!speechStream_) {
            voiceengine::StreamParams params;
            params.sample_rate = sampleRate;
            auto stream = speechContext_->create_stream(params);
            speechStream_ = std::make_unique<voiceengine::Stream>(
                std::move(stream));
        }
        const int count = pcm16le.size() / 2;
        const auto* input = reinterpret_cast<const std::int16_t*>(
            pcm16le.constData());
        std::vector<float> samples(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index)
            samples[static_cast<std::size_t>(index)] =
                static_cast<float>(input[index]) / 32768.0f;
        pendingMicAudio_.push_back(std::move(samples));
        drainMicrophoneAudio();
    } catch (const std::exception& error) {
        microphone_->stop();
        pendingMicAudio_.clear();
        showOperationError(QStringLiteral("麦克风"),
                           QString::fromUtf8(error.what()));
        speechStream_.reset();
        updateControls();
    }
}

void MainWindow::drainMicrophoneAudio() {
    if (!speechStream_)
        return;
    while (!pendingMicAudio_.empty()) {
        try {
            speechStream_->push(pendingMicAudio_.front());
            pendingMicAudio_.pop_front();
        } catch (const voiceengine::Error& error) {
            if (error.status() == VE_ERR_BUSY)
                return;
            microphone_->stop();
            pendingMicAudio_.clear();
            showOperationError(QStringLiteral("麦克风"),
                               QString::fromUtf8(error.what()));
            speechStream_.reset();
            microphoneInputClosed_ = false;
            updateControls();
            return;
        }
    }
    if (microphoneInputClosed_) {
        try {
            speechStream_->finish();
        } catch (const std::exception& error) {
            showOperationError(QStringLiteral("麦克风"),
                               QString::fromUtf8(error.what()));
            speechStream_.reset();
        }
        microphoneInputClosed_ = false;
    }
}

void MainWindow::appendSpeechFinal(const QString& text) {
    if (text.isEmpty())
        return;
    if (!speechFinalHasText_) {
        speechResult_->appendPlainText(text);
        speechFinalHasText_ = true;
        return;
    }

    QTextCursor cursor = speechResult_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    speechResult_->setTextCursor(cursor);
    speechResult_->ensureCursorVisible();
}

void MainWindow::pollSpeechStream() {
    if (!speechStream_)
        return;
    try {
        for (int index = 0; index < 16; ++index) {
            const auto event = speechStream_->poll();
            if (!event)
                return;
            const QString text = QString::fromUtf8(event->text.c_str());
            if (event->type == VE_STREAM_EVENT_PARTIAL) {
                speechInterim_->setPlainText(text);
                speechProgress_->setValue(50);
            } else if (event->type == VE_STREAM_EVENT_FINAL) {
                speechInterim_->clear();
                appendSpeechFinal(text);
                speechProgress_->setValue(100);
            } else if (event->type == VE_STREAM_EVENT_ERROR) {
                speechFinalHasText_ = false;
                appendLog(speechResult_,
                          QStringLiteral("流式识别错误（状态 %1）：%2")
                              .arg(static_cast<int>(event->error)).arg(text));
            } else if (event->type == VE_STREAM_EVENT_END) {
                speechStream_.reset();
                pendingMicAudio_.clear();
                microphoneInputClosed_ = false;
                speechFinalHasText_ = false;
                speechProgress_->setValue(0);
                statusBar()->showMessage(QStringLiteral("麦克风流已结束"));
                updateControls();
                return;
            }
        }
    } catch (const std::exception& error) {
        speechFinalHasText_ = false;
        appendLog(speechResult_, QString::fromUtf8(error.what()));
        speechStream_.reset();
        updateControls();
    }
}

void MainWindow::cancelSpeech() {
    if (microphone_->running())
        microphone_->stop();
    if (speechStream_) {
        try {
            speechStream_->cancel();
        } catch (...) {
        }
    }
    pendingMicAudio_.clear();
    microphoneInputClosed_ = false;
    speechFinalHasText_ = false;
    if (speechTask_) {
        try {
            speechTask_->cancel();
        } catch (...) {
        }
    }
    statusBar()->showMessage(QStringLiteral("正在取消语音任务"));
    updateControls();
}

void MainWindow::chooseDocument() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择纸质扫描图片"), QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp);;所有文件 (*.*)"));
    if (!path.isEmpty())
        documentPath_->setText(QDir::toNativeSeparators(path));
    updateControls();
}

void MainWindow::chooseOutputDirectory() {
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择结果输出目录"), outputPath_->text());
    if (!path.isEmpty())
        outputPath_->setText(QDir::toNativeSeparators(path));
}

void MainWindow::parseDocument() {
    if (!documentContext_ || operationActive())
        return;
    const QString input = documentPath_->text().trimmed();
    const QString output = outputPath_->text().trimmed();
    if (!QFileInfo::exists(input)) {
        showOperationError(QStringLiteral("纸质扫描识别"),
                           QStringLiteral("输入图片不存在"));
        return;
    }
    if (output.isEmpty() || !QDir().mkpath(output)) {
        showOperationError(QStringLiteral("纸质扫描识别"),
                           QStringLiteral("结果输出目录不可用"));
        return;
    }
    try {
        auto task = documentContext_->submit_file(
            toWidePath(input), toWidePath(output), SE_EFFORT_MEDIUM);
        documentTask_ = std::make_unique<scanengine::Task>(std::move(task));
        documentProgress_->setValue(0);
        documentResult_->clear();
        appendLog(documentResult_, QStringLiteral("开始识别：%1").arg(input));
        statusBar()->showMessage(QStringLiteral("正在识别纸质扫描图片"));
    } catch (const std::exception& error) {
        showOperationError(QStringLiteral("纸质扫描识别"),
                           QString::fromUtf8(error.what()));
    }
    updateControls();
}

void MainWindow::cancelDocument() {
    if (!documentTask_)
        return;
    try {
        documentTask_->cancel();
        statusBar()->showMessage(QStringLiteral("正在取消文档任务"));
    } catch (const std::exception& error) {
        showOperationError(QStringLiteral("纸质扫描识别"),
                           QString::fromUtf8(error.what()));
    }
}

void MainWindow::openDocumentOutput() {
    if (!lastDocumentOutput_.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(lastDocumentOutput_));
}

void MainWindow::chooseAcceptanceManifest() {
    if (acceptanceRunning_)
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择冻结测试集清单"),
        acceptanceManifestPath_->text(), QStringLiteral("JSON 清单 (*.json)"));
    if (path.isEmpty())
        return;
    acceptance::Dataset dataset;
    QString error;
    if (!acceptance::loadDataset(path, &dataset, &error)) {
        acceptanceDatasetLoaded_ = false;
        acceptanceDatasetStatus_->setText(QStringLiteral("清单无效：%1").arg(error));
        showOperationError(QStringLiteral("验收验证"), error);
        updateControls();
        return;
    }
    acceptanceDataset_ = std::move(dataset);
    acceptanceDatasetLoaded_ = true;
    acceptanceManifestPath_->setText(QDir::toNativeSeparators(path));
    acceptanceManifestPath_->setReadOnly(true);
    acceptanceDatasetStatus_->setText(
        QStringLiteral("已冻结：%1 · %2 · %3 个样本 · SHA-256 %4… · 阈值 %5")
            .arg(acceptanceDataset_.id,
                 acceptanceDataset_.kind == acceptance::Kind::Scan
                     ? QStringLiteral("纸质扫描") : QStringLiteral("语音识别"))
            .arg(acceptanceDataset_.samples.size())
            .arg(acceptanceDataset_.manifestSha256.left(12),
                 percent(acceptanceDataset_.threshold)));
    acceptanceVerdict_->setText(QStringLiteral("未验证"));
    acceptanceVerdict_->setProperty("passed", QVariant());
    acceptanceVerdict_->style()->unpolish(acceptanceVerdict_);
    acceptanceVerdict_->style()->polish(acceptanceVerdict_);
    acceptanceAccuracy_->setText(QStringLiteral("—"));
    acceptanceConfidence_->setText(QStringLiteral("—"));
    updateControls();
}

void MainWindow::chooseAcceptanceOutput() {
    if (acceptanceRunning_)
        return;
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择验收证据输出目录"),
        acceptanceOutputPath_->text());
    if (!path.isEmpty())
        acceptanceOutputPath_->setText(QDir::toNativeSeparators(path));
}

void MainWindow::startAcceptance() {
    if (!acceptanceDatasetLoaded_ || operationActive())
        return;
    if (acceptanceDataset_.kind == acceptance::Kind::Voice
        && (!speechContext_ || !speechContext_->model_loaded())) {
        showOperationError(QStringLiteral("验收验证"),
                           QStringLiteral("请先在语音识别页加载语音模型"));
        return;
    }
    const QString outputRoot = acceptanceOutputPath_->text().trimmed();
    if (outputRoot.isEmpty() || !QDir().mkpath(outputRoot)) {
        showOperationError(QStringLiteral("验收验证"),
                           QStringLiteral("验收证据输出目录不可用"));
        return;
    }
    const QString runName =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss_"))
        + safeFileName(acceptanceDataset_.id);
    acceptanceRunDirectory_ = QDir(outputRoot).filePath(runName);
    if (!QDir().mkpath(acceptanceRunDirectory_)) {
        showOperationError(QStringLiteral("验收验证"),
                           QStringLiteral("无法创建本次验收证据目录"));
        return;
    }
    acceptanceRunning_ = true;
    acceptanceCancelRequested_ = false;
    acceptanceIndex_ = 0;
    acceptanceFailures_ = 0;
    acceptanceReferenceCharacters_ = 0;
    acceptanceHypothesisCharacters_ = 0;
    acceptanceEditDistance_ = 0;
    acceptanceConfidenceCount_ = 0;
    acceptanceConfidenceTotal_ = 0.0;
    acceptanceResults_ = QJsonArray();
    acceptanceTable_->setRowCount(0);
    acceptanceProgress_->setValue(0);
    acceptanceAccuracy_->setText(QStringLiteral("—"));
    acceptanceConfidence_->setText(QStringLiteral("—"));
    acceptanceVerdict_->setText(QStringLiteral("验证中"));
    acceptanceVerdict_->setProperty("passed", QVariant());
    acceptanceVerdict_->style()->unpolish(acceptanceVerdict_);
    acceptanceVerdict_->style()->polish(acceptanceVerdict_);
    statusBar()->showMessage(QStringLiteral("正在执行冻结测试集验收验证"));
    updateControls();
    startNextAcceptanceSample();
}

void MainWindow::cancelAcceptance() {
    if (!acceptanceRunning_)
        return;
    acceptanceCancelRequested_ = true;
    try {
        if (acceptanceDataset_.kind == acceptance::Kind::Voice && speechTask_)
            speechTask_->cancel();
        if (acceptanceDataset_.kind == acceptance::Kind::Scan && documentTask_)
            documentTask_->cancel();
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()));
    }
    acceptanceVerdict_->setText(QStringLiteral("正在取消"));
    if (!speechTask_ && !documentTask_)
        finishAcceptance(QStringLiteral("cancelled"));
}

void MainWindow::openAcceptanceOutput() {
    if (!acceptanceRunDirectory_.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(acceptanceRunDirectory_));
}

void MainWindow::startNextAcceptanceSample() {
    if (!acceptanceRunning_)
        return;
    if (acceptanceCancelRequested_) {
        finishAcceptance(QStringLiteral("cancelled"));
        return;
    }
    if (acceptanceIndex_ >= acceptanceDataset_.samples.size()) {
        finishAcceptance();
        return;
    }
    const acceptance::Sample& sample = acceptanceDataset_.samples[acceptanceIndex_];
    const QString sampleDirectory = QDir(acceptanceRunDirectory_)
        .filePath(QStringLiteral("%1_%2")
                      .arg(acceptanceIndex_ + 1, 4, 10, QLatin1Char('0'))
                      .arg(safeFileName(sample.id)));
    if (!QDir().mkpath(sampleDirectory)) {
        failAcceptanceSample(QStringLiteral("无法创建样本证据目录"));
        return;
    }
    if (acceptance::sha256File(sample.inputPath) != sample.inputSha256) {
        failAcceptanceSample(QStringLiteral("运行前输入 SHA-256 已变化"));
        return;
    }
    try {
        if (acceptanceDataset_.kind == acceptance::Kind::Voice) {
            auto task = speechContext_->submit_file(toWidePath(sample.inputPath));
            speechTask_ = std::make_unique<voiceengine::Task>(std::move(task));
        } else {
            auto task = documentContext_->submit_file(
                toWidePath(sample.inputPath), toWidePath(sampleDirectory),
                SE_EFFORT_MEDIUM);
            documentTask_ = std::make_unique<scanengine::Task>(std::move(task));
        }
        statusBar()->showMessage(
            QStringLiteral("验收样本 %1/%2：%3")
                .arg(acceptanceIndex_ + 1)
                .arg(acceptanceDataset_.samples.size())
                .arg(sample.id));
    } catch (const std::exception& error) {
        failAcceptanceSample(QString::fromUtf8(error.what()));
    }
    updateControls();
}

void MainWindow::completeAcceptanceVoice(const voiceengine::Result& result) {
    const acceptance::ConfidenceSummary confidence =
        acceptance::extractVoiceConfidence(QByteArray::fromStdString(result.json));
    QJsonObject evidence;
    evidence.insert(QStringLiteral("engine_result"),
                    QJsonDocument::fromJson(QByteArray::fromStdString(result.json)).object());
    evidence.insert(QStringLiteral("duration_sec"), result.duration_sec);
    recordAcceptanceSample(QString::fromUtf8(result.text.c_str()), confidence,
                           QStringLiteral("completed"), evidence);
}

void MainWindow::completeAcceptanceDocument(const scanengine::Result& result) {
    const QString jsonPath = fromWidePath(result.json_path);
    QString error;
    const QString hypothesis = acceptance::extractScanText(jsonPath, &error);
    if (!error.isEmpty()) {
        failAcceptanceSample(error);
        return;
    }
    const acceptance::ConfidenceSummary confidence =
        acceptance::extractScanConfidence(jsonPath);
    QJsonObject evidence;
    evidence.insert(QStringLiteral("output_dir"), fromWidePath(result.output_dir));
    evidence.insert(QStringLiteral("excel_path"), fromWidePath(result.excel_path));
    evidence.insert(QStringLiteral("markdown_path"), fromWidePath(result.markdown_path));
    evidence.insert(QStringLiteral("json_path"), jsonPath);
    evidence.insert(QStringLiteral("fallback_count"), result.fallback_count);
    recordAcceptanceSample(hypothesis, confidence,
                           QStringLiteral("completed"), evidence);
}

void MainWindow::failAcceptanceSample(const QString& message) {
    QJsonObject evidence;
    evidence.insert(QStringLiteral("error"), message);
    recordAcceptanceSample({}, {}, QStringLiteral("failed"), evidence);
}

void MainWindow::recordAcceptanceSample(
    const QString& hypothesis,
    const acceptance::ConfidenceSummary& confidence,
    const QString& state,
    const QJsonObject& evidence) {
    if (!acceptanceRunning_ || acceptanceIndex_ >= acceptanceDataset_.samples.size())
        return;
    const acceptance::Sample& sample = acceptanceDataset_.samples[acceptanceIndex_];
    const acceptance::Score score =
        acceptance::compare(sample.reference, hypothesis, acceptanceDataset_);
    acceptanceReferenceCharacters_ += score.referenceCharacters;
    acceptanceHypothesisCharacters_ += score.hypothesisCharacters;
    acceptanceEditDistance_ += score.editDistance;
    if (confidence.count > 0) {
        acceptanceConfidenceCount_ += confidence.count;
        acceptanceConfidenceTotal_ += confidence.mean * confidence.count;
    }

    const QString sampleDirectory = QDir(acceptanceRunDirectory_)
        .filePath(QStringLiteral("%1_%2")
                      .arg(acceptanceIndex_ + 1, 4, 10, QLatin1Char('0'))
                      .arg(safeFileName(sample.id)));
    QString sampleState = state;
    QString writeError;
    const bool referenceWritten = acceptance::writeUtf8(
        QDir(sampleDirectory).filePath(QStringLiteral("reference.txt")),
        sample.reference, &writeError);
    const bool hypothesisWritten = referenceWritten && acceptance::writeUtf8(
        QDir(sampleDirectory).filePath(QStringLiteral("hypothesis.txt")),
        hypothesis, &writeError);
    if (!hypothesisWritten && sampleState == QLatin1String("completed"))
        sampleState = QStringLiteral("evidence_failed");

    QJsonObject result;
    result.insert(QStringLiteral("id"), sample.id);
    result.insert(QStringLiteral("input"), sample.inputPath);
    result.insert(QStringLiteral("input_sha256"), acceptance::sha256File(sample.inputPath));
    result.insert(QStringLiteral("reference_sha256"), sample.referenceSha256);
    result.insert(QStringLiteral("state"), sampleState);
    result.insert(QStringLiteral("reference_characters"),
                  static_cast<double>(score.referenceCharacters));
    result.insert(QStringLiteral("hypothesis_characters"),
                  static_cast<double>(score.hypothesisCharacters));
    result.insert(QStringLiteral("edit_distance"),
                  static_cast<double>(score.editDistance));
    result.insert(QStringLiteral("accuracy"), score.accuracy());
    if (confidence.count > 0) {
        QJsonObject confidenceObject;
        confidenceObject.insert(QStringLiteral("mean"), confidence.mean);
        confidenceObject.insert(QStringLiteral("minimum"), confidence.minimum);
        confidenceObject.insert(QStringLiteral("maximum"), confidence.maximum);
        confidenceObject.insert(QStringLiteral("count"), confidence.count);
        confidenceObject.insert(QStringLiteral("source"), confidence.source);
        confidenceObject.insert(QStringLiteral("calibrated"), false);
        result.insert(QStringLiteral("confidence"), confidenceObject);
    }
    QJsonObject recordedEvidence = evidence;
    if (!hypothesisWritten)
        recordedEvidence.insert(QStringLiteral("evidence_write_error"), writeError);
    result.insert(QStringLiteral("evidence"), recordedEvidence);
    if (!acceptance::writeJson(
            QDir(sampleDirectory).filePath(QStringLiteral("sample.json")),
            result, &writeError)
        && sampleState == QLatin1String("completed")) {
        sampleState = QStringLiteral("evidence_failed");
        result.insert(QStringLiteral("state"), sampleState);
        recordedEvidence.insert(QStringLiteral("evidence_write_error"), writeError);
        result.insert(QStringLiteral("evidence"), recordedEvidence);
    }
    if (sampleState != QLatin1String("completed"))
        ++acceptanceFailures_;
    acceptanceResults_.append(result);

    const int row = acceptanceTable_->rowCount();
    acceptanceTable_->insertRow(row);
    const QStringList values = {
        sample.id,
        sampleState == QLatin1String("completed") ? QStringLiteral("完成")
                                                  : QStringLiteral("失败"),
        QString::number(score.referenceCharacters),
        QString::number(score.editDistance),
        percent(score.accuracy()),
        confidence.count > 0 ? percent(confidence.mean) + QStringLiteral("（未校准）")
                             : QStringLiteral("不可用")};
    for (int column = 0; column < values.size(); ++column)
        acceptanceTable_->setItem(row, column, new QTableWidgetItem(values[column]));

    ++acceptanceIndex_;
    acceptanceProgress_->setValue(
        acceptanceDataset_.samples.isEmpty() ? 0
        : acceptanceIndex_ * 100 / acceptanceDataset_.samples.size());
    updateAcceptanceSummary();
    QTimer::singleShot(0, this, &MainWindow::startNextAcceptanceSample);
}

void MainWindow::updateAcceptanceSummary() {
    if (acceptanceReferenceCharacters_ > 0) {
        const double accuracy = 1.0
            - static_cast<double>(acceptanceEditDistance_)
                  / static_cast<double>(acceptanceReferenceCharacters_);
        acceptanceAccuracy_->setText(
            QStringLiteral("%1 · 距离 %2 / 真值 %3")
                .arg(percent(accuracy))
                .arg(acceptanceEditDistance_)
                .arg(acceptanceReferenceCharacters_));
    }
    acceptanceConfidence_->setText(
        acceptanceConfidenceCount_ > 0
            ? percent(acceptanceConfidenceTotal_ / acceptanceConfidenceCount_)
                  + QStringLiteral(" · 未校准")
            : QStringLiteral("不可用"));
}

void MainWindow::finishAcceptance(const QString& requestedState) {
    if (!acceptanceRunning_)
        return;
    const bool cancelled = requestedState == QLatin1String("cancelled");
    const double accuracy = acceptanceReferenceCharacters_ > 0
        ? 1.0 - static_cast<double>(acceptanceEditDistance_)
                    / static_cast<double>(acceptanceReferenceCharacters_)
        : 0.0;
    const bool complete = !cancelled
        && acceptanceIndex_ == acceptanceDataset_.samples.size();
    const bool valid = complete && acceptanceFailures_ == 0
        && acceptanceReferenceCharacters_ > 0;
    const bool passed = valid && accuracy >= acceptanceDataset_.threshold;
    const QString verdict = cancelled ? QStringLiteral("已取消")
        : !valid ? QStringLiteral("验证无效")
        : passed ? QStringLiteral("通过") : QStringLiteral("不通过");
    acceptanceVerdict_->setText(verdict);
    acceptanceVerdict_->setProperty("passed", passed);
    acceptanceVerdict_->style()->unpolish(acceptanceVerdict_);
    acceptanceVerdict_->style()->polish(acceptanceVerdict_);
    updateAcceptanceSummary();

    QJsonObject metrics;
    metrics.insert(QStringLiteral("reference_characters"),
                   static_cast<double>(acceptanceReferenceCharacters_));
    metrics.insert(QStringLiteral("hypothesis_characters"),
                   static_cast<double>(acceptanceHypothesisCharacters_));
    metrics.insert(QStringLiteral("edit_distance"),
                   static_cast<double>(acceptanceEditDistance_));
    metrics.insert(QStringLiteral("accuracy"), accuracy);
    metrics.insert(QStringLiteral("threshold"), acceptanceDataset_.threshold);
    metrics.insert(QStringLiteral("failed_samples"), acceptanceFailures_);
    if (acceptanceConfidenceCount_ > 0) {
        metrics.insert(QStringLiteral("mean_confidence"),
                       acceptanceConfidenceTotal_ / acceptanceConfidenceCount_);
        metrics.insert(QStringLiteral("confidence_samples"),
                       acceptanceConfidenceCount_);
        metrics.insert(QStringLiteral("confidence_calibrated"), false);
    }
    QJsonObject report;
    report.insert(QStringLiteral("schema"),
                  QStringLiteral("RecognitionStudio-AcceptanceReport/1"));
    report.insert(QStringLiteral("dataset_id"), acceptanceDataset_.id);
    report.insert(QStringLiteral("dataset_manifest"), acceptanceDataset_.manifestPath);
    report.insert(QStringLiteral("dataset_manifest_sha256"),
                  acceptanceDataset_.manifestSha256);
    report.insert(QStringLiteral("task_type"),
                  acceptanceDataset_.kind == acceptance::Kind::Scan
                      ? QStringLiteral("scan") : QStringLiteral("voice"));
    report.insert(QStringLiteral("completed_at"),
                  QDateTime::currentDateTime().toString(Qt::ISODate));
    QJsonObject runtimeEvidence;
    runtimeEvidence.insert(QStringLiteral("application_sha256"),
        acceptance::sha256File(QCoreApplication::applicationFilePath()));
    runtimeEvidence.insert(QStringLiteral("voice_core_sha256"),
        acceptance::sha256File(QDir(speechRuntimeRoot_)
                                   .filePath(QStringLiteral("VoiceEngineCore.dll"))));
    runtimeEvidence.insert(QStringLiteral("scan_core_sha256"),
        acceptance::sha256File(QDir(documentRuntimeRoot_)
                                   .filePath(QStringLiteral("ScanEngineCore.dll"))));
    runtimeEvidence.insert(QStringLiteral("voice_runtime_status"),
                           speechRuntimeStatus_->text());
    runtimeEvidence.insert(QStringLiteral("scan_runtime_status"),
                           documentRuntimeStatus_->text());
    report.insert(QStringLiteral("runtime"), runtimeEvidence);
    report.insert(QStringLiteral("verdict"),
                  cancelled ? QStringLiteral("cancelled")
                  : !valid ? QStringLiteral("invalid")
                  : passed ? QStringLiteral("passed") : QStringLiteral("failed"));
    report.insert(QStringLiteral("metrics"), metrics);
    report.insert(QStringLiteral("samples"), acceptanceResults_);
    QString writeError;
    if (!acceptance::writeJson(
            QDir(acceptanceRunDirectory_).filePath(QStringLiteral("report.json")),
            report, &writeError)) {
        statusBar()->showMessage(
            QStringLiteral("验收结束，但报告写入失败：%1").arg(writeError));
    } else {
        statusBar()->showMessage(
            QStringLiteral("验收验证结束：%1；报告已写入 %2")
                .arg(verdict, acceptanceRunDirectory_));
    }
    acceptanceRunning_ = false;
    acceptanceCancelRequested_ = false;
    updateControls();
}

void MainWindow::pollSdkTasks() {
    drainMicrophoneAudio();
    pollSpeechStream();

    if (speechTask_) {
        try {
            const auto status = speechTask_->poll();
            speechProgress_->setValue(
                static_cast<int>(status.progress * 100.0f));
            if (!status.interim_text.empty())
                speechInterim_->setPlainText(
                    QString::fromUtf8(status.interim_text.c_str()));
            if (status.finished()) {
                const bool acceptanceTask = acceptanceRunning_
                    && acceptanceDataset_.kind == acceptance::Kind::Voice;
                if (status.state == VE_TASK_DONE) {
                    const auto result = speechTask_->result();
                    speechTask_.reset();
                    if (acceptanceTask) {
                        completeAcceptanceVoice(result);
                    } else {
                        speechResult_->appendPlainText(
                            QString::fromUtf8(result.text.c_str()));
                        const auto confidence = acceptance::extractVoiceConfidence(
                            QByteArray::fromStdString(result.json));
                        if (confidence.count > 0) {
                            appendLog(speechResult_,
                                      QStringLiteral("模型置信度：%1（未校准）")
                                          .arg(percent(confidence.mean)));
                        }
                        statusBar()->showMessage(QStringLiteral("音频识别完成"));
                        speechProgress_->setValue(100);
                    }
                } else {
                    QString failure = status.state == VE_TASK_CANCELLED
                        ? QStringLiteral("音频识别已取消")
                        : QStringLiteral("音频识别失败（错误码 %1）")
                              .arg(static_cast<int>(status.error));
                    if (status.state == VE_TASK_FAILED) {
                        try {
                            (void)speechTask_->result();
                        } catch (const std::exception& error) {
                            failure += QStringLiteral("：%1")
                                           .arg(QString::fromUtf8(error.what()));
                        }
                    }
                    speechTask_.reset();
                    if (acceptanceTask) {
                        if (acceptanceCancelRequested_)
                            finishAcceptance(QStringLiteral("cancelled"));
                        else
                            failAcceptanceSample(failure);
                    } else {
                        appendLog(speechResult_, failure);
                        statusBar()->showMessage(failure);
                    }
                }
                speechInterim_->clear();
                updateControls();
            }
        } catch (const std::exception& error) {
            const QString message = QString::fromUtf8(error.what());
            speechTask_.reset();
            if (acceptanceRunning_
                && acceptanceDataset_.kind == acceptance::Kind::Voice)
                failAcceptanceSample(message);
            else
                appendLog(speechResult_, message);
            updateControls();
        }
    }

    if (documentTask_) {
        try {
            const auto status = documentTask_->poll();
            documentProgress_->setValue(
                static_cast<int>(status.progress * 100.0f));
            if (!status.message.empty())
                statusBar()->showMessage(
                    QString::fromUtf8(status.message.c_str()));
            if (status.finished()) {
                const bool acceptanceTask = acceptanceRunning_
                    && acceptanceDataset_.kind == acceptance::Kind::Scan;
                if (status.state == SE_TASK_DONE) {
                    const auto result = documentTask_->result();
                    documentTask_.reset();
                    if (acceptanceTask) {
                        completeAcceptanceDocument(result);
                    } else {
                        lastDocumentOutput_ = fromWidePath(result.output_dir);
                        appendLog(documentResult_, QStringLiteral("识别完成"));
                        appendLog(documentResult_,
                                  QStringLiteral("Excel：%1")
                                      .arg(fromWidePath(result.excel_path)));
                        appendLog(documentResult_,
                                  QStringLiteral("输出目录：%1")
                                      .arg(lastDocumentOutput_));
                        appendLog(documentResult_,
                                  QStringLiteral("中间结果：%1")
                                      .arg(fromWidePath(result.json_path)));
                        appendLog(documentResult_,
                                  QStringLiteral("fallback_count：%1")
                                      .arg(result.fallback_count));
                        const auto confidence = acceptance::extractScanConfidence(
                            fromWidePath(result.json_path));
                        if (confidence.count > 0) {
                            appendLog(documentResult_,
                                      QStringLiteral("中间分数均值：%1（%2 项，未校准）")
                                          .arg(percent(confidence.mean))
                                          .arg(confidence.count));
                        }
                        documentProgress_->setValue(100);
                        statusBar()->showMessage(QStringLiteral("纸质扫描识别完成"));
                    }
                } else {
                    const QString failure = status.state == SE_TASK_CANCELLED
                        ? QStringLiteral("纸质扫描识别已取消")
                        : QStringLiteral("纸质扫描识别失败：%1")
                              .arg(QString::fromUtf8(status.message.c_str()));
                    documentTask_.reset();
                    if (acceptanceTask) {
                        if (acceptanceCancelRequested_)
                            finishAcceptance(QStringLiteral("cancelled"));
                        else
                            failAcceptanceSample(failure);
                    } else {
                        appendLog(documentResult_, failure);
                    }
                }
                updateControls();
            }
        } catch (const std::exception& error) {
            const QString message = QString::fromUtf8(error.what());
            documentTask_.reset();
            if (acceptanceRunning_
                && acceptanceDataset_.kind == acceptance::Kind::Scan)
                failAcceptanceSample(message);
            else
                appendLog(documentResult_, message);
            updateControls();
        }
    }
}

void MainWindow::appendLog(QPlainTextEdit* output, const QString& text) {
    output->appendPlainText(
        QStringLiteral("[%1] %2")
            .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")),
                 text));
}

void MainWindow::showOperationError(const QString& title,
                                    const QString& text) {
    statusBar()->showMessage(text);
    QMessageBox::warning(this, title, text);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (operationActive()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("退出确认"),
            QStringLiteral("当前仍有任务运行，确定退出？"));
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }
    event->accept();
}

}  // namespace speechdoc

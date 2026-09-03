#include "main_window.h"
#include "acceptance_evidence_panel.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
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
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStatusBar>
#include <QStyle>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVariant>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

#include <cstdint>
#include <exception>
#include <functional>
#include <utility>
#include <vector>

#ifndef VOICEENGINE_SDK_CHECKSUMS_SHA256
#error VOICEENGINE_SDK_CHECKSUMS_SHA256 must be supplied by CMake
#endif
#ifndef SCANENGINE_SDK_CHECKSUMS_SHA256
#error SCANENGINE_SDK_CHECKSUMS_SHA256 must be supplied by CMake
#endif
#ifndef VOICEENGINE_SDK_MANIFEST_SHA256
#error VOICEENGINE_SDK_MANIFEST_SHA256 must be supplied by CMake
#endif
#ifndef SCANENGINE_SDK_MANIFEST_SHA256
#error SCANENGINE_SDK_MANIFEST_SHA256 must be supplied by CMake
#endif

namespace speechdoc {
namespace {

const auto kScanWorkerSchema = "RecognitionStudio-ScanWorker/1";
constexpr int kWorkerDiagnosticLimit = 256 * 1024;

std::wstring toWidePath(const QString& path) {
    return QDir::toNativeSeparators(path).toStdWString();
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

void addFileEvidence(QJsonObject* evidence, const QString& name,
                     const QString& path, const QString& sha256) {
    if (!evidence || path.isEmpty() || sha256.isEmpty())
        return;
    evidence->insert(name + QStringLiteral("_path"), path);
    evidence->insert(name + QStringLiteral("_sha256"), sha256);
}

QString sampleEvidenceDirectory(const QString& runDirectory, int index,
                                const QString& sampleId) {
    return QDir(runDirectory).filePath(
        QStringLiteral("%1_%2")
            .arg(index + 1, 4, 10, QLatin1Char('0'))
            .arg(safeFileName(sampleId)));
}

bool canonicalPathIsWithin(const QString& rootPath,
                           const QString& candidatePath) {
    const QString root = QFileInfo(rootPath).canonicalFilePath();
    const QString candidate = QFileInfo(candidatePath).canonicalFilePath();
    if (root.isEmpty() || candidate.isEmpty())
        return false;
    const QString relative = QDir(root).relativeFilePath(candidate);
    return relative == QLatin1String(".")
        || (!QDir::isAbsolutePath(relative)
            && relative != QLatin1String("..")
            && !relative.startsWith(QStringLiteral("../")));
}

struct SuiteRuntimeVerification {
    bool valid = false;
    QString error;
    QJsonObject evidence;
};

acceptance::RuntimePayloadVerification verifyPublishedPayload(
    const QString& runtimeRoot, bool voice,
    const std::atomic_bool* cancelled) {
    const QString checksumPath = QDir(runtimeRoot).filePath(
        QStringLiteral("SDK_SHA256SUMS.txt"));
    const QString buildChecksumAnchor = QString::fromLatin1(
        voice ? VOICEENGINE_SDK_CHECKSUMS_SHA256
              : SCANENGINE_SDK_CHECKSUMS_SHA256);
    const QString buildManifestAnchor = QString::fromLatin1(
        voice ? VOICEENGINE_SDK_MANIFEST_SHA256
              : SCANENGINE_SDK_MANIFEST_SHA256);
    QMap<QString, QString> entries;
    QString preparationError;
    if (!acceptance::loadSha256Manifest(
            checksumPath, &entries, &preparationError)) {
        entries.clear();
    }

    QVector<acceptance::RuntimeFileExpectation> expectations;
    for (auto iterator = entries.constBegin(); iterator != entries.constEnd();
         ++iterator) {
        const QString& manifestPath = iterator.key();
        if (!manifestPath.startsWith(QStringLiteral("bin/"))) {
            continue;
        }
        if (voice
            && (manifestPath.endsWith(QStringLiteral(".gguf.part1"))
                || manifestPath.endsWith(QStringLiteral(".gguf.part2")))) {
            continue;
        }
        acceptance::RuntimeFileExpectation expectation;
        expectation.manifestPath = manifestPath;
        expectation.runtimeRelativePath = manifestPath.mid(4);
        expectation.expectedSha256 = iterator.value();
        expectations.append(expectation);
    }
    acceptance::RuntimeFileExpectation sdkManifest;
    sdkManifest.manifestPath = QStringLiteral(
        "SDK_MANIFEST.json (build anchor)");
    sdkManifest.runtimeRelativePath = QStringLiteral("SDK_MANIFEST.json");
    sdkManifest.expectedSha256 = buildManifestAnchor;
    expectations.append(sdkManifest);

    if (voice) {
        const QString modelDirectory = QDir(runtimeRoot).filePath(
            QStringLiteral("models/qwen3-asr-1.7b"));
        const QString modelChecksums = QDir(modelDirectory).filePath(
            QStringLiteral("SHA256SUMS.txt"));
        QMap<QString, QString> modelEntries;
        QString modelError;
        if (!acceptance::loadSha256Manifest(
                modelChecksums, &modelEntries, &modelError)) {
            if (!preparationError.isEmpty())
                preparationError += QStringLiteral("；");
            preparationError += modelError;
        }
        acceptance::RuntimeFileExpectation materializedModel;
        materializedModel.manifestPath = QStringLiteral(
            "bin/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf "
            "(materialized from anchored parts)");
        materializedModel.runtimeRelativePath = QStringLiteral(
            "models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf");
        materializedModel.expectedSha256 = modelEntries.value(
            QStringLiteral("Qwen3-ASR-1.7B-bf16.gguf"));
        if (materializedModel.expectedSha256.isEmpty()) {
            if (!preparationError.isEmpty())
                preparationError += QStringLiteral("；");
            preparationError += QStringLiteral(
                "语音模型校验清单缺少合并 GGUF 条目");
        }
        expectations.append(materializedModel);
    }

    acceptance::RuntimePayloadVerification result =
        acceptance::verifyRuntimePayload(
            runtimeRoot, checksumPath, buildChecksumAnchor, expectations,
            cancelled);
    result.evidence.insert(
        QStringLiteral("verification_scope"),
        voice
            ? QStringLiteral(
                  "all deployed SDK bin files except removed GGUF parts; "
                  "materialized GGUF verified through the anchored model manifest")
            : QStringLiteral(
                  "all deployed SDK bin files, including NVRTC runtime headers"));
    if (!preparationError.isEmpty()) {
        result.valid = false;
        result.error = preparationError
            + (result.error.isEmpty()
                   ? QString() : QStringLiteral("；") + result.error);
        result.evidence.insert(QStringLiteral("matched"), false);
        result.evidence.insert(QStringLiteral("preparation_error"),
                               preparationError);
        result.evidence.insert(QStringLiteral("error"), result.error);
    }
    return result;
}

acceptance::RuntimePayloadVerification verifyHostRuntimeDependencies(
    const QString& applicationRoot, const QString& scanRuntimeRoot,
    const std::atomic_bool* cancelled) {
    acceptance::RuntimePayloadVerification result;
    QJsonObject evidence;
    const QString checksumPath = QDir(scanRuntimeRoot).filePath(
        QStringLiteral("SDK_SHA256SUMS.txt"));
    const QString expectedManifestHash = QString::fromLatin1(
        SCANENGINE_SDK_CHECKSUMS_SHA256).toLower();
    const QString actualManifestHash = acceptance::sha256File(
        checksumPath, cancelled);
    const bool manifestMatched = !actualManifestHash.isEmpty()
        && actualManifestHash == expectedManifestHash;
    QMap<QString, QString> entries;
    QString manifestError;
    const bool manifestLoaded = acceptance::loadSha256Manifest(
        checksumPath, &entries, &manifestError);

    const QStringList dependencyPaths = {
        QStringLiteral("concrt140.dll"),
        QStringLiteral("D3Dcompiler_47.dll"),
        QStringLiteral("iconengines/qsvgicon.dll"),
        QStringLiteral("imageformats/qgif.dll"),
        QStringLiteral("imageformats/qicns.dll"),
        QStringLiteral("imageformats/qico.dll"),
        QStringLiteral("imageformats/qjpeg.dll"),
        QStringLiteral("imageformats/qsvg.dll"),
        QStringLiteral("imageformats/qtga.dll"),
        QStringLiteral("imageformats/qtiff.dll"),
        QStringLiteral("imageformats/qwbmp.dll"),
        QStringLiteral("imageformats/qwebp.dll"),
        QStringLiteral("libEGL.dll"),
        QStringLiteral("libGLESV2.dll"),
        QStringLiteral("msvcp140.dll"),
        QStringLiteral("msvcp140_1.dll"),
        QStringLiteral("msvcp140_2.dll"),
        QStringLiteral("msvcp140_atomic_wait.dll"),
        QStringLiteral("msvcp140_codecvt_ids.dll"),
        QStringLiteral("opengl32sw.dll"),
        QStringLiteral("platforms/qwindows.dll"),
        QStringLiteral("Qt5Core.dll"),
        QStringLiteral("Qt5Gui.dll"),
        QStringLiteral("Qt5Svg.dll"),
        QStringLiteral("Qt5Widgets.dll"),
        QStringLiteral("styles/qwindowsvistastyle.dll"),
        QStringLiteral("vcruntime140.dll"),
        QStringLiteral("vcruntime140_1.dll")};

    bool dependenciesMatched = manifestMatched && manifestLoaded;
    qint64 checkedBytes = 0;
    QSet<QString> expectedPaths;
    QJsonArray files;
    QStringList errors;
    if (!manifestMatched) {
        errors.append(QStringLiteral(
            "宿主依赖所用 ScanEngine 校验清单与构建锚点不匹配"));
    }
    if (!manifestLoaded)
        errors.append(manifestError);
    for (const QString& relativePath : dependencyPaths) {
        if (cancelled && cancelled->load()) {
            dependenciesMatched = false;
            evidence.insert(QStringLiteral("cancelled"), true);
            errors.append(QStringLiteral("宿主运行依赖校验已取消"));
            break;
        }
        expectedPaths.insert(relativePath);
        const QString manifestPath = QStringLiteral("bin/") + relativePath;
        const QString expectedHash = entries.value(manifestPath).toLower();
        const QString runtimePath =
            QDir(applicationRoot).absoluteFilePath(relativePath);
        const QString actualHash = acceptance::sha256File(
            runtimePath, cancelled);
        const QFileInfo info(runtimePath);
        const bool matched = expectedHash.size() == 64
            && !actualHash.isEmpty() && actualHash == expectedHash;
        dependenciesMatched = dependenciesMatched && matched;
        if (info.isFile())
            checkedBytes += info.size();
        QJsonObject fileEvidence;
        fileEvidence.insert(QStringLiteral("manifest_path"), manifestPath);
        fileEvidence.insert(QStringLiteral("runtime_path"), runtimePath);
        fileEvidence.insert(QStringLiteral("expected_sha256"), expectedHash);
        fileEvidence.insert(QStringLiteral("actual_sha256"), actualHash);
        fileEvidence.insert(QStringLiteral("bytes"),
                            static_cast<double>(
                                info.isFile() ? info.size() : 0));
        fileEvidence.insert(QStringLiteral("matched"), matched);
        files.append(fileEvidence);
        if (!matched && errors.size() < 8) {
            errors.append(QStringLiteral("宿主运行依赖校验失败：%1")
                              .arg(relativePath));
        }
    }

    QSet<QString> actualPaths;
    QJsonArray unexpectedFiles;
    bool enumerationCancelled = false;
    QDirIterator iterator(
        QDir(applicationRoot).absolutePath(),
        QStringList{QStringLiteral("*.dll")},
        QDir::Files | QDir::Hidden | QDir::System,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        if (cancelled && cancelled->load()) {
            dependenciesMatched = false;
            enumerationCancelled = true;
            evidence.insert(QStringLiteral("cancelled"), true);
            if (!errors.contains(QStringLiteral("宿主运行依赖校验已取消")))
                errors.append(QStringLiteral("宿主运行依赖校验已取消"));
            break;
        }
        const QString path = iterator.next();
        const QString relativePath = QDir::cleanPath(
            QDir::fromNativeSeparators(
                QDir(applicationRoot).relativeFilePath(path)));
        if (relativePath.startsWith(QStringLiteral("components/"),
                                    Qt::CaseInsensitive)
            || relativePath.startsWith(QStringLiteral("licenses/"),
                                       Qt::CaseInsensitive)
            || relativePath.startsWith(QStringLiteral("output/"),
                                       Qt::CaseInsensitive)) {
            continue;
        }
        actualPaths.insert(relativePath);
        if (!expectedPaths.contains(relativePath))
            unexpectedFiles.append(relativePath);
    }
    const bool exactFileSetMatched = !enumerationCancelled
        && expectedPaths.size() == dependencyPaths.size()
        && actualPaths == expectedPaths && unexpectedFiles.isEmpty();
    dependenciesMatched = dependenciesMatched && exactFileSetMatched;
    if (!unexpectedFiles.isEmpty()) {
        errors.append(QStringLiteral("宿主目录包含 %1 个非发布依赖 DLL")
                          .arg(unexpectedFiles.size()));
    } else if (actualPaths != expectedPaths) {
        errors.append(QStringLiteral("宿主目录缺少发布依赖 DLL"));
    }

    result.valid = dependenciesMatched;
    result.error = errors.join(QStringLiteral("；"));
    evidence.insert(QStringLiteral("runtime_root"),
                    QDir(applicationRoot).absolutePath());
    evidence.insert(QStringLiteral("source_checksum_manifest"), checksumPath);
    evidence.insert(QStringLiteral("expected_checksum_manifest_sha256"),
                    expectedManifestHash);
    evidence.insert(QStringLiteral("actual_checksum_manifest_sha256"),
                    actualManifestHash);
    evidence.insert(QStringLiteral("checksum_manifest_matches_build_anchor"),
                    manifestMatched);
    evidence.insert(QStringLiteral("checked_file_count"), files.size());
    evidence.insert(QStringLiteral("checked_bytes"),
                    static_cast<double>(checkedBytes));
    evidence.insert(QStringLiteral("files"), files);
    evidence.insert(QStringLiteral("unexpected_files"), unexpectedFiles);
    evidence.insert(QStringLiteral("exact_file_set_match"),
                    exactFileSetMatched);
    evidence.insert(QStringLiteral("matched"), result.valid);
    if (!result.error.isEmpty())
        evidence.insert(QStringLiteral("error"), result.error);
    result.evidence = evidence;
    return result;
}

SuiteRuntimeVerification verifySuiteRuntime(
    const QString& voiceRuntimeRoot, const QString& scanRuntimeRoot,
    const std::atomic_bool* cancelled = nullptr) {
    SuiteRuntimeVerification result;
    const acceptance::RuntimePayloadVerification voice =
        verifyPublishedPayload(voiceRuntimeRoot, true, cancelled);
    const acceptance::RuntimePayloadVerification scan =
        verifyPublishedPayload(scanRuntimeRoot, false, cancelled);
    const acceptance::RuntimePayloadVerification host =
        verifyHostRuntimeDependencies(
            QCoreApplication::applicationDirPath(), scanRuntimeRoot,
            cancelled);
    const QString applicationPath = QCoreApplication::applicationFilePath();
    const QString workerPath = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("RecognitionStudioScanWorker.exe"));
    const QString applicationHash = acceptance::sha256File(
        applicationPath, cancelled);
    const QString workerHash = acceptance::sha256File(workerPath, cancelled);
    result.valid = voice.valid && scan.valid && host.valid
        && !applicationHash.isEmpty() && !workerHash.isEmpty();
    QStringList errors;
    if (!voice.error.isEmpty())
        errors.append(QStringLiteral("VoiceEngine：%1").arg(voice.error));
    if (!scan.error.isEmpty())
        errors.append(QStringLiteral("ScanEngine：%1").arg(scan.error));
    if (!host.error.isEmpty())
        errors.append(QStringLiteral("宿主运行依赖：%1").arg(host.error));
    if (applicationHash.isEmpty())
        errors.append(QStringLiteral("无法哈希 RecognitionStudio.exe"));
    if (workerHash.isEmpty())
        errors.append(QStringLiteral("无法哈希扫描工作进程"));
    result.error = errors.join(QStringLiteral("；"));
    QJsonObject evidence;
    evidence.insert(QStringLiteral("application_path"), applicationPath);
    evidence.insert(QStringLiteral("application_sha256"), applicationHash);
    evidence.insert(QStringLiteral("scan_worker_path"), workerPath);
    evidence.insert(QStringLiteral("scan_worker_sha256"), workerHash);
    evidence.insert(QStringLiteral("voice"), voice.evidence);
    evidence.insert(QStringLiteral("scan"), scan.evidence);
    evidence.insert(QStringLiteral("host_dependencies"), host.evidence);
    evidence.insert(QStringLiteral("matched"), result.valid);
    if (!result.error.isEmpty())
        evidence.insert(QStringLiteral("error"), result.error);
    result.evidence = evidence;
    return result;
}

QJsonObject findWorkerEvent(const QByteArray& output,
                            const QString& eventType) {
    const QList<QByteArray> lines = output.split('\n');
    for (const QByteArray& rawLine : lines) {
        const QJsonDocument document =
            QJsonDocument::fromJson(rawLine.trimmed());
        if (!document.isObject())
            continue;
        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("schema")).toString()
                == QLatin1String(kScanWorkerSchema)
            && object.value(QStringLiteral("event")).toString()
                == eventType) {
            return object;
        }
    }
    return {};
}

bool isReadableJsonFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError error{};
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &error);
    return error.error == QJsonParseError::NoError && !document.isNull();
}

QJsonObject readJsonObjectFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
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

bool MainWindow::publishedRuntimeMatches(QString* error) const {
    const SuiteRuntimeVerification verification =
        verifySuiteRuntime(speechRuntimeRoot_, documentRuntimeRoot_);
    if (error)
        *error = verification.error;
    return verification.valid;
}

MainWindow::~MainWindow() {
    closing_.store(true);
    acceptanceRuntimeAbort_.store(true);
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
    if (documentProcess_) {
        disconnect(documentProcess_, nullptr, this, nullptr);
        if (documentProcess_->state() != QProcess::NotRunning) {
            documentProcess_->kill();
            documentProcess_->waitForFinished(5000);
        }
        documentProcess_ = nullptr;
    }
    if (speechModelThread_.joinable())
        speechModelThread_.join();
    if (acceptanceRuntimeThread_.joinable())
        acceptanceRuntimeThread_.join();
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
    acceptanceConfidence_->setToolTip(QStringLiteral(
        "未校准：模型内部评分尚未用独立校准集映射为真实正确概率；"
        "98% 验收结论只由冻结真值与识别文本的编辑距离决定。"));
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
    acceptanceEvidenceButton_ =
        new QPushButton(QStringLiteral("查看所选证据"), acceptancePage);
    acceptanceEvidenceButton_->setObjectName(
        QStringLiteral("viewAcceptanceEvidenceButton"));
    acceptanceButtons->addWidget(acceptanceStartButton_);
    acceptanceButtons->addWidget(acceptanceCancelButton_);
    acceptanceButtons->addWidget(acceptanceEvidenceButton_);
    acceptanceButtons->addWidget(acceptanceOpenButton_);
    acceptanceButtons->addStretch(1);
    acceptanceLayout->addLayout(acceptanceButtons);

    acceptanceTable_ = new QTableWidget(acceptancePage);
    acceptanceTable_->setObjectName(QStringLiteral("acceptanceTable"));
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
    acceptanceTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    auto* evidenceSplitter = new QSplitter(Qt::Vertical, acceptancePage);
    evidenceSplitter->setObjectName(QStringLiteral("acceptanceEvidenceSplitter"));
    evidenceSplitter->addWidget(acceptanceTable_);
    acceptanceEvidencePanel_ = new AcceptanceEvidencePanel(evidenceSplitter);
    evidenceSplitter->addWidget(acceptanceEvidencePanel_);
    evidenceSplitter->setStretchFactor(0, 2);
    evidenceSplitter->setStretchFactor(1, 3);
    acceptanceLayout->addWidget(evidenceSplitter, 1);

    connect(acceptanceStartButton_, &QPushButton::clicked,
            this, &MainWindow::startAcceptance);
    connect(acceptanceCancelButton_, &QPushButton::clicked,
            this, &MainWindow::cancelAcceptance);
    connect(acceptanceOpenButton_, &QPushButton::clicked,
            this, &MainWindow::openAcceptanceOutput);
    connect(acceptanceEvidenceButton_, &QPushButton::clicked,
            this, &MainWindow::showSelectedAcceptanceEvidence);
    connect(acceptanceTable_, &QTableWidget::itemSelectionChanged,
            this, &MainWindow::showSelectedAcceptanceEvidence);
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

    const QString workerPath =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("RecognitionStudioScanWorker.exe"));
    if (!QFileInfo(workerPath).isFile()) {
        documentReady_ = false;
        setRuntimeStatus(
            documentRuntimeStatus_, false,
            QStringLiteral("扫描工作进程不存在：%1").arg(workerPath));
    } else {
        QProcess probe;
        probe.setProgram(workerPath);
        probe.setArguments({
            QStringLiteral("--check"),
            QStringLiteral("--runtime"), documentRuntimeRoot_});
        probe.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
        probe.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments* arguments) {
                arguments->flags |= CREATE_NO_WINDOW;
            });
#endif
        probe.start();
        const bool started = probe.waitForStarted(10000);
        const bool finished = started && probe.waitForFinished(30000);
        if (started && !finished) {
            probe.kill();
            probe.waitForFinished(5000);
        }
        const QJsonObject check = findWorkerEvent(
            probe.readAllStandardOutput(), QStringLiteral("check"));
        const bool scanGpuAvailable =
            check.value(QStringLiteral("gpu_available")).toBool(false);
        documentReady_ = finished && probe.exitCode() == 0
            && scanGpuAvailable
            && (!gpuArchitectureKnown || gpuArchitectureSupported);
        if (documentReady_) {
            setRuntimeStatus(
                documentRuntimeStatus_, true,
                QStringLiteral("就绪 · %1 · 独立 GPU 进程")
                    .arg(check.value(QStringLiteral("backend")).toString()));
        } else if (scanGpuAvailable && gpuArchitectureKnown
                   && !gpuArchitectureSupported) {
            setRuntimeStatus(
                documentRuntimeStatus_, false,
                QStringLiteral("GPU 架构不受支持；需要 RTX 40 (sm_89) 或 RTX 50 (sm_120)"));
        } else {
            QString detail = started
                ? QString::fromUtf8(probe.readAllStandardError()).trimmed()
                : probe.errorString();
            if (detail.isEmpty())
                detail = QStringLiteral("GPU 后端不可用或探测超时");
            setRuntimeStatus(documentRuntimeStatus_, false, detail);
        }
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
    return speechModelLoading_ || speechTask_ || documentProcess_
        || speechStream_ || acceptanceRunning_
        || (microphone_ && microphone_->running());
}

void MainWindow::updateControls() {
    const bool micRunning = microphone_ && microphone_->running();
    const bool speechBusy = speechModelLoading_ || speechTask_ || speechStream_
        || micRunning;
    const bool documentBusy = documentProcess_ != nullptr;
    const bool otherBusy = speechBusy || documentBusy || acceptanceRunning_;
    loadModelButton_->setEnabled(speechReady_ && !otherBusy);
    recognizeButton_->setEnabled(
        speechReady_ && speechContext_ && !otherBusy
        && !audioPath_->text().trimmed().isEmpty());
    microphoneButton_->setEnabled(
        speechReady_ && speechContext_
        && (micRunning || speechStream_
            || !otherBusy));
    microphoneButton_->setText(micRunning
        ? QStringLiteral("停止麦克风") : QStringLiteral("开始麦克风"));
    microphoneButton_->setProperty("recording", micRunning);
    microphoneButton_->style()->unpolish(microphoneButton_);
    microphoneButton_->style()->polish(microphoneButton_);
    cancelSpeechButton_->setEnabled(
        speechTask_ || speechStream_ || micRunning
        || (speechModelLoading_
            && (pendingSpeechRecognition_ || pendingMicrophoneStart_)));
    cancelSpeechButton_->setText(
        speechModelLoading_
            && (pendingSpeechRecognition_ || pendingMicrophoneStart_)
            ? QStringLiteral("取消后续语音操作") : QStringLiteral("取消"));

    parseButton_->setEnabled(
        documentReady_ && !otherBusy
        && !documentPath_->text().trimmed().isEmpty());
    cancelDocumentButton_->setEnabled(documentBusy);
    openOutputButton_->setEnabled(!lastDocumentOutput_.isEmpty());

    const bool acceptanceRuntimeReady = acceptanceDatasetLoaded_
        && (acceptanceDataset_.kind == acceptance::Kind::Scan
                ? documentReady_
                : speechReady_ && speechContext_);
    acceptanceStartButton_->setEnabled(
        acceptanceRuntimeReady && !operationActive());
    acceptanceCancelButton_->setEnabled(
        acceptanceRunning_ || pendingVoiceAcceptance_);
    acceptanceOpenButton_->setEnabled(!acceptanceRunDirectory_.isEmpty());
    acceptanceEvidenceButton_->setEnabled(
        acceptanceTable_->currentRow() >= 0
        && acceptanceTable_->currentRow() < acceptanceResults_.size());
    acceptanceManifestPath_->setEnabled(
        !acceptanceRunning_ && !pendingVoiceAcceptance_);
    acceptanceOutputPath_->setEnabled(
        !acceptanceRunning_ && !pendingVoiceAcceptance_);
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
            const bool resumeRecognition = ok && pendingSpeechRecognition_;
            const bool resumeMicrophone = ok && pendingMicrophoneStart_;
            const bool resumeAcceptance = ok && pendingVoiceAcceptance_;
            pendingSpeechRecognition_ = false;
            pendingMicrophoneStart_ = false;
            pendingVoiceAcceptance_ = false;
            updateControls();
            if (resumeRecognition) {
                QTimer::singleShot(0, this, &MainWindow::recognizeAudio);
            } else if (resumeMicrophone) {
                QTimer::singleShot(0, this, &MainWindow::toggleMicrophone);
            } else if (resumeAcceptance) {
                startAcceptance();
            }
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
        pendingSpeechRecognition_ = true;
        appendLog(speechResult_,
                  QStringLiteral("语音模型未驻留，正在自动加载…"));
        statusBar()->showMessage(
            QStringLiteral("正在加载语音模型，完成后将自动识别"));
        loadSpeechModel();
        return;
    }
    try {
        auto task = speechContext_->submit_file(toWidePath(path));
        speechTask_ = std::make_unique<voiceengine::Task>(std::move(task));
        speechProgress_->setRange(0, 0);
        speechInterim_->clear();
        appendLog(speechResult_, QStringLiteral("开始识别：%1").arg(path));
        statusBar()->showMessage(QStringLiteral("GPU 正在识别音频，请稍候"));
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
    if (operationActive()) {
        showOperationError(QStringLiteral("麦克风"),
                           QStringLiteral("请等待当前任务结束"));
        return;
    }
    if (!speechContext_->model_loaded()) {
        pendingMicrophoneStart_ = true;
        appendLog(speechResult_,
                  QStringLiteral("语音模型未驻留，正在自动加载；完成后将打开麦克风"));
        statusBar()->showMessage(
            QStringLiteral("正在加载语音模型，完成后将自动打开麦克风"));
        loadSpeechModel();
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
    if (speechModelLoading_
        && (pendingSpeechRecognition_ || pendingMicrophoneStart_)) {
        pendingSpeechRecognition_ = false;
        pendingMicrophoneStart_ = false;
        statusBar()->showMessage(QStringLiteral(
            "已取消后续语音操作；模型加载本身将安全完成"));
        updateControls();
        return;
    }
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
    if (!documentReady_ || operationActive())
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
    documentResult_->clear();
    appendLog(documentResult_, QStringLiteral("开始识别：%1").arg(input));
    startDocumentWorker(input, output);
    updateControls();
}

void MainWindow::cancelDocument() {
    if (!documentProcess_)
        return;
    documentWorkerCancelled_ = true;
    documentProcess_->kill();
    statusBar()->showMessage(QStringLiteral("正在取消文档任务"));
}

void MainWindow::openDocumentOutput() {
    if (!lastDocumentOutput_.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(lastDocumentOutput_));
}

void MainWindow::startDocumentWorker(const QString& input,
                                     const QString& output) {
    if (documentProcess_)
        return;

    if (speechContext_ && speechContext_->model_loaded()) {
        if (speechModelThread_.joinable())
            speechModelThread_.join();
        try {
            statusBar()->showMessage(
                QStringLiteral("正在释放语音模型显存并切换到扫描识别"));
            speechContext_->unload_model();
            appendLog(speechResult_,
                      QStringLiteral("已卸载语音模型，为扫描识别释放 GPU 显存"));
        } catch (const std::exception& error) {
            const QString message =
                QStringLiteral("无法释放语音模型：%1")
                    .arg(QString::fromUtf8(error.what()));
            if (acceptanceRunning_)
                failAcceptanceSample(message);
            else
                showOperationError(QStringLiteral("纸质扫描识别"), message);
            return;
        }
    }

    const QString workerPath =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("RecognitionStudioScanWorker.exe"));
    if (!QFileInfo(workerPath).isFile()) {
        const QString message =
            QStringLiteral("扫描工作进程不存在：%1").arg(workerPath);
        if (acceptanceRunning_)
            failAcceptanceSample(message);
        else
            showOperationError(QStringLiteral("纸质扫描识别"), message);
        return;
    }

    auto* process = new QProcess(this);
    documentProcess_ = process;
    documentWorkerStdout_.clear();
    documentWorkerTranscript_.clear();
    documentWorkerStderr_.clear();
    documentWorkerResult_ = QJsonObject();
    documentWorkerError_.clear();
    documentWorkerCancelled_ = false;
    documentProgress_->setRange(0, 100);
    documentProgress_->setValue(0);

    process->setProgram(workerPath);
    process->setArguments({
        QStringLiteral("--runtime"), documentRuntimeRoot_,
        QStringLiteral("--input"), input,
        QStringLiteral("--output"), output});
    process->setWorkingDirectory(QCoreApplication::applicationDirPath());
    process->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
        });
#endif
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process] {
                if (documentProcess_ == process)
                    readDocumentWorkerOutput();
            });
    connect(process, &QProcess::readyReadStandardError, this,
            [this, process] {
                if (documentProcess_ == process)
                    readDocumentWorkerErrors();
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
                if (documentProcess_ == process
                    && error == QProcess::FailedToStart) {
                    QTimer::singleShot(0, this,
                                       &MainWindow::failDocumentWorkerStart);
                }
            });
    connect(process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus status) {
                if (documentProcess_ == process) {
                    finishDocumentWorker(
                        exitCode, status == QProcess::NormalExit);
                }
            });
    process->start();
    statusBar()->showMessage(
        QStringLiteral("扫描工作进程已启动；完成后将自动释放 GPU 显存"));
}

void MainWindow::readDocumentWorkerOutput() {
    if (!documentProcess_)
        return;
    const QByteArray bytes = documentProcess_->readAllStandardOutput();
    documentWorkerStdout_ += bytes;
    documentWorkerTranscript_ += bytes;
    if (documentWorkerTranscript_.size() > kWorkerDiagnosticLimit) {
        documentWorkerTranscript_ =
            documentWorkerTranscript_.right(kWorkerDiagnosticLimit);
    }
    if (documentWorkerStdout_.size() > kWorkerDiagnosticLimit) {
        documentWorkerError_ = QStringLiteral(
            "扫描工作进程输出超过协议上限（可能缺少换行或协议已损坏）");
        documentWorkerStdout_ =
            documentWorkerStdout_.right(kWorkerDiagnosticLimit);
        documentProcess_->kill();
        return;
    }
    for (;;) {
        const int newline = documentWorkerStdout_.indexOf('\n');
        if (newline < 0)
            break;
        QByteArray line = documentWorkerStdout_.left(newline);
        documentWorkerStdout_.remove(0, newline + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        processDocumentWorkerLine(line);
    }
}

void MainWindow::readDocumentWorkerErrors() {
    if (!documentProcess_)
        return;
    documentWorkerStderr_ += documentProcess_->readAllStandardError();
    if (documentWorkerStderr_.size() > kWorkerDiagnosticLimit) {
        documentWorkerStderr_ =
            documentWorkerStderr_.right(kWorkerDiagnosticLimit);
    }
}

void MainWindow::processDocumentWorkerLine(const QByteArray& line) {
    if (line.trimmed().isEmpty())
        return;
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (!document.isObject()) {
        documentWorkerStderr_ += line + '\n';
        if (documentWorkerStderr_.size() > kWorkerDiagnosticLimit) {
            documentWorkerStderr_ =
                documentWorkerStderr_.right(kWorkerDiagnosticLimit);
        }
        return;
    }
    const QJsonObject event = document.object();
    if (event.value(QStringLiteral("schema")).toString()
        != QLatin1String(kScanWorkerSchema)) {
        documentWorkerStderr_ += line + '\n';
        if (documentWorkerStderr_.size() > kWorkerDiagnosticLimit) {
            documentWorkerStderr_ =
                documentWorkerStderr_.right(kWorkerDiagnosticLimit);
        }
        return;
    }
    const QString type = event.value(QStringLiteral("event")).toString();
    if (type == QLatin1String("progress")) {
        documentProgress_->setValue(
            qBound(0, event.value(QStringLiteral("progress")).toInt(), 100));
        const QString message =
            event.value(QStringLiteral("message")).toString().trimmed();
        if (!message.isEmpty())
            statusBar()->showMessage(message);
    } else if (type == QLatin1String("result")) {
        if (documentWorkerResult_.isEmpty())
            documentWorkerResult_ = event;
        else
            documentWorkerError_ =
                QStringLiteral("扫描工作进程返回了重复结果");
    } else if (type == QLatin1String("error")) {
        documentWorkerError_ =
            event.value(QStringLiteral("message")).toString();
    }
}

void MainWindow::failDocumentWorkerStart() {
    if (documentProcess_)
        finishDocumentWorker(-1, false);
}

void MainWindow::finishDocumentWorker(int exitCode, bool normalExit) {
    if (!documentProcess_)
        return;
    QProcess* process = documentProcess_;
    readDocumentWorkerOutput();
    if (!documentWorkerStdout_.trimmed().isEmpty()) {
        processDocumentWorkerLine(documentWorkerStdout_);
        documentWorkerStdout_.clear();
    }
    readDocumentWorkerErrors();
    disconnect(process, nullptr, this, nullptr);
    documentProcess_ = nullptr;
    process->deleteLater();
    documentProgress_->setRange(0, 100);

    const bool cancelled = documentWorkerCancelled_;
    const bool success = !cancelled && normalExit && exitCode == 0
        && documentWorkerError_.isEmpty()
        && !documentWorkerResult_.isEmpty();
    const bool acceptanceTask = acceptanceRunning_
        && acceptanceDataset_.kind == acceptance::Kind::Scan;
    if (cancelled) {
        documentProgress_->setValue(0);
        if (acceptanceTask)
            finishAcceptance(QStringLiteral("cancelled"));
        else {
            appendLog(documentResult_,
                      QStringLiteral("纸质扫描识别已取消"));
            statusBar()->showMessage(QStringLiteral("纸质扫描识别已取消"));
        }
    } else if (success) {
        if (acceptanceTask) {
            completeAcceptanceDocument(documentWorkerResult_);
        } else {
            lastDocumentOutput_ =
                documentWorkerResult_.value(QStringLiteral("output_dir"))
                    .toString();
            const QString excelPath =
                documentWorkerResult_.value(QStringLiteral("excel_path"))
                    .toString();
            const QString jsonPath =
                documentWorkerResult_.value(QStringLiteral("json_path"))
                    .toString();
            appendLog(documentResult_, QStringLiteral("识别完成"));
            appendLog(documentResult_,
                      QStringLiteral("Excel：%1").arg(excelPath));
            appendLog(documentResult_,
                      QStringLiteral("输出目录：%1").arg(lastDocumentOutput_));
            appendLog(documentResult_,
                      QStringLiteral("中间结果：%1").arg(jsonPath));
            appendLog(documentResult_,
                      QStringLiteral("fallback_count：%1")
                          .arg(documentWorkerResult_
                                   .value(QStringLiteral("fallback_count"))
                                   .toInt()));
            const auto confidence =
                acceptance::extractScanConfidence(jsonPath);
            if (confidence.count > 0) {
                appendLog(documentResult_,
                          QStringLiteral("中间分数均值：%1（%2 项，未校准）")
                              .arg(percent(confidence.mean))
                              .arg(confidence.count));
            }
            documentProgress_->setValue(100);
            statusBar()->showMessage(
                QStringLiteral("纸质扫描识别完成；扫描 GPU 资源已释放"));
        }
    } else {
        QString failure = documentWorkerError_.trimmed();
        if (failure.isEmpty())
            failure = QString::fromUtf8(documentWorkerStderr_).trimmed();
        if (failure.isEmpty()) {
            failure = QStringLiteral("扫描工作进程异常退出（退出码 %1）")
                          .arg(exitCode);
        }
        documentProgress_->setValue(0);
        if (acceptanceTask) {
            QJsonObject failureEvidence;
            failureEvidence.insert(QStringLiteral("worker_schema"),
                                   QString::fromLatin1(kScanWorkerSchema));
            failureEvidence.insert(QStringLiteral("exit_code"), exitCode);
            failureEvidence.insert(QStringLiteral("normal_exit"), normalExit);
            failureEvidence.insert(QStringLiteral("worker_error"),
                                   documentWorkerError_);
            failureEvidence.insert(QStringLiteral("stdout_tail_utf8"),
                                   QString::fromUtf8(documentWorkerTranscript_));
            failureEvidence.insert(QStringLiteral("stderr_tail_utf8"),
                                   QString::fromUtf8(documentWorkerStderr_));
            if (!documentWorkerResult_.isEmpty()) {
                failureEvidence.insert(QStringLiteral("worker_result"),
                                       documentWorkerResult_);
            }
            failAcceptanceSample(failure, failureEvidence);
        } else {
            appendLog(documentResult_,
                      QStringLiteral("纸质扫描识别失败：%1").arg(failure));
            statusBar()->showMessage(failure);
        }
    }
    updateControls();
}

void MainWindow::chooseAcceptanceManifest() {
    if (acceptanceRunning_ || pendingVoiceAcceptance_)
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
    acceptanceResults_ = QJsonArray();
    acceptanceReport_ = QJsonObject();
    acceptanceRuntimeStartEvidence_ = QJsonObject();
    acceptanceRuntimeStartValid_ = false;
    acceptanceRuntimeStartError_.clear();
    acceptanceRuntimeEndEvidence_ = QJsonObject();
    acceptanceRuntimeEndValid_ = false;
    acceptanceRuntimeEndVerified_ = false;
    acceptanceRuntimeEndError_.clear();
    pendingAcceptanceFinishState_.clear();
    acceptanceRuntimeAbort_.store(false);
    acceptanceHypotheses_.clear();
    acceptanceTable_->setRowCount(0);
    acceptanceEvidencePanel_->clearEvidence();
    updateControls();
}

void MainWindow::chooseAcceptanceOutput() {
    if (acceptanceRunning_ || pendingVoiceAcceptance_)
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
        if (!speechContext_) {
            showOperationError(QStringLiteral("验收验证"),
                               QStringLiteral("VoiceEngine 尚未就绪"));
            return;
        }
        pendingVoiceAcceptance_ = true;
        statusBar()->showMessage(
            QStringLiteral("正在加载语音模型，完成后将自动开始验收"));
        loadSpeechModel();
        return;
    }
    const QString outputRoot = acceptanceOutputPath_->text().trimmed();
    if (outputRoot.isEmpty() || !QDir().mkpath(outputRoot)) {
        showOperationError(QStringLiteral("验收验证"),
                           QStringLiteral("验收证据输出目录不可用"));
        return;
    }
    const QString runName =
        QDateTime::currentDateTime().toString(
            QStringLiteral("yyyyMMdd-HHmmss-zzz_"))
        + safeFileName(acceptanceDataset_.id)
        + QLatin1Char('_')
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
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
    acceptanceReport_ = QJsonObject();
    acceptanceRuntimeStartEvidence_ = QJsonObject();
    acceptanceRuntimeStartValid_ = false;
    acceptanceRuntimeStartError_.clear();
    acceptanceRuntimeEndEvidence_ = QJsonObject();
    acceptanceRuntimeEndValid_ = false;
    acceptanceRuntimeEndVerified_ = false;
    acceptanceRuntimeEndError_.clear();
    pendingAcceptanceFinishState_.clear();
    acceptanceRuntimeAbort_.store(false);
    acceptanceHypotheses_.clear();
    acceptanceTable_->setRowCount(0);
    acceptanceEvidencePanel_->clearEvidence();
    acceptanceProgress_->setValue(0);
    acceptanceAccuracy_->setText(QStringLiteral("—"));
    acceptanceConfidence_->setText(QStringLiteral("—"));
    acceptanceVerdict_->setText(QStringLiteral("验证中"));
    acceptanceVerdict_->setProperty("passed", QVariant());
    acceptanceVerdict_->style()->unpolish(acceptanceVerdict_);
    acceptanceVerdict_->style()->polish(acceptanceVerdict_);
    verifyAcceptanceRuntimeStart();
}

void MainWindow::verifyAcceptanceRuntimeStart() {
    if (!acceptanceRunning_ || acceptanceRuntimeVerifying_)
        return;
    if (acceptanceRuntimeThread_.joinable())
        acceptanceRuntimeThread_.join();
    acceptanceRuntimeAbort_.store(false);
    acceptanceRuntimeVerifying_ = true;
    acceptanceProgress_->setRange(0, 0);
    acceptanceVerdict_->setText(QStringLiteral("正在核验运行基线"));
    statusBar()->showMessage(QStringLiteral(
        "正在后台逐项核验实际 SDK、模型与构建时发布基线…"));
    updateControls();
    const QString voiceRoot = speechRuntimeRoot_;
    const QString scanRoot = documentRuntimeRoot_;
    acceptanceRuntimeThread_ = std::thread([this, voiceRoot, scanRoot] {
        const SuiteRuntimeVerification verification =
            verifySuiteRuntime(voiceRoot, scanRoot,
                               &acceptanceRuntimeAbort_);
        if (closing_.load())
            return;
        QMetaObject::invokeMethod(
            this, [this, verification] {
                if (acceptanceRuntimeThread_.joinable())
                    acceptanceRuntimeThread_.join();
                acceptanceRuntimeVerifying_ = false;
                acceptanceProgress_->setRange(0, 100);
                acceptanceProgress_->setValue(0);
                if (!acceptanceRunning_)
                    return;
                acceptanceRuntimeStartEvidence_ = verification.evidence;
                acceptanceRuntimeStartValid_ = verification.valid;
                acceptanceRuntimeStartError_ = verification.error;
                if (acceptanceCancelRequested_) {
                    finishAcceptance(QStringLiteral("cancelled"));
                } else if (!verification.valid) {
                    ++acceptanceFailures_;
                    acceptanceVerdict_->setText(
                        QStringLiteral("验证无效（运行基线不匹配）"));
                    statusBar()->showMessage(
                        QStringLiteral("运行基线校验失败：%1")
                            .arg(verification.error));
                    finishAcceptance(QStringLiteral("runtime_invalid"));
                } else {
                    pendingAcceptanceFinishState_.clear();
                    acceptanceVerdict_->setText(QStringLiteral("验证中"));
                    statusBar()->showMessage(QStringLiteral(
                        "发布基线匹配，正在执行冻结测试集验收验证"));
                    updateControls();
                    startNextAcceptanceSample();
                }
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::verifyAcceptanceRuntimeEnd(const QString& requestedState) {
    if (!acceptanceRunning_ || acceptanceRuntimeVerifying_)
        return;
    if (acceptanceRuntimeThread_.joinable())
        acceptanceRuntimeThread_.join();
    acceptanceRuntimeAbort_.store(false);
    acceptanceRuntimeVerifying_ = true;
    pendingAcceptanceFinishState_ = requestedState;
    acceptanceProgress_->setRange(0, 0);
    acceptanceVerdict_->setText(QStringLiteral("正在复核运行基线"));
    statusBar()->showMessage(QStringLiteral(
        "正在后台复核实际执行的 SDK、模型与发布基线…"));
    updateControls();
    const QString voiceRoot = speechRuntimeRoot_;
    const QString scanRoot = documentRuntimeRoot_;
    acceptanceRuntimeThread_ = std::thread([this, voiceRoot, scanRoot] {
        const SuiteRuntimeVerification verification =
            verifySuiteRuntime(voiceRoot, scanRoot,
                               &acceptanceRuntimeAbort_);
        if (closing_.load())
            return;
        QMetaObject::invokeMethod(
            this, [this, verification] {
                if (acceptanceRuntimeThread_.joinable())
                    acceptanceRuntimeThread_.join();
                acceptanceRuntimeVerifying_ = false;
                acceptanceRuntimeEndEvidence_ = verification.evidence;
                acceptanceRuntimeEndValid_ = verification.valid;
                acceptanceRuntimeEndVerified_ = true;
                acceptanceRuntimeEndError_ = verification.error;
                acceptanceProgress_->setRange(0, 100);
                acceptanceProgress_->setValue(100);
                const QString requestedState = acceptanceCancelRequested_
                    ? QStringLiteral("cancelled")
                    : pendingAcceptanceFinishState_;
                pendingAcceptanceFinishState_.clear();
                finishAcceptance(requestedState);
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::cancelAcceptance() {
    if (!acceptanceRunning_ && pendingVoiceAcceptance_) {
        pendingVoiceAcceptance_ = false;
        statusBar()->showMessage(QStringLiteral(
            "已取消自动开始验收；模型加载本身将安全完成"));
        updateControls();
        return;
    }
    if (!acceptanceRunning_)
        return;
    acceptanceCancelRequested_ = true;
    acceptanceRuntimeAbort_.store(true);
    try {
        if (acceptanceDataset_.kind == acceptance::Kind::Voice && speechTask_)
            speechTask_->cancel();
        if (acceptanceDataset_.kind == acceptance::Kind::Scan
            && documentProcess_) {
            documentWorkerCancelled_ = true;
            documentProcess_->kill();
        }
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()));
    }
    acceptanceVerdict_->setText(QStringLiteral("正在取消"));
    if (!speechTask_ && !documentProcess_)
        finishAcceptance(QStringLiteral("cancelled"));
}

void MainWindow::openAcceptanceOutput() {
    if (!acceptanceRunDirectory_.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(acceptanceRunDirectory_));
}

void MainWindow::showSelectedAcceptanceEvidence() {
    const int row = acceptanceTable_->currentRow();
    if (row < 0) {
        updateControls();
        return;
    }
    const QTableWidgetItem* firstItem = acceptanceTable_->item(row, 0);
    const int resultIndex = firstItem
        ? firstItem->data(Qt::UserRole).toInt() : row;
    if (resultIndex < 0 || resultIndex >= acceptanceResults_.size()
        || resultIndex >= acceptanceHypotheses_.size()
        || resultIndex >= acceptanceDataset_.samples.size()) {
        updateControls();
        return;
    }
    const acceptance::Sample& sample =
        acceptanceDataset_.samples[resultIndex];
    acceptanceEvidencePanel_->showSample(
        sample.id, sample.reference, acceptanceHypotheses_.at(resultIndex),
        acceptanceResults_.at(resultIndex).toObject());
    acceptanceEvidencePanel_->focusSample();
    updateControls();
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
    const QString sampleDirectory = sampleEvidenceDirectory(
        acceptanceRunDirectory_, acceptanceIndex_, sample.id);
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
            speechProgress_->setRange(0, 0);
        } else {
            startDocumentWorker(sample.inputPath, sampleDirectory);
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
    const QByteArray rawResult = QByteArray::fromStdString(result.json);
    QJsonParseError parseError{};
    const QJsonDocument engineDocument = QJsonDocument::fromJson(
        rawResult, &parseError);
    if (!engineDocument.isObject()) {
        QJsonObject evidence;
        evidence.insert(QStringLiteral("raw_result_parse_error"),
                        parseError.errorString());
        failAcceptanceSample(
            QStringLiteral("语音原始结果 JSON 无效：%1")
                .arg(parseError.errorString()), evidence, rawResult);
        return;
    }
    const QJsonObject engineResult = engineDocument.object();
    QJsonObject evidence;
    evidence.insert(QStringLiteral("engine_result"), engineResult);
    evidence.insert(QStringLiteral("duration_sec"), result.duration_sec);
    if (engineResult.value(QStringLiteral("schema_version")).toInt(-1) != 2
        || !engineResult.value(QStringLiteral("text")).isString()) {
        failAcceptanceSample(
            QStringLiteral("语音原始结果缺少受支持的 schema_version=2 或 text"),
            evidence, rawResult);
        return;
    }
    const QString rawText =
        engineResult.value(QStringLiteral("text")).toString();
    const QString abiText = QString::fromUtf8(result.text.c_str());
    if (rawText != abiText) {
        evidence.insert(QStringLiteral("abi_text"), abiText);
        evidence.insert(QStringLiteral("raw_json_text"), rawText);
        failAcceptanceSample(
            QStringLiteral("语音 ABI 文本与原始 JSON 文本不一致"),
            evidence, rawResult);
        return;
    }
    const acceptance::ConfidenceSummary confidence =
        acceptance::extractVoiceConfidence(rawResult);
    if (confidence.count <= 0) {
        failAcceptanceSample(
            QStringLiteral("语音结果缺少可用置信度证据"),
            evidence, rawResult);
        return;
    }
    recordAcceptanceSample(rawText, confidence,
                           QStringLiteral("completed"), evidence, rawResult);
}

void MainWindow::completeAcceptanceDocument(const QJsonObject& result) {
    const QString jsonPath =
        result.value(QStringLiteral("json_path")).toString();
    const QString excelPath =
        result.value(QStringLiteral("excel_path")).toString();
    const QString markdownPath =
        result.value(QStringLiteral("markdown_path")).toString();
    const QFileInfo contentInfo(jsonPath);
    QString stem = contentInfo.completeBaseName();
    stem.remove(QRegularExpression(QStringLiteral("_content_list$")));
    const QString middlePath =
        contentInfo.dir().filePath(stem + QStringLiteral("_middle.json"));
    const QString modelPath =
        contentInfo.dir().filePath(stem + QStringLiteral("_model.json"));
    const QString outputDirectory =
        result.value(QStringLiteral("output_dir")).toString();
    const QString sampleDirectory = sampleEvidenceDirectory(
        acceptanceRunDirectory_, acceptanceIndex_,
        acceptanceDataset_.samples.at(acceptanceIndex_).id);
    QJsonObject evidence;
    evidence.insert(QStringLiteral("worker_schema"),
                    QString::fromLatin1(kScanWorkerSchema));
    evidence.insert(QStringLiteral("worker_result"), result);
    evidence.insert(QStringLiteral("expected_sample_directory"),
                    sampleDirectory);
    evidence.insert(QStringLiteral("output_dir"), outputDirectory);
    if (!QFileInfo(outputDirectory).isAbsolute()
        || !QFileInfo(outputDirectory).isDir()
        || !canonicalPathIsWithin(sampleDirectory, outputDirectory)) {
        failAcceptanceSample(
            QStringLiteral("扫描输出目录不属于当前验收样本：%1")
                .arg(outputDirectory), evidence);
        return;
    }

    struct NamedEvidenceFile {
        QString name;
        QString path;
        QString requiredSuffix;
        bool json = false;
    };
    const QVector<NamedEvidenceFile> requiredEvidence = {
        {QStringLiteral("excel"), excelPath, QStringLiteral(".xlsx"), false},
        {QStringLiteral("markdown"), markdownPath, QStringLiteral(".md"), false},
        {QStringLiteral("content_list"), jsonPath,
         QStringLiteral("_content_list.json"), true},
        {QStringLiteral("middle"), middlePath,
         QStringLiteral("_middle.json"), true},
        {QStringLiteral("model"), modelPath,
         QStringLiteral("_model.json"), true}};
    QMap<QString, QString> evidenceHashes;
    for (const NamedEvidenceFile& item : requiredEvidence) {
        const QFileInfo info(item.path);
        if (!info.isAbsolute() || !info.isFile()
            || !canonicalPathIsWithin(outputDirectory, item.path)
            || !canonicalPathIsWithin(sampleDirectory, item.path)
            || !item.path.endsWith(item.requiredSuffix,
                                   Qt::CaseInsensitive)) {
            failAcceptanceSample(
                QStringLiteral("扫描证据路径不属于当前样本或类型不符：%1")
                    .arg(item.path), evidence);
            return;
        }
        const QString hash = acceptance::sha256File(item.path);
        if (hash.isEmpty()) {
            failAcceptanceSample(
                QStringLiteral("扫描证据文件无法哈希：%1").arg(item.path),
                evidence);
            return;
        }
        evidenceHashes.insert(item.name, hash);
        addFileEvidence(&evidence, item.name, item.path, hash);
    }
    for (const NamedEvidenceFile& item : requiredEvidence) {
        if (item.json && !isReadableJsonFile(item.path)) {
            failAcceptanceSample(
                QStringLiteral("扫描证据 JSON 无效：%1").arg(item.path),
                evidence);
            return;
        }
    }
    QString error;
    const QString hypothesis = acceptance::extractScanText(jsonPath, &error);
    if (!error.isEmpty()) {
        failAcceptanceSample(error, evidence);
        return;
    }
    const acceptance::ConfidenceSummary confidence =
        acceptance::extractScanConfidence(jsonPath);
    if (confidence.count <= 0) {
        failAcceptanceSample(
            QStringLiteral("扫描中间结果缺少可用置信度证据"), evidence);
        return;
    }
    for (const NamedEvidenceFile& item : requiredEvidence) {
        if (acceptance::sha256File(item.path)
            != evidenceHashes.value(item.name)) {
            failAcceptanceSample(
                QStringLiteral("扫描证据在读取期间发生变化：%1")
                    .arg(item.path), evidence);
            return;
        }
    }
    evidence.insert(QStringLiteral("fallback_count"),
                    result.value(QStringLiteral("fallback_count")).toInt());
    recordAcceptanceSample(hypothesis, confidence,
                           QStringLiteral("completed"), evidence);
}

void MainWindow::failAcceptanceSample(const QString& message,
                                      const QJsonObject& evidence,
                                      const QByteArray& rawResult) {
    QJsonObject failureEvidence = evidence;
    failureEvidence.insert(QStringLiteral("error"), message);
    recordAcceptanceSample({}, {}, QStringLiteral("failed"),
                           failureEvidence, rawResult);
}

void MainWindow::recordAcceptanceSample(
    const QString& hypothesis,
    const acceptance::ConfidenceSummary& confidence,
    const QString& state,
    const QJsonObject& evidence,
    const QByteArray& rawResult) {
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

    const QString sampleDirectory = sampleEvidenceDirectory(
        acceptanceRunDirectory_, acceptanceIndex_, sample.id);
    const QString referencePath =
        QDir(sampleDirectory).filePath(QStringLiteral("reference.txt"));
    const QString hypothesisPath =
        QDir(sampleDirectory).filePath(QStringLiteral("hypothesis.txt"));
    const QString sampleJsonPath =
        QDir(sampleDirectory).filePath(QStringLiteral("sample.json"));
    QString sampleState = state;
    QString writeError;
    const QString observedInputSha256 =
        acceptance::sha256File(sample.inputPath);
    if (observedInputSha256 != sample.inputSha256
        && sampleState == QLatin1String("completed")) {
        sampleState = QStringLiteral("evidence_failed");
        writeError = QStringLiteral("识别期间输入 SHA-256 发生变化");
    }
    const bool referenceWritten = acceptance::writeUtf8(
        referencePath, sample.reference, &writeError);
    const bool hypothesisWritten = referenceWritten && acceptance::writeUtf8(
        hypothesisPath, hypothesis, &writeError);
    if (!hypothesisWritten && sampleState == QLatin1String("completed"))
        sampleState = QStringLiteral("evidence_failed");
    const QString referenceEvidenceSha256 = referenceWritten
        ? acceptance::sha256File(referencePath) : QString();
    const QString hypothesisEvidenceSha256 = hypothesisWritten
        ? acceptance::sha256File(hypothesisPath) : QString();
    if (sampleState == QLatin1String("completed")
        && (referenceEvidenceSha256.isEmpty()
            || hypothesisEvidenceSha256.isEmpty())) {
        sampleState = QStringLiteral("evidence_failed");
        writeError = QStringLiteral(
            "无法计算冻结真值或识别文本证据的 SHA-256");
    }

    QJsonObject recordedEvidence = evidence;
    if (!rawResult.isEmpty()) {
        const QString rawResultPath =
            QDir(sampleDirectory).filePath(QStringLiteral("raw-result.json"));
        if (acceptance::writeBytes(
                rawResultPath, rawResult, &writeError)) {
            const QString rawResultSha256 =
                acceptance::sha256File(rawResultPath);
            const QString expectedRawResultSha256 =
                acceptance::sha256Bytes(rawResult);
            recordedEvidence.insert(QStringLiteral("raw_result_path"),
                                    rawResultPath);
            recordedEvidence.insert(QStringLiteral("raw_result_sha256"),
                                    rawResultSha256);
            recordedEvidence.insert(
                QStringLiteral("expected_raw_result_sha256"),
                expectedRawResultSha256);
            if ((rawResultSha256.isEmpty()
                 || rawResultSha256 != expectedRawResultSha256)
                && sampleState == QLatin1String("completed")) {
                sampleState = QStringLiteral("evidence_failed");
                writeError = QStringLiteral(
                    "语音原始结果写入后 SHA-256 不一致");
            }
        } else if (sampleState == QLatin1String("completed")) {
            sampleState = QStringLiteral("evidence_failed");
        }
    }

    QJsonObject result;
    result.insert(QStringLiteral("id"), sample.id);
    result.insert(QStringLiteral("input"), sample.inputPath);
    result.insert(QStringLiteral("expected_input_sha256"),
                  sample.inputSha256);
    result.insert(QStringLiteral("input_sha256"),
                  observedInputSha256);
    result.insert(QStringLiteral("reference_sha256"), sample.referenceSha256);
    result.insert(QStringLiteral("hypothesis_sha256"),
                  hypothesisEvidenceSha256);
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
    recordedEvidence.insert(QStringLiteral("sample_directory"),
                            sampleDirectory);
    recordedEvidence.insert(QStringLiteral("reference_path"), referencePath);
    recordedEvidence.insert(QStringLiteral("reference_evidence_sha256"),
                            referenceEvidenceSha256);
    recordedEvidence.insert(QStringLiteral("hypothesis_path"),
                            hypothesisPath);
    recordedEvidence.insert(QStringLiteral("hypothesis_sha256"),
                            hypothesisEvidenceSha256);
    recordedEvidence.insert(QStringLiteral("sample_json_path"),
                            sampleJsonPath);
    if (!writeError.isEmpty()) {
        recordedEvidence.insert(QStringLiteral("evidence_write_error"), writeError);
    }
    result.insert(QStringLiteral("evidence"), recordedEvidence);
    bool sampleJsonWritten =
        acceptance::writeJson(sampleJsonPath, result, &writeError);
    if (!sampleJsonWritten) {
        if (sampleState == QLatin1String("completed"))
            sampleState = QStringLiteral("evidence_failed");
        recordedEvidence.insert(QStringLiteral("evidence_write_error"), writeError);
        result.insert(QStringLiteral("state"), sampleState);
        result.insert(QStringLiteral("evidence"), recordedEvidence);
        QString recoveryError;
        sampleJsonWritten = acceptance::writeJson(
            sampleJsonPath, result, &recoveryError);
        if (!sampleJsonWritten && !recoveryError.isEmpty()) {
            recordedEvidence.insert(
                QStringLiteral("evidence_write_error"),
                writeError + QStringLiteral("；无效样本记录恢复失败：%1")
                                 .arg(recoveryError));
            result.insert(QStringLiteral("evidence"), recordedEvidence);
        }
    }
    QString sampleJsonSha256 = sampleJsonWritten
        ? acceptance::sha256File(sampleJsonPath) : QString();
    if (sampleJsonSha256.isEmpty()) {
        if (sampleState == QLatin1String("completed"))
            sampleState = QStringLiteral("evidence_failed");
        recordedEvidence.insert(
            QStringLiteral("evidence_write_error"),
            QStringLiteral("无法计算样本 JSON SHA-256"));
        result.insert(QStringLiteral("state"), sampleState);
        result.insert(QStringLiteral("evidence"), recordedEvidence);
        QString recoveryError;
        if (acceptance::writeJson(sampleJsonPath, result, &recoveryError))
            sampleJsonSha256 = acceptance::sha256File(sampleJsonPath);
        if (sampleJsonSha256.isEmpty() && !recoveryError.isEmpty()) {
            recordedEvidence.insert(
                QStringLiteral("evidence_write_error"),
                QStringLiteral("无法计算样本 JSON SHA-256；恢复失败：%1")
                    .arg(recoveryError));
        }
    }
    recordedEvidence.insert(QStringLiteral("sample_json_sha256"),
                            sampleJsonSha256);
    result.insert(QStringLiteral("state"), sampleState);
    result.insert(QStringLiteral("evidence"), recordedEvidence);
    if (sampleState != QLatin1String("completed"))
        ++acceptanceFailures_;
    acceptanceResults_.append(result);
    acceptanceHypotheses_.append(hypothesis);

    const int row = acceptanceTable_->rowCount();
    acceptanceTable_->insertRow(row);
    const QString displayState =
        sampleState == QLatin1String("completed") ? QStringLiteral("完成")
        : sampleState == QLatin1String("evidence_failed")
            ? QStringLiteral("证据失败") : QStringLiteral("识别失败");
    const QStringList values = {
        sample.id,
        displayState,
        QString::number(score.referenceCharacters),
        QString::number(score.editDistance),
        percent(score.accuracy()),
        confidence.count > 0 ? percent(confidence.mean) + QStringLiteral("（未校准）")
                             : QStringLiteral("不可用")};
    for (int column = 0; column < values.size(); ++column) {
        auto* item = new QTableWidgetItem(values[column]);
        item->setData(Qt::UserRole, acceptanceResults_.size() - 1);
        acceptanceTable_->setItem(row, column, item);
    }
    acceptanceTable_->selectRow(row);
    showSelectedAcceptanceEvidence();

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
    if (acceptanceRuntimeVerifying_) {
        if (cancelled || pendingAcceptanceFinishState_.isEmpty())
            pendingAcceptanceFinishState_ = requestedState;
        return;
    }
    if (!cancelled && acceptanceRuntimeStartValid_
        && !acceptanceRuntimeEndVerified_) {
        verifyAcceptanceRuntimeEnd(requestedState);
        return;
    }
    const double accuracy = acceptanceReferenceCharacters_ > 0
        ? 1.0 - static_cast<double>(acceptanceEditDistance_)
                    / static_cast<double>(acceptanceReferenceCharacters_)
        : 0.0;
    const bool complete = !cancelled
        && acceptanceIndex_ == acceptanceDataset_.samples.size();
    bool valid = complete && acceptanceFailures_ == 0
        && acceptanceReferenceCharacters_ > 0;
    bool passed = false;
    QString verdict;
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
        int confidenceSampleCount = 0;
        QJsonArray confidenceSources;
        for (const QJsonValue& value : acceptanceResults_) {
            const QJsonObject confidence =
                value.toObject().value(QStringLiteral("confidence")).toObject();
            if (confidence.isEmpty())
                continue;
            ++confidenceSampleCount;
            const QString source =
                confidence.value(QStringLiteral("source")).toString();
            if (!source.isEmpty()
                && !confidenceSources.contains(QJsonValue(source))) {
                confidenceSources.append(source);
            }
        }
        metrics.insert(QStringLiteral("mean_confidence"),
                       acceptanceConfidenceTotal_ / acceptanceConfidenceCount_);
        metrics.insert(QStringLiteral("confidence_observations"),
                       acceptanceConfidenceCount_);
        metrics.insert(QStringLiteral("confidence_sample_count"),
                       confidenceSampleCount);
        metrics.insert(QStringLiteral("confidence_sources"),
                       confidenceSources);
        metrics.insert(QStringLiteral("confidence_aggregation"),
                       QStringLiteral(
                           "observation-weighted mean of per-sample means"));
        metrics.insert(QStringLiteral("confidence_calibrated"), false);
        metrics.insert(QStringLiteral("confidence_semantics"),
                       QStringLiteral("diagnostic_only"));
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
    report.insert(QStringLiteral("run_id"),
                  QFileInfo(acceptanceRunDirectory_).fileName());
    report.insert(QStringLiteral("run_directory"),
                  acceptanceRunDirectory_);
    report.insert(QStringLiteral("completed_at"),
                  QDateTime::currentDateTime().toString(Qt::ISODate));
    if (!requestedState.isEmpty())
        report.insert(QStringLiteral("termination_state"), requestedState);
    QJsonObject runtimeEvidence;
    runtimeEvidence.insert(QStringLiteral("start"),
                           acceptanceRuntimeStartEvidence_);
    runtimeEvidence.insert(QStringLiteral("start_matches_published_baseline"),
                           acceptanceRuntimeStartValid_);
    if (!acceptanceRuntimeStartError_.isEmpty()) {
        runtimeEvidence.insert(QStringLiteral("start_error"),
                               acceptanceRuntimeStartError_);
    }
    const bool hasEndEvidence = acceptanceRuntimeEndVerified_;
    const bool verifyEnd = !cancelled && acceptanceRuntimeStartValid_
        && hasEndEvidence;
    if (hasEndEvidence) {
        runtimeEvidence.insert(QStringLiteral("end"),
                               acceptanceRuntimeEndEvidence_);
        runtimeEvidence.insert(QStringLiteral("end_matches_published_baseline"),
                               acceptanceRuntimeEndValid_);
        if (!acceptanceRuntimeEndError_.isEmpty()) {
            runtimeEvidence.insert(QStringLiteral("end_error"),
                                   acceptanceRuntimeEndError_);
        }
        if (cancelled) {
            runtimeEvidence.insert(QStringLiteral("end_verification_cancelled"),
                                   true);
        }
    } else {
        runtimeEvidence.insert(
            QStringLiteral("end_verification_skipped"),
            cancelled ? QStringLiteral("validation cancelled")
                      : QStringLiteral("start baseline verification failed"));
    }
    const bool executionIdentityStable = verifyEnd && acceptanceRuntimeEndValid_
        && acceptanceRuntimeStartEvidence_
               .value(QStringLiteral("application_sha256")).toString()
            == acceptanceRuntimeEndEvidence_
                   .value(QStringLiteral("application_sha256")).toString()
        && acceptanceRuntimeStartEvidence_
               .value(QStringLiteral("scan_worker_sha256")).toString()
            == acceptanceRuntimeEndEvidence_
                   .value(QStringLiteral("scan_worker_sha256")).toString();
    const bool runtimePayloadsValid = acceptanceRuntimeStartValid_
        && acceptanceRuntimeEndValid_ && acceptanceRuntimeEndVerified_
        && executionIdentityStable;
    runtimeEvidence.insert(QStringLiteral("execution_identity_stable"),
                           executionIdentityStable);
    runtimeEvidence.insert(QStringLiteral("published_payloads_match"),
                           runtimePayloadsValid);
    runtimeEvidence.insert(
        QStringLiteral("verification_policy"),
        QStringLiteral(
            "SDK checksum and manifest hashes are compiled build anchors; "
            "all deployed SDK bin files are verified before and after "
            "recognition; host Qt/MSVC DLLs are verified against the "
            "published ScanEngine baseline; materialized Voice GGUF is "
            "verified through its anchored model manifest"));
    runtimeEvidence.insert(QStringLiteral("voice_runtime_status"),
                           speechRuntimeStatus_->text());
    runtimeEvidence.insert(QStringLiteral("scan_runtime_status"),
                           documentRuntimeStatus_->text());
    report.insert(QStringLiteral("runtime"), runtimeEvidence);
    if (!cancelled && !runtimePayloadsValid)
        valid = false;
    passed = valid && accuracy >= acceptanceDataset_.threshold;
    verdict = cancelled ? QStringLiteral("已取消")
        : !valid ? QStringLiteral("验证无效")
        : passed ? QStringLiteral("通过") : QStringLiteral("不通过");
    QJsonObject normalization;
    normalization.insert(QStringLiteral("unicode"),
                         QStringLiteral("NFC"));
    normalization.insert(QStringLiteral("line_endings"),
                         QStringLiteral("LF"));
    normalization.insert(QStringLiteral("trim_outer_whitespace"), true);
    normalization.insert(QStringLiteral("remove_whitespace"),
                         acceptanceDataset_.removeWhitespace);
    normalization.insert(QStringLiteral("ignore_punctuation"),
                         acceptanceDataset_.ignorePunctuation);
    normalization.insert(QStringLiteral("case_sensitive"),
                         acceptanceDataset_.caseSensitive);
    report.insert(QStringLiteral("normalization"), normalization);
    report.insert(QStringLiteral("metric"),
                  QStringLiteral("micro character Levenshtein accuracy"));
    report.insert(QStringLiteral("verdict"),
                  cancelled ? QStringLiteral("cancelled")
                  : !valid ? QStringLiteral("invalid")
                  : passed ? QStringLiteral("passed") : QStringLiteral("failed"));
    report.insert(QStringLiteral("metrics"), metrics);
    report.insert(QStringLiteral("samples"), acceptanceResults_);
    const QString reportPath =
        QDir(acceptanceRunDirectory_).filePath(QStringLiteral("report.json"));
    const QString checksumPath =
        QDir(acceptanceRunDirectory_).filePath(
            QStringLiteral("SHA256SUMS.txt"));
    report.insert(QStringLiteral("report_path"), reportPath);
    QJsonObject evidenceChain;
    evidenceChain.insert(QStringLiteral("algorithm"),
                         QStringLiteral("SHA-256"));
    evidenceChain.insert(QStringLiteral("checksum_path"), checksumPath);
    evidenceChain.insert(QStringLiteral("required"), true);
    evidenceChain.insert(
        QStringLiteral("validation_rule"),
        QStringLiteral(
            "verdict is authoritative only when SHA256SUMS.txt exists "
            "and matches report.json"));
    report.insert(QStringLiteral("evidence_chain"), evidenceChain);
    QString writeError;
    QString reportSha256;
    QString checksumSha256;
    bool evidenceChainWritten = acceptance::writeChecksummedJson(
        reportPath, checksumPath, report,
        &reportSha256, &checksumSha256, &writeError);
    if (!evidenceChainWritten) {
        const QString initialError = writeError;
        passed = false;
        verdict = QStringLiteral("验证无效（证据链写入失败）");
        report.insert(QStringLiteral("verdict"), QStringLiteral("invalid"));
        evidenceChain.insert(QStringLiteral("write_error"), initialError);
        report.insert(QStringLiteral("evidence_chain"), evidenceChain);
        QString recoveryError;
        const bool recovered = acceptance::writeChecksummedJson(
            reportPath, checksumPath, report,
            &reportSha256, &checksumSha256, &recoveryError);
        writeError = initialError;
        if (!recovered && !recoveryError.isEmpty())
            writeError += QStringLiteral("；无效报告恢复失败：%1")
                              .arg(recoveryError);
        statusBar()->showMessage(
            QStringLiteral("验收结束，但证据链写入失败：%1").arg(writeError));
    } else {
        statusBar()->showMessage(
            QStringLiteral("验收验证结束：%1；报告已写入 %2")
                .arg(verdict, acceptanceRunDirectory_));
    }
    acceptanceReport_ = readJsonObjectFile(reportPath);
    const bool checksumMatches = !acceptanceReport_.isEmpty()
        && !reportSha256.isEmpty()
        && acceptance::sha256File(reportPath) == reportSha256
        && !checksumSha256.isEmpty()
        && acceptance::sha256File(checksumPath) == checksumSha256;
    if (!checksumMatches) {
        passed = false;
        verdict = QStringLiteral("验证无效（报告哈希不匹配）");
        statusBar()->showMessage(verdict);
    }
    QJsonObject verification;
    verification.insert(QStringLiteral("verdict"), verdict);
    verification.insert(
        QStringLiteral("verdict_code"),
        !passed && verdict.startsWith(QStringLiteral("验证无效"))
            ? QStringLiteral("invalid")
        : cancelled ? QStringLiteral("cancelled")
        : passed ? QStringLiteral("passed") : QStringLiteral("failed"));
    verification.insert(QStringLiteral("report_path"), reportPath);
    verification.insert(QStringLiteral("report_sha256"), reportSha256);
    verification.insert(QStringLiteral("checksum_path"), checksumPath);
    verification.insert(QStringLiteral("checksum_file_sha256"),
                        checksumSha256);
    verification.insert(QStringLiteral("checksum_matches_report"),
                        checksumMatches);
    if (!evidenceChainWritten)
        verification.insert(QStringLiteral("write_error"), writeError);
    acceptanceEvidencePanel_->showReport(
        acceptanceReport_, verification);
    acceptanceEvidencePanel_->focusReport();
    acceptanceVerdict_->setText(verdict);
    acceptanceVerdict_->setProperty("passed", passed);
    acceptanceVerdict_->style()->unpolish(acceptanceVerdict_);
    acceptanceVerdict_->style()->polish(acceptanceVerdict_);
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
            if (status.progress > 0.0f && speechProgress_->maximum() == 0)
                speechProgress_->setRange(0, 100);
            if (speechProgress_->maximum() > 0) {
                speechProgress_->setValue(
                    static_cast<int>(status.progress * 100.0f));
            }
            if (!status.interim_text.empty())
                speechInterim_->setPlainText(
                    QString::fromUtf8(status.interim_text.c_str()));
            if (status.finished()) {
                const bool acceptanceTask = acceptanceRunning_
                    && acceptanceDataset_.kind == acceptance::Kind::Voice;
                speechProgress_->setRange(0, 100);
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
            speechProgress_->setRange(0, 100);
            speechProgress_->setValue(0);
            if (acceptanceRunning_
                && acceptanceDataset_.kind == acceptance::Kind::Voice)
                failAcceptanceSample(message);
            else
                appendLog(speechResult_, message);
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

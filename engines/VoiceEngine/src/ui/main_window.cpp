#include "main_window.h"

#include "drop_zone.h"
#include "mic_capture.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <atomic>
#include <thread>
#include <vector>
#include <windows.h>

namespace voiceengine {

static std::wstring qToWide(const QString& s) {
    return std::wstring(reinterpret_cast<const wchar_t*>(s.utf16()));
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("VoiceEngine"));
    resize(980, 720);

    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);

    m_drop = new DropZone(this);
    connect(m_drop, &DropZone::clicked, this, &MainWindow::pickFiles);
    connect(m_drop, &DropZone::filesDropped, this, &MainWindow::onDropped);
    root->addWidget(m_drop);

    auto* row = new QHBoxLayout();
    m_load = new QPushButton(QString::fromUtf8("加载模型"), this);
    m_start = new QPushButton(QString::fromUtf8("开始识别"), this);
    m_cancel = new QPushButton(QString::fromUtf8("取消"), this);
    m_copy = new QPushButton(QString::fromUtf8("复制"), this);
    m_save = new QPushButton(QString::fromUtf8("保存"), this);
    m_mic = new QPushButton(QString::fromUtf8("麦克风"), this);
    row->addWidget(m_load);
    row->addWidget(m_start);
    row->addWidget(m_cancel);
    row->addWidget(m_copy);
    row->addWidget(m_save);
    row->addWidget(m_mic);
    root->addLayout(row);

    m_modelStatus = new QLabel(QString::fromUtf8("模型：未加载"), this);
    m_status = new QLabel(QString::fromUtf8("就绪"), this);
    root->addWidget(m_modelStatus);
    root->addWidget(m_status);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    root->addWidget(m_progress);

    auto* mid = new QHBoxLayout();
    m_queue = new QListWidget(this);
    m_queue->setMinimumWidth(220);
    mid->addWidget(m_queue, 1);
    auto* texts = new QVBoxLayout();
    texts->addWidget(new QLabel(QString::fromUtf8("临时文本"), this));
    m_interim = new QPlainTextEdit(this);
    m_interim->setReadOnly(true);
    texts->addWidget(m_interim, 1);
    texts->addWidget(new QLabel(QString::fromUtf8("最终文本"), this));
    m_final = new QPlainTextEdit(this);
    m_final->setReadOnly(true);
    texts->addWidget(m_final, 2);
    mid->addLayout(texts, 3);
    root->addLayout(mid, 1);

    connect(m_load, &QPushButton::clicked, this, &MainWindow::loadModel);
    connect(m_start, &QPushButton::clicked, this, &MainWindow::startNext);
    connect(m_cancel, &QPushButton::clicked, this, &MainWindow::cancelCurrent);
    connect(m_copy, &QPushButton::clicked, this, &MainWindow::copyText);
    connect(m_save, &QPushButton::clicked, this, &MainWindow::saveText);
    connect(m_mic, &QPushButton::clicked, this, &MainWindow::toggleMic);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::poll);
    m_timer->start(80);

    m_micCap = new MicCapture(this);
    connect(m_micCap, &MicCapture::pcmReady, this, &MainWindow::onMicPcm);
    connect(m_micCap, &MicCapture::error, this, [this](const QString& e) { showError(e); });

    ve_context_params cp{};
    cp.struct_size = sizeof(cp);
    cp.api_version = VOICEENGINE_API_VERSION;
    const QString rootDir = QCoreApplication::applicationDirPath();
    const std::wstring rootW = qToWide(QDir::toNativeSeparators(rootDir));
    cp.runtime_dir_utf16 = rootW.c_str();
    if (voiceengine_context_create(&cp, &m_ctx) != VE_OK) {
        showError(QString::fromUtf8("无法创建识别上下文"));
    }

    statusBar()->showMessage(QString::fromUtf8("VoiceEngine 直接调用 VoiceEngineCore.dll，不启动 CLI"));
}

MainWindow::~MainWindow() {
    if (m_micCap) {
        m_micCap->stop();
    }
    stopMicStream(true);
    if (m_ctx) {
        voiceengine_context_destroy(m_ctx);
        m_ctx = nullptr;
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (m_micCap) {
        m_micCap->stop();
    }
    stopMicStream(true);
    if (m_active && m_ctx) {
        voiceengine_task_cancel(m_ctx, m_active);
    }
    event->accept();
}

void MainWindow::pickFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        QString::fromUtf8("选择音频"),
        QString(),
        QString::fromUtf8("音频 (*.wav *.mp3 *.m4a *.aac *.flac *.opus *.mp4 *.mkv);;全部 (*.*)"));
    enqueue(files);
}

void MainWindow::onDropped(const QStringList& paths) { enqueue(paths); }

void MainWindow::enqueue(const QStringList& paths) {
    for (const QString& p : paths) {
        m_pending << p;
        m_queue->addItem(p);
    }
    setStatus(QString::fromUtf8("队列 %1 个任务，点「开始识别」").arg(m_pending.size()));
}

QString MainWindow::modelDir() const {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("models/qwen3-asr-1.7b"));
}

void MainWindow::loadModel() {
    if (!m_ctx || m_loading) {
        return;
    }
    ve_cuda_info info{};
    info.struct_size = sizeof(info);
    if (voiceengine_cuda_check(m_ctx, &info) != VE_OK || !info.available) {
        showError(QString::fromUtf8("CUDA 失败，已停止识别，不会改为 CPU ASR。\n%1")
                      .arg(QString::fromUtf8(voiceengine_last_error(m_ctx))));
        m_modelStatus->setText(QString::fromUtf8("模型：CUDA 不可用"));
        return;
    }
    m_loading = true;
    m_load->setEnabled(false);
    m_start->setEnabled(false);
    m_mic->setEnabled(false);
    const QString device = QString::fromUtf8(info.name);
    setStatus(QString::fromUtf8("正在加载模型，界面可保持打开…"));
    m_modelStatus->setText(QString::fromUtf8("模型：加载中（%1）").arg(device));
    const std::wstring dir = qToWide(QDir::toNativeSeparators(modelDir()));
    std::thread([this, dir, device]() {
        ve_model_params mp{};
        mp.struct_size = sizeof(mp);
        mp.api_version = VOICEENGINE_API_VERSION;
        mp.model_dir_utf16 = dir.c_str();
        const ve_status st = voiceengine_model_load(m_ctx, &mp);
        const QString err = QString::fromUtf8(voiceengine_last_error(m_ctx));
        QMetaObject::invokeMethod(this, "onModelLoadFinished", Qt::QueuedConnection,
                                  Q_ARG(int, static_cast<int>(st)),
                                  Q_ARG(QString, err),
                                  Q_ARG(QString, device));
    }).detach();
}

void MainWindow::onModelLoadFinished(int status, const QString& err, const QString& device) {
    m_loading = false;
    m_load->setEnabled(true);
    m_start->setEnabled(true);
    m_mic->setEnabled(true);
    if (status != VE_OK) {
        showError(err);
        m_modelStatus->setText(QString::fromUtf8("模型：加载失败"));
        return;
    }
    m_modelStatus->setText(QString::fromUtf8("模型：Qwen3-ASR-1.7B BF16 已加载（%1）").arg(device));
    setStatus(QString::fromUtf8("模型已加载，可拖文件或开麦克风"));
}

void MainWindow::watchTask(uint64_t id, const QString& status) {
    m_watch.append(id);
    if (!m_busy) {
        m_active = id;
        m_busy = true;
    }
    setStatus(status);
}

void MainWindow::finishActive() {
    m_watch.removeAll(m_active);
    m_active = 0;
    m_busy = false;
    if (!m_watch.isEmpty()) {
        m_active = m_watch.takeFirst();
        m_busy = true;
        return;
    }
    if (!m_pending.isEmpty() && !m_micCap->running()) {
        startNext();
    }
}

void MainWindow::startNext() {
    if (!m_ctx || m_busy || m_loading) {
        return;
    }
    if (!voiceengine_model_loaded(m_ctx)) {
        loadModel();
        setStatus(QString::fromUtf8("请等模型加载完成后再点「开始识别」"));
        return;
    }
    if (m_stream) {
        setStatus(QString::fromUtf8("请先停止麦克风并等待流式识别结束"));
        return;
    }
    if (m_pending.isEmpty()) {
        setStatus(QString::fromUtf8("队列为空，先拖文件或点上方区域选择"));
        return;
    }
    const QString path = m_pending.takeFirst();
    if (m_queue->count() > 0) {
        delete m_queue->takeItem(0);
    }
    uint64_t id = 0;
    const std::wstring w = qToWide(QDir::toNativeSeparators(path));
    const ve_status st = voiceengine_submit_file(m_ctx, w.c_str(), &id);
    if (st != VE_OK) {
        showError(QString::fromUtf8(voiceengine_last_error(m_ctx)));
        return;
    }
    watchTask(id, QString::fromUtf8("识别中：%1").arg(path));
}

void MainWindow::cancelCurrent() {
    if (!m_ctx) {
        return;
    }
    if (m_micCap && m_micCap->running()) {
        m_micCap->stop();
        m_mic->setText(QString::fromUtf8("麦克风"));
    }
    if (m_stream) {
        voiceengine_stream_cancel(m_stream);
    }
    for (quint64 id : m_watch) {
        voiceengine_task_cancel(m_ctx, id);
    }
    if (m_active) {
        voiceengine_task_cancel(m_ctx, m_active);
    }
    setStatus(QString::fromUtf8("正在取消…"));
}

void MainWindow::poll() {
    pollMicStream();
    if (!m_ctx || !m_active) {
        return;
    }
    ve_task_status st{};
    st.struct_size = sizeof(st);
    if (voiceengine_task_poll(m_ctx, m_active, &st) != VE_OK) {
        return;
    }
    m_progress->setValue(static_cast<int>(st.progress * 100.0f));
    if (st.interim_utf8[0]) {
        m_interim->setPlainText(QString::fromUtf8(st.interim_utf8));
    }
    if (st.state == VE_TASK_DONE || st.state == VE_TASK_FAILED || st.state == VE_TASK_CANCELLED) {
        ve_result r{};
        r.struct_size = sizeof(r);
        voiceengine_task_result(m_ctx, m_active, &r);
        if (r.text_utf8 && r.text_utf8[0]) {
            m_final->appendPlainText(QString::fromUtf8(r.text_utf8));
        }
        if (st.state == VE_TASK_FAILED) {
            showError(QString::fromUtf8(voiceengine_last_error(m_ctx)));
        } else if (st.state == VE_TASK_CANCELLED) {
            setStatus(QString::fromUtf8("已取消"));
        } else {
            setStatus(QString::fromUtf8("完成"));
        }
        voiceengine_result_free(&r);
        voiceengine_task_release(m_ctx, m_active);
        finishActive();
    }
}

void MainWindow::copyText() {
    QApplication::clipboard()->setText(m_final->toPlainText());
    setStatus(QString::fromUtf8("已复制"));
}

void MainWindow::saveText() {
    const QString path = QFileDialog::getSaveFileName(this, QString::fromUtf8("保存结果"),
                                                      QStringLiteral("output.txt"),
                                                      QStringLiteral("Text (*.txt);;JSON (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    QFile f(path);
    if (!f.open(QFile::WriteOnly | QFile::Text)) {
        showError(QString::fromUtf8("无法写入文件"));
        return;
    }
    QTextStream ts(&f);
    ts.setCodec("UTF-8");
    ts << m_final->toPlainText();
}

void MainWindow::toggleMic() {
    if (m_micCap->running()) {
        m_micCap->stop();
        if (m_stream) {
            voiceengine_stream_finish(m_stream);
        }
        m_mic->setText(QString::fromUtf8("麦克风"));
        setStatus(QString::fromUtf8("麦克风已停止，等待当前片段出字"));
        return;
    }
    if (m_loading) {
        setStatus(QString::fromUtf8("模型还在加载，请稍候"));
        return;
    }
    if (!voiceengine_model_loaded(m_ctx)) {
        loadModel();
        setStatus(QString::fromUtf8("请等模型加载完成后再开麦克风"));
        return;
    }
    if (m_active || !m_watch.isEmpty()) {
        setStatus(QString::fromUtf8("请等待当前文件识别完成后再开麦克风"));
        return;
    }
    if (m_stream) {
        setStatus(QString::fromUtf8("上一段麦克风音频仍在收尾，请稍候"));
        return;
    }
    if (m_micCap->start()) {
        m_mic->setText(QString::fromUtf8("停止麦克风"));
        setStatus(QString::fromUtf8("流式采集中，请对着默认麦克风说话"));
    }
}

void MainWindow::onMicPcm(const QByteArray& pcm16, int sampleRate) {
    if (!m_ctx || pcm16.size() < 4 || !m_micCap->running()) {
        return;
    }
    const int n = pcm16.size() / 2;
    std::vector<float> f(static_cast<size_t>(n));
    const auto* s = reinterpret_cast<const int16_t*>(pcm16.constData());
    for (int i = 0; i < n; ++i) {
        const float v = static_cast<float>(s[i]) / 32768.0f;
        f[static_cast<size_t>(i)] = v;
    }
    if (!m_stream) {
        ve_stream_params params{};
        params.struct_size = sizeof(params);
        params.api_version = VOICEENGINE_API_VERSION;
        params.sample_rate = sampleRate;
        params.channels = 1;
        params.chunk_ms = 2000;
        params.overlap_ms = 200;
        params.endpoint_silence_ms = 800;
        params.pre_roll_ms = 200;
        params.vad_rms_threshold = 0.004f;
        const ve_status created = voiceengine_stream_create(m_ctx, &params, &m_stream);
        if (created != VE_OK) {
            m_micCap->stop();
            m_mic->setText(QString::fromUtf8("麦克风"));
            showError(QString::fromUtf8(voiceengine_last_error(m_ctx)));
            return;
        }
    }
    const ve_status st = voiceengine_stream_push_f32(
        m_stream, f.data(), static_cast<uint64_t>(n));
    if (st != VE_OK) {
        if (st == VE_ERR_BUSY) {
            setStatus(QString::fromUtf8("流式识别暂时积压，正在追赶音频"));
        } else {
            showError(QString::fromUtf8("麦克风流写入失败（%1）").arg(static_cast<int>(st)));
        }
        return;
    }
}

void MainWindow::pollMicStream() {
    if (!m_stream) {
        return;
    }
    for (;;) {
        ve_stream_event event{};
        event.struct_size = sizeof(event);
        event.api_version = VOICEENGINE_API_VERSION;
        const ve_status st = voiceengine_stream_poll(m_stream, &event);
        if (st == VE_ERR_BUSY) {
            return;
        }
        if (st != VE_OK) {
            setStatus(QString::fromUtf8("读取麦克风流事件失败（%1）").arg(static_cast<int>(st)));
            return;
        }
        if (event.type == VE_STREAM_EVENT_PARTIAL) {
            m_interim->setPlainText(QString::fromUtf8(event.text_utf8));
            m_progress->setValue(50);
        } else if (event.type == VE_STREAM_EVENT_FINAL) {
            if (event.text_utf8[0]) {
                m_final->appendPlainText(QString::fromUtf8(event.text_utf8));
            }
            m_interim->clear();
            m_progress->setValue(100);
            setStatus(QString::fromUtf8("麦克风片段识别完成"));
        } else if (event.type == VE_STREAM_EVENT_ERROR) {
            setStatus(QString::fromUtf8("麦克风流错误（%1）：%2")
                          .arg(static_cast<int>(event.error))
                          .arg(QString::fromUtf8(event.text_utf8)));
        } else if (event.type == VE_STREAM_EVENT_END) {
            voiceengine_stream_destroy(m_stream);
            m_stream = nullptr;
            m_interim->clear();
            m_progress->setValue(0);
            setStatus(event.error == VE_OK
                ? QString::fromUtf8("麦克风流已结束")
                : QString::fromUtf8("麦克风流已取消"));
            return;
        }
    }
}

void MainWindow::stopMicStream(bool cancel) {
    if (!m_stream) {
        return;
    }
    if (cancel) {
        voiceengine_stream_cancel(m_stream);
    } else {
        voiceengine_stream_finish(m_stream);
    }
    voiceengine_stream_destroy(m_stream);
    m_stream = nullptr;
}

void MainWindow::setStatus(const QString& s) {
    m_status->setText(s);
    statusBar()->showMessage(s);
}

void MainWindow::showError(const QString& s) {
    setStatus(s);
    QMessageBox::warning(this, QString::fromUtf8("VoiceEngine"), s);
}

}  // namespace voiceengine

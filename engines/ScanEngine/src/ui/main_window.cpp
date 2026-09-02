#include "main_window.h"

#include "parse_controller.h"
#include "widgets/control_panel.h"
#include "widgets/preview_pane.h"
#include "widgets/result_pane.h"

#include "scanengine/config.hpp"
#include "scanengine/ui_options.hpp"

#include <QAction>
#include <QByteArray>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaType>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <exception>

namespace scanengine {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    qRegisterMetaType<ParseResult>("scanengine::ParseResult");
    ensureRuntimeDirs();
    setWindowTitle(QString::fromUtf8(kHeaderTitle));
    resize(1440, 900);

    m_controller = new ParseController(this);
    connect(m_controller, &ParseController::logLine, this, &MainWindow::onLog);
    connect(m_controller, &ParseController::statusChanged, this, &MainWindow::onStatus);
    connect(m_controller, &ParseController::finished, this, &MainWindow::onFinished);

    buildUi();
    buildMenu();

    const QJsonObject cfg = loadConfig();
    m_control->applyConfig(cfg);
    const QJsonValue geom = cfg.value(QStringLiteral("window_geometry"));
    if (geom.isString() && !geom.toString().isEmpty())
        restoreGeometry(QByteArray::fromHex(geom.toString().toLatin1()));
}

void MainWindow::buildUi() {
    auto* root = new QWidget;
    root->setObjectName(QStringLiteral("centralRoot"));
    setCentralWidget(root);

    auto* outer = new QVBoxLayout(root);
    outer->setContentsMargins(14, 14, 14, 14);
    outer->setSpacing(12);

    auto* header = new QHBoxLayout;
    header->setSpacing(10);
    auto* brandIcon = new QLabel;
    brandIcon->setFixedSize(36, 36);
    brandIcon->setPixmap(QIcon(QStringLiteral(":/icons/scanengine.png")).pixmap(36, 36));
    brandIcon->setScaledContents(true);
    auto* headerText = new QVBoxLayout;
    headerText->setSpacing(2);
    auto* title = new QLabel(QString::fromUtf8(kHeaderTitle));
    title->setObjectName(QStringLiteral("headerTitle"));
    auto* subtitle = new QLabel(QString::fromUtf8(kHeaderSubtitle));
    subtitle->setObjectName(QStringLiteral("headerSubtitle"));
    headerText->addWidget(title);
    headerText->addWidget(subtitle);
    header->addWidget(brandIcon, 0, Qt::AlignVCenter);
    header->addLayout(headerText, 1);
    outer->addLayout(header);

    auto* splitter = new QSplitter(Qt::Horizontal);
    m_control = new ControlPanel;
    m_preview = new PreviewPane;
    m_result = new ResultPane;
    splitter->addWidget(m_control);
    splitter->addWidget(m_preview);
    splitter->addWidget(m_result);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 4);
    splitter->setStretchFactor(2, 4);
    splitter->setSizes(QList<int>() << 320 << 520 << 520);
    outer->addWidget(splitter, 1);

    connect(m_control, &ControlPanel::convertRequested, this, &MainWindow::startParse);
    connect(m_control, &ControlPanel::clearRequested, this, &MainWindow::clearAll);
    connect(m_control, &ControlPanel::cancelRequested, this, &MainWindow::cancelParse);
    connect(m_control, &ControlPanel::inputChanged, this, &MainWindow::onInputChanged);
}

void MainWindow::buildMenu() {
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
    auto* actClipboard = new QAction(QStringLiteral("从剪贴板导入"), this);
    actClipboard->setShortcut(QKeySequence::Paste);
    connect(actClipboard, &QAction::triggered, m_control, &ControlPanel::importClipboard);
    fileMenu->addAction(actClipboard);
    fileMenu->addSeparator();
    auto* actQuit = new QAction(QStringLiteral("退出"), this);
    connect(actQuit, &QAction::triggered, this, &QWidget::close);
    fileMenu->addAction(actQuit);

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
    auto* actAbout = new QAction(QStringLiteral("关于"), this);
    connect(actAbout, &QAction::triggered, this, &MainWindow::about);
    helpMenu->addAction(actAbout);
}

void MainWindow::about() {
    QMessageBox::information(
        this,
        QStringLiteral("关于"),
        QStringLiteral("ScanEngine\n"
                       "本地 C++ GPU 图片结构化与表格提取\n"
                       "需要 NVIDIA GPU（MLX CUDA）"));
}

void MainWindow::onInputChanged() {
    m_preview->showPath(m_control->primaryInput());
}

void MainWindow::startParse() {
    if (m_controller->isRunning()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("已有任务在运行"));
        return;
    }
    const QStringList files = m_control->inputFiles();
    if (files.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先选择、拖放或从剪贴板导入图片"));
        return;
    }

    QVector<ParseOptions> jobs;
    for (const QString& src : files) {
        const auto opts = m_control->buildOptions(src);
        if (!opts.has_value())
            continue;
        jobs.append(*opts);
    }
    if (jobs.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("没有可解析的文件"));
        return;
    }

    saveConfig(m_control->exportConfig());
    m_queue = jobs;
    m_queueTotal = jobs.size();
    m_lastArtifacts.clear();
    m_result->clear();
    m_control->setResultFiles(QStringList());
    m_control->setBusy(true);
    m_result->appendLog(QStringLiteral("共 %1 个任务 · C++ GPU").arg(jobs.size()));
    startNextJob();
}

void MainWindow::startNextJob() {
    if (m_queue.isEmpty()) {
        m_control->setBusy(false);
        return;
    }
    if (m_controller->isRunning()) {
        QTimer::singleShot(20, this, &MainWindow::startNextJob);
        return;
    }
    const ParseOptions options = m_queue.takeFirst();
    const int index = m_queueTotal - m_queue.size();
    m_control->setProgress(index, m_queueTotal);
    m_control->setStatus(QStringLiteral("prepare"), QString());
    m_result->appendLog(QStringLiteral("[%1/%2] %3")
                            .arg(index)
                            .arg(m_queueTotal)
                            .arg(options.inputPath));
    try {
        m_controller->start(options);
    } catch (const std::exception& exc) {
        m_control->setBusy(false);
        const QString msg = QString::fromUtf8(exc.what());
        m_control->setStatus(QStringLiteral("failed"), msg);
        QMessageBox::critical(this, QStringLiteral("启动失败"), msg);
    }
}

void MainWindow::cancelParse() {
    m_queue.clear();
    m_controller->cancel();
    m_control->setStatus(QStringLiteral("process"), QStringLiteral("正在取消…"));
}

void MainWindow::clearAll() {
    if (m_controller->isRunning()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先取消当前任务"));
        return;
    }
    m_control->clearInputs();
    m_preview->clear();
    m_result->clear();
}

void MainWindow::onLog(const QString& line) {
    m_result->appendLog(line);
}

void MainWindow::onStatus(const QString& status, const QString& message) {
    m_control->setStatus(status, message);
}

void MainWindow::onFinished(const ParseResult& result) {
    QStringList shown;
    if (!result.excelPath.isEmpty())
        shown.append(result.excelPath);
    if (!result.outputDir.isEmpty())
        shown.append(result.outputDir);
    m_lastArtifacts.append(shown);
    m_control->setResultFiles(m_lastArtifacts);
    if (!result.markdownPath.isEmpty())
        m_result->loadMarkdown(result.markdownPath);
    if (!result.excelPath.isEmpty())
        m_result->appendLog(QStringLiteral("Excel: %1").arg(result.excelPath));
    if (!result.outputDir.isEmpty())
        m_result->appendLog(QStringLiteral("输出目录: %1").arg(result.outputDir));
    if (!result.success) {
        if (!result.logTail.isEmpty()) {
            m_result->appendLog(QStringLiteral("--- 错误摘要 ---"));
            m_result->appendLog(result.logTail);
        }
        if (result.status != TaskStatus::Cancelled && m_queue.isEmpty())
            QMessageBox::warning(this, QStringLiteral("解析未成功"), result.message);
    }

    if (!m_queue.isEmpty() && result.status != TaskStatus::Cancelled) {
        startNextJob();
        return;
    }
    m_control->setBusy(false);
    m_control->setStatus(taskStatusValue(result.status), result.message);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (m_controller->isRunning()) {
        const int ret = QMessageBox::question(
            this,
            QStringLiteral("退出确认"),
            QStringLiteral("解析任务仍在运行，确定退出？"));
        if (ret != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_controller->cancel();
    }
    QJsonObject cfg = m_control->exportConfig();
    cfg.insert(QStringLiteral("window_geometry"),
               QString::fromLatin1(saveGeometry().toHex()));
    saveConfig(cfg);
    QMainWindow::closeEvent(event);
}

}  // namespace scanengine

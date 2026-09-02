#include "control_panel.h"

#include "drop_zone.h"
#include "status_panel.h"

#include "scanengine/config.hpp"
#include "scanengine/ui_options.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace scanengine {
namespace {

QStringList clipboardLines(const QString& text) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    return text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
#else
    return text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), QString::SkipEmptyParts);
#endif
}

}  // namespace

ControlPanel::ControlPanel(QWidget* parent)
    : QFrame(parent)
    , m_outputDir(defaultOutputDir()) {
    setObjectName(QStringLiteral("panel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    m_dropZone = new DropZone;
    connect(m_dropZone, &DropZone::clicked, this, &ControlPanel::pickFiles);
    connect(m_dropZone, &DropZone::filesDropped, this, &ControlPanel::onPaths);
    root->addWidget(m_dropZone);

    auto* inputRow = new QHBoxLayout;
    m_btnPickFiles = new QPushButton(QStringLiteral("选择图片…"));
    m_btnPickDir = new QPushButton(QStringLiteral("选择文件夹…"));
    m_btnClipboard = new QPushButton(QStringLiteral("粘贴图片  Ctrl+V"));
    connect(m_btnPickFiles, &QPushButton::clicked, this, &ControlPanel::pickFiles);
    connect(m_btnPickDir, &QPushButton::clicked, this, &ControlPanel::pickDir);
    connect(m_btnClipboard, &QPushButton::clicked, this, &ControlPanel::importClipboard);
    inputRow->addWidget(m_btnPickFiles);
    inputRow->addWidget(m_btnPickDir);
    inputRow->addWidget(m_btnClipboard);
    root->addLayout(inputRow);

    auto* outputTitle = new QLabel(QStringLiteral("输出目录"));
    outputTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
    root->addWidget(outputTitle);

    auto* outputRow = new QHBoxLayout;
    m_lblOutput = new QLabel(m_outputDir);
    m_lblOutput->setWordWrap(true);
    m_lblOutput->setObjectName(QStringLiteral("optionInfo"));
    m_btnOutput = new QPushButton(QStringLiteral("更改…"));
    connect(m_btnOutput, &QPushButton::clicked, this, &ControlPanel::pickOutput);
    outputRow->addWidget(m_lblOutput, 1);
    outputRow->addWidget(m_btnOutput);
    root->addLayout(outputRow);

    auto* actionRow = new QHBoxLayout;
    m_btnConvert = new QPushButton(QStringLiteral("开始解析"));
    m_btnConvert->setObjectName(QStringLiteral("primaryButton"));
    m_btnClear = new QPushButton(QStringLiteral("清除"));
    m_btnCancel = new QPushButton(QStringLiteral("取消"));
    m_btnCancel->setEnabled(false);
    connect(m_btnConvert, &QPushButton::clicked, this, &ControlPanel::convertRequested);
    connect(m_btnClear, &QPushButton::clicked, this, &ControlPanel::clearRequested);
    connect(m_btnCancel, &QPushButton::clicked, this, &ControlPanel::cancelRequested);
    actionRow->addWidget(m_btnConvert, 1);
    actionRow->addWidget(m_btnClear);
    actionRow->addWidget(m_btnCancel);
    root->addLayout(actionRow);

    auto* resultHead = new QHBoxLayout;
    auto* resultTitle = new QLabel(QStringLiteral("解析文件"));
    resultTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
    m_btnOpenSelected = new QPushButton(QStringLiteral("打开所选"));
    auto* btnOpenOutput = new QPushButton(QStringLiteral("打开输出目录"));
    connect(m_btnOpenSelected, &QPushButton::clicked, this, &ControlPanel::openSelectedArtifact);
    connect(btnOpenOutput, &QPushButton::clicked, this, &ControlPanel::openOutputDir);
    resultHead->addWidget(resultTitle);
    resultHead->addStretch(1);
    resultHead->addWidget(m_btnOpenSelected);
    resultHead->addWidget(btnOpenOutput);
    root->addLayout(resultHead);

    auto* resultHint = new QLabel(QStringLiteral("双击打开 Excel 或目录"));
    resultHint->setObjectName(QStringLiteral("optionInfo"));
    resultHint->setWordWrap(true);
    root->addWidget(resultHint);

    m_resultFiles = new QListWidget;
    m_resultFiles->setObjectName(QStringLiteral("resultFiles"));
    m_resultFiles->setMinimumHeight(72);
    connect(m_resultFiles, &QListWidget::itemActivated, this, &ControlPanel::openArtifact);
    root->addWidget(m_resultFiles);

    m_statusPanel = new StatusPanel;
    root->addWidget(m_statusPanel);
    root->addStretch(1);
}

void ControlPanel::onPaths(const QStringList& paths) {
    m_paths = paths;
    m_dropZone->setPaths(paths);
    emit inputChanged();
}

void ControlPanel::pickFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("选择图片"),
        QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp)"));
    if (!files.isEmpty())
        onPaths(files);
}

void ControlPanel::pickDir() {
    const QString directory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择图片文件夹"));
    if (!directory.isEmpty())
        onPaths(QStringList() << directory);
}

void ControlPanel::importClipboard() {
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    QStringList paths;
    if (mime->hasUrls()) {
        for (const QUrl& url : mime->urls()) {
            const QString path = url.toLocalFile();
            const QFileInfo info(path);
            if (info.isDir() || (info.isFile() && isSupportedPath(path)))
                paths.append(path);
        }
    }
    if (paths.isEmpty() && mime->hasText()) {
        const QStringList lines = clipboardLines(mime->text());
        for (const QString& line : lines) {
            const QString path = line.trimmed();
            const QFileInfo info(path);
            if (info.isDir() || (info.isFile() && isSupportedPath(path)))
                paths.append(path);
        }
    }
    if (!paths.isEmpty()) {
        onPaths(paths);
        return;
    }
    if (mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) {
            QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            if (cacheRoot.isEmpty())
                cacheRoot = QDir::tempPath() + QStringLiteral("/ScanEngine");
            const QString clipboardDir = QDir(cacheRoot).filePath(QStringLiteral("clipboard"));
            QDir().mkpath(clipboardDir);
            const QString path = QDir(clipboardDir).filePath(
                QStringLiteral("clipboard-%1.png")
                    .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))));
            if (image.save(path, "PNG")) {
                onPaths(QStringList() << path);
                return;
            }
        }
    }
    QMessageBox::information(
        this,
        QStringLiteral("剪贴板导入"),
        QStringLiteral("剪贴板中没有图片。"));
}

QStringList ControlPanel::inputFiles() const {
    return expandInputPaths(m_paths);
}

QString ControlPanel::primaryInput() const {
    const QStringList files = inputFiles();
    return files.isEmpty() ? QString() : files.first();
}

QString ControlPanel::outputDir() const {
    return m_outputDir;
}

void ControlPanel::setOutputDir(const QString& path) {
    m_outputDir = path;
    m_lblOutput->setText(path);
}

std::optional<ParseOptions> ControlPanel::buildOptions(const QString& inputPath) const {
    const QString path = inputPath.isEmpty() ? primaryInput() : inputPath;
    if (path.isEmpty())
        return std::nullopt;
    ParseOptions options;
    options.inputPath = path;
    options.outputDir = m_outputDir;
    options.device = QStringLiteral("gpu-required");
    return options;
}

void ControlPanel::applyConfig(const QJsonObject& config) {
    const QString outputDir = config.value(QStringLiteral("output_dir")).toString();
    if (!outputDir.isEmpty())
        setOutputDir(outputDir);
}

QJsonObject ControlPanel::exportConfig() const {
    return QJsonObject{{QStringLiteral("output_dir"), m_outputDir}};
}

void ControlPanel::pickOutput() {
    const QString directory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择输出目录"), m_outputDir);
    if (!directory.isEmpty())
        setOutputDir(directory);
}

void ControlPanel::openOutputDir() {
    QDir().mkpath(m_outputDir);
    openLocalPath(m_outputDir);
}

void ControlPanel::openSelectedArtifact() {
    openArtifact(m_resultFiles->currentItem());
}

void ControlPanel::openArtifact(QListWidgetItem* item) {
    if (!item)
        return;
    const QString path = item->data(Qt::UserRole).toString();
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        openLocalPath(path);
        emit artifactOpenRequested(path);
    }
}

void ControlPanel::setBusy(bool busy) {
    m_btnConvert->setEnabled(!busy);
    m_btnClear->setEnabled(!busy);
    m_btnCancel->setEnabled(busy);
    m_btnPickFiles->setEnabled(!busy);
    m_btnPickDir->setEnabled(!busy);
    m_btnClipboard->setEnabled(!busy);
    if (m_btnOutput)
        m_btnOutput->setEnabled(!busy);
}

void ControlPanel::setProgress(int current, int total) {
    m_statusPanel->setProgress(current, total);
}

void ControlPanel::setStatus(const QString& status, const QString& message) {
    m_statusPanel->setStatus(status, message);
}

void ControlPanel::setResultFiles(const QStringList& paths) {
    m_resultFiles->clear();
    for (const QString& path : paths) {
        const QFileInfo info(path);
        QString name = info.fileName();
        if (name.isEmpty())
            name = QDir(path).dirName();
        const QString kind = info.isDir() ? QStringLiteral("目录")
                                         : (info.suffix().compare(QStringLiteral("xlsx"), Qt::CaseInsensitive) == 0
                                                ? QStringLiteral("Excel")
                                                : QStringLiteral("文件"));
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 · %2").arg(kind, name.isEmpty() ? path : name));
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        m_resultFiles->addItem(item);
    }
    if (m_resultFiles->count() > 0)
        m_resultFiles->setCurrentRow(0);
}

void ControlPanel::setResultText(const QString& text) {
    if (!text.isEmpty() && m_resultFiles->count() == 0)
        m_resultFiles->addItem(text);
}

void ControlPanel::clearInputs() {
    m_paths.clear();
    m_dropZone->setPaths(QStringList());
    m_resultFiles->clear();
    m_statusPanel->reset();
    emit inputChanged();
}

}  // namespace scanengine

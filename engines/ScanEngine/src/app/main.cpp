#include "ui/main_window.h"

#include "scanengine/config.hpp"
#include "scanengine/hybrid/device_policy.hpp"
#include "scanengine/models.hpp"

#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QMetaType>
#include <QMessageBox>

int main(int argc, char* argv[]) {
    QString runtimeError;
    const bool runtimeReady =
        scanengine::hybrid::initializeHybridRuntime(&runtimeError);
    QApplication app(argc, argv);
    if (!runtimeReady) {
        QMessageBox::critical(nullptr, QStringLiteral("ScanEngine"), runtimeError);
        return 1;
    }
    qRegisterMetaType<scanengine::ParseResult>("scanengine::ParseResult");
    app.setApplicationName(QStringLiteral("ScanEngine"));
    app.setOrganizationName(QStringLiteral("ScanEngine"));
    app.setOrganizationDomain(QStringLiteral("scanengine.local"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/scanengine.png")));

    scanengine::ensureRuntimeDirs();

    QFile qss(QStringLiteral(":/styles.qss"));
    if (qss.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));

    scanengine::MainWindow window;
    window.show();
    return app.exec();
}

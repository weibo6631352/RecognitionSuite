#include "main_window.h"

#include <QApplication>
#include <QFile>
#include <QStringList>

int main(int argc, char* argv[]) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("RecognitionStudio"));
    app.setOrganizationName(QStringLiteral("RecognitionStudio"));

    QFile style(QStringLiteral(":/styles.qss"));
    if (style.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(style.readAll()));

    speechdoc::MainWindow window;
    if (app.arguments().contains(QStringLiteral("--sdk-check")))
        return window.sdksReady() ? 0 : 2;

    window.show();
    return app.exec();
}

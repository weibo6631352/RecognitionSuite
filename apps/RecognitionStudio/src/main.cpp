#include "main_window.h"

#include <QApplication>
#include <QFile>
#include <QStringList>

#include <cstdio>

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
    if (app.arguments().contains(QStringLiteral("--sdk-check"))) {
        if (!window.sdksReady())
            return 2;
        QString error;
        if (!window.publishedRuntimeMatches(&error)) {
            std::fprintf(stderr, "%s\n", qPrintable(error));
            return 3;
        }
        return 0;
    }

    window.show();
    return app.exec();
}

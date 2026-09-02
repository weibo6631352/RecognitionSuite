#include "main_window.h"
#include "runtime_bootstrap.hpp"

#include <QApplication>
#include <QFile>
#include <windows.h>

int main(int argc, char* argv[]) {
    std::wstring runtime_error;
    if (!voiceengine_app::initialize_private_runtimes(nullptr, &runtime_error)) {
        MessageBoxW(nullptr, runtime_error.c_str(), L"VoiceEngine",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("VoiceEngine"));
    QFile qss(QStringLiteral(":/styles.qss"));
    if (qss.open(QFile::ReadOnly)) {
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
    }
    voiceengine::MainWindow w;
    w.show();
    return app.exec();
}

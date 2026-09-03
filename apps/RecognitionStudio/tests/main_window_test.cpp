#include "main_window.h"

#include <QApplication>
#include <QLabel>
#include <QTabWidget>

#include <iostream>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    speechdoc::MainWindow window;
    QTabWidget* tabs = window.findChild<QTabWidget*>();
    if (!tabs) {
        std::cerr << "tab widget was not created\n";
        return 1;
    }
    int acceptanceTab = -1;
    for (int index = 0; index < tabs->count(); ++index) {
        if (tabs->tabText(index) == QStringLiteral("验收验证")) {
            acceptanceTab = index;
            break;
        }
    }
    if (acceptanceTab < 0) {
        std::cerr << "acceptance validation tab was not created\n";
        return 1;
    }
    const QList<QLabel*> labels = tabs->widget(acceptanceTab)->findChildren<QLabel*>();
    bool hasAccuracy = false;
    bool hasConfidence = false;
    bool hasVerdict = false;
    for (const QLabel* label : labels) {
        hasAccuracy = hasAccuracy || label->text() == QStringLiteral("实际准确率");
        hasConfidence = hasConfidence || label->text() == QStringLiteral("模型置信度");
        hasVerdict = hasVerdict || label->text() == QStringLiteral("验收结论");
    }
    if (!hasAccuracy || !hasConfidence || !hasVerdict) {
        std::cerr << "acceptance metric labels are incomplete\n";
        return 1;
    }
    return 0;
}

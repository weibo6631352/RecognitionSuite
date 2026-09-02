#include "status_panel.h"

#include <QLabel>
#include <QVBoxLayout>

namespace scanengine {
namespace {

double taskProgress(const QString& status) {
    if (status == QLatin1String("prepare"))
        return 0.05;
    if (status == QLatin1String("process"))
        return 0.60;
    if (status == QLatin1String("outputs"))
        return 0.90;
    if (status == QLatin1String("done"))
        return 1.0;
    return 0.0;
}

QString statusText(const QString& status) {
    if (status == QLatin1String("prepare"))
        return QStringLiteral("准备");
    if (status == QLatin1String("process"))
        return QStringLiteral("正在解析");
    if (status == QLatin1String("outputs"))
        return QStringLiteral("整理结果");
    if (status == QLatin1String("done"))
        return QStringLiteral("完成");
    if (status == QLatin1String("failed"))
        return QStringLiteral("失败");
    if (status == QLatin1String("cancelled"))
        return QStringLiteral("已取消");
    return QStringLiteral("尚未开始");
}

}  // namespace

StatusPanel::StatusPanel(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("statusPanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 6, 0, 0);
    m_text = new QLabel;
    m_text->setObjectName(QStringLiteral("statusText"));
    m_text->setWordWrap(false);
    root->addWidget(m_text);
    render();
}

void StatusPanel::setProgress(int current, int total) {
    m_total = qMax(0, total);
    m_current = m_total > 0 ? qBound(1, current, m_total) : 0;
    render();
}

void StatusPanel::setStatus(const QString& status, const QString& message) {
    m_status = status;
    m_message = message;
    render();
}

void StatusPanel::reset() {
    m_current = 0;
    m_total = 0;
    m_status = QStringLiteral("idle");
    m_message.clear();
    render();
}

void StatusPanel::render() {
    QString text = statusText(m_status);
    if (m_total > 0 && m_current > 0) {
        double fraction = taskProgress(m_status);
        if (m_status == QLatin1String("failed") || m_status == QLatin1String("cancelled"))
            fraction = 0.0;
        const int percent = qBound(
            0,
            int((double(m_current - 1) + fraction) * 100.0 / double(m_total)),
            100);
        text = QStringLiteral("第 %1/共 %2 · %3% · %4")
                   .arg(m_current)
                   .arg(m_total)
                   .arg(percent)
                   .arg(text);
    }
    if (m_status == QLatin1String("failed") && !m_message.isEmpty()) {
        QString detail = m_message.simplified();
        if (detail.size() > 48)
            detail = detail.left(47) + QChar(0x2026);
        text += QStringLiteral(" · %1").arg(detail);
    }
    m_text->setText(text);
}

}  // namespace scanengine

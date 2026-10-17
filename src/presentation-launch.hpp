#pragma once

#include "presentation-result.hpp"
#include <QStringList>

class QObject;
class QProcess;

namespace obs_seeker {
// 空命令表示使用系统默认打开方式。
QStringList presentationCommand(const QString &path);
#ifdef Q_OS_WIN
QProcess *preloadPowerPoint(QObject *owner, QStringList command = {});
#elif defined(Q_OS_MACOS)
void retryPresentationAutomation(QObject *owner);
#endif
}

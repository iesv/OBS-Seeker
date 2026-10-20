#include "window-activation.hpp"

#include <QApplication>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace obs_seeker {
#ifdef Q_OS_MACOS
void activateMacApplication();
#endif

void bringWindowForward(QWidget *window)
{
    if (!window)
        return;
    if (window->isMinimized())
        window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
    window->show();
#ifdef Q_OS_MACOS
    activateMacApplication();
#endif
    window->raise();
    window->activateWindow();
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HWND>(window->winId());
    if (IsIconic(handle))
        ShowWindow(handle, SW_RESTORE);
    // 使用标准前台请求；系统拒绝时只提示任务栏。
    if (!SetForegroundWindow(handle))
        QApplication::alert(window, 3000);
#endif
}
}

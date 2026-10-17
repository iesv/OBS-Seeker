#include "presentation-launch.hpp"
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDebug>
#include <QProcess>
#include <QTimer>
#include <QUuid>

#include <windows.h>

namespace obs_seeker {
QStringList presentationCommand(const QString &path)
{
    const auto extension = QFileInfo(path).suffix().toLower();
    const bool powerpoint = extension == QStringLiteral("ppt") || extension == QStringLiteral("pptx");
    if (!powerpoint)
        return {};
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&presentationCommand), &module))
        return {};
    wchar_t name[32768];
    const auto length = GetModuleFileNameW(module, name, 32768);
    if (!length || length >= 32768)
        return {};
    return {QFileInfo(QString::fromWCharArray(name, int(length))).dir().filePath(QStringLiteral("OBS-Seeker-present.exe")), path};
}

QProcess *preloadPowerPoint(QObject *owner, QStringList command)
{
    if (command.isEmpty()) {
        command = presentationCommand(QStringLiteral("preload.pptx"));
        if (command.isEmpty())
            return nullptr;
        command.removeLast();
    }
    const auto eventName = QStringLiteral("Local\\OBS-Seeker-preload-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto stop = CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<LPCWSTR>(eventName.utf16()));
    if (!stop)
        return nullptr;
    auto *process = new QProcess(owner);
    process->setObjectName(QStringLiteral("powerPointPreload"));
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, process, &QProcess::kill);
    QObject::connect(process, &QProcess::readyReadStandardOutput, process,
        [process, timeout, output = QByteArray{}]() mutable {
            output += process->readAllStandardOutput();
            if (output.contains("READY\n"))
                timeout->stop();
        });
    QObject::connect(process, &QProcess::finished, process, [process](int code, QProcess::ExitStatus status) {
        if (code != 0 || status != QProcess::NormalExit)
            qWarning() << "[OBS-Seeker] PowerPoint preload unavailable:" << code;
        process->deleteLater();
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            qWarning() << "[OBS-Seeker] PowerPoint preload unavailable:" << process->errorString();
            process->deleteLater();
        }
    });
    // 正常退出通知助手释放 COM；初始化卡住时仅结束助手，不结束 PowerPoint。
    QObject::connect(owner, &QObject::destroyed, process, [process, stop] {
        SetEvent(stop);
        if (process->state() != QProcess::NotRunning && !process->waitForFinished(1000)) {
            process->kill();
            process->waitForFinished(1000);
        }
    });
    QObject::connect(process, &QObject::destroyed, [stop] { CloseHandle(stop); });
    const auto program = command.takeFirst();
    command << QStringLiteral("--preload") << eventName << QString::number(QCoreApplication::applicationPid());
    timeout->start(30000);
    process->start(program, command);
    return process;
}
}

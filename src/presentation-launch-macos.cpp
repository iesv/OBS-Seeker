#include "presentation-launch.hpp"
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QProcess>
#include <QTemporaryFile>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>

namespace obs_seeker {
static QStringList helperCommand(const QString &argument)
{
    Dl_info module{};
    if (!dladdr(reinterpret_cast<const void *>(&presentationCommand), &module))
        return {};
    const auto helper = QFileInfo(QString::fromUtf8(module.dli_fname)).dir()
        .filePath(QStringLiteral("../Helpers/OBS-Seeker-present.app"));
    return {QStringLiteral("/usr/bin/open"), QStringLiteral("-n"), QStringLiteral("-W"), QStringLiteral("-g"),
            QStringLiteral("-a"), helper, QStringLiteral("--args"), argument};
}

QStringList presentationCommand(const QString &path)
{
    const auto extension = QFileInfo(path).suffix().toLower();
    if (extension != QStringLiteral("ppt") && extension != QStringLiteral("pptx") && extension != QStringLiteral("key"))
        return {};
    return helperCommand(path);
}

void retryPresentationAutomation(QObject *owner)
{
    if (!CFPreferencesGetAppBooleanValue(CFSTR("RetryAutomation"), CFSTR("com.andy.SeekerPresentation"), nullptr))
        return;
    auto command = helperCommand(QStringLiteral("--retry-automation"));
    if (command.isEmpty())
        return;
    auto *process = new QProcess(owner);
    auto *result = new QTemporaryFile(process);
    if (!result->open()) {
        delete process;
        return;
    }
    result->write("2\n");
    result->close();
    command.append(result->fileName());
    QObject::connect(process, &QProcess::finished, process, [process, result](int code, QProcess::ExitStatus status) {
        if (result->open()) {
            const auto output = result->readAll().trimmed();
            if (code != 0 || status != QProcess::NormalExit || !output.startsWith('0'))
                qWarning().noquote() << "[OBS-Seeker] Automation retry:" << output;
        }
        process->deleteLater();
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            qWarning() << "[OBS-Seeker] Cannot start automation retry:" << process->errorString();
            process->deleteLater();
        }
    });
    // Removing the result file cancels the helper if OBS closes during a permission prompt.
    QObject::connect(owner, &QObject::destroyed, process, [process, result] {
        process->blockSignals(true);
        result->remove();
        process->kill();
        process->waitForFinished(1000);
    });
    const auto program = command.takeFirst();
    process->start(program, command);
}
}

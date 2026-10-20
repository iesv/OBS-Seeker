#include "presentation-escape.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <iostream>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
void waitFor(const std::function<bool()> &ready)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    require(ready(), "Native keyboard operation timed out");
}
QByteArray readLog(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().replace("\r\n", "\n") : QByteArray{};
}
void key(WORD code, bool down)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = code;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    require(SendInput(1, &input, sizeof(input)) == 1, "Cannot send test key");
}
void escape()
{
    key(VK_ESCAPE, true);
    key(VK_ESCAPE, false);
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QProcess fixture;
    QProcess otherFixture;
    try {
        if (argc == 3 && QString::fromUtf8(argv[1]) == QStringLiteral("--observe")) {
            QFile log(QString::fromUtf8(argv[2]));
            require(log.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write observation log");
            const auto record = [&](const char *line) { log.write(line); log.flush(); };
            obs_seeker::PresentationEscape hook(&app, [&](auto complete) {
                record("captured\n");
                QTimer::singleShot(500, &app, [&, complete] {
                    record("exit-delay-elapsed\n");
                    complete(true);
                    QTimer::singleShot(500, &app, &QCoreApplication::quit);
                });
            });
            hook.setEnabled(true);
            hook.start();
            QTimer::singleShot(30000, &app, &QCoreApplication::quit);
            record("ready\n");
            return app.exec();
        }
        require(argc == 2, "Missing native fixture");
        QTemporaryDir temp;
        const auto log = temp.path() + QStringLiteral("/keys.txt");
        fixture.start(QString::fromUtf8(argv[1]), {log, QStringLiteral("screenClass")});
        require(fixture.waitForStarted(), "Cannot start native fixture");
        HWND window = nullptr;
        waitFor([&] {
            window = FindWindowW(L"screenClass", reinterpret_cast<LPCWSTR>(log.utf16()));
            return window && GetForegroundWindow() == window;
        });
        int requests = 0;
        obs_seeker::PresentationEscape::Completion complete;
        obs_seeker::PresentationEscape hook(&app, [&](auto callback) { ++requests; complete = std::move(callback); });
        hook.setEnabled(true);
        hook.start();
        // 等待钩子线程的事件循环就绪。
        auto *hookThread = hook.findChild<QThread *>();
        require(hookThread, "Missing Windows hook thread");
        waitFor([&] { return hookThread->isRunning() && hookThread->eventDispatcher(); });
        QThread::msleep(50);
        key(VK_ESCAPE, true);
        for (int repeat = 0; repeat < 500; ++repeat)
            key(VK_ESCAPE, true);
        key(VK_ESCAPE, false);
        waitFor([&] { return requests == 1; });
        QThread::msleep(100);
        QCoreApplication::processEvents();
        require(readLog(log).isEmpty(), "Esc reached the show before the exit delay elapsed");
        escape();
        QThread::msleep(30);
        QCoreApplication::processEvents();
        require(requests == 1 && readLog(log).isEmpty(), "Pending Esc repeated or escaped interception");
        complete(true);
        waitFor([&] { return readLog(log) == "down\nup\n"; });
        escape();
        waitFor([&] { return requests == 2; });
        complete(false);
        require(readLog(log) == "down\nup\n", "Canceled exit reached the show");
        escape();
        waitFor([&] { return requests == 3; });
        ShowWindow(window, SW_HIDE);
        complete(true);
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        require(readLog(log) == "down\nup\n", "Changed foreground received the delayed Esc");
        key(VK_SHIFT, true);
        escape();
        key(VK_SHIFT, false);
        waitFor([&] { return readLog(log).count("down") == 2; });
        require(requests == 3, "Modified Esc was intercepted");
        SetWindowPos(window, nullptr, 20, 20, 500, 400, SWP_NOZORDER);
        escape();
        waitFor([&] { return readLog(log).count("down") == 3; });
        require(requests == 3, "Windowed show was intercepted");
        const auto presenter = FindWindowW(L"PodiumParent", reinterpret_cast<LPCWSTR>(log.utf16()));
        const auto editor = FindWindowW(L"PPTFrameClass", reinterpret_cast<LPCWSTR>(log.utf16()));
        require(presenter && editor, "Missing presenter/editor fixtures");
        RECT bounds{};
        GetWindowRect(presenter, &bounds);
        SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                     bounds.bottom - bounds.top, SWP_NOZORDER);
        const auto activate = [&](HWND target) {
            ShowWindow(target, SW_SHOW);
            SetForegroundWindow(target);
            waitFor([&] { return GetForegroundWindow() == target; });
        };
        const auto presenterLog = log + QStringLiteral("PodiumParent");
        const auto editorLog = log + QStringLiteral("PPTFrameClass");
        for (int index = 0; index < 100; ++index) {
            const bool toPresenter = index % 2 == 0;
            activate(toPresenter ? window : presenter);
            const auto targetLog = toPresenter ? log : presenterLog;
            const auto before = readLog(targetLog);
            escape();
            waitFor([&] { return requests == 4 + index; });
            require(readLog(targetLog) == before, "Esc arrived before the exit delay elapsed");
            activate(toPresenter ? presenter : window);
            complete(true);
            waitFor([&] { return readLog(targetLog) == before + "down\nup\n"; });
        }
        activate(presenter);
        escape();
        waitFor([&] { return requests == 104; });
        activate(editor);
        complete(true);
        waitFor([&] { return readLog(presenterLog).count("down") == 51; });
        require(readLog(editorLog).isEmpty(), "Delayed Esc reached the editor");
        escape();
        waitFor([&] { return readLog(editorLog) == "down\nup\n"; });
        require(requests == 104, "Editor Esc was intercepted");
        activate(window);
        escape();
        waitFor([&] { return requests == 105; });
        const auto beforeOther = readLog(log);
        const auto otherLog = temp.path() + QStringLiteral("/other.txt");
        otherFixture.start(QString::fromUtf8(argv[1]), {otherLog, QStringLiteral("screenClass")});
        require(otherFixture.waitForStarted(), "Cannot start other PowerPoint fixture");
        HWND otherWindow = nullptr;
        waitFor([&] {
            otherWindow = FindWindowW(L"screenClass", reinterpret_cast<LPCWSTR>(otherLog.utf16()));
            return otherWindow && GetForegroundWindow() == otherWindow;
        });
        complete(true);
        waitFor([&] { return readLog(log) == beforeOther + "down\nup\n"; });
        require(readLog(otherLog).isEmpty(), "Delayed Esc reached another PowerPoint process");
        PostMessageW(otherWindow, WM_CLOSE, 0, 0);
        require(otherFixture.waitForFinished(), "Other fixture did not close");
        SetWindowPos(presenter, nullptr, 20, 20, 500, 400, SWP_NOZORDER);
        activate(presenter);
        escape();
        waitFor([&] { return readLog(presenterLog).count("down") == 52; });
        require(requests == 105, "Windowed presenter was intercepted");
        activate(window);
        hook.setEnabled(false);
        escape();
        waitFor([&] { return readLog(log).count("down") == 55; });
        hook.stop();
        escape();
        waitFor([&] { return readLog(log).count("down") == 56; });
        PostMessageW(window, WM_CLOSE, 0, 0);
        require(fixture.waitForFinished(), "Native fixture did not close");
        std::cout << "PASS: real Windows hook, 500 held repeats, 100 presenter/show focus switches, editor/other-process isolation, cancellation, modified/windowed Esc and shutdown\n";
        return 0;
    } catch (const std::exception &error) {
        if (otherFixture.state() != QProcess::NotRunning) {
            otherFixture.kill();
            otherFixture.waitForFinished();
        }
        fixture.kill();
        fixture.waitForFinished();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

#include "presentation-escape.hpp"
#include <QDebug>
#include <QEventLoop>
#include <QThread>
#include <cwchar>
#include <utility>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace obs_seeker {
void PresentationEscape::stop()
{
    enabled_ = false;
    if (auto *thread = std::exchange(thread_, nullptr)) {
        thread->requestInterruption();
        thread->quit();
        thread->wait();
        delete thread;
    }
}

namespace {
thread_local PresentationEscape *interceptor = nullptr;
thread_local bool held = false;

bool unmodified()
{
    return !(GetAsyncKeyState(VK_SHIFT) & 0x8000) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)
        && !(GetAsyncKeyState(VK_MENU) & 0x8000) && !(GetAsyncKeyState(VK_LWIN) & 0x8000)
        && !(GetAsyncKeyState(VK_RWIN) & 0x8000);
}

DWORD powerpointShow(HWND window)
{
    wchar_t name[64]{};
    if (!window || !IsWindowVisible(window) || !GetClassNameW(window, name, 64)
        || (_wcsicmp(name, L"screenClass") != 0 && _wcsicmp(name, L"PodiumParent") != 0))
        return 0;
    RECT rect{};
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetWindowRect(window, &rect) || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)
        || rect.left > monitor.rcMonitor.left || rect.top > monitor.rcMonitor.top
        || rect.right < monitor.rcMonitor.right || rect.bottom < monitor.rcMonitor.bottom)
        return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return 0;
    wchar_t path[32768]{};
    DWORD length = 32768;
    const bool found = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    const auto *base = wcsrchr(path, L'\\');
    return found && base && _wcsicmp(base + 1, L"POWERPNT.EXE") == 0 ? pid : 0;
}
}

void PresentationEscape::start()
{
    if (!enabled_) {
        stop();
        return;
    }
    if (thread_)
        return;
    thread_ = QThread::create([this] {
        interceptor = this;
        held = false;
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&powerpointShow), &module);
        const auto hook = SetWindowsHookExW(WH_KEYBOARD_LL, [](int code, WPARAM message, LPARAM data) -> LRESULT {
            const auto *key = reinterpret_cast<KBDLLHOOKSTRUCT *>(data);
            if (code == HC_ACTION && key->vkCode == VK_ESCAPE) {
                const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                if (held) {
                    if (!down)
                        held = false;
                    return 1;
                }
                if (down && interceptor->enabled_ && unmodified()) {
                    const auto window = GetForegroundWindow();
                    if (const auto pid = powerpointShow(window)) {
                        held = true;
                        GUITHREADINFO focus{sizeof(GUITHREADINFO)};
                        GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &focus);
                        const auto target = focus.hwndFocus && (focus.hwndFocus == window || IsChild(window, focus.hwndFocus))
                            ? focus.hwndFocus : window;
                        interceptor->captured([window, target, pid] {
                            if (powerpointShow(window) != pid)
                                return;
                            // 演示者视图由子控件接收按键；只通知原放映窗口内的控件。
                            const auto destination = target == window || IsChild(window, target) ? target : window;
                            constexpr LPARAM downMessage = 1 | (1 << 16);
                            constexpr LPARAM upMessage = downMessage | (LPARAM(1) << 30) | (LPARAM(1) << 31);
                            if (!PostMessageW(destination, WM_KEYDOWN, VK_ESCAPE, downMessage)
                                || !PostMessageW(destination, WM_KEYUP, VK_ESCAPE, upMessage))
                                qWarning() << "[OBS-Seeker] Cannot deliver presentation Esc:" << GetLastError();
                        });
                        return 1;
                    }
                }
            }
            return CallNextHookEx(nullptr, code, message, data);
        }, module, 0);
        if (!hook) {
            qWarning() << "[OBS-Seeker] Cannot intercept presentation Esc:" << GetLastError();
            return;
        }
        if (!QThread::currentThread()->isInterruptionRequested()) {
            QEventLoop loop;
            loop.exec();
        }
        UnhookWindowsHookEx(hook);
        interceptor = nullptr;
    });
    thread_->setParent(this);
    thread_->start();
}
}

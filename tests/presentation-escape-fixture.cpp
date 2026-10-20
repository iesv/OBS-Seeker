#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <fstream>
#include <filesystem>
#include <cwchar>

namespace {
std::filesystem::path logPath;
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM key, LPARAM data)
{
    if (message == WM_SETFOCUS) {
        if (const auto pane = FindWindowExW(window, nullptr, L"PresenterSlidePane", nullptr))
            SetFocus(pane);
    }
    if ((message == WM_KEYDOWN || message == WM_KEYUP) && key == VK_ESCAPE) {
        wchar_t type[64]{};
        const auto root = GetAncestor(window, GA_ROOT);
        GetClassNameW(root, type, 64);
        if (window == root && std::wcscmp(type, L"PodiumParent") == 0)
            return 0; // 演示者顶层窗口不处理 Esc。
        auto path = logPath;
        if (std::wcscmp(type, L"screenClass") != 0)
            path += type;
        std::ofstream(path, std::ios::app) << (message == WM_KEYDOWN ? "down\n" : "up\n");
    }
    if (message == WM_DESTROY)
        PostQuitMessage(0);
    return DefWindowProcW(window, message, key, data);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    auto **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc != 3) {
        LocalFree(argv);
        return 1;
    }
    logPath = argv[1];
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpszClassName = argv[2];
    type.lpfnWndProc = windowProc;
    RegisterClassW(&type);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const auto &rect = monitor.rcMonitor;
    auto window = CreateWindowExW(0, argv[2], argv[1], WS_POPUP | WS_VISIBLE, rect.left, rect.top,
                                 rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    for (const auto *name : {L"PodiumParent", L"PPTFrameClass"}) {
        type.lpszClassName = name;
        RegisterClassW(&type);
        const auto secondary = CreateWindowExW(0, name, argv[1], WS_POPUP, rect.left, rect.top,
                                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
        if (std::wcscmp(name, L"PodiumParent") == 0) {
            type.lpszClassName = L"PresenterSlidePane";
            RegisterClassW(&type);
            CreateWindowExW(0, type.lpszClassName, L"Slide", WS_CHILD | WS_VISIBLE,
                            0, 0, 400, 300, secondary, nullptr, instance, nullptr);
        }
    }
    LocalFree(argv);
    if (!window)
        return 1;
    SetForegroundWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

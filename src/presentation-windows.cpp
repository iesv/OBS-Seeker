#include "presentation-result.hpp"
#include <windows.h>
#include <shellapi.h>
#include <comdef.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {
using Dispatch = _com_ptr_t<_com_IIID<IDispatch, &IID_IDispatch>>;

_variant_t invoke(IDispatch *object, const wchar_t *name, WORD flags,
                  std::initializer_list<_variant_t> values = {})
{
    DISPID id;
    auto *member = const_cast<wchar_t *>(name);
    _com_util::CheckError(object->GetIDsOfNames(IID_NULL, &member, 1, LOCALE_USER_DEFAULT, &id));
    std::vector<_variant_t> arguments(values);
    std::reverse(arguments.begin(), arguments.end());
    static_assert(sizeof(_variant_t) == sizeof(VARIANT));
    DISPID put = DISPID_PROPERTYPUT;
    DISPPARAMS parameters{reinterpret_cast<VARIANT *>(arguments.data()),
                          flags == DISPATCH_PROPERTYPUT ? &put : nullptr,
                          static_cast<UINT>(arguments.size()), flags == DISPATCH_PROPERTYPUT ? 1u : 0u};
    _variant_t result;
    _com_util::CheckError(object->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &parameters, &result, nullptr, nullptr));
    return result;
}

Dispatch object(const _variant_t &value)
{
    if (value.vt != VT_DISPATCH || !value.pdispVal)
        _com_issue_error(E_NOINTERFACE);
    return Dispatch(value.pdispVal);
}

int preload(const wchar_t *eventName, const wchar_t *parentId)
{
    using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
    Handle stop(OpenEventW(SYNCHRONIZE, FALSE, eventName), CloseHandle);
    Handle parent(OpenProcess(SYNCHRONIZE, FALSE, wcstoul(parentId, nullptr, 10)), CloseHandle);
    if (!stop || !parent)
        return 1;
    HANDLE handles[]{stop.get(), parent.get()};
    if (WaitForMultipleObjects(2, handles, FALSE, 0) != WAIT_TIMEOUT)
        return 0;
    Dispatch application;
    _com_util::CheckError(application.CreateInstance(L"PowerPoint.Application"));
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "READY\n", 6, &written, nullptr);
    // 保持 STA 消息处理；停止或 OBS 退出后释放引用，不调用 Quit。
    DWORD index;
    _com_util::CheckError(CoWaitForMultipleHandles(COWAIT_DEFAULT, INFINITE, 2, handles, &index));
    return 0;
}

int present(const wchar_t *inputPath, bool &canFallback)
{
    std::wstring nativePath(inputPath);
    std::replace(nativePath.begin(), nativePath.end(), L'/', L'\\');
    const auto *path = nativePath.c_str();
    Dispatch application;
    _com_util::CheckError(application.CreateInstance(L"PowerPoint.Application"));
    const auto presentations = object(invoke(application, L"Presentations", DISPATCH_PROPERTYGET));
    Dispatch presentation;
    const long count = invoke(presentations, L"Count", DISPATCH_PROPERTYGET);
    for (long index = 1; index <= count; ++index) {
        const auto candidate = object(invoke(presentations, L"Item", DISPATCH_METHOD | DISPATCH_PROPERTYGET, {_variant_t(index)}));
        const _bstr_t fullName(invoke(candidate, L"FullName", DISPATCH_PROPERTYGET));
        if (_wcsicmp(static_cast<const wchar_t *>(fullName), path) == 0) {
            presentation = candidate;
            canFallback = false;
            break;
        }
    }
    if (!presentation) {
        const auto security = invoke(application, L"AutomationSecurity", DISPATCH_PROPERTYGET);
        invoke(application, L"AutomationSecurity", DISPATCH_PROPERTYPUT, {_variant_t(3L)});
        try {
            // 只读打开，不创建编辑窗口。
            const auto opened = invoke(presentations, L"Open", DISPATCH_METHOD,
                {_variant_t(path), _variant_t(-1L), _variant_t(0L), _variant_t(0L)});
            canFallback = false;
            presentation = object(opened);
        } catch (const _com_error &) {
            invoke(application, L"AutomationSecurity", DISPATCH_PROPERTYPUT, {security});
            throw;
        }
        invoke(application, L"AutomationSecurity", DISPATCH_PROPERTYPUT, {security});
        // 新开的只读文稿不保留临时状态；不写入文件。
        invoke(presentation, L"Saved", DISPATCH_PROPERTYPUT, {_variant_t(-1L)});
    }
    Dispatch window;
    try {
        window = object(invoke(presentation, L"SlideShowWindow", DISPATCH_PROPERTYGET));
    } catch (const _com_error &) {
        // 尚未放映时没有 SlideShowWindow。
    }
    if (window) {
        if (long(invoke(window, L"IsFullScreen", DISPATCH_PROPERTYGET)) != 0)
            return 0;
        invoke(object(invoke(window, L"View", DISPATCH_PROPERTYGET)), L"Exit", DISPATCH_METHOD);
    }
    const auto settings = object(invoke(presentation, L"SlideShowSettings", DISPATCH_PROPERTYGET));
    const auto previousType = invoke(settings, L"ShowType", DISPATCH_PROPERTYGET);
    if (long(previousType) == 1L) {
        window = object(invoke(settings, L"Run", DISPATCH_METHOD));
        return long(invoke(window, L"IsFullScreen", DISPATCH_PROPERTYGET)) != 0 ? 0 : 1;
    }
    const auto previousSaved = invoke(presentation, L"Saved", DISPATCH_PROPERTYGET);
    invoke(settings, L"ShowType", DISPATCH_PROPERTYPUT, {_variant_t(1L)});
    invoke(presentation, L"Saved", DISPATCH_PROPERTYPUT, {previousSaved});
    try {
        window = object(invoke(settings, L"Run", DISPATCH_METHOD));
    } catch (const _com_error &) {
        invoke(settings, L"ShowType", DISPATCH_PROPERTYPUT, {previousType});
        invoke(presentation, L"Saved", DISPATCH_PROPERTYPUT, {previousSaved});
        throw;
    }
    invoke(settings, L"ShowType", DISPATCH_PROPERTYPUT, {previousType});
    invoke(presentation, L"Saved", DISPATCH_PROPERTYPUT, {previousSaved});
    return long(invoke(window, L"IsFullScreen", DISPATCH_PROPERTYGET)) != 0 ? 0 : 1;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    auto **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        return 1;
    const bool preloading = count == 4 && wcscmp(arguments[1], L"--preload") == 0;
    const auto attributes = count == 2 ? GetFileAttributesW(arguments[1]) : INVALID_FILE_ATTRIBUTES;
    int result = 1;
    bool canFallback = true;
    auto error = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(error)) {
        if (preloading || (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))) {
            try {
                result = preloading ? preload(arguments[2], arguments[3]) : present(arguments[1], canFallback);
            } catch (const _com_error &failure) {
                error = failure.Error();
                result = canFallback ? obs_seeker::PresentationNotOpened : 1;
            }
        }
        CoUninitialize();
    } else {
        result = obs_seeker::PresentationNotOpened;
    }
    if (FAILED(error)) {
        char message[128];
        std::snprintf(message, sizeof(message), "PowerPoint %s failed (HRESULT 0x%08lX)\n",
                      preloading ? "preload" : canFallback ? "initialization" : "open/slideshow", static_cast<unsigned long>(error));
        DWORD written;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
    }
    LocalFree(arguments);
    return result;
}

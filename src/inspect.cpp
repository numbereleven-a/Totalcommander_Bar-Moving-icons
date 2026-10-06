#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include "protocol.h"

static DWORD processId;
static BOOL CALLBACK property(HWND, LPWSTR name, HANDLE, ULONG_PTR) {
    if (reinterpret_cast<ULONG_PTR>(name) > 65535) std::wprintf(L"property=%ls\n", name);
    else std::wprintf(L"property=atom:%llu\n", static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(name)));
    return TRUE;
}
static BOOL CALLBACK child(HWND window, LPARAM) {
    wchar_t name[128]{};
    GetClassNameW(window, name, 128);
    RECT rectangle{};
    GetWindowRect(window, &rectangle);
    DWORD_PTR buttons{};
    if (!wcscmp(name, L"ToolbarWindow32"))
        SendMessageTimeoutW(window, TB_BUTTONCOUNT, 0, 0, SMTO_ABORTIFHUNG, 100, &buttons);
    std::wprintf(L"class=%ls handle=%p visible=%d rectangle=%ld,%ld,%ld,%ld buttons=%lld\n",
        name, window, IsWindowVisible(window), rectangle.left, rectangle.top,
        rectangle.right, rectangle.bottom, static_cast<long long>(buttons));
    return TRUE;
}
static BOOL CALLBACK top(HWND window, LPARAM) {
    DWORD id{};
    GetWindowThreadProcessId(window, &id);
    if (id == processId) {
        child(window, 0);
        wchar_t name[128]{};
        GetClassNameW(window, name, 128);
        if (!wcscmp(name, L"TTOTAL_CMD")) {
            HWND bar = reinterpret_cast<HWND>(SendMessageW(window, WM_USER + 50, 28, 0));
            std::wprintf(L"horizontal-bar=%p\n", bar);
            GUITHREADINFO gui{};
            gui.cbSize = sizeof(gui);
            GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &gui);
            std::wprintf(L"toolbar-focused=%d\n", gui.hwndFocus == bar);
            std::wprintf(L"extension-attached=%d editing=%d error=%d\n",
                GetPropW(window, barmove::AttachedProperty) != nullptr,
                GetPropW(window, barmove::EditingProperty) != nullptr,
                GetPropW(window, barmove::ErrorProperty) != nullptr);
            EnumPropsExW(bar, property, 0);
            wchar_t caption[1024]{};
            GetWindowTextW(bar, caption, 1024);
            std::wprintf(L"bar-caption=%ls\n", caption);
        }
        EnumChildWindows(window, child, 0);
    }
    return TRUE;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    processId = wcstoul(argv[1], nullptr, 10);
    EnumWindows(top, 0);
    return 0;
}

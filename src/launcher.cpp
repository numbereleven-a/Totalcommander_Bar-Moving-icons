#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <string>
#include "protocol.h"

static DWORD parent_process() {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent{};
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ProcessID == GetCurrentProcessId()) { parent = entry.th32ParentProcessID; break; }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return parent;
}
struct Search { DWORD process; HWND window = nullptr; };
static BOOL CALLBACK find_window(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<Search*>(parameter);
    DWORD process{};
    GetWindowThreadProcessId(window, &process);
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    if (process == search.process && !wcscmp(name, L"TTOTAL_CMD")) search.window = window;
    return TRUE;
}
static int run(int argc, wchar_t** argv) {
    DWORD process = parent_process();
    int argument = 1;
    if (argc > 3 && !wcscmp(argv[1], L"--pid")) {
        process = wcstoul(argv[2], nullptr, 10);
        argument = 3;
    }
    if (argument + 1 != argc) return 2;
    Search search{process};
    EnumWindows(find_window, reinterpret_cast<LPARAM>(&search));
    if (!search.window) return 3;
    if (!GetPropW(search.window, barmove::AttachedProperty)) {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return 4;
        path.resize(length);
        path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"TcBarMove.dll";
        HMODULE module = LoadLibraryW(path.c_str());
        if (!module) return 4;
        const auto hookProcedure = reinterpret_cast<HOOKPROC>(reinterpret_cast<void*>(GetProcAddress(module, "BarMoveHook")));
        const DWORD thread = GetWindowThreadProcessId(search.window, nullptr);
        const auto hook = hookProcedure ? SetWindowsHookExW(WH_CALLWNDPROC, hookProcedure, module, thread) : nullptr;
        if (!hook) { FreeLibrary(module); return 5; }
        DWORD_PTR response{};
        const bool delivered = SendMessageTimeoutW(search.window, barmove::attach_message(), 0, 0,
            SMTO_ABORTIFHUNG, 5000, &response) != 0;
        const bool attached = GetPropW(search.window, barmove::AttachedProperty) != nullptr;
        UnhookWindowsHookEx(hook);
        FreeLibrary(module);
        if (!delivered || !attached) return 6;
    }
    wchar_t fullpath[32768]{};
    const DWORD length = GetFullPathNameW(argv[argument], 32768, fullpath, nullptr);
    if (!length || length >= 32768) return 7;
    COPYDATASTRUCT data{barmove::ConfigureMessage, static_cast<DWORD>((wcslen(fullpath) + 1) * sizeof(wchar_t)), fullpath};
    DWORD_PTR response{};
    if (!SendMessageTimeoutW(search.window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG, 5000, &response) || !response) return 8;
    if (response != static_cast<DWORD_PTR>(barmove::ConfigureResult::Success))
        return response <= static_cast<DWORD_PTR>(barmove::ConfigureResult::InvalidRequest) ? 8 + response : 8;
    return 0;
}
static const wchar_t* error_text(int result) {
    switch (result) {
    case 2: return L"Start this command from a Total Commander button and pass the active .bar file.";
    case 3: return L"Cannot find the Total Commander window. Start this command from its button bar.";
    case 4: return L"Cannot load TcBarMove.dll. Place the EXE and DLL together.";
    case 5: case 6: return L"Cannot attach to Total Commander. This build requires Total Commander x64 and matching process privileges.";
    case 7: return L"Cannot resolve the supplied .bar path.";
    case 8: return L"Total Commander did not accept the request or did not respond.";
    case 10: return L"The horizontal button bar is unavailable.";
    case 11: return L"The supplied .bar file does not match the active button bar. Correct the button Parameters field.";
    case 12: return L"The .bar file has no TcBarMove.exe mode button.";
    case 13: return L"Cannot read the settings file specified by COMMANDER_INI.";
    case 14: return L"The configured button height is invalid.";
    case 15: return L"Cannot read this .bar file safely. Check its encoding, buttoncount and file permissions.";
    case 16: return L"The button movement request is invalid.";
    default: return L"Cannot enable button movement.";
    }
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool quiet = argv && argc > 1 && !wcscmp(argv[1], L"--quiet");
    const int result = argv ? run(quiet ? argc - 1 : argc, quiet ? argv + 1 : argv) : 2;
    LocalFree(argv);
    if (result && !quiet) {
        const auto message = std::wstring(error_text(result)) + L" (error " + std::to_wstring(result) + L")";
        MessageBoxW(nullptr, message.c_str(), L"Button movement", MB_OK | MB_ICONERROR);
    }
    return result;
}

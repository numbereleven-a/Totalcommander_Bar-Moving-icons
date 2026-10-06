#pragma once
#include <windows.h>

namespace barmove {
constexpr ULONG_PTR ConfigureMessage = 0x42524d31;
constexpr UINT_PTR SubclassId = 0x42524d31;
constexpr wchar_t AttachedProperty[] = L"TcBarMove.Attached.1";
constexpr wchar_t EditingProperty[] = L"TcBarMove.Editing.1";
constexpr wchar_t ErrorProperty[] = L"TcBarMove.Error.1";
enum class ConfigureResult : ULONG_PTR {
    Success = 1, BarUnavailable, WrongBar, ModeButtonMissing,
    SettingsMissing, InvalidSize, FileError, InvalidRequest
};
inline UINT attach_message() {
    static const UINT message = RegisterWindowMessageW(L"TcBarMove.Attach.1");
    return message;
}
inline UINT finish_reload_message() {
    static const UINT message = RegisterWindowMessageW(L"TcBarMove.Reloaded.1");
    return message;
}
}

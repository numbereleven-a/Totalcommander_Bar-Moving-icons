#include <windows.h>
#include <cstdio>
#include <string>
#include <stdexcept>

static DWORD replacementError;
static BOOL WINAPI replace_file(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
#define ReplaceFileW replace_file
#include "../src/extension.cpp"
#undef ReplaceFileW

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static BOOL WINAPI replace_file(LPCWSTR original, LPCWSTR replacement, LPCWSTR backup,
                               DWORD flags, LPVOID exclude, LPVOID reserved) {
    if (!replacementError) return ReplaceFileW(original, replacement, backup, flags, exclude, reserved);
    const DWORD error = replacementError;
    replacementError = 0;
    if (error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2)
        require(MoveFileW(original, (std::wstring(original) + L".renamed").c_str()) != 0,
                "Cannot simulate a partially completed replacement");
    SetLastError(error);
    return FALSE;
}
struct Fixtures {
    std::wstring directory;
    Fixtures() {
        wchar_t temporary[MAX_PATH]{}, name[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, temporary) && GetTempFileNameW(temporary, L"bmt", 0, name),
                "Cannot allocate fixture directory");
        directory = name;
        require(DeleteFileW(name) && CreateDirectoryW(name, nullptr), "Cannot create fixture directory");
    }
    ~Fixtures() {
        WIN32_FIND_DATAW entry{};
        HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &entry);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    DeleteFileW((directory + L"\\" + entry.cFileName).c_str());
            } while (FindNextFileW(search, &entry));
            FindClose(search);
        }
        RemoveDirectoryW(directory.c_str());
    }
    std::wstring path(const wchar_t* name) const { return directory + L"\\" + name; }
};
static void write_bytes(const std::wstring& path, const std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Cannot create fixture");
    DWORD written{};
    const bool success = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    require(success && written == bytes.size(), "Cannot write fixture");
}
static std::string contents(const std::wstring& path) {
    const auto bytes = read_bytes(path);
    return {bytes.begin(), bytes.end()};
}
static std::vector<std::wstring> backups(const std::wstring& path) {
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((path + L".barmove-*.bak").c_str(), &entry);
    std::vector<std::wstring> result;
    if (search != INVALID_HANDLE_VALUE) {
        do { result.push_back(path.substr(0, path.find_last_of(L'\\') + 1) + entry.cFileName); }
        while (FindNextFileW(search, &entry));
        FindClose(search);
    }
    return result;
}
static const std::string fixture =
    "[Buttonbar]\r\nbuttoncount=6\r\nbutton1=lock.ico\r\ncmd1=TcBarMove.exe\r\n"
    "button2=a.ico\r\ncmd2=A\r\nbutton3=b.ico\r\ncmd3=B\r\ncmd4=-2\r\n"
    "button5=c.ico\r\ncmd5=C\r\nbutton6=d.ico\r\ncmd6=D\r\n";
static const std::string movedFixture =
    "[Buttonbar]\r\nbuttoncount=6\r\nbutton1=lock.ico\r\ncmd1=TcBarMove.exe\r\n"
    "button3=a.ico\r\ncmd3=A\r\nbutton4=b.ico\r\ncmd4=B\r\ncmd5=-2\r\n"
    "button6=c.ico\r\ncmd6=C\r\nbutton2=d.ico\r\ncmd2=D\r\n";
static std::string utf16(const std::string& ascii) {
    std::string bytes("\xff\xfe", 2);
    for (char value : ascii) { bytes += value; bytes += '\0'; }
    // A non-ASCII comment in UTF-16LE.
    bytes.append(";\0 \0\x16\x04\r\0\n\0", 10);
    return bytes;
}
static void file_checks(Fixtures& files) {
    const std::string unknown = "unknown999999999999999999999999=keep\r\n=unknown2";
    const auto path = files.path(L"text.bar");
    const auto original = fixture + unknown;
    write_bytes(path, original);
    BarFile file(path);
    file.move(1, 2);
    require(backups(path).empty() && contents(path) == original, "A no-op created a backup or changed the file");
    file.move(5, 1);
    require(contents(path) == movedFixture + unknown, "Moving changed unrelated text or malformed keys");
    file.move(1, 6);
    const auto copies = backups(path);
    require(copies.size() == 1 && contents(copies[0]) == original, "Session backup is missing, repeated or incorrect");
    write_bytes(path, original + "\r\n; external change");
    bool rejected = false;
    try { file.move(5, 1); } catch (const std::exception&) { rejected = true; }
    require(rejected && contents(path) == original + "\r\n; external change", "External edit was overwritten");

    const auto utf8Path = files.path(L"utf8.bar");
    const std::string prefix("\xef\xbb\xbf", 3), comment = "; \xd0\x96\r\n";
    write_bytes(utf8Path, prefix + fixture + comment);
    BarFile utf8File(utf8Path);
    utf8File.move(5, 1);
    require(contents(utf8Path) == prefix + movedFixture + comment, "UTF-8 text or BOM changed");
    const auto utf16Path = files.path(L"utf16.bar");
    write_bytes(utf16Path, utf16(fixture));
    BarFile utf16File(utf16Path);
    utf16File.move(5, 1);
    require(contents(utf16Path) == utf16(movedFixture), "UTF-16 text or BOM changed");
    const auto invalidPath = files.path(L"invalid.bar");
    const auto invalid = prefix + fixture + "; invalid byte: \xff\r\n";
    write_bytes(invalidPath, invalid);
    rejected = false;
    try { BarFile invalidFile(invalidPath); } catch (const std::exception&) { rejected = true; }
    require(rejected && contents(invalidPath) == invalid, "Invalid encoding was accepted or modified");
    const auto retainedPath = files.path(L"retained.bar");
    write_bytes(retainedPath, fixture);
    for (const auto* stamp : {L"20000101-000000-000", L"20010101-000000-000", L"20020101-000000-000"})
        write_bytes(retainedPath + L".barmove-" + stamp + L".bak", "old backup");
    const auto newestOldBackup = retainedPath + L".barmove-20030101-000000-000.bak";
    write_bytes(newestOldBackup, "newest old backup");
    const auto manualBackup = retainedPath + L".barmove-manual.bak";
    const auto otherBackup = files.path(L"other.bar.barmove-20000101-000000-000.bak");
    write_bytes(manualBackup, "manual backup");
    write_bytes(otherBackup, "other bar backup");
    BarFile retainedFile(retainedPath);
    retainedFile.move(5, 1);
    auto retainedCopies = backups(retainedPath);
    retainedCopies.erase(std::remove(retainedCopies.begin(), retainedCopies.end(), manualBackup), retainedCopies.end());
    require(retainedCopies.size() == 2 && contents(newestOldBackup) == "newest old backup",
            "Backup retention did not keep the two latest automatic backups");
    const auto currentBackup = retainedCopies[0] == newestOldBackup ? retainedCopies[1] : retainedCopies[0];
    require(contents(currentBackup) == fixture && contents(manualBackup) == "manual backup" &&
            contents(otherBackup) == "other bar backup", "Retention removed unrelated files or lost the current backup");
    retainedFile.move(1, 6);
    require(contents(currentBackup) == fixture, "A later move changed the session backup");
    std::puts("PASS: file preservation, encoding, two-backup retention and external edits");
}
static void replacement_checks(Fixtures& files) {
    const auto path = files.path(L"replace.bar");
    write_bytes(path, fixture);
    BarFile file(path);
    const auto temporary = path + L".barmove-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    replacementError = ERROR_ACCESS_DENIED;
    bool rejected = false;
    try { file.move(5, 1); } catch (const std::exception&) { rejected = true; }
    require(rejected && contents(path) == fixture && GetFileAttributesW(temporary.c_str()) == INVALID_FILE_ATTRIBUTES,
            "Ordinary save failure lost the original or retained an incomplete update");
    replacementError = ERROR_UNABLE_TO_MOVE_REPLACEMENT_2;
    rejected = false;
    try { file.move(5, 1); } catch (const std::exception&) { rejected = true; }
    require(rejected && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
            contents(temporary) == movedFixture && contents(backups(path).at(0)) == fixture,
            "Partial replacement failure lost the recovery files");
    std::puts("PASS: ordinary and partially completed replacement failures (injected Win32 results)");
}
static HWND hostBar;
static int nativeUps, systemKeys, menuKeys;
static bool lockOnFocus;
static bool nativeIconInUpdate, nativeModeInUpdate, nativeOtherInUpdate;
static int nativePaints;
static LRESULT CALLBACK host_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_USER + 50 && wparam == 28) return reinterpret_cast<LRESULT>(hostBar);
    return DefWindowProcW(window, message, wparam, lparam);
}
static LRESULT CALLBACK native_bar_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND && wparam) {
        const RECT client{0, 0, 240, 80};
        HBRUSH brush = CreateSolidBrush(RGB(80, 90, 100));
        FillRect(reinterpret_cast<HDC>(wparam), &client, brush);
        DeleteObject(brush);
        return 1;
    }
    if (message == WM_PAINT) {
        HRGN update = CreateRectRgn(0, 0, 0, 0);
        GetUpdateRgn(window, update, FALSE);
        const RECT icon{9, 9, 25, 25}, modeButton{1, 1, 33, 33}, otherIcon{41, 9, 57, 25};
        nativeIconInUpdate = RectInRegion(update, &icon) != 0;
        nativeModeInUpdate = RectInRegion(update, &modeButton) != 0;
        nativeOtherInUpdate = RectInRegion(update, &otherIcon) != 0;
        ++nativePaints;
        DeleteObject(update);
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_LBUTTONUP) { ++nativeUps; return 0; }
    if (message == WM_SYSKEYDOWN || message == WM_SYSCHAR) { ++systemKeys; return 0; }
    if (message == WM_KEYDOWN && wparam == VK_F10) { ++menuKeys; return 0; }
    if (message == WM_SETFOCUS && lockOnFocus) lock();
    return DefWindowProcW(window, message, wparam, lparam);
}
static HWND create_bar(const std::wstring& path) {
    return CreateWindowExW(0, L"BarMoveTestBar", basename(path).c_str(), WS_CHILD | WS_VISIBLE,
        0, 0, 240, 80, commander, nullptr, GetModuleHandleW(nullptr), nullptr);
}
static ConfigureResult configure(const std::wstring& path) {
    COPYDATASTRUCT data{ConfigureMessage, static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)),
                        const_cast<wchar_t*>(path.c_str())};
    return static_cast<ConfigureResult>(SendMessageW(commander, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)));
}
static void lock_overlay_check() {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 240;
    info.bmiHeader.biHeight = -80;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels{};
    HDC dc = CreateCompatibleDC(nullptr), imageDc = CreateCompatibleDC(nullptr);
    HBITMAP surface = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto previous = SelectObject(dc, surface);
    info.bmiHeader.biWidth = 32;
    info.bmiHeader.biHeight = -32;
    dragBitmap = CreateDIBSection(imageDc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto imagePrevious = SelectObject(imageDc, dragBitmap);
    HBRUSH background = CreateSolidBrush(RGB(40, 50, 60)), red = CreateSolidBrush(RGB(220, 30, 30));
    const RECT client{0, 0, 240, 80}, cell{0, 0, 32, 32};
    FillRect(imageDc, &cell, red);
    SelectObject(imageDc, imagePrevious);
    FillRect(dc, &client, background);
    SendMessageW(toolbar, WM_ERASEBKGND, reinterpret_cast<WPARAM>(dc), 0);
    const bool frameProtected = GetPixel(dc, 1, 16) == RGB(40, 50, 60) &&
        GetPixel(dc, 16, 16) == RGB(40, 50, 60) && GetPixel(dc, 49, 16) == RGB(80, 90, 100);
    FillRect(dc, &client, background);
    draw_feedback(dc);
    const COLORREF original = GetPixel(dc, 16, 16);
    source = 1;
    dragging = true;
    mousePoint = POINT{24, 16};
    insertion = target_at(mousePoint);
    FillRect(dc, &client, background);
    draw_feedback(dc);
    const bool stable = GetPixel(dc, 16, 16) == original;
    const bool ghostVisible = GetPixel(dc, 39, 16) == RGB(220, 30, 30);
    const bool markerVisible = GetPixel(dc, 33, 16) == RGB(0, 122, 255);
    SelectObject(dc, previous);
    DeleteDC(imageDc);
    DeleteDC(dc);
    DeleteObject(surface);
    DeleteObject(background);
    DeleteObject(red);
    cancel_drag();
    require(frameProtected, "Native background erasure touched the mode button or was suppressed for other buttons");
    require(stable, "Dragged image overwrote the mode icon");
    require(ghostVisible, "Drag preview disappeared outside the mode button");
    require(markerVisible, "Dragged image hid the insertion marker");
    std::puts("PASS: dragged image leaves mode icon unchanged at its boundary");
}
static void input_checks(Fixtures& files) {
    const auto path = files.path(L"input.bar"), ini = files.path(L"settings.ini");
    write_bytes(path, fixture);
    write_bytes(ini, "[Buttonbar]\r\nButtonheight96=32\r\n");
    SetEnvironmentVariableW(L"COMMANDER_INI", ini.c_str());
    moduleInstance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.hInstance = moduleInstance;
    type.lpszClassName = L"BarMoveTestHost";
    type.lpfnWndProc = host_proc;
    require(RegisterClassW(&type) != 0, "Cannot register test host");
    type.lpszClassName = L"BarMoveTestBar";
    type.lpfnWndProc = native_bar_proc;
    type.style = CS_DBLCLKS;
    require(RegisterClassW(&type) != 0, "Cannot register test bar");
    commander = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"BarMoveTestHost", L"", WS_POPUP | WS_VISIBLE,
        -32000, -32000, 400, 100, nullptr, nullptr, moduleInstance, nullptr);
    require(commander != nullptr, "Cannot create test host");
    hostBar = toolbar = create_bar(path);
    require(IsWindowVisible(toolbar) != 0, "Test bar is not visible to Win32");
    require(SetWindowSubclass(commander, main_proc, SubclassId, 0) &&
            SetWindowSubclass(toolbar, bar_proc, SubclassId, 0), "Cannot attach test subclasses");
    SetPropW(commander, AttachedProperty, reinterpret_cast<HANDLE>(1));
    require(configure(files.path(L"wrong.bar")) == ConfigureResult::WrongBar, "Wrong-bar error was not distinguished");
    SetWindowTextW(toolbar, path.c_str());
    require(configure(files.path(L"other\\input.bar")) == ConfigureResult::WrongBar,
            "Full caption accepted a different file with the same basename");
    SetWindowTextW(toolbar, basename(path).c_str());
    SetEnvironmentVariableW(L"COMMANDER_INI", files.path(L"missing.ini").c_str());
    require(configure(path) == ConfigureResult::SettingsMissing, "Missing-settings error was not distinguished");
    SetEnvironmentVariableW(L"COMMANDER_INI", ini.c_str());
    require(configure(path) == ConfigureResult::Success && editing, "Cannot enable movement in test host");
    InvalidateRect(toolbar, nullptr, FALSE);
    UpdateWindow(toolbar);
    require(nativePaints > 0 && !nativeIconInUpdate && !nativeModeInUpdate && nativeOtherInUpdate,
            "Native paint can briefly replace the mode icon or erase its frame");
    lock();
    InvalidateRect(toolbar, nullptr, FALSE);
    UpdateWindow(toolbar);
    require(nativeIconInUpdate, "Locked mode did not restore native icon painting");
    require(configure(path) == ConfigureResult::Success, "Cannot enable after paint check");
    lock_overlay_check();
    source = 5;
    require(target_at(POINT{80, 0}).index == -1 && target_at(POINT{80, 75}).index == -1,
            "Vertical slack became an insertion target");
    const auto end = target_at(POINT{140, 16}), beginning = target_at(POINT{0, 49});
    require(end.index == 3 && end.edge.left == 97 && end.edge.top == 1 &&
            beginning.index == 4 && beginning.edge.top == 33, "Manual line-break targets disagree with their markers");
    source = 5;
    const auto boundary = target_at(POINT{0, 16});
    require(boundary.index == 1 && boundary.edge.left == 33, "Mode-button boundary has a misleading marker");
    SetWindowPos(toolbar, nullptr, 0, 0, 72, 120, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    source = 5;
    const auto wrappedEnd = target_at(POINT{70, 16}), wrappedStart = target_at(POINT{0, 49});
    require(wrappedEnd.index == 2 && wrappedEnd.edge.left == 65 && wrappedEnd.edge.top == 1 &&
            wrappedStart.index == 2 && wrappedStart.edge.left == 1 && wrappedStart.edge.top == 33,
            "Automatic wrapping moved the marker to a different row from the pointer");
    SetWindowPos(toolbar, nullptr, 0, 0, 240, 80, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    source = -1;
    HWND panel = CreateWindowExW(0, L"STATIC", L"panel", WS_CHILD | WS_VISIBLE,
        0, 85, 100, 10, commander, nullptr, moduleInstance, nullptr);
    SetFocus(panel);
    require(GetFocus() == panel, "Test panel could not take focus");
    SendMessageW(toolbar, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(49, 49));
    require(source == 5 && GetFocus() == toolbar, "Double-click did not start a new gesture");
    SendMessageW(toolbar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(140, 16));
    require(insertion.index == 3 && insertion.edge.top == 1, "Drag displayed the wrong insertion target");
    SendMessageW(toolbar, WM_LBUTTONUP, 0, MAKELPARAM(140, 16));
    require(bar->field(3, L"cmd") == L"D" && bar->linebreak(4) && GetFocus() == panel,
            "Drop used a different target or failed to restore focus");
    SendMessageW(commander, finish_reload_message(), 0, 0);
    const auto before = contents(path);
    SendMessageW(toolbar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(49, 16));
    SendMessageW(toolbar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(80, 0));
    SendMessageW(toolbar, WM_LBUTTONUP, 0, MAKELPARAM(80, 0));
    require(contents(path) == before && GetFocus() == panel, "Drop in slack changed the file or focus");
    SendMessageW(toolbar, WM_SYSKEYDOWN, VK_F4, 0);
    SendMessageW(toolbar, WM_SYSCHAR, L'f', 0);
    SendMessageW(toolbar, WM_KEYDOWN, VK_F10, 0);
    require(systemKeys == 2 && menuKeys == 1, "System or menu keys were swallowed");
    SendMessageW(toolbar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(49, 16));
    SendMessageW(toolbar, WM_KEYDOWN, VK_ESCAPE, 0);
    require(source == -1 && GetFocus() == panel, "Escape did not cancel and restore focus");
    SendMessageW(toolbar, WM_LBUTTONUP, 0, MAKELPARAM(49, 16));
    SendMessageW(toolbar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(16, 16));
    require(!editing, "Mode button did not lock movement");
    const int ups = nativeUps;
    SendMessageW(toolbar, WM_LBUTTONUP, 0, MAKELPARAM(16, 16));
    require(nativeUps == ups, "Lock forwarded an unpaired button release");
    SendMessageW(toolbar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(49, 16));
    SendMessageW(toolbar, WM_LBUTTONUP, 0, MAKELPARAM(49, 16));
    require(nativeUps == ups + 1, "Locked mode swallowed a normal click");
    require(configure(path) == ConfigureResult::Success, "Cannot re-enable movement");
    lockOnFocus = true;
    SetFocus(panel);
    SendMessageW(toolbar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(49, 16));
    lockOnFocus = false;
    require(!editing && source == -1 && !dragBitmap && GetFocus() == panel,
            "Focus reentry continued a cancelled gesture or failed to restore focus");
    require(configure(path) == ConfigureResult::Success, "Cannot enable before bar recreation");
    DestroyWindow(hostBar);
    require(!toolbar && GetPropW(commander, AttachedProperty), "Bar teardown lost the root attachment");
    hostBar = create_bar(path);
    require(configure(path) == ConfigureResult::Success && toolbar == hostBar && editing,
            "Recreated toolbar was not subclassed again");
    SendMessageW(toolbar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(49, 16));
    SetWindowTextW(toolbar, L"another.bar");
    require(!editing && source == -1, "Bar switch left a stale gesture active");
    const int releases = nativeUps;
    SendMessageW(toolbar, WM_LBUTTONUP, 0, MAKELPARAM(49, 16));
    require(nativeUps == releases, "Bar switch forwarded a cancelled gesture's release");
    SetWindowTextW(toolbar, basename(path).c_str());
    require(configure(path) == ConfigureResult::Success, "Cannot enable before DPI change");
    SendMessageW(toolbar, WM_DPICHANGED_AFTERPARENT, 0, 0);
    require(!editing, "DPI change left stale drag geometry active");
    DestroyWindow(commander);
    std::puts("PASS: targets, drag input, focus reentry, keys, lock release and bar recreation");
}
int main() {
    try {
        Fixtures files;
        file_checks(files);
        replacement_checks(files);
        input_checks(files);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}

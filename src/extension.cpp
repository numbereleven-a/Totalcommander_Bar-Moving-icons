#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <memory>
#include <string>
#include <vector>
#include "bar_file.h"
#include "protocol.h"

using namespace barmove;
static HINSTANCE moduleInstance;
static HWND commander, toolbar, previousFocus;
static bool editing, reloading, swallowRelease;
static int source = -1, buttonSize = 32;
struct Target { int index = -1; RECT edge{}; };
static Target insertion;
static POINT startPoint{}, mousePoint{};
static bool dragging;
static HBITMAP dragBitmap;
static COLORREF modeBackground;
static std::unique_ptr<BarFile> bar;
static std::vector<RECT> rectangles;
static LRESULT CALLBACK main_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK bar_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

static bool active_bar_matches(const std::wstring& path) {
    std::wstring text(GetWindowTextLengthW(toolbar) + 1, L'\0');
    const int length = GetWindowTextW(toolbar, text.data(), static_cast<int>(text.size()));
    text.resize(length);
    if (text.find_first_of(L"\\/:") == std::wstring::npos) return lower(text) == lower(basename(path));
    const auto fullPath = [](const std::wstring& value) {
        const DWORD size = GetFullPathNameW(value.c_str(), 0, nullptr, nullptr);
        if (!size) return std::wstring{};
        std::wstring full(size, L'\0');
        const DWORD copied = GetFullPathNameW(value.c_str(), size, full.data(), nullptr);
        if (!copied || copied >= size) return std::wstring{};
        full.resize(copied);
        return lower(std::move(full));
    };
    const auto active = fullPath(text);
    return !active.empty() && active == fullPath(path);
}
static void repaint() { if (toolbar) InvalidateRect(toolbar, nullptr, FALSE); }
static void cancel_drag() {
    const HWND restore = previousFocus;
    previousFocus = nullptr;
    source = -1;
    insertion = {};
    dragging = false;
    if (GetCapture() == toolbar) ReleaseCapture();
    if (dragBitmap) { DeleteObject(dragBitmap); dragBitmap = nullptr; }
    if (restore && GetFocus() == toolbar && IsWindow(restore)) SetFocus(restore);
    repaint();
}
static void lock() {
    editing = false;
    cancel_drag();
    RemovePropW(commander, EditingProperty);
    bar.reset();
    rectangles.clear();
}
static void layout() {
    rectangles.assign(bar->size(), RECT{});
    RECT client{};
    GetClientRect(toolbar, &client);
    int x = 1, y = 1;
    for (int i = 0; i < bar->size(); ++i) {
        if (bar->linebreak(i)) { x = 1; y += buttonSize; continue; }
        const int width = bar->separator(i) ? buttonSize / 3 : buttonSize;
        if (x + width > client.right - 1 && x > 1) { x = 1; y += buttonSize; }
        rectangles[i] = RECT{x, y, x + width, y + buttonSize};
        x += width;
    }
}
static int hit(POINT point) {
    for (int i = 0; i < static_cast<int>(rectangles.size()); ++i)
        if (PtInRect(&rectangles[i], point)) return i;
    return -1;
}
static Target target_at(POINT point) {
    RECT client{};
    GetClientRect(toolbar, &client);
    Target target;
    if (!PtInRect(&client, point)) return target;
    for (int i = 0; i < bar->size(); ++i) {
        auto rectangle = rectangles[i];
        if (IsRectEmpty(&rectangle)) continue;
        if (point.y >= rectangle.top && point.y < rectangle.bottom) {
            if (point.x < (rectangle.left + rectangle.right) / 2) { target = {i, rectangle}; break; }
            rectangle.left = rectangle.right;
            target = {i + 1, rectangle};
        }
    }
    if (target.index < 0) return target;
    // Keep the mode button at its original position.
    const int lockIndex = bar->lock_index();
    if (source > lockIndex && target.index <= lockIndex) {
        RECT edge = rectangles[lockIndex];
        edge.left = edge.right;
        target = {lockIndex + 1, edge};
    }
    if (source < lockIndex && target.index > lockIndex) target = {lockIndex, rectangles[lockIndex]};
    return target;
}
static void draw_insertion_marker(HDC dc, const RECT& edge, UINT dpi) {
    const auto pixels = [dpi](int value) { return std::max(1, MulDiv(value, dpi, 96)); };
    const int x = edge.left;
    const int top = edge.top + pixels(1), bottom = edge.bottom - pixels(1);
    const int halfWidth = pixels(2), haloWidth = pixels(1);
    const int arrowWidth = pixels(6), arrowHeight = pixels(5);
    HBRUSH blue = CreateSolidBrush(RGB(0, 122, 255));
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    const RECT halo{x - halfWidth - haloWidth, top, x + halfWidth + haloWidth + 1, bottom};
    const RECT line{x - halfWidth, top, x + halfWidth + 1, bottom};
    FillRect(dc, &halo, white);
    FillRect(dc, &line, blue);
    HPEN outline = CreatePen(PS_SOLID, pixels(1), RGB(255, 255, 255));
    const auto previousPen = SelectObject(dc, outline);
    const auto previousBrush = SelectObject(dc, blue);
    const POINT upper[]{{x - arrowWidth, top}, {x + arrowWidth, top}, {x, top + arrowHeight}};
    const POINT lower[]{{x - arrowWidth, bottom - 1}, {x + arrowWidth, bottom - 1},
                        {x, bottom - 1 - arrowHeight}};
    Polygon(dc, upper, 3);
    Polygon(dc, lower, 3);
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(outline);
    DeleteObject(white);
    DeleteObject(blue);
}
static RECT mode_icon_rectangle(const RECT& button) {
    const int size = MulDiv(16, GetDpiForWindow(toolbar), 96);
    const int x = (button.left + button.right - size) / 2;
    const int y = (button.top + button.bottom - size) / 2;
    return RECT{x, y, x + size, y + size};
}
static void draw_feedback(HDC dc) {
    if (!editing || !bar) return;
    const int lockIndex = bar->lock_index();
    if (lockIndex >= 0) {
        RECT rectangle = rectangles[lockIndex];
        const auto brush = CreateSolidBrush(modeBackground);
        const RECT iconRectangle = mode_icon_rectangle(rectangle);
        const int size = iconRectangle.right - iconRectangle.left;
        FillRect(dc, &rectangle, brush);
        DeleteObject(brush);
        HICON icon = static_cast<HICON>(LoadImageW(moduleInstance, MAKEINTRESOURCEW(2), IMAGE_ICON,
            size, size, LR_DEFAULTCOLOR));
        if (icon) {
            DrawIconEx(dc, iconRectangle.left, iconRectangle.top, icon, size, size, 0, nullptr, DI_NORMAL);
            DestroyIcon(icon);
        }
        HBRUSH frame = CreateSolidBrush(RGB(212, 141, 0));
        FrameRect(dc, &rectangle, frame);
        DeleteObject(frame);
    }
    if (dragging && source >= 0 && insertion.index >= 0) {
        if (dragBitmap) {
            HDC imageDc = CreateCompatibleDC(dc);
            auto previous = SelectObject(imageDc, dragBitmap);
            const auto& original = rectangles[source];
            const int width = original.right - original.left, height = original.bottom - original.top;
            const int saved = SaveDC(dc);
            if (saved) {
                // Keep the fixed mode button visible underneath the drag preview.
                if (lockIndex >= 0) {
                    const auto& modeButton = rectangles[lockIndex];
                    ExcludeClipRect(dc, modeButton.left, modeButton.top, modeButton.right, modeButton.bottom);
                }
                BitBlt(dc, mousePoint.x - width / 2, mousePoint.y - height / 2, width, height,
                    imageDc, 0, 0, SRCCOPY);
                RestoreDC(dc, saved);
            }
            SelectObject(imageDc, previous);
            DeleteDC(imageDc);
        }
        // Paint the insertion marker last so the dragged image cannot obscure it.
        draw_insertion_marker(dc, insertion.edge, GetDpiForWindow(toolbar));
    }
}
static void fail(const std::exception& error) {
    lock();
    SetPropW(commander, ErrorProperty, reinterpret_cast<HANDLE>(1));
    const std::string message = error.what();
    std::wstring wide(message.begin(), message.end());
    MessageBoxW(commander, wide.c_str(), L"Button movement", MB_OK | MB_ICONERROR);
}
static ConfigureResult enable(const std::wstring& path) {
    if (editing) { lock(); return ConfigureResult::Success; }
    try {
        const auto current = reinterpret_cast<HWND>(SendMessageW(commander, WM_USER + 50, 28, 0));
        if (!current || !IsWindowVisible(current)) return ConfigureResult::BarUnavailable;
        if (current != toolbar) {
            if (!SetWindowSubclass(current, bar_proc, SubclassId, 0)) return ConfigureResult::BarUnavailable;
            if (toolbar) RemoveWindowSubclass(toolbar, bar_proc, SubclassId);
            toolbar = current;
        }
        if (!active_bar_matches(path)) return ConfigureResult::WrongBar;
        auto file = std::make_unique<BarFile>(path);
        if (file->lock_index() < 0) return ConfigureResult::ModeButtonMissing;
        const DWORD length = GetEnvironmentVariableW(L"COMMANDER_INI", nullptr, 0);
        if (!length) return ConfigureResult::SettingsMissing;
        std::wstring ini(length, L'\0');
        const DWORD copied = GetEnvironmentVariableW(L"COMMANDER_INI", ini.data(), length);
        if (!copied || copied >= length) return ConfigureResult::SettingsMissing;
        ini.resize(copied);
        HANDLE settings = CreateFileW(ini.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
            FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (settings == INVALID_HANDLE_VALUE) return ConfigureResult::SettingsMissing;
        CloseHandle(settings);
        const UINT dpi = GetDpiForWindow(toolbar);
        const auto dpiKey = L"Buttonheight" + std::to_wstring(dpi);
        buttonSize = GetPrivateProfileIntW(L"Buttonbar", dpiKey.c_str(), 0, ini.c_str());
        if (!buttonSize) {
            const int size = GetPrivateProfileIntW(L"Buttonbar", L"Buttonheight", 29, ini.c_str());
            const int savedDpi = GetPrivateProfileIntW(L"Buttonbar", L"DefaultDpi", 96, ini.c_str());
            buttonSize = MulDiv(size, dpi, savedDpi ? savedDpi : 96);
        }
        if (buttonSize < 8) return ConfigureResult::InvalidSize;
        bar = std::move(file);
        layout();
        const auto& modeButton = rectangles[bar->lock_index()];
        HDC dc = GetDC(toolbar);
        modeBackground = GetPixel(dc, modeButton.left + 1, modeButton.top + 1);
        ReleaseDC(toolbar, dc);
        if (modeBackground == CLR_INVALID) modeBackground = GetSysColor(COLOR_BTNFACE);
        editing = true;
        reloading = false;
        RemovePropW(commander, ErrorProperty);
        SetPropW(commander, EditingProperty, reinterpret_cast<HANDLE>(1));
        repaint();
        return ConfigureResult::Success;
    } catch (const std::exception&) { return ConfigureResult::FileError; }
}
static LRESULT CALLBACK main_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (message == WM_COPYDATA) {
        auto data = reinterpret_cast<COPYDATASTRUCT*>(lparam);
        if (data && data->dwData == ConfigureMessage) {
            if (!data->lpData || data->cbData < sizeof(wchar_t) || data->cbData > 65536 ||
                data->cbData % sizeof(wchar_t)) return static_cast<LRESULT>(ConfigureResult::InvalidRequest);
            const auto text = static_cast<const wchar_t*>(data->lpData);
            const size_t count = data->cbData / sizeof(wchar_t);
            if (text[count - 1] != 0) return static_cast<LRESULT>(ConfigureResult::InvalidRequest);
            return static_cast<LRESULT>(enable(std::wstring(text, count - 1)));
        }
    }
    if (message == finish_reload_message()) {
        reloading = false;
        if (editing) layout();
        repaint();
        return 0;
    }
    if (message == WM_ACTIVATEAPP && !wparam) cancel_drag();
    if (message == WM_DPICHANGED) lock();
    if (message == WM_NCDESTROY) {
        lock();
        RemovePropW(window, AttachedProperty);
        RemovePropW(window, ErrorProperty);
        RemoveWindowSubclass(window, main_proc, SubclassId);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
static LRESULT CALLBACK bar_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        lock();
        RemoveWindowSubclass(window, bar_proc, SubclassId);
        toolbar = nullptr;
        swallowRelease = false;
        return DefSubclassProc(window, message, wparam, lparam);
    }
    const bool ownedRelease = message == WM_LBUTTONUP && swallowRelease;
    if (message == WM_LBUTTONUP) swallowRelease = false;
    if (!editing) {
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) swallowRelease = false;
        return ownedRelease ? 0 : DefSubclassProc(window, message, wparam, lparam);
    }
    if (message == WM_SETTEXT) {
        const auto result = DefSubclassProc(window, message, wparam, lparam);
        if (editing && !active_bar_matches(bar->path())) lock();
        return result;
    }
    if ((message == WM_PAINT || message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK ||
         message == WM_LBUTTONUP) &&
        !active_bar_matches(bar->path())) {
        lock();
        return ownedRelease ? 0 : DefSubclassProc(window, message, wparam, lparam);
    }
    if (message == WM_ERASEBKGND && wparam) {
        HDC dc = reinterpret_cast<HDC>(wparam);
        const int saved = SaveDC(dc);
        if (saved) {
            const auto& modeButton = rectangles[bar->lock_index()];
            ExcludeClipRect(dc, modeButton.left, modeButton.top, modeButton.right, modeButton.bottom);
        }
        const auto result = DefSubclassProc(window, message, wparam, lparam);
        if (saved) RestoreDC(dc, saved);
        return result;
    }
    if (message == WM_PAINT) {
        // Keep the entire mode button, including its frame, out of native
        // painting. Present the replacement and drag feedback as one image.
        const RECT modeButton = rectangles[bar->lock_index()];
        ValidateRect(window, &modeButton);
        const auto result = DefSubclassProc(window, message, wparam, lparam);
        HDC dc = GetDC(window);
        RECT client{};
        GetClientRect(window, &client);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        if (buffer && bitmap) {
            const auto previous = SelectObject(buffer, bitmap);
            if (BitBlt(buffer, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY)) {
                draw_feedback(buffer);
                BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
            }
            SelectObject(buffer, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        ReleaseDC(window, dc);
        return result;
    }
    if (message == WM_SIZE) { cancel_drag(); if (editing) layout(); }
    if (message == WM_DPICHANGED || message == WM_DPICHANGED_AFTERPARENT) {
        lock();
        return DefSubclassProc(window, message, wparam, lparam);
    }
    if (message == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return TRUE; }
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS) cancel_drag();
    if (message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
        message == WM_CHAR) return 0;
    if (message == WM_KEYDOWN) {
        if (wparam == VK_ESCAPE) { cancel_drag(); return 0; }
        if (wparam == VK_RETURN || wparam == VK_SPACE || wparam == VK_LEFT || wparam == VK_RIGHT ||
            wparam == VK_UP || wparam == VK_DOWN || wparam == VK_HOME || wparam == VK_END ||
            wparam == VK_PRIOR || wparam == VK_NEXT) return 0;
    }
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
        cancel_drag();
        if (!editing) return 0;
        swallowRelease = true;
        if (reloading) return 0;
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int position = hit(point);
        if (position == bar->lock_index()) { lock(); return 0; }
        if (position >= 0) {
            const HWND focus = GetFocus();
            previousFocus = focus != window ? focus : nullptr;
            SetFocus(window);
            if (!editing || toolbar != window || GetFocus() != window) { cancel_drag(); return 0; }
            startPoint = mousePoint = point;
            source = position;
            SetCapture(window);
            if (!editing || source < 0 || toolbar != window) return 0;
            const RECT rectangle = rectangles[source];
            HDC dc = GetDC(window), imageDc = CreateCompatibleDC(dc);
            dragBitmap = CreateCompatibleBitmap(dc, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top);
            const auto previous = SelectObject(imageDc, dragBitmap);
            BitBlt(imageDc, 0, 0, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
                dc, rectangle.left, rectangle.top, SRCCOPY);
            SelectObject(imageDc, previous);
            DeleteDC(imageDc);
            ReleaseDC(window, dc);
        }
        return 0;
    }
    if (message == WM_MOUSEMOVE) {
        mousePoint = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (source >= 0 && (abs(mousePoint.x - startPoint.x) >= GetSystemMetrics(SM_CXDRAG) ||
            abs(mousePoint.y - startPoint.y) >= GetSystemMetrics(SM_CYDRAG))) dragging = true;
        if (dragging) { insertion = target_at(mousePoint); repaint(); }
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        const int from = source;
        const Target target = target_at(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        const bool perform = dragging && from >= 0 && target.index >= 0;
        const auto activeFile = bar.get();
        cancel_drag();
        if (perform && editing && bar.get() == activeFile) try {
            bar->move(from, target.index);
            layout();
            reloading = true;
            PostMessageW(commander, WM_USER + 51, 2945, 0);
            PostMessageW(commander, finish_reload_message(), 0, 0);
        } catch (const std::exception& error) { fail(error); }
        return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
extern "C" __declspec(dllexport) LRESULT CALLBACK BarMoveHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0) {
        const auto message = reinterpret_cast<CWPSTRUCT*>(lparam);
        if (message->message == attach_message() && !GetPropW(message->hwnd, AttachedProperty)) {
            wchar_t name[64]{};
            GetClassNameW(message->hwnd, name, 64);
            if (!wcscmp(name, L"TTOTAL_CMD")) {
                const auto handle = reinterpret_cast<HWND>(SendMessageW(message->hwnd, WM_USER + 50, 28, 0));
                HMODULE pinned{};
                if (handle && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&BarMoveHook), &pinned)) {
                    commander = message->hwnd;
                    toolbar = handle;
                    if (SetWindowSubclass(commander, main_proc, SubclassId, 0) &&
                        SetWindowSubclass(toolbar, bar_proc, SubclassId, 0))
                        SetPropW(commander, AttachedProperty, reinterpret_cast<HANDLE>(1));
                    else {
                        RemoveWindowSubclass(commander, main_proc, SubclassId);
                        RemoveWindowSubclass(toolbar, bar_proc, SubclassId);
                    }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { moduleInstance = instance; DisableThreadLibraryCalls(instance); }
    return TRUE;
}

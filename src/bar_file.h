#pragma once
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace barmove {
inline std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}
inline std::wstring trim(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    return value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
}
inline std::wstring basename(const std::wstring& path) {
    return path.substr(path.find_last_of(L"\\/") + 1);
}
inline std::vector<char> read_bytes(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot read button bar");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > MAXDWORD || size.QuadPart < 0) {
        CloseHandle(file);
        throw std::runtime_error("Invalid button bar size");
    }
    std::vector<char> data(static_cast<size_t>(size.QuadPart));
    DWORD read{};
    bool success = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);
    if (!success || read != data.size()) throw std::runtime_error("Cannot read button bar");
    return data;
}

// Keep each line, unknown fields, comments and the original text encoding. Moving
// a button changes only the numeric suffixes of that button's INI keys.
class BarFile {
    struct Line {
        std::wstring text;
        int index = -1;
        size_t numberStart = 0, numberEnd = 0;
    };
    enum class Encoding { Ansi, Utf8, Utf16 };
    Encoding encoding = Encoding::Ansi;
    std::vector<Line> lines;
    std::vector<int> order;
    std::vector<char> expected;
    std::wstring filename;
    bool backedUp = false;

    std::wstring decode(const std::vector<char>& bytes) {
        size_t skip = 0;
        UINT codepage = CP_ACP;
        if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xff &&
            (unsigned char)bytes[1] == 0xfe) {
            encoding = Encoding::Utf16;
            if (bytes.size() % 2) throw std::runtime_error("Invalid UTF-16 button bar");
            return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2),
                                (bytes.size() - 2) / sizeof(wchar_t));
        }
        if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xef &&
            (unsigned char)bytes[1] == 0xbb && (unsigned char)bytes[2] == 0xbf) {
            encoding = Encoding::Utf8;
            skip = 3;
            codepage = CP_UTF8;
        }
        const int size = static_cast<int>(bytes.size() - skip);
        const int count = MultiByteToWideChar(codepage, 0, bytes.data() + skip, size, nullptr, 0);
        std::wstring text(count, L'\0');
        if (size && !MultiByteToWideChar(codepage, 0, bytes.data() + skip, size, text.data(), count))
            throw std::runtime_error("Cannot decode button bar");
        return text;
    }
    std::vector<char> encode(const std::wstring& text) const {
        if (encoding == Encoding::Utf16) {
            std::vector<char> result{char(0xff), char(0xfe)};
            const char* data = reinterpret_cast<const char*>(text.data());
            result.insert(result.end(), data, data + text.size() * sizeof(wchar_t));
            return result;
        }
        const UINT codepage = encoding == Encoding::Utf8 ? CP_UTF8 : CP_ACP;
        const int count = WideCharToMultiByte(codepage, 0, text.data(), static_cast<int>(text.size()),
            nullptr, 0, nullptr, nullptr);
        std::vector<char> result(encoding == Encoding::Utf8 ? 3 : 0);
        if (!result.empty()) result = {char(0xef), char(0xbb), char(0xbf)};
        const size_t start = result.size();
        result.resize(start + count);
        if (!WideCharToMultiByte(codepage, 0, text.data(), static_cast<int>(text.size()),
            result.data() + start, count, nullptr, nullptr))
            throw std::runtime_error("Cannot encode button bar");
        return result;
    }
public:
    explicit BarFile(std::wstring path) : filename(std::move(path)) {
        expected = read_bytes(filename);
        const auto text = decode(expected);
        if (encode(text) != expected)
            throw std::runtime_error("Button bar cannot be rewritten without altering other text");
        bool buttonSection = false;
        int count = -1;
        for (size_t position = 0; position < text.size();) {
            auto end = text.find(L'\n', position);
            end = end == std::wstring::npos ? text.size() : end + 1;
            Line line{text.substr(position, end - position)};
            position = end;
            const auto clean = trim(line.text);
            if (!clean.empty() && clean.front() == L'[') buttonSection = lower(clean) == L"[buttonbar]";
            const auto equals = line.text.find(L'=');
            if (buttonSection && equals != std::wstring::npos && !clean.empty() && clean.front() != L';') {
                const auto key = lower(trim(line.text.substr(0, equals)));
                if (key == L"buttoncount") count = std::stoi(trim(line.text.substr(equals + 1)));
            }
            lines.push_back(std::move(line));
        }
        if (count <= 0) throw std::runtime_error("Button bar has no buttons");
        order.resize(count);
        std::iota(order.begin(), order.end(), 0);
        buttonSection = false;
        for (auto& line : lines) {
            const auto clean = trim(line.text);
            if (!clean.empty() && clean.front() == L'[') buttonSection = lower(clean) == L"[buttonbar]";
            if (!buttonSection || clean.empty() || clean.front() == L';') continue;
            const auto equals = line.text.find(L'=');
            if (equals == std::wstring::npos || equals == 0) continue;
            auto end = line.text.find_last_not_of(L" \t", equals - 1);
            if (end == std::wstring::npos) continue;
            ++end;
            size_t start = end;
            while (start && line.text[start - 1] >= L'0' && line.text[start - 1] <= L'9') --start;
            if (start == end || start == 0) continue;
            int index = 0;
            for (size_t position = start; position < end; ++position) {
                const int digit = line.text[position] - L'0';
                if (index > count / 10 || (index == count / 10 && digit > count % 10)) {
                    index = 0;
                    break;
                }
                index = index * 10 + digit;
            }
            if (index < 1 || index > count) continue;
            line.index = index - 1;
            line.numberStart = start;
            line.numberEnd = end;
        }
    }
    int size() const { return static_cast<int>(order.size()); }
    const std::wstring& path() const { return filename; }
    std::wstring field(int position, const wchar_t* name) const {
        const int original = order.at(position);
        for (const auto& line : lines) {
            if (line.index != original) continue;
            if (lower(trim(line.text.substr(0, line.numberStart))) == name)
                return trim(line.text.substr(line.text.find(L'=') + 1));
        }
        return {};
    }
    bool separator(int position) const { return field(position, L"button").empty(); }
    bool linebreak(int position) const { return separator(position) && field(position, L"cmd") == L"-2"; }
    int lock_index() const {
        for (int i = 0; i < size(); ++i) {
            auto command = trim(field(i, L"cmd"));
            if (command.size() > 1 && command.front() == L'"' && command.back() == L'"')
                command = command.substr(1, command.size() - 2);
            if (lower(basename(command)) == L"tcbarmove.exe") return i;
        }
        return -1;
    }
    void backup() const {
        SYSTEMTIME time{};
        GetSystemTime(&time);
        wchar_t suffix[96]{};
        swprintf(suffix, 96, L".barmove-%04u%02u%02u-%02u%02u%02u-%03u.bak",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
        if (!CopyFileW(filename.c_str(), (filename + suffix).c_str(), TRUE))
            throw std::runtime_error("Cannot back up button bar");
    }
    void prune_backups() const {
        const auto prefix = basename(filename) + L".barmove-";
        const auto directory = filename.substr(0, filename.size() - basename(filename).size());
        WIN32_FIND_DATAW entry{};
        HANDLE search = FindFirstFileW((filename + L".barmove-*.bak").c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) return;
            throw std::runtime_error("Button bar saved, but cannot list old backups");
        }
        std::vector<std::wstring> copies;
        do {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring name = entry.cFileName;
            if (name.compare(0, prefix.size(), prefix) != 0) continue;
            const auto suffix = name.substr(prefix.size());
            if (suffix.size() != 23 || suffix[8] != L'-' || suffix[15] != L'-' ||
                suffix.substr(19) != L".bak") continue;
            bool valid = true;
            for (size_t i = 0; i < 19; ++i)
                if (i != 8 && i != 15 && (suffix[i] < L'0' || suffix[i] > L'9')) valid = false;
            if (valid) copies.push_back(name);
        } while (FindNextFileW(search, &entry));
        const DWORD error = GetLastError();
        FindClose(search);
        if (error != ERROR_NO_MORE_FILES)
            throw std::runtime_error("Button bar saved, but cannot list old backups");
        std::sort(copies.begin(), copies.end());
        for (size_t i = 0; i + 2 < copies.size(); ++i)
            if (!DeleteFileW((directory + copies[i]).c_str()))
                throw std::runtime_error("Button bar saved, but cannot remove an old backup");
    }
    void move(int source, int insertion) {
        if (source < 0 || source >= size() || insertion < 0 || insertion > size())
            throw std::runtime_error("Invalid button position");
        if (insertion > source) --insertion;
        if (insertion == source) return;
        if (read_bytes(filename) != expected)
            throw std::runtime_error("Button bar changed outside the editor; unlock it again");
        if (!backedUp) { backup(); backedUp = true; }
        auto moved = order;
        const int button = moved.at(source);
        moved.erase(moved.begin() + source);
        moved.insert(moved.begin() + insertion, button);
        std::vector<int> positions(size());
        for (int i = 0; i < size(); ++i) positions[moved[i]] = i;
        std::wstring text;
        for (const auto& line : lines) {
            if (line.index < 0) text += line.text;
            else text += line.text.substr(0, line.numberStart) + std::to_wstring(positions[line.index] + 1) +
                line.text.substr(line.numberEnd);
        }
        const auto data = encode(text);
        const auto temporary = filename + L".barmove-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create button bar update");
        DWORD written{};
        const bool success = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
            && written == data.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (!success) {
            DeleteFileW(temporary.c_str());
            throw std::runtime_error("Cannot save button bar");
        }
        if (!ReplaceFileW(filename.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr)) {
            const DWORD error = GetLastError();
            // These failures can leave the original missing or renamed. Keep
            // the complete replacement, as well as the session backup.
            if (error == ERROR_UNABLE_TO_MOVE_REPLACEMENT || error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2)
                throw std::runtime_error("Cannot finish replacing button bar; recover it from the .barmove .bak or .tmp file");
            DeleteFileW(temporary.c_str());
            throw std::runtime_error("Cannot save button bar (Windows error " + std::to_string(error) + ")");
        }
        expected = data;
        order = std::move(moved);
        prune_backups();
    }
};
}

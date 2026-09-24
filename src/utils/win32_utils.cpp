#include "utils/win32_utils.h"

#include <cstdio>

namespace woke::util {
namespace {

// Appends a trailing separator when missing, so callers can concatenate leaf names
// without repeating the check everywhere.
void ensure_trailing_separator(std::wstring& path) {
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
}

std::wstring current_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = ::GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
        if (buffer.size() > 32768) {
            return {};
        }
    }
}

bool file_exists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

std::wstring executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        // A full buffer means the path was truncated; grow and retry.
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        buffer.resize(buffer.size() * 2);
        if (buffer.size() > 32768) {
            return {};
        }
    }

    const std::size_t separator = buffer.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return {};
    }
    buffer.resize(separator + 1);
    return buffer;
}

std::wstring resolve_runtime_path(const wchar_t* leaf) {
    if (leaf == nullptr || *leaf == L'\0') {
        return {};
    }

    std::wstring primary = executable_directory();
    ensure_trailing_separator(primary);
    primary += leaf;
    if (file_exists(primary)) {
        return primary;
    }

    std::wstring fallback = current_directory();
    ensure_trailing_separator(fallback);
    fallback += leaf;
    if (file_exists(fallback)) {
        return fallback;
    }

    // Neither exists yet (first run): the executable directory is the canonical home,
    // and the caller reports the concrete failure when it cannot create/open the path.
    return primary;
}

bool ensure_directory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    if (::CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
        return true;
    }
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool attach_console() {
    if (::GetConsoleWindow() == nullptr) {
        if (::AttachConsole(ATTACH_PARENT_PROCESS) == FALSE && ::AllocConsole() == FALSE) {
            return false;
        }
    }

    FILE* stream = nullptr;
    (void)::freopen_s(&stream, "CONOUT$", "w", stdout);
    (void)::freopen_s(&stream, "CONOUT$", "w", stderr);
    (void)::freopen_s(&stream, "CONIN$", "r", stdin);
    (void)::SetConsoleOutputCP(CP_UTF8);
    return true;
}

void enable_virtual_terminal() {
    const auto enable_for = [](DWORD handle_id) {
        const HANDLE handle = ::GetStdHandle(handle_id);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
            return;
        }
        DWORD mode = 0;
        if (::GetConsoleMode(handle, &mode) == FALSE) {
            return;
        }
        (void)::SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    };

    enable_for(STD_OUTPUT_HANDLE);
    enable_for(STD_ERROR_HANDLE);
}

void write_console(const char* text, std::size_t length) {
    if (text == nullptr || length == 0) {
        return;
    }
    const HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }

    std::size_t offset = 0;
    while (offset < length) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(length - offset);
        if (::WriteFile(handle, text + offset, chunk, &written, nullptr) == FALSE || written == 0) {
            return;
        }
        offset += written;
    }
}

void write_debugger(const char* text, std::size_t length) {
    if (text == nullptr || length == 0) {
        return;
    }
    // OutputDebugStringA requires a NUL-terminated string; truncate into stack storage.
    char scratch[512];
    const std::size_t take = length < (sizeof(scratch) - 1) ? length : (sizeof(scratch) - 1);
    for (std::size_t index = 0; index < take; ++index) {
        scratch[index] = text[index];
    }
    scratch[take] = '\0';
    ::OutputDebugStringA(scratch);
}

} // namespace woke::util

#include "core/logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/version.h"
#include "utils/string_buffer.h"
#include "utils/time_format.h"
#include "utils/win32_utils.h"

namespace woke::logger {
namespace {

constexpr std::size_t kQueueSlots = 256;       // pending file lines
constexpr std::size_t kLineCapacity = 512;     // one fully formatted log line
constexpr std::size_t kMessageCapacity = 384;  // the caller's formatted message
constexpr std::size_t kMaxFlushPerCall = 64;   // bounded work per flush tick
constexpr std::size_t kSessionsToKeep = 20;    // log files retained on disk

using Line = util::FixedString<kLineCapacity>;

struct Slot {
    Line text;
    Level level = Level::Info;
};

std::array<Slot, kQueueSlots> g_queue{};
std::atomic<std::size_t> g_head{0};  // next slot to fill (producers)
std::atomic<std::size_t> g_tail{0};  // next slot to drain (worker)
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
std::atomic<std::size_t> g_dropped{0};

std::FILE* g_session = nullptr;
std::FILE* g_latest = nullptr;
std::wstring g_session_path{};
bool g_initialized = false;
bool g_console_ready = false;

// ── Helpers ──────────────────────────────────────────────────────────────────────

// Very short critical section (one memcpy) guarded by one flag: this keeps multi-producer
// logging correct without a kernel lock, and without ever blocking a frame for long.
class SpinGuard {
public:
    SpinGuard() noexcept {
        while (g_lock.test_and_set(std::memory_order_acquire)) {
            // spin: the guarded section is a single bounded copy
        }
    }
    ~SpinGuard() noexcept { g_lock.clear(std::memory_order_release); }

    SpinGuard(const SpinGuard&) = delete;
    SpinGuard& operator=(const SpinGuard&) = delete;
};

const char* level_name(Level level) noexcept {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "INFO";
}

// ANSI colors: DEBUG gray, INFO cyan, WARN yellow, ERROR red (spec-mandated mapping).
const char* level_color(Level level) noexcept {
    switch (level) {
        case Level::Debug: return "\x1b[90m";
        case Level::Info:  return "\x1b[36m";
        case Level::Warn:  return "\x1b[33m";
        case Level::Error: return "\x1b[31m";
    }
    return "\x1b[0m";
}

// __FILE__ expands to a full path; only the leaf name belongs in a log line.
const char* file_basename(const char* path) noexcept {
    if (path == nullptr) {
        return "?";
    }
    const char* base = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            base = cursor + 1;
        }
    }
    return base;
}

void ensure_trailing_separator(std::wstring& path) {
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
}

std::wstring widen_ascii(const char* text) {
    std::wstring result;
    if (text == nullptr) {
        return result;
    }
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*cursor)));
    }
    return result;
}

bool enqueue(const Line& line, Level level) noexcept {
    SpinGuard guard;
    const std::size_t head = g_head.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) % kQueueSlots;
    if (next == g_tail.load(std::memory_order_acquire)) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_queue[head].text.assign(line.view());
    g_queue[head].level = level;
    g_head.store(next, std::memory_order_release);
    return true;
}

// Session names are timestamp-shaped, so lexicographic order is chronological order.
void prune_old_sessions(const std::wstring& logs_directory, std::size_t keep) {
    if (logs_directory.empty()) {
        return;
    }

    WIN32_FIND_DATAW entry{};
    const std::wstring pattern = logs_directory + L"*.log";
    const HANDLE find = ::FindFirstFileW(pattern.c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }

    std::vector<std::wstring> names;
    names.reserve(32);
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::wstring name = entry.cFileName;
        if (name == L"latest.log") {
            continue;  // the mirror is overwritten, never pruned by age
        }
        names.push_back(name);
    } while (::FindNextFileW(find, &entry) != FALSE);
    (void)::FindClose(find);

    if (names.size() < keep) {
        return;
    }

    std::sort(names.begin(), names.end());
    // Keep room for the session file this boot is about to create.
    const std::size_t remove_count = names.size() - keep + 1;
    for (std::size_t index = 0; index < remove_count; ++index) {
        (void)::DeleteFileW((logs_directory + names[index]).c_str());
    }
}

void write_v(Level level, const char* file, int line_number, const char* format,
    va_list args) noexcept {
    util::FixedString<kMessageCapacity> message;
    message.format_v(format, args);

    util::LocalTime now = util::now_local();
    util::FixedString<40> timestamp;
    util::format_timestamp(now, timestamp);

    Line body;  // "[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] message (file:line)"
    body.format("%s [%-5s] %s (%s:%d)", timestamp.c_str(), level_name(level), message.c_str(),
        file_basename(file), line_number);

    Line console_line;
    console_line.format("%s%s\x1b[0m\r\n", level_color(level), body.c_str());
    if (g_console_ready) {
        util::write_console(console_line.c_str(), console_line.size());
    } else {
        util::write_debugger(console_line.c_str(), console_line.size());
    }

    if (!g_initialized) {
        return;
    }

    Line file_line;  // text-mode streams translate '\n' into the platform newline
    file_line.format("%s\n", body.c_str());
    (void)enqueue(file_line, level);
}

} // namespace

const wchar_t* session_log_path() noexcept {
    return g_session_path.c_str();
}

bool init() noexcept {
    if (g_initialized) {
        return g_session != nullptr || g_latest != nullptr;
    }

    g_console_ready = util::attach_console();
    if (g_console_ready) {
        util::enable_virtual_terminal();
    }

    std::wstring logs_directory = util::resolve_runtime_path(L"logs");
    ensure_trailing_separator(logs_directory);
    if (!util::ensure_directory(logs_directory)) {
        g_initialized = true;
        WOKE_LOG_ERROR("logger: could not create '%ls'", logs_directory.c_str());
        return false;
    }

    prune_old_sessions(logs_directory, kSessionsToKeep);

    util::LocalTime now = util::now_local();
    util::FixedString<32> session_name;
    util::format_session_name(now, session_name);

    std::wstring session_path = logs_directory;
    session_path += widen_ascii(session_name.c_str());
    session_path += L".log";

    g_session = ::_wfopen(session_path.c_str(), L"w");
    if (g_session != nullptr) {
        g_session_path = session_path;
    }

    const std::wstring latest_path = logs_directory + L"latest.log";
    g_latest = ::_wfopen(latest_path.c_str(), L"w");

    g_initialized = true;

    if (g_session == nullptr) {
        WOKE_LOG_ERROR("logger: session file '%ls' could not be opened", session_path.c_str());
    }
    if (g_latest == nullptr) {
        WOKE_LOG_WARN("logger: latest.log mirror could not be opened in '%ls'",
            logs_directory.c_str());
    }
    return g_session != nullptr || g_latest != nullptr;
}

std::size_t flush() noexcept {
    if (!g_initialized) {
        return 0;
    }

    std::size_t drained = 0;
    while (drained < kMaxFlushPerCall) {
        const std::size_t tail = g_tail.load(std::memory_order_relaxed);
        if (tail == g_head.load(std::memory_order_acquire)) {
            break;
        }

        Slot& slot = g_queue[tail];
        const char* text = slot.text.c_str();
        const std::size_t length = slot.text.size();
        if (g_session != nullptr) {
            (void)std::fwrite(text, 1, length, g_session);
        }
        if (g_latest != nullptr) {
            (void)std::fwrite(text, 1, length, g_latest);
        }
        slot.text.clear();
        g_tail.store((tail + 1) % kQueueSlots, std::memory_order_release);
        ++drained;
    }

    if (drained > 0) {
        if (g_session != nullptr) {
            (void)std::fflush(g_session);
        }
        if (g_latest != nullptr) {
            (void)std::fflush(g_latest);
        }
    }
    return drained;
}

void shutdown() noexcept {
    if (!g_initialized) {
        return;
    }

    // Drain whatever is still queued; the worker owns the consumer side, so no
    // concurrent producer can be running at this point in the lifecycle.
    for (int pass = 0; pass < 8; ++pass) {
        if (flush() == 0 && g_tail.load(std::memory_order_acquire) == g_head.load(std::memory_order_acquire)) {
            break;
        }
    }

    if (g_session != nullptr) {
        std::fclose(g_session);
        g_session = nullptr;
    }
    if (g_latest != nullptr) {
        std::fclose(g_latest);
        g_latest = nullptr;
    }

    g_console_ready = false;
    g_initialized = false;
}

void write(Level level, const char* file, int line, const char* format, ...) noexcept {
    va_list args;
    va_start(args, format);
    write_v(level, file, line, format, args);
    va_end(args);
}

std::size_t dropped_line_count() noexcept {
    return g_dropped.load(std::memory_order_relaxed);
}

} // namespace woke::logger

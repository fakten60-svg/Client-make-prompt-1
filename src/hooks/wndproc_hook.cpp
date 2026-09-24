#include "hooks/wndproc_hook.h"

#include <windows.h>

#include <atomic>

#include "core/event_bus.h"
#include "core/events.h"
#include "core/logger.h"
#include "hooks/hook_manager.h"
#include "ui/gui.h"

namespace woke::hooks {
namespace {

// LWJGL's GLFW binding. It exports the raw GLFW entry points alongside the JNI wrappers,
// which lets the client ask GLFW for its own window without reading a single game field.
constexpr const wchar_t* kGlfwModuleName = L"lwjgl_glfw.dll";

// GLFW's window class is "GLFW30" across the 3.x line; matching the prefix survives a bump.
constexpr const wchar_t* kGlfwClassPrefix = L"GLFW";
constexpr int kClassNameCapacity = 64;

WNDPROC g_previous_proc = nullptr;
HWND g_window = nullptr;

// Written by the message stream on the game thread only.
bool g_num_lock = false;
bool g_scroll_lock = false;
bool g_caps_lock = false;

// Atomic so the worker thread can report it during shutdown without a data race.
std::atomic<unsigned long long> g_routed_messages{0};

bool class_name_is_glfw(HWND window) {
    wchar_t name[kClassNameCapacity] = {};
    const int length = ::GetClassNameW(window, name, kClassNameCapacity);
    if (length < 4) {
        return false;
    }
    return ::CompareStringOrdinal(name, 4, kGlfwClassPrefix, 4, TRUE) == CSTR_EQUAL;
}

// The fallback path: the process' own visible top-level window whose class starts with GLFW.
BOOL CALLBACK find_game_window(HWND window, LPARAM parameter) {
    DWORD process_id = 0;
    (void)::GetWindowThreadProcessId(window, &process_id);
    if (process_id != ::GetCurrentProcessId()) {
        return TRUE;
    }
    if (::IsWindowVisible(window) == FALSE) {
        return TRUE;
    }

    RECT client{};
    if (::GetClientRect(window, &client) == FALSE) {
        return TRUE;
    }
    if ((client.right - client.left) <= 0 || (client.bottom - client.top) <= 0) {
        return TRUE;
    }
    if (!class_name_is_glfw(window)) {
        return TRUE;
    }

    *reinterpret_cast<HWND*>(parameter) = window;
    return FALSE; // stop enumerating
}

HWND resolve_game_window() {
    const HMODULE glfw = ::GetModuleHandleW(kGlfwModuleName);
    if (glfw != nullptr) {
#pragma warning(push)
#pragma warning(disable : 4191) // GetProcAddress hands back an address, not a typed pointer
        // void* stands in for GLFWwindow*: this file never dereferences it, it only hands it
        // straight back to GLFW.
        using GetCurrentContextFn = void* (*)();
        using GetWin32WindowFn = HWND (*)(void*);
        const auto get_current_context =
            reinterpret_cast<GetCurrentContextFn>(::GetProcAddress(glfw, "glfwGetCurrentContext"));
        const auto get_win32_window =
            reinterpret_cast<GetWin32WindowFn>(::GetProcAddress(glfw, "glfwGetWin32Window"));
#pragma warning(pop)

        if (get_current_context != nullptr && get_win32_window != nullptr) {
            void* context = get_current_context();
            if (context != nullptr) {
                const HWND window = get_win32_window(context);
                if (window != nullptr) {
                    WOKE_LOG_DEBUG("wndproc-hook: window resolved through GLFW");
                    return window;
                }
            }
        }
    }

    HWND found = nullptr;
    (void)::EnumWindows(&find_game_window, reinterpret_cast<LPARAM>(&found));
    if (found != nullptr) {
        WOKE_LOG_DEBUG("wndproc-hook: window resolved by class name scan");
    }
    return found;
}

void capture_lock_state() noexcept {
    g_num_lock = (::GetKeyState(VK_NUMLOCK) & 1) != 0;
    g_scroll_lock = (::GetKeyState(VK_SCROLL) & 1) != 0;
    g_caps_lock = (::GetKeyState(VK_CAPITAL) & 1) != 0;
}

void update_lock_state(int virtual_key, bool down) noexcept {
    if (down) {
        return; // a lock key finishes toggling on key-up
    }
    switch (virtual_key) {
    case VK_NUMLOCK:
        g_num_lock = (::GetKeyState(VK_NUMLOCK) & 1) != 0;
        break;
    case VK_SCROLL:
        g_scroll_lock = (::GetKeyState(VK_SCROLL) & 1) != 0;
        break;
    case VK_CAPITAL:
        g_caps_lock = (::GetKeyState(VK_CAPITAL) & 1) != 0;
        break;
    default:
        break;
    }
}

int mouse_button_for(UINT message, WPARAM wparam) noexcept {
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        return 1;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return 2;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        return GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 3 : 4;
    default:
        return -1;
    }
}

// Returns true when the message was consumed and must not reach the game.
bool route_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        events::KeyEvent event;
        event.virtual_key = static_cast<int>(wparam);
        event.scan_code = static_cast<int>((static_cast<unsigned long long>(lparam) >> 16) & 0xFF);
        event.down = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
        event.repeat = (static_cast<unsigned long long>(lparam) & (1ULL << 30)) != 0;
        update_lock_state(event.virtual_key, event.down);
        events::bus().post(event);
        return event.consumed;
    }

    case WM_MOUSEMOVE: {
        events::MouseEvent event;
        event.x = static_cast<double>(static_cast<short>(lparam & 0xFFFF));
        event.y = static_cast<double>(static_cast<short>((lparam >> 16) & 0xFFFF));
        events::bus().post(event);
        return event.consumed;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        events::MouseEvent event;
        event.button = mouse_button_for(message, wparam);
        event.down = (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN
            || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN);
        event.x = static_cast<double>(static_cast<short>(lparam & 0xFFFF));
        event.y = static_cast<double>(static_cast<short>((lparam >> 16) & 0xFFFF));
        events::bus().post(event);
        return event.consumed;
    }

    case WM_MOUSEWHEEL: {
        events::MouseEvent event;
        event.wheel_delta = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        events::bus().post(event);
        return event.consumed;
    }

    case WM_KILLFOCUS:
    case WM_SETFOCUS: {
        // Focus loss closes the ClickGUI without this file knowing that a GUI exists.
        events::FocusEvent event;
        event.focused = (message == WM_SETFOCUS);
        events::bus().post(event);
        return false;
    }

    case WM_NCDESTROY:
        // The window is going away and our subclass dies with it. Clearing the handle keeps
        // teardown from restoring a procedure on a destroyed window.
        g_window = nullptr;
        g_previous_proc = nullptr;
        WOKE_LOG_WARN("wndproc-hook: the game window was destroyed");
        return false;

    default:
        return false;
    }
}

LRESULT CALLBACK wndproc_detour(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    g_routed_messages.fetch_add(1, std::memory_order_relaxed);

    // Step 4: the ClickGUI sees every message first. ui::handle_window_message feeds ImGui's Win32
    // backend (cursor position, buttons, wheel, keys - and WM_CHAR for text fields) and reports
    // whether the GUI took the message; a consumed message is swallowed here so the game never sees
    // a click or a keystroke that belonged to the overlay. The toggle key is deliberately left
    // alone, because it is the event bus that closes the GUI again.
    if (ui::handle_window_message(
            static_cast<void*>(window), message, static_cast<unsigned long long>(wparam),
            static_cast<long long>(lparam))) {
        return 0;
    }

    if (route_message(message, wparam, lparam)) {
        return 0;
    }

    if (g_previous_proc == nullptr) {
        return ::DefWindowProcW(window, message, wparam, lparam);
    }
    return ::CallWindowProcW(g_previous_proc, window, message, wparam, lparam);
}

} // namespace

bool install_wndproc_hook() noexcept {
    if (g_window != nullptr) {
        return true;
    }

    const HWND window = resolve_game_window();
    if (window == nullptr) {
        WOKE_LOG_ERROR("wndproc-hook: no GLFW window found in this process - input is unavailable");
        return false;
    }

    capture_lock_state();

    const LONG_PTR replacement =
        reinterpret_cast<LONG_PTR>(detail::as_address(&wndproc_detour));
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = ::SetWindowLongPtrW(window, GWLP_WNDPROC, replacement);
    if (previous == 0 && ::GetLastError() != ERROR_SUCCESS) {
        WOKE_LOG_ERROR("wndproc-hook: subclassing window %p failed (error %lu)",
            static_cast<void*>(window), ::GetLastError());
        return false;
    }

    g_previous_proc = reinterpret_cast<WNDPROC>(previous);
    g_window = window;
    g_routed_messages.store(0, std::memory_order_relaxed);
    WOKE_LOG_INFO("wndproc-hook: window %p subclassed (num %s, scroll %s, caps %s)",
        static_cast<void*>(window), g_num_lock ? "on" : "off", g_scroll_lock ? "on" : "off",
        g_caps_lock ? "on" : "off");
    return true;
}

void remove_wndproc_hook() noexcept {
    if (g_window == nullptr) {
        return;
    }

    if (g_previous_proc != nullptr && ::IsWindow(g_window) != FALSE) {
        const LONG_PTR original = reinterpret_cast<LONG_PTR>(g_previous_proc);
        (void)::SetWindowLongPtrW(g_window, GWLP_WNDPROC, original);
        WOKE_LOG_INFO("wndproc-hook: window procedure restored (%llu message(s) routed)",
            g_routed_messages.load(std::memory_order_relaxed));
    }

    g_previous_proc = nullptr;
    g_window = nullptr;
}

bool wndproc_hook_installed() noexcept {
    return g_window != nullptr;
}

void* window_handle() noexcept {
    return static_cast<void*>(g_window);
}

bool num_lock_state() noexcept {
    return g_num_lock;
}

bool scroll_lock_state() noexcept {
    return g_scroll_lock;
}

bool caps_lock_state() noexcept {
    return g_caps_lock;
}

unsigned long long routed_message_count() noexcept {
    return g_routed_messages.load(std::memory_order_relaxed);
}

} // namespace woke::hooks

#pragma once

// The complete vocabulary of cross-system communication (blueprint §3.1, §4.1).
//
// Events are plain structs with value members. A handler receives a mutable reference, so a
// handler that consumes an input - the ClickGUI taking a key before a module sees it - says
// so by setting a flag instead of needing a second event, a global or a return channel.
// Nothing here allocates, owns a resource or knows about any other system.

#include <cstdint>

namespace woke::events {

struct FrameEvent {
    float delta_seconds = 0.0f;
    std::uint64_t frame_index = 0;
    float frames_per_second = 0.0f;
};

struct TickEvent {
    float delta_seconds = 0.0f;
    std::uint64_t tick_index = 0;
};

struct KeyEvent {
    int virtual_key = 0;
    int scan_code = 0;
    bool down = false;
    bool repeat = false;   // auto-repeat, not a fresh press
    bool consumed = false; // a handler handled it: the WndProc must swallow the message
};

struct MouseEvent {
    double x = 0.0;
    double y = 0.0;
    int button = -1; // -1 for a plain move, 0/1/2 for left/right/middle
    bool down = false;
    double wheel_delta = 0.0;
    bool consumed = false;
};

// Posted when the game window gains or loses focus. WM_KILLFOCUS closes the ClickGUI
// (macOS-style behaviour, §4.7) without this file needing to know that a GUI exists.
struct FocusEvent {
    bool focused = false;
};

struct WorldChangeEvent {
    bool joined = false;
};

struct PlayerChangeEvent {
    bool present = false;
};

struct ConfigLoadedEvent {
    const char* name = nullptr;
    bool succeeded = false;
};

struct ShutdownEvent {
    const char* reason = nullptr;
};

} // namespace woke::events

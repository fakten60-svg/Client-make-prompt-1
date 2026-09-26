#pragma once

// In-world overlay policy (blueprint §7.6) - roadmap step 7.
//
// One small predicate in its own translation unit because the callers are genuinely different
// layers: the ImGui suppression gate (ui::needs_render, reached through the toast pool) and the
// frame scheduler's flag both have to know whether an enabled overlay module would draw.
//
// Portable: it reads the module registry's erased interface plus the three overlay modules'
// headers, so the host test suite asserts the policy with no game and no draw list.

namespace woke::ui::overlay {

// True when an enabled overlay module would emit something this frame: the HUD's watermark or
// arraylist, the custom crosshair, a trajectory prediction with a live player view, or any of the
// step-8 Combat/Mace readouts (whose counter chips draw even with no world loaded).
[[nodiscard]] bool wanted() noexcept;

} // namespace woke::ui::overlay

#pragma once

// Step-8 readout frame fill (blueprint §8: Combat/Mace) - roadmap step 8.
//
// The Windows-only counterpart to build_hud_frame(): one translation unit that consults the enabled
// Combat/Mace modules. It lives apart from gui.cpp because that composition is already the largest
// file in the client, and because everything in here is guarded by the same JNI-compiled-or-not
// switch, so the portable host build never sees the game facade.
//
// Contract, identical to the velocity chip's: a read that fails leaves the frame's flag false, and
// the renderer draws nothing. A missing handle degrades to "absent", never to a wrong number.
//
// This file also owns the one input the counters need: the left mouse button's press edge. The
// counter modules count the player's *own* swings, and the only honest place to observe a swing is
// the event the window procedure already produced. Recording lives here rather than in the modules
// so a module stays a pure counter with no event-bus dependency of its own.

#include "ui/hud.h"

namespace woke::ui {

// Fills the step-8 readout fields of `frame` from the enabled modules' settings and the live game
// state. Called once per composed frame, after the step-7 fields are set.
void fill_step8_readouts(hud::Frame& frame) noexcept;

// Releases the counter subscription. Called from the overlay's shutdown, before the event bus is
// reset, so a hot unload can never leave the bus calling into unmapped code.
void shutdown_step8() noexcept;

} // namespace woke::ui

#pragma once

// Hot-unload audit (roadmap step 9, gate: "unload leaves process stable").
//
// shutdown() already tears everything down in reverse boot order; what it has never had is a
// *verification* that the teardown actually completed. This file is that audit: after the
// teardown steps run, one function checks the observable residue - event-bus subscriptions,
// hook state, the frame scheduler, the overlay's flags, cached game references, attached
// threads - and reports each finding as a log line. The gate evidence for "inject/eject x20
// stable" is a shutdown transcript with no AUDIT finding lines.
//
// The checks are reads of public accessors only; the audit owns nothing and changes nothing.

#ifndef _WIN32
#error "core/teardown.h is Windows-only; portable translation units must not include it."
#endif

#include <cstddef>

namespace woke::perf {

// One teardown finding: which system, what is still live.
struct TeardownFinding {
    const char* system = nullptr; // "event-bus", "hooks", "frame-scheduler", ...
    const char* detail = nullptr; // human-readable residue description
};

// Runs the audit. Returns the number of findings (0 = the process was left clean) and fills
// `out` with up to `capacity` of them. `bus_handlers` is the total subscription count the
// caller read from the event bus before the audit - the bus itself is a template API, so the
// caller summarises it.
std::size_t verify_teardown(TeardownFinding* out, std::size_t capacity,
    std::size_t bus_handlers) noexcept;

} // namespace woke::perf

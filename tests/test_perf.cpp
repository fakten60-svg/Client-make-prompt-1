#include "test_harness.h"

// Step-9 tests (roadmap step 9: perf + hardening).
//
// The step's gate needs the game - "sustained <0.5 ms overlay; unload leaves process stable".
// What runs here is the policy behind that gate, which is exactly the part that must not be
// wrong on the day the game is finally attached:
//
//   * the EMA the chrome and pipeline meters smooth through (seed rule, fold rate, poison
//     refusal), because a NaN in the average would make every later soak line meaningless,
//   * the budget relationship (chrome inside pipeline) as a compile-time fact plus the
//     soak-interval pass policy, which is the number the §12.2 transcript is judged against,
//   * the §11 network-safety invariant as a data-driven check, mirroring the CI grep gate so a
//     violation fails the suite here too, not only on the runner.
//
// What this cannot cover is the in-game soak itself (§12.2): that is the manual pass with the
// log transcript this step's reporter emits.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "core/perf.h"

namespace {

namespace perf = woke::perf;

// The §11 gate's forbidden fragments, exactly as the CI workflow greps them. Duplicated here on
// purpose: two independent enforcers of one invariant, so drifting either one is visible.
constexpr const char* kForbiddenNetworkFragments[] = {
    "ClientConnection",
    "sendPacket",
    "PacketByteBufs",
    "networkHandler",
};

[[nodiscard]] bool contains_ignore_case(const std::string& haystack, const char* needle) noexcept {
    const std::size_t needle_length = std::strlen(needle);
    if (needle_length == 0 || haystack.size() < needle_length) {
        return false;
    }
    const auto lower = [](char a, char b) noexcept {
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        return a == b;
    };
    return std::search(haystack.begin(), haystack.end(), needle, needle + needle_length, lower)
        != haystack.end();
}

} // namespace

void test_perf_policy() {
    woke_test::section("step 9 budgets and soak policy");

    // The budget relationship is a static_assert in perf.h; asserted here as well so the failure
    // names itself in test output rather than only at compile time.
    WOKE_CHECK(perf::kChromeBudgetMs < perf::kPipelineBudgetMs);
    WOKE_CHECK(perf::kPipelineBudgetMs == 0.5f); // §6.4, the number the gate quotes

    // The soak interval is a policy constant: one report per interval means a 10-minute soak
    // produces ten lines.
    WOKE_CHECK(perf::kSoakIntervalSeconds == 60.0);

    // ── EMA: seed, fold, refusal ─────────────────────────────────────────────────────
    perf::Ema average;

    // Unseeded: the first sample is the average, not half of it.
    WOKE_CHECK(!average.seeded);
    WOKE_CHECK(average.add(4.0f, 0.05f) == 4.0f);
    WOKE_CHECK(average.seeded);
    WOKE_CHECK(average.value == 4.0f);

    // The fold rate is the caller's: at 0.05 the sample contributes 5%.
    WOKE_CHECK(average.add(6.0f, 0.05f) == 4.1f);

    // Rate 1 means "replace": a legitimate degenerate use, and it must work.
    WOKE_CHECK(average.add(10.0f, 1.0f) == 10.0f);

    // A negative sample is a broken meter, not a fast frame: refused, average unchanged.
    WOKE_CHECK(average.add(-3.0f, 0.5f) == 10.0f);

    // A NaN would poison the average forever (NaN + x = NaN): refused the same way. NaN != NaN
    // is the identity check, spelled that way in perf.h because it needs no <cmath>.
    const float nan = 0.0f / 0.0f;
    WOKE_CHECK(average.add(nan, 0.5f) == 10.0f);
    WOKE_CHECK(average.value == 10.0f);

    // Reset returns to the unseeded state - the next sample seeds again.
    average.reset();
    WOKE_CHECK(!average.seeded);
    WOKE_CHECK(average.add(2.0f, 0.05f) == 2.0f);

    // ── Soak interval policy ────────────────────────────────────────────────────────
    // The gate is "sustained", so the averages are judged, not the worst spike.
    WOKE_CHECK(perf::soak_interval_ok(0.0f, 0.0f));
    WOKE_CHECK(perf::soak_interval_ok(0.2f, 0.5f)); // exactly at both budgets: still a pass
    WOKE_CHECK(perf::soak_interval_ok(0.19f, 0.49f));

    // Over either budget alone fails: the chrome is part of the pipeline, and vice versa.
    WOKE_CHECK(!perf::soak_interval_ok(0.21f, 0.4f));
    WOKE_CHECK(!perf::soak_interval_ok(0.1f, 0.51f));

    // A NaN average must read as a failure, never as a pass.
    WOKE_CHECK(!perf::soak_interval_ok(nan, 0.4f));
    WOKE_CHECK(!perf::soak_interval_ok(0.1f, nan));
}

void test_network_safety_invariant() {
    woke_test::section("step 9 network-safety invariant (§11)");

    // Walk the module sources the way the CI grep gate does. The invariant: no module code
    // references the game's network layer - the client reads and writes client-side state
    // only. A violation fails the host suite here and the grep on the runner.
    const std::filesystem::path modules_dir = WOKE_MODULES_SOURCE_DIR;
    WOKE_CHECK(std::filesystem::exists(modules_dir));

    std::size_t files_scanned = 0;
    std::size_t violations = 0;
    const char* first_violation = "";
    for (const auto& entry : std::filesystem::recursive_directory_iterator(modules_dir)) {
        const std::string extension = entry.path().extension().string();
        if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".h")) {
            continue;
        }
        std::ifstream stream(entry.path());
        if (!stream) {
            continue;
        }
        ++files_scanned;
        std::string line;
        while (std::getline(stream, line)) {
            for (const char* fragment : kForbiddenNetworkFragments) {
                if (contains_ignore_case(line, fragment)) {
                    if (violations == 0) {
                        first_violation = entry.path().filename().string().c_str();
                    }
                    ++violations;
                }
            }
        }
    }

    // The scan must have actually seen the module tree - a wrong path would silently pass.
    WOKE_CHECK(files_scanned > 20);
    WOKE_CHECK(violations == 0); // (void)first_violation keeps -Wunused quiet on the success path
    (void)first_violation;
}


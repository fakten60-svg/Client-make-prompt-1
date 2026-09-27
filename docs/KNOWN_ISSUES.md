# Known Issues — v0.1.0/v0.1.1

Findings from the QA & hardening pass that were **deferred** rather than fixed. Each entry
names its severity and the reason it is safe to defer. Fixed findings are not listed here;
they live in `docs/QA_REPORT_v0.1.0.md` §6.

## Style / cosmetic (no behavioral risk)

| ID | Severity | Area | Description | Workaround / plan |
|---|---|---|---|---|
| KI-1 | LOW | API style | `bugprone-easily-swappable-parameters` on 8 domain APIs (`seed(gamma, fov)`, `acquire_spring(value, stiffness, damping)`, `clamp_value(lo, hi)`, `push(title, message)`, …). Same-type adjacent parameters are a readability lint, not a defect: every site is exercised by the host suite with both orders of arguments pinned. | Waived in the clang-tidy CI gate; revisit if a public-API refactor touches these files anyway. |
| KI-2 | LOW | Perf metering | `PerfMeter`/`Ema` reset the seed on first sample rather than presenting a warm-up window; the first soak interval of a session is slightly optimistic. | Cosmetic: in-game gate (§12.2) measures a 10-minute soak; the first 60 s are not the gate. |
| KI-3 | LOW | Tooling | The MinGW syntax pass cannot verify the sandbox's mingw-GCC-10 heterogeneous-lookup false positive on `jni/mappings.cpp` (documented §12.3); that TU's syntax is covered only by host GCC-11 and MSVC CI. | Accept: two independent compilers cover the TU. |

## Deferred to v0.1.2

| ID | Severity | Area | Description | Plan |
|---|---|---|---|---|
| KI-4 | MEDIUM | Config | The F2 atomic-write window: `std::rename` cannot replace an existing file on every Windows runtime, so `write()` does `remove(target)` then `rename(temp, target)`. A crash in that single-call gap leaves **no** config file (defaults return), never a truncated one. Closing the window fully needs `ReplaceFileW` (Windows-only backend code, testable only in-game). | v0.1.2: add a Win32 `FileStorage::write` specialization over `ReplaceFileW` with a host-test on the portable path unchanged. |
| KI-5 | MEDIUM | Teardown audit | `verify_teardown` reports residue only; it does not yet check *attached-thread count* or *logger queue state* — the two findings a bad unload would also show — because both accessors live in Windows-only TUs. | v0.1.2: extend the audit with a Windows TU contribution once the in-game protocol (§7) has validated the current transcript format. |
| KI-6 | LOW | Mappings | An asset **without** a `version` field is accepted (schema-variant tolerance, QA F3 kept this behavior). A future generator regression could silently drop the field and re-open the mismatch window. | v0.1.2: emit a WARN when the field is absent, keep accepting; consider refusing after a deprecation period. |

# Central warning policy for woke.wtf first-party targets.
#
# Vendored code (ImGui, MinHook) intentionally never receives these flags: upstream
# warnings must never be able to fail our build, and our warnings must never be
# masked by upstream noise. Only call this for targets we own.
function(woke_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4             # high warning level
            /permissive-    # standards conformance mode
            /Zc:__cplusplus # report the real __cplusplus value
            /utf-8          # source and execution charset
            /MP             # parallel compilation (faster low-end builds)
        )
        # QA hardening: MSVC's static analyzer, first-party sources only. The CI static
        # analysis job turns this on; external (vendored and system) code stays out of the
        # analysis scope via /analyze:external-, so upstream noise can never reach the log.
        # Deliberately combined with neither /Wx nor a curated ruleset: the pass runs as an
        # evidence producer (findings land in the CI log for triage) until a real MSVC pass
        # has told us which rules are actionable for game-thread code.
        if(WOKE_ENABLE_MSVC_ANALYZE)
            target_compile_options(${target} PRIVATE /analyze /analyze:external-)
        endif()
        if(WOKE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(WOKE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

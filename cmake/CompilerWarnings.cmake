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

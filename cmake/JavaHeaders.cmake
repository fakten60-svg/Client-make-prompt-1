# JDK JNI header discovery.
#
# The JVM bridge needs jni.h and jni_md.h. They come from the JDK that runs the game, and
# the client deliberately does NOT ship a copy: vendoring Java headers is how a project
# silently drifts away from the runtime it actually attaches to.
#
# Resolution order:
#   1. -DWOKE_JNI_INCLUDE_DIR=<jdk>/include   (explicit override, used by CI or a custom JDK)
#   2. $ENV{JAVA_HOME}/include
#   3. The usual Windows install roots
#
# When none of them yields jni.h, WOKE_HAVE_JNI is FALSE and the JVM bridge is compiled out:
# the DLL still builds, still boots, and reports "built without JDK JNI headers" instead of
# failing the build on a machine that cannot run Minecraft anyway.

# ON makes a missing JDK a configuration error instead of a soft skip. CI turns it on so
# the JVM bridge is always compiled there - a green build then proves the bridge builds,
# not merely that it was left out.
option(WOKE_REQUIRE_JNI "Fail configuration when no JDK jni.h can be found" OFF)

set(WOKE_JNI_INCLUDE_DIR "" CACHE PATH "JDK 'include' directory containing jni.h (empty = autodetect)")

set(_woke_jni_candidates "")
if(WOKE_JNI_INCLUDE_DIR)
    list(APPEND _woke_jni_candidates "${WOKE_JNI_INCLUDE_DIR}")
endif()
if(DEFINED ENV{JAVA_HOME} AND NOT "$ENV{JAVA_HOME}" STREQUAL "")
    list(APPEND _woke_jni_candidates "$ENV{JAVA_HOME}/include")
endif()

file(GLOB _woke_jdk_roots
    "$ENV{ProgramFiles}/Eclipse Adoptium/jdk-*"
    "$ENV{ProgramFiles}/Eclipse Adoptium/jdk*"
    "$ENV{ProgramFiles}/Java/jdk-*"
    "$ENV{ProgramFiles}/Microsoft/jdk-*"
    "$ENV{ProgramFiles}/Zulu/zulu-*"
    "$ENV{ProgramFiles}/Amazon Corretto/*"
    "$ENV{ProgramFiles}/BellSoft/*jdk*"
    # Literal path because CMake's $ENV{...} cannot express 'ProgramFiles(x86)' - a
    # parenthesis is not valid in a variable name, and there is no escape for it.
    "C:/Program Files (x86)/Java/jdk-*")
foreach(_root IN LISTS _woke_jdk_roots)
    list(APPEND _woke_jni_candidates "${_root}/include")
endforeach()

set(WOKE_HAVE_JNI FALSE)
foreach(_candidate IN LISTS _woke_jni_candidates)
    if(EXISTS "${_candidate}/jni.h")
        set(WOKE_JNI_INCLUDE_DIR "${_candidate}" CACHE PATH
            "JDK 'include' directory containing jni.h" FORCE)
        set(WOKE_HAVE_JNI TRUE)
        break()
    endif()
endforeach()

if(WOKE_HAVE_JNI)
    add_library(jni_headers INTERFACE)
    # include/ holds jni.h, include/win32 holds jni_md.h.
    target_include_directories(jni_headers INTERFACE
        "${WOKE_JNI_INCLUDE_DIR}"
        "${WOKE_JNI_INCLUDE_DIR}/win32")
    message(STATUS "woke: JNI headers found at ${WOKE_JNI_INCLUDE_DIR}")
elseif(WOKE_REQUIRE_JNI)
    message(FATAL_ERROR
        "woke: no JDK jni.h found (set JAVA_HOME or -DWOKE_JNI_INCLUDE_DIR). "
        "WOKE_REQUIRE_JNI=ON is set, so this is fatal: the JVM bridge would be silently "
        "compiled out and roadmap step 2 would not actually be verified.")
else()
    message(WARNING
        "woke: no JDK jni.h found (set JAVA_HOME or -DWOKE_JNI_INCLUDE_DIR). "
        "The JVM bridge (jni_context, reflection_cache, game_instance) is skipped; the DLL "
        "still builds, boots and logs, but cannot read game state.")
endif()

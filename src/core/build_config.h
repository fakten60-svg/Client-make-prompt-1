#pragma once

// Build-time capability flags, in one place.
//
// CMake defines these on the DLL target, so this header only supplies the fallback that keeps
// a translation unit compilable outside the project's own build (an editor's syntax check, a
// one-off compiler invocation). Without it, every `#if` would need its own `#ifndef` guard and
// the flag would end up meaning slightly different things in different files.

// 1 when the JDK's jni.h was found at configure time and the JVM bridge is part of the build.
#ifndef WOKE_HAVE_JNI
#define WOKE_HAVE_JNI 0
#endif

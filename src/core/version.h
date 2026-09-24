#pragma once

// Single source of truth for build metadata. The sidebar logo, HUD watermark, boot
// banner and crash-triage line all read from here, so a version bump can never leave
// one surface reporting a stale value.
#ifndef WOKE_VERSION_STRING
#define WOKE_VERSION_STRING "0.1.0-dev"
#endif

namespace woke::version {

inline constexpr const char* kClientName = "woke.wtf";
inline constexpr const char* kVersion = WOKE_VERSION_STRING;
inline constexpr const char* kTargetGame = "Minecraft 1.21.11 Fabric";
inline constexpr const char* kTargetArch = "x64 Windows (javaw.exe)";
inline constexpr const char* kBuildDate = __DATE__;
inline constexpr const char* kBuildTime = __TIME__;

} // namespace woke::version

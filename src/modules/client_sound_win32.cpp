#include "modules/misc/client_sound.h"

// The Windows audio backend for Client Sound: a short click/success/warning waveform synthesized in
// memory and handed to PlaySound with SND_MEMORY | SND_ASYNC. There is no audio asset to ship and no
// worker thread: the buffer is a static, prebuilt RIFF/PCM block, and PlaySound owns the playback.
//
// Windows-only by construction, so it lives in its own translation unit that only the DLL target
// compiles; the host tests keep the recording sink.

#include <windows.h>
#include <mmsystem.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace woke::modules::misc {
namespace {

constexpr std::size_t kSampleRate = 22050;
constexpr std::size_t kMaxSamples = 1024;               // ~46 ms ceiling
constexpr std::size_t kWavBytes = 44 + (kMaxSamples * 2); // 16-bit mono
constexpr std::size_t kCueCount = static_cast<std::size_t>(SoundCue::Count);
constexpr double kTwoPi = 6.28318530717958647692;

// One buffer per cue, reused across plays: PlaySound's async playback reads the block, so it must
// outlive the call. The buffer is rebuilt on each play so the module's volume is honoured by scaling
// the waveform's amplitude (PlaySound itself has no per-call volume).
std::array<std::array<std::uint8_t, kWavBytes>, kCueCount> g_buffers{};

void put_u32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value & 0xFFu);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

void put_u16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value & 0xFFu);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void put_str(std::uint8_t* out, const char* four) noexcept {
    std::memcpy(out, four, 4);
}

// Builds a RIFF/WAVE/PCM block: a square-ish tone with a linear decay so the cue ends cleanly
// instead of popping, scaled by `volume` (0..1).
std::size_t build_cue(std::uint8_t* out, double frequency, float volume) noexcept {
    const std::size_t samples = kMaxSamples / 2;
    const std::size_t data_bytes = samples * 2;

    put_str(out, "RIFF");
    put_u32(out + 4, static_cast<std::uint32_t>(36 + data_bytes));
    put_str(out + 8, "WAVE");
    put_str(out + 12, "fmt ");
    put_u32(out + 16, 16);                                   // PCM chunk size
    put_u16(out + 20, 1);                                    // PCM
    put_u16(out + 22, 1);                                    // mono
    put_u32(out + 24, static_cast<std::uint32_t>(kSampleRate));
    put_u32(out + 28, static_cast<std::uint32_t>(kSampleRate * 2)); // byte rate
    put_u16(out + 32, 2);                                    // block align
    put_u16(out + 34, 16);                                   // bits per sample
    put_str(out + 36, "data");
    put_u32(out + 40, static_cast<std::uint32_t>(data_bytes));

    const double amplitude = 0.28 * static_cast<double>(volume < 0.0f ? 0.0f : volume);
    for (std::size_t index = 0; index < samples; ++index) {
        const double t = static_cast<double>(index) / static_cast<double>(kSampleRate);
        const double envelope = 1.0 - (static_cast<double>(index) / static_cast<double>(samples));
        const double wave = std::sin(kTwoPi * frequency * t) >= 0.0 ? 1.0 : -1.0;
        const double sample = wave * envelope * amplitude;
        const auto quantized = static_cast<std::int16_t>(sample * 32767.0);
        put_u16(out + 44 + (index * 2), static_cast<std::uint16_t>(quantized));
    }
    return 44 + data_bytes;
}

double cue_frequency(SoundCue cue) noexcept {
    switch (cue) {
    case SoundCue::Success:
        return 880.0;
    case SoundCue::Warning:
        return 320.0;
    case SoundCue::Toggle:
    default:
        return 640.0;
    }
}

class Win32ClientSoundSink final : public ClientSoundSink {
public:
    void play(SoundCue cue, float volume) noexcept override {
        const std::size_t index = static_cast<std::size_t>(cue);
        if (index >= kCueCount) {
            return;
        }
        build_cue(g_buffers[index].data(), cue_frequency(cue), volume);
        (void)::PlaySoundW(reinterpret_cast<LPCWSTR>(g_buffers[index].data()), nullptr,
            SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
};

Win32ClientSoundSink g_win32_sound{};

} // namespace

// Installed by the modules boot step, so every cue after boot reaches the audio backend.
void install_client_sound_backend() noexcept {
    (void)set_client_sound_sink(&g_win32_sound);
}

} // namespace woke::modules::misc

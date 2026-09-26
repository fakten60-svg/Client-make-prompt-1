#pragma once

// Client Sound (blueprint §8: Misc). Roadmap step 8.
//
// Local UI feedback only: a short cue when the client itself does something the user did (a module
// toggle, a config load, a warning). It is a *client* sound in the strictest sense - the sink plays
// it on this machine and nothing is sent anywhere - and the module's settings are the whole feature:
// per-cue toggles and a volume.
//
// The actual audio call is behind a portable seam, exactly like modules/game_writes.h: the module
// asks the sink to play a cue, the Windows backend (client_sound_win32.cpp) synthesizes a short
// waveform and hands it to PlaySound, and the process default is a recorder. That split is what
// makes "a warning while its toggle is off plays nothing" a host-testable fact instead of a claim.
//
// The module never plays anything on its own initiative: every cue is emitted by a visible action
// (a toggle, a load, a refused enable), so the sound is always the acknowledgement of something the
// user can see.

#include <array>
#include <cstdint>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::misc {

// The cues the client can emit. Kept small and semantic: the sink decides what a "warning" sounds
// like, not the module.
enum class SoundCue : std::uint8_t {
    Toggle = 0,  // a module/state change was acknowledged
    Success = 1, // a config loaded/saved
    Warning = 2, // a refused action or a degraded read
    Count = 3,
};

// The play sink. Portable and dependency-free so the host tests can observe intent without audio.
class ClientSoundSink {
public:
    virtual ~ClientSoundSink() = default;

    // `volume` is 0..1 as the module's slider says it. Implementations must not block: the caller is
    // the render thread.
    virtual void play(SoundCue cue, float volume) noexcept = 0;
};

// Installs a backend and returns the previous one (config::set_storage's shape). nullptr restores
// the built-in recorder.
[[nodiscard]] ClientSoundSink* set_client_sound_sink(ClientSoundSink* sink) noexcept;
[[nodiscard]] ClientSoundSink& client_sound_sink() noexcept;

// Installs the platform audio backend over the recorder (client_sound_win32.cpp). Called by the
// modules boot step; a build without a backend keeps the recorder, and the module still reports the
// cue it would have played.
void install_client_sound_backend() noexcept;

// The process default: records the last cue instead of making a sound.
class RecordingClientSoundSink final : public ClientSoundSink {
public:
    void play(SoundCue cue, float volume) noexcept override;

    [[nodiscard]] std::size_t plays() const noexcept { return plays_; }
    [[nodiscard]] SoundCue last_cue() const noexcept { return last_cue_; }
    [[nodiscard]] float last_volume() const noexcept { return last_volume_; }
    void reset() noexcept {
        plays_ = 0;
        last_cue_ = SoundCue::Toggle;
        last_volume_ = 0.0f;
    }

private:
    std::size_t plays_ = 0;
    SoundCue last_cue_ = SoundCue::Toggle;
    float last_volume_ = 0.0f;
};

class ClientSound final : public BaseModule {
public:
    ClientSound() noexcept
        : BaseModule("Client Sound",
            "Local UI cues for toggles, loads and warnings.", Category::Misc, 0) {
        register_settings(std::array<settings::Setting*, 3>{&toggles_, &success_, &volume_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] bool toggles_enabled() const noexcept { return toggles_.value(); }
    [[nodiscard]] bool success_enabled() const noexcept { return success_.value(); }
    [[nodiscard]] float volume() const noexcept { return volume_.value(); }

    // True when the cue's own toggle is on and a positive volume is set. Warnings always play while
    // the module is enabled: an alert the user silenced alongside their toggle preference would be
    // an alert that never arrives.
    [[nodiscard]] bool should_play(SoundCue cue) const noexcept {
        if (!enabled() || volume_.value() <= 0.0f) {
            return false;
        }
        switch (cue) {
        case SoundCue::Toggle:
            return toggles_.value();
        case SoundCue::Success:
            return success_.value();
        default:
            return true;
        }
    }

    // Emits a cue through the sink when the module says it should. Returns true when the sink was
    // asked, which is what a caller (and the test suite) treats as "the cue happened".
    bool play(SoundCue cue) noexcept {
        if (!should_play(cue)) {
            return false;
        }
        client_sound_sink().play(cue, volume_.value());
        return true;
    }

private:
    settings::BoolSetting toggles_{"toggles", "Cue on a module toggle", true};
    settings::BoolSetting success_{"success", "Cue on a config load or save", true};
    settings::SliderSetting volume_{"volume", "Cue volume", 0.60f, 0.0f, 1.0f, 0.05f};
};

} // namespace woke::modules::misc

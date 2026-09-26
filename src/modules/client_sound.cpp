#include "modules/misc/client_sound.h"

// The sink storage and the process default. Portable on purpose: the host tests link this
// translation unit and the Windows DLL installs the audio backend over the recorder, so there is
// never more than one definition of these symbols in a target.

namespace woke::modules::misc {
namespace {

RecordingClientSoundSink g_default_sound{};
ClientSoundSink* g_active = &g_default_sound;

} // namespace

void RecordingClientSoundSink::play(SoundCue cue, float volume) noexcept {
    ++plays_;
    last_cue_ = cue;
    last_volume_ = volume;
}

ClientSoundSink* set_client_sound_sink(ClientSoundSink* sink) noexcept {
    ClientSoundSink* previous = g_active;
    g_active = sink != nullptr ? sink : &g_default_sound;
    return previous;
}

ClientSoundSink& client_sound_sink() noexcept {
    return *g_active;
}

} // namespace woke::modules::misc

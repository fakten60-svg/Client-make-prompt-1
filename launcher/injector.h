#pragma once

namespace woke::launcher {

// Local singleplayer testing helper: loads woke.dll into an already-running game
// process via the standard LoadLibraryW remote-thread technique.
//
// Arguments:
//   <path-to-woke.dll> [--pid <number>] [--process <name>]
// Defaults to searching for javaw.exe when neither selector is given.
int run(int argc, wchar_t** argv);

} // namespace woke::launcher

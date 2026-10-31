// Setup built without its window (XBE_SETUP_WINDOW off, as on the launcher branch): the command line only. Conker:
// Recomped & Reloaded's launcher runs Setup with --cli and is the window, and without FLTK Setup links no X11,
// Wayland, Pango or Cairo libraries.
#include <cstdio>

#include "setup.h"

namespace setup {

bool WindowAvailable() { return false; }

int RunWindow(const Options&) {
    std::fprintf(stderr, "This Setup has no window: run it with --cli (see --help), or use the launcher.\n");
    return 2;
}

}  // namespace setup

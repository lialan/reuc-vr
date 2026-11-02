// Wii Remotes off Windows: the HID transport (SetupDi/HidD and overlapped I/O, wiimote_hid.cpp) is Windows-only for now; SDL_hid would carry it.
#include "wiimote_transport.h"

namespace uc {
std::unique_ptr<WiiTransport> wii_open_first(std::string& error) {
    error = "Wii Remotes are supported in the Windows version only";
    return nullptr;
}
}  // namespace uc

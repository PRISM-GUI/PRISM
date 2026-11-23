#include <prism/audio/backend.hpp>

// The SDL backend is rebuilt here next: a DeviceBackend listing SDL's playback devices and opening
// them as a DeviceStream, registered with prism::audio::register_backend when SDL is compiled in.
// Until then Output renders on its timer, so this file holds nothing.

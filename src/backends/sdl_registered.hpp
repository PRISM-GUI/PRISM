#pragma once

// The SDL backends the registries start with, when the software backend is built (PRISM_SDL_BACKENDS).
// The registries call these instead of the backends registering themselves from a static object:
// in a static link (the web build) nothing references such an object, so the linker drops it and
// the registry stays empty.

namespace prism::audio {
class DeviceBackend;
DeviceBackend& sdl_audio_backend();
} // namespace prism::audio

namespace prism::render {
class ImageDecoder;
ImageDecoder& sdl_image_decoder();
} // namespace prism::render

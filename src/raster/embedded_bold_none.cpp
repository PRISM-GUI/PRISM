#include "embedded_fonts.hpp"

// Built instead of the generated bold face when the meson option embedded_bold_face=off: the
// rasterizer then draws bold text with the regular face (Fonts' constructor copies it over).
namespace prism::raster::embedded {

std::span<const std::uint8_t> noto_sans_bold_latin() { return {}; }

} // namespace prism::raster::embedded

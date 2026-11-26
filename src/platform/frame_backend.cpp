#include "frame_backend.hpp"

#include <utility>

namespace prism::platform::platform_detail {

PanelWindow::PanelWindow(core::Size size)
    : width_(static_cast<int>(size.w.raw())), height_(static_cast<int>(size.h.raw())) {}

std::pair<int, int> PanelWindow::size() const
{
    return {width_, height_};
}

FrameBackend::FrameBackend(app::Window& window, std::function<void()> quit)
    : window_(window), quit_(std::move(quit)) {}

void FrameBackend::quit()
{
    if (quit_)
        quit_();
}

} // namespace prism::platform::platform_detail

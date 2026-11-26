#pragma once

// The panel as a Window and the backend that shows it: what a frame target needs of app::Window
// and app::BackendBase so that an app's code -- AppContext, open_window, the waker -- runs
// unchanged. One window: the panel. Nothing here opens a window, prompts for a file or draws; the
// frame runtime owns the pixels, and the port owns the display.

#include <prism/app/backend.hpp>
#include <prism/app/window.hpp>
#include <prism/core/types.hpp>

#include <functional>

namespace prism::platform::platform_detail {

// The panel, decorated None: the board's display has no title bar to put a mode on. Its size is
// the panel's in logical units, so the tree lays out for the screen it will be drawn on.
class PanelWindow final : public app::Window {
public:
    explicit PanelWindow(core::Size size);

    app::WindowId id() const override { return 1; }

    void set_title(std::string_view) override {}
    void set_size(int, int) override {}   // the panel's size is its panel's, not the app's
    std::pair<int, int> size() const override;
    void set_position(int, int) override {}
    std::pair<int, int> position() const override { return {0, 0}; }

    void set_decoration_mode(app::DecorationMode) override {}
    app::DecorationMode decoration_mode() const override { return app::DecorationMode::None; }

    void set_resizable(bool) override {}
    bool is_resizable() const override { return false; }
    void set_fullscreen(bool) override {}
    bool is_fullscreen() const override { return true; }
    void minimize() override {}
    void maximize() override {}
    void restore() override {}
    void show() override {}
    void hide() override {}
    void close() override {}
    void set_cursor(ui::CursorShape) override {}

private:
    int width_;
    int height_;
};

// The backend of a frame run. The frame runtime, not a renderer behind this, draws and presents;
// so submit/wake/run do nothing, and only quit() means something: it ends the run. There are no
// PlatformServices either, so AppContext answers a file request with an error.
class FrameBackend final : public app::BackendBase {
public:
    FrameBackend(app::Window& window, std::function<void()> quit);

    app::Window& create_window(app::WindowConfig) override { return window_; }
    // A board shows one app, on one panel: a second window request fails, and AppContext::open_window
    // hands the app 0, as it does when a desktop backend refuses one.
    app::Window* request_window(app::WindowConfig) override { return nullptr; }

    void run(std::function<void(const app::WindowEvent&)>) override {}
    void submit(app::WindowId, std::shared_ptr<const render::SceneSnapshot>) override {}
    void wake() override {}
    void quit() override;
    // One window, so the runtime asks the model for no more (AppContext::serve_window_requests).
    bool opens_windows() const override { return false; }

private:
    app::Window& window_;
    std::function<void()> quit_;
};

} // namespace prism::platform::platform_detail

// Pixel tests: real widgets rendered through the real SDL backend (offscreen driver), at 1x and
// 2x. They check properties of the pixels -- a selection covers exactly its word's ink, an icon
// doesn't overlap its label -- rather than comparing to reference images, so they hold across
// FreeType versions and font hinting while still catching the bugs that only real pixels show.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/app/event_routing.hpp>
#include <prism/app/widget_tree.hpp>
#include <prism/backends/sdl_font_set.hpp>
#include <prism/backends/sdl_text_measurer.hpp>
#include <prism/backends/sdl_window.hpp>
#include <prism/nodes/graph.hpp>
#include <prism/render/image_ops.hpp>
#ifdef PRISM_TEST_WEBGPU
#include <prism/backends/wgpu_renderer.hpp>
#endif

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace prism::core {} namespace prism::render {} namespace prism::input {}
namespace prism::ui {} namespace prism::app {}
namespace prism {
using namespace core; using namespace render; using namespace input;
using namespace ui; using namespace app;
}
using namespace prism;

namespace {

struct Frame {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
    const std::uint8_t* at(int x, int y) const { return &rgba[(static_cast<size_t>(y) * w + x) * 4]; }
    bool differs(const Frame& o, int x, int y, int threshold = 24) const {
        auto *a = at(x, y), *b = o.at(x, y);
        return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) > threshold;
    }
};

// Column ranges [first, last] of x where `ink(x)` holds, merged across gaps below `min_gap`.
template <typename Ink>
std::vector<std::pair<int, int>> runs(int w, int min_gap, Ink ink) {
    std::vector<std::pair<int, int>> out;
    for (int x = 0; x < w; ++x) {
        if (!ink(x)) continue;
        if (!out.empty() && x - out.back().second <= min_gap) out.back().second = x;
        else out.push_back({x, x});
    }
    return out;
}

enum class Via { Sdl, WebGpu };

// One font set, rendering a model's widget tree at a device scale: through an SDL window
// (SDL_Renderer), or offscreen through the WebGPU renderer with the same fonts.
struct Rig {
    static constexpr int w = 480, h = 60;
    explicit Rig(Via via = Via::Sdl) : via(via) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        REQUIRE(SDL_Init(SDL_INIT_VIDEO));
        REQUIRE(TTF_Init());
        REQUIRE(fonts.open(PRISM_TEST_UI_FONT_PATH, PRISM_TEST_MONO_FONT_PATH, 14.f));
    }
    ~Rig() {
        fonts.close();
        TTF_Quit();
        SDL_Quit();
    }

    template <typename Model>
    Frame render(WidgetTree& tree, float scale, const char* name, int width = w, int height = h) {
        // Every pixel in this file was checked against the classic look: ask for it by name.
        tree.set_theme(prism::classic_dark_theme());
#ifdef PRISM_TEST_WEBGPU
        if (via == Via::WebGpu) return render_webgpu(tree, scale, name);
#endif
        backends::SdlWindow win{1, WindowConfig{.title = name, .width = static_cast<int>(width * scale),
                                                .height = static_cast<int>(height * scale),
                                                .decoration = DecorationMode::None}};
        win.ensure_created();
        win.force_device_scale(scale);
        tree.set_text_measurer(&measurer);
        auto snap = tree.build_snapshot(width, height, ++version);
        win.render_snapshot(*snap, fonts, /*present=*/false);
        SDL_Surface* raw = SDL_RenderReadPixels(win.renderer(), nullptr);
        REQUIRE(raw);
        SDL_Surface* px = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        REQUIRE(px);
        if (const char* dir = std::getenv("PRISM_PIXEL_DUMP"))   // for looking at a failure
            SDL_SaveBMP(px, (std::string(dir) + "/" + name + ".bmp").c_str());
        Frame f{px->w, px->h, {}};
        for (int y = 0; y < px->h; ++y) {
            auto* row = static_cast<std::uint8_t*>(px->pixels) + static_cast<size_t>(y) * px->pitch;
            f.rgba.insert(f.rgba.end(), row, row + px->w * 4);
        }
        SDL_DestroySurface(px);
        return f;
    }

#ifdef PRISM_TEST_WEBGPU
    Frame render_webgpu(WidgetTree& tree, float scale, const char* name) {
        static auto ctx = backends::WgpuContext::create();
        REQUIRE(ctx);
        tree.set_text_measurer(&measurer);
        auto snap = tree.build_snapshot(w, h, ++version);
        int pw = static_cast<int>(w * scale), ph = static_cast<int>(h * scale);
        auto* f = &fonts;
        Frame out{pw, ph, backends::render_to_pixels(*ctx, *snap, pw, ph, scale, tree.theme().canvas_bg,
                    [f](std::string_view text, float px, FontStyle font) { return f->rasterize(text, px, font); })};
        REQUIRE(out.rgba.size() == static_cast<size_t>(pw) * ph * 4);
        if (const char* dir = std::getenv("PRISM_PIXEL_DUMP"))
            if (SDL_Surface* s = SDL_CreateSurfaceFrom(pw, ph, SDL_PIXELFORMAT_RGBA32, out.rgba.data(), pw * 4)) {
                SDL_SaveBMP(s, (std::string(dir) + "/webgpu-" + name + ".bmp").c_str());
                SDL_DestroySurface(s);
            }
        return out;
    }
#endif

    Via via;
    backends::SdlFontSet fonts;
    backends::SdlTextMeasurer measurer{PRISM_TEST_UI_FONT_PATH, PRISM_TEST_MONO_FONT_PATH};
    std::uint64_t version = 0;
};

// Frame pixels per logical pixel: the forced scale, times the display's on a HiDPI desktop.
int device_scale(const Frame& f) { return std::max(1, f.w / Rig::w); }

WidgetNode& first_leaf(WidgetTree& tree) {
    WidgetNode* n = &tree.root();
    while (n->is_container && !n->children.empty()) n = &n->children.front();
    return *n;
}

struct Field1 {
    Field<TextField<>> text;
    void view(WidgetTree::ViewBuilder& vb) { vb.widget(text); }
};

constexpr int ink_threshold = 60;

// Ink: pixels far from the field's background colour, in the text's half of the field.
struct Ink {
    const Frame& f;
    const std::uint8_t* bg;
    explicit Ink(const Frame& frame) : f(frame), bg(frame.at(frame.w - 2, frame.h / 4)) {}
    bool at(int x, int y) const {
        auto* p = f.at(x, y);
        return std::abs(p[0] - bg[0]) + std::abs(p[1] - bg[1]) + std::abs(p[2] - bg[2]) > ink_threshold;
    }
    bool column(int x, int top, int bottom) const {
        for (int y = top; y <= bottom; ++y) if (at(x, y)) return true;
        return false;
    }
};

// The value drawn in a text field: no selection unless `selection` is given.
Frame field_frame(Rig& rig, std::string value, float scale, std::optional<std::pair<size_t, size_t>> selection = {}) {
    Field1 model;
    model.text.set({.value = std::move(value)});
    WidgetTree tree{model};
    if (selection) {
        auto& es = first_leaf(tree).get_or_create<TextEditState>();
        es.anchor = selection->first;
        es.cursor = selection->second;
    }
    std::string tag = "field@" + std::to_string(static_cast<int>(scale)) + "x-" + std::to_string(rig.version);
    return rig.render<Field1>(tree, scale, tag.c_str());
}

void check_selection_covers_its_word(float scale, Via via = Via::Sdl) {
    Rig rig{via};
    // The selected word sits far along the line: measurement drift grows with distance.
    std::string before = "The quick brown fox jumps over the";
    std::string word = " lazy";
    std::string full = before + word + " dog";
    Frame the = field_frame(rig, before, scale);
    Frame the_quick = field_frame(rig, before + word, scale);
    Frame plain = field_frame(rig, full, scale);
    Frame selected = field_frame(rig, full, scale, std::pair<size_t, size_t>{before.size() + 1, before.size() + word.size()});
    int s = device_scale(plain);

    // Glyph rows, from the plain frame.
    Ink ink{plain};
    int top = -1, bottom = -1;
    for (int y = 0; y < plain.h; ++y)
        for (int x = 0; x < plain.w - 4 * s; ++x)
            if (ink.at(x, y)) { if (top < 0) top = y; bottom = y; break; }
    REQUIRE(top > 0);

    // Each word's ink is what the longer text adds: rendering is left-aligned and positional.
    auto last_ink = [&](const Frame& f) {
        Ink k{f};
        int last = -1;
        for (int x = 0; x < f.w - 4 * s; ++x) if (k.column(x, top, bottom)) last = x;
        return last;
    };
    auto first_ink_after = [&](const Frame& f, int after) {
        Ink k{f};
        for (int x = after + 1; x < f.w - 4 * s; ++x) if (k.column(x, top, bottom)) return x;
        return -1;
    };
    int the_end = last_ink(the);
    int quick_begin = first_ink_after(the_quick, the_end);
    int quick_end = last_ink(the_quick);
    int brown_begin = first_ink_after(plain, quick_end);
    REQUIRE(quick_begin > the_end);
    REQUIRE(brown_begin > quick_end);

    // The highlight's extent, on a row inside it but above the glyphs (the line is taller).
    auto highlight = runs(plain.w, 0, [&](int x) { return selected.differs(plain, x, top - 1); });
    REQUIRE_FALSE(highlight.empty());
    int left = highlight.front().first, right = highlight.back().second;

    INFO("scale " << scale << ": highlight [" << left << ", " << right << "], word ink ["
         << quick_begin << ", " << quick_end << "], previous word ends " << the_end << ", next starts " << brown_begin);
    int slack = s;   // one device pixel of antialiasing either way
    CHECK(left <= quick_begin + slack);
    CHECK(right >= quick_end - slack);
    CHECK(left > the_end - slack);
    CHECK(right < brown_begin + slack);
}

struct RowModel {
    Field<TreeRow> row;
    void view(WidgetTree::ViewBuilder& vb) { vb.widget(row); }
};

Frame row_frame(Rig& rig, std::string label, float scale) {
    RowModel model;
    model.row.set({.id = 1, .label = std::move(label), .icon = std::string{"\xef\x81\xbb"},
                   .depth = 0, .has_children = true});
    WidgetTree tree{model};
    std::string tag = "tree-row@" + std::to_string(static_cast<int>(scale)) + "x-" + std::to_string(rig.version);
    return rig.render<RowModel>(tree, scale, tag.c_str());
}

void check_icon_clear_of_label(float scale, Via via = Via::Sdl) {
    Rig rig{via};
    Frame icon_only = row_frame(rig, "", scale);
    Frame with_label = row_frame(rig, "examples", scale);
    int s = device_scale(icon_only);
    Ink ink{icon_only};
    int band = 24 * s;
    int icon_end = -1;
    for (int x = 0; x < icon_only.w - 4 * s; ++x) if (ink.column(x, 0, band)) icon_end = x;
    int label_begin = -1;
    for (int x = 0; x < with_label.w && label_begin < 0; ++x)
        for (int y = 0; y < band; ++y)
            if (with_label.differs(icon_only, x, y)) { label_begin = x; break; }
    INFO("scale " << scale << ": icon ink ends " << icon_end << ", label ink begins " << label_begin);
    REQUIRE(icon_end > 0);
    REQUIRE(label_begin > 0);
    CHECK(label_begin - icon_end >= 2 * s);
}

} // namespace

// In a Hebrew text field, an English word after the Hebrew one is drawn first, on the left (a
// right-to-left line), and in its own left-to-right order. The window's renderer once shaped the
// whole string as one run, which drew the word backwards. The line is right-aligned, so the word
// sits at a fractional pixel: it is compared by which word its ink looks most like, "ab" or "ba".
void check_english_word_in_hebrew_line(float scale) {
    Rig rig;
    const std::string hebrew = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
    Frame ab = field_frame(rig, "ab", scale);
    Frame ba = field_frame(rig, "ba", scale);
    Frame line = field_frame(rig, hebrew + " ab", scale);
    int s = device_scale(ab);
    int width = rig.fonts.rasterize("ab", 14.f * s, FontRole::Ui)->width;
    int y0 = 6 * s, y1 = 24 * s;   // the field's text, inside its padding
    auto ink_start = [&](const Frame& f) {
        Ink ink(f);
        for (int x = 0; x < f.w; ++x) if (ink.column(x, y0, y1)) return x;
        return -1;
    };
    // Pixels that differ between the first `width` columns of ink of two frames.
    auto distance = [&](const Frame& a, const Frame& b) {
        int xa = ink_start(a), xb = ink_start(b), n = 0;
        REQUIRE(xa >= 0);
        REQUIRE(xb >= 0);
        for (int dx = 0; dx < width; ++dx)
            for (int y = y0; y < y1; ++y) {
                auto *p = a.at(xa + dx, y), *q = b.at(xb + dx, y);
                n += std::abs(p[0] - q[0]) + std::abs(p[1] - q[1]) + std::abs(p[2] - q[2]) > 24 ? 1 : 0;
            }
        return n;
    };
    CHECK(distance(line, ab) < distance(line, ba));
    CHECK(ink_start(line) > ink_start(ab) + 4 * s);   // and the line is right-aligned
}

TEST_CASE("an English word in a Hebrew line is drawn on the left, in order, at 1x") { check_english_word_in_hebrew_line(1.f); }
TEST_CASE("an English word in a Hebrew line is drawn on the left, in order, at 2x") { check_english_word_in_hebrew_line(2.f); }

TEST_CASE("a selection covers exactly its word, at 1x") { check_selection_covers_its_word(1.f); }
TEST_CASE("a selection covers exactly its word, at 2x (HiDPI)") { check_selection_covers_its_word(2.f); }
TEST_CASE("a tree row's icon is clear of its label, at 1x") { check_icon_clear_of_label(1.f); }
TEST_CASE("a tree row's icon is clear of its label, at 2x") { check_icon_clear_of_label(2.f); }

#ifdef PRISM_TEST_WEBGPU
// The same properties through the WebGPU renderer: text measured by SDL_ttf must land where the
// GPU draws it, at 1x and 2x.
TEST_CASE("webgpu: a selection covers exactly its word, at 1x") { check_selection_covers_its_word(1.f, Via::WebGpu); }
TEST_CASE("webgpu: a selection covers exactly its word, at 2x (HiDPI)") { check_selection_covers_its_word(2.f, Via::WebGpu); }
TEST_CASE("webgpu: a tree row's icon is clear of its label, at 1x") { check_icon_clear_of_label(1.f, Via::WebGpu); }
TEST_CASE("webgpu: a tree row's icon is clear of its label, at 2x") { check_icon_clear_of_label(2.f, Via::WebGpu); }
#endif

namespace {
struct LevelNode {
    Field<Slider<>> level{{.value = 0.75}};
    WidgetId id = 0;
    std::vector<nodes::Port*> ports() { return {}; }
    void view(WidgetTree::ViewBuilder& vb) { id = vb.widget(level).node_ref.id; }
};

struct ZoomedBoard {
    LevelNode node;
    nodes::Graph graph;
    nodes::NodeId a = graph.add(node, {.title = "Level", .family = "math", .at = Point{X{60}, Y{80}}});
    void view(WidgetTree::ViewBuilder& vb) { vb.node_board(graph); }
};

bool near_color(const std::uint8_t* p, Color c) {
    return std::abs(p[0] - c.r) + std::abs(p[1] - c.g) + std::abs(p[2] - c.b) < 12;
}

// The bounding box of the pixels of colour `c`, in logical pixels.
std::optional<Rect> pixels_of(const Frame& f, Color c, int s) {
    int x0 = f.w, y0 = f.h, x1 = -1, y1 = -1;
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x)
            if (near_color(f.at(x, y), c)) {
                x0 = std::min(x0, x); y0 = std::min(y0, y);
                x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
    if (x1 < 0) return std::nullopt;
    const float k = 1.f / static_cast<float>(s);
    return Rect{Point{X{x0 * k}, Y{y0 * k}}, Size{Width{(x1 + 1 - x0) * k}, Height{(y1 + 1 - y0) * k}}};
}

bool contains(Rect outer, Rect inner, float slack) {
    return inner.origin.x.raw() >= outer.origin.x.raw() - slack && inner.origin.y.raw() >= outer.origin.y.raw() - slack
        && inner.origin.x.raw() + inner.extent.w.raw() <= outer.origin.x.raw() + outer.extent.w.raw() + slack
        && inner.origin.y.raw() + inner.extent.h.raw() <= outer.origin.y.raw() + outer.extent.h.raw() + slack;
}
}

TEST_CASE("a slider in a node at zoom 0.5 is drawn where it is hit") {
    constexpr int width = 800, height = 600;
    Rig rig;
    ZoomedBoard model;
    model.graph.set_view({.pan = {}, .zoom = 0.5f});
    WidgetTree tree{model};
    Frame f = rig.render<ZoomedBoard>(tree, 1.f, "zoomed-slider", width, height);
    auto snap = tree.build_snapshot(width, height, ++rig.version);
    const Color accent = tree.theme().accent;
    const auto hit = input::find_widget_rect(*snap, model.node.id);   // where clicks reach the slider
    REQUIRE(hit);
    auto seen = pixels_of(f, accent, std::max(1, f.w / width));
    REQUIRE(seen);
    INFO("slider hit at " << hit->origin.x.raw() << "," << hit->origin.y.raw() << " " << hit->extent.w.raw()
         << "x" << hit->extent.h.raw() << "; accent pixels at " << seen->origin.x.raw() << "," << seen->origin.y.raw()
         << " " << seen->extent.w.raw() << "x" << seen->extent.h.raw());
    CHECK(contains(*hit, *seen, 1.f));
    CHECK(seen->extent.w.raw() == doctest::Approx(Widget<Slider<>>::thumb_len * 0.5f).epsilon(0.2));

    const Point at = seen->origin + Offset{DX{seen->extent.w.raw() / 2}, DY{seen->extent.h.raw() / 2}};
    app::widget_detail::route_event(tree, snap.get(), MouseButton{at, buttons::left, true});
    CHECK(model.node.level.get().value == doctest::Approx(0.75).epsilon(0.1));
    snap = tree.build_snapshot(width, height, ++rig.version);
    app::widget_detail::route_event(tree, snap.get(), MouseMove{at - Offset{DX{40}, DY{0}}, 1});
    CHECK(model.node.level.get().value < 0.6);
}

namespace {
// A board alone in the window with one node at the top, so the bottom of the board is bare grid.
struct GridBoard {
    LevelNode node;
    nodes::Graph graph;
    nodes::NodeId a = graph.add(node, {.title = "Level", .family = "math", .at = Point{X{40}, Y{40}}});
    void view(WidgetTree::ViewBuilder& vb) { vb.node_board(graph); }
};

// Whether logical (x, y) holds a grid dot: a pixel within one of it is nearer the grid's colour
// than the board's background, however much of the backing it covers after antialiasing.
bool on_grid(const Frame& f, Color grid, Color bg, int x, int y, int s) {
    const auto color_gap = [](const std::uint8_t* p, Color c) {
        return std::abs(p[0] - c.r) + std::abs(p[1] - c.g) + std::abs(p[2] - c.b);
    };
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const std::uint8_t* p = f.at(x * s + dx, y * s + dy);
            if (color_gap(p, grid) < color_gap(p, bg)) return true;
        }
    return false;
}
}

TEST_CASE("the board's grid is a dot every 20 units at zoom 1, and every 10 at zoom 0.5") {
    constexpr int width = 480, height = 320;
    Rig rig;
    GridBoard model;
    WidgetTree tree{model};
    const Color grid = tree.theme().node_grid, bg = tree.theme().node_board_bg;

    Frame one = rig.render<GridBoard>(tree, 1.f, "grid-at-1", width, height);
    const int s = std::max(1, one.w / width);
    CHECK(on_grid(one, grid, bg, 240, 280, s));
    CHECK_FALSE(on_grid(one, grid, bg, 250, 280, s));   // the half step, with dots 20 px apart

    model.graph.set_view({.pan = {}, .zoom = 0.5f});
    Frame half = rig.render<GridBoard>(tree, 1.f, "grid-at-0.5", width, height);
    const int sh = std::max(1, half.w / width);
    CHECK(on_grid(half, grid, bg, 240, 280, sh));
    CHECK(on_grid(half, grid, bg, 250, 280, sh));       // dots 10 px apart reach here too
    CHECK_FALSE(on_grid(half, grid, bg, 245, 280, sh));
}

namespace {
struct PictureModel {
    Field<Picture> picture;
    void view(WidgetTree::ViewBuilder& vb) { vb.widget(picture); }
};
} // namespace

// The SDL texture upload has to know how many bytes a pixel is: an rgb565 image read as rgba8888
// reads past every row and draws garbage.
TEST_CASE("an rgb565 image and its rgba8888 conversion render the same pixels") {
    // A checkerboard of two colours the 565 packing keeps apart, at the picture's own size.
    std::vector<std::uint16_t> words;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            words.push_back(static_cast<std::uint16_t>((x < 8) == (y < 8) ? 0xF800 : 0x07E0));
    const auto packed = *Image::from_rgb565(16, 16, std::move(words));
    const auto rgba = converted(packed, PixelFormat::rgba8888);
    REQUIRE(rgba);
    REQUIRE(rgba->format() == PixelFormat::rgba8888);

    Rig rig;
    PictureModel packed_page{.picture = {{.image = packed}}};
    WidgetTree packed_tree{packed_page};
    const Frame packed_frame = rig.render<PictureModel>(packed_tree, 1.f, "rgb565-picture");

    PictureModel rgba_page{.picture = {{.image = rgba}}};
    WidgetTree rgba_tree{rgba_page};
    const Frame rgba_frame = rig.render<PictureModel>(rgba_tree, 1.f, "rgba-picture");

    REQUIRE(packed_frame.w == rgba_frame.w);
    REQUIRE(packed_frame.h == rgba_frame.h);
    int differing = 0;
    for (int y = 0; y < packed_frame.h; ++y)
        for (int x = 0; x < packed_frame.w; ++x) differing += packed_frame.differs(rgba_frame, x, y, 3) ? 1 : 0;
    CHECK(differing == 0);
}

namespace {

// A widget that is nothing but a gradient: the draw command through the real renderer.
struct GradientBox {
    prism::render::Gradient fill;
    bool operator==(const GradientBox&) const = default;
};

} // namespace

namespace prism::ui {
template <>
struct Widget<GradientBox> {
    static constexpr FocusPolicy focus_policy = FocusPolicy::none;

    static void record(DrawList& dl, const Field<GradientBox>& f, WidgetNode& node) {
        // A fixed box: a leaf is measured from what it draws, and its allocation is 0 x 0 then.
        (void)node;
        const Rect box{Point{X{0}, Y{0}}, Size{Width{240}, Height{40}}};
        dl.gradient_rect(box, value(f).fill);
    }
    static void handle_input(Field<GradientBox>&, const InputEvent&, WidgetNode&) {}
};
} // namespace prism::ui

namespace {

struct GradientModel {
    Field<GradientBox> box{GradientBox{prism::render::Gradient{prism::Color::rgba(44, 160, 178),
                                                               prism::Color::rgba(59, 130, 246)}}};
    void view(WidgetTree::ViewBuilder& vb) { vb.widget(box); }
};

bool near(const std::uint8_t* px, prism::Color want, int tol) {
    for (int k = 0; k < 3; ++k) {
        const int got = k == 0 ? px[0] : (k == 1 ? px[1] : px[2]);
        const int expected = k == 0 ? want.r : (k == 1 ? want.g : want.b);
        if (std::abs(got - expected) > tol) return false;
    }
    return true;
}

} // namespace

TEST_CASE("a gradient renders from its start colour to its end colour") {
    Rig rig;
    GradientModel model;
    WidgetTree tree{model};
    const Frame frame = rig.render<GradientModel>(tree, 1.f, "gradient", 240, 40);

    const prism::Color from = prism::Color::rgba(44, 160, 178);
    const prism::Color to = prism::Color::rgba(59, 130, 246);
    const prism::Color middle = prism::Color::rgba(51, 145, 212);
    const int y = frame.h / 2;

    CHECK(near(frame.at(4, y), from, 24));
    CHECK(near(frame.at(frame.w / 2, y), middle, 24));
    CHECK(near(frame.at(frame.w - 5, y), to, 24));
    CHECK(std::abs(frame.at(4, y)[2] - frame.at(frame.w - 5, y)[2]) > 20);   // it really runs

    const auto* top = frame.at(frame.w / 2, y);
    const auto* bottom = frame.at(frame.w / 2, std::min(frame.h - 5, y + 6));
    CHECK(std::abs(top[2] - bottom[2]) <= 3);   // a horizontal gradient does not change down a column
}

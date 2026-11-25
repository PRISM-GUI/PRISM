// The "a struct is a UI" diagrams (doc/screenshots/struct_*.svg): a struct's source on the left,
// the UI PRISM makes from it on the right, and, member by member, a box on the source line, a box
// on its widget and an arrow between them. The widget boxes come from the snapshot's own
// geometry, so a picture cannot promise a mapping PRISM does not make. Animated with CSS only (a
// highlight walks the members), which GitHub and the manual both play; with reduced motion,
// every mapping shows at once.
//
// Usage: showcase_struct_to_ui <this source file> <struct_to_ui.svg> <struct_annotations.svg>
//        <struct_widgets.svg>
// Each code panel is read from this file, between its scene's BEGIN/END markers, so the code
// shown is the code compiled.
#include "showcase_common.hpp"

#if __cpp_impl_reflection
#include <prism/app/widget_tree.hpp>
#include <prism/widgets/inspector.hpp>

#include <fmt/format.h>

#include <regex>
#include <sstream>
#include <vector>

using namespace prism;

// BEGIN plain
enum class Theme { Light, Dark, System };

struct Profile {
    std::string name = "Ada Lovelace";
    int age = 36;
    bool newsletter = true;
    Theme theme = Theme::Dark;
    double volume = 0.7;
};
// END plain

// BEGIN annotations
struct Bench {
    [[=core::label<"Sample rate">]] int rate = 48000;
    [[=core::range<0.0, 30.0, 0.5>, =core::unit<"V">]] double supply = 12.0;
    [[=core::readonly]] bool connected = true;
    [[=core::section<"Output">]] bool enabled = true;
    [[=core::skip]] int build_number = 1187;
};
// END annotations

// BEGIN widgets
struct Player {
    core::Field<ui::Label<>> title{{.value = "Now playing", .heading = true}};
    core::Field<ui::Slider<>> volume{{.value = 0.6, .label = "volume"}};
    core::Field<ui::Checkbox> shuffle{{.checked = true, .label = "shuffle"}};
    core::Field<ui::Button> play{{.text = "Play"}};
};
// END widgets

namespace {

constexpr float kUiWidth = 300.f;
constexpr float kGap = 90.f;            // between the code panel and the UI: room for the arrows
constexpr float kMargin = 24.f;
constexpr float kTitleHeight = 40.f;
constexpr float kLineHeight = 20.f;
constexpr float kCodeFont = 13.f;
constexpr float kCharWidth = 7.9f;      // a monospace glyph at kCodeFont, for the panel's width
constexpr float kSecondsPerField = 1.6f;

struct CodeLine {
    std::string text;
    std::string member;   // the member the line declares and shows, else empty
};

// A member declaration: an optional [[=...]] annotation, a type, a name, then `=` or `{`.
// A member annotated `skip` is shown in the code but has no widget, so it is not a member here.
std::vector<CodeLine> read_scene(const std::string& path, const std::string& scene)
{
    std::ifstream in(path);
    std::vector<CodeLine> lines;
    bool inside = false;
    const std::regex member{R"(^\s+(?:\[\[.*\]\]\s+)?[\w:<>]+\s+(\w+)\s*[={])"};
    for (std::string line; std::getline(in, line);) {
        if (line.find("// END " + scene) != std::string::npos) break;
        if (inside) {
            std::smatch m;
            const bool shown = line.find("core::skip") == std::string::npos;
            lines.push_back({line, shown && std::regex_search(line, m, member) ? m[1].str() : std::string{}});
        }
        if (line.find("// BEGIN " + scene) != std::string::npos) inside = true;
    }
    return lines;
}

std::string escaped(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

std::optional<core::Rect> rect_of(const render::SceneSnapshot& snap, ui::WidgetId id)
{
    for (const auto& [wid, rect] : snap.geometry)
        if (wid == id) return rect;
    return std::nullopt;
}

core::Rect united(const core::Rect& a, const core::Rect& b)
{
    const float x0 = std::min(a.origin.x.raw(), b.origin.x.raw());
    const float y0 = std::min(a.origin.y.raw(), b.origin.y.raw());
    const float x1 = std::max(a.origin.x.raw() + a.extent.w.raw(), b.origin.x.raw() + b.extent.w.raw());
    const float y1 = std::max(a.origin.y.raw() + a.extent.h.raw(), b.origin.y.raw() + b.extent.h.raw());
    return {core::Point{core::X{x0}, core::Y{y0}}, core::Size{core::Width{x1 - x0}, core::Height{y1 - y0}}};
}

// Leaf widgets with a name, in tree order: a model's fields (named after their members) or the
// Inspector's rows (each a caption named `name` and a widget named `value`).
void collect_named(const ui::WidgetNode& node, const render::SceneSnapshot& snap,
                   std::vector<std::pair<std::string, core::Rect>>& out)
{
    if (!node.name.empty() && !node.is_container)
        if (const auto r = rect_of(snap, node.id)) out.emplace_back(node.name, *r);
    for (const auto& child : node.children) collect_named(child, snap, out);
}

// The Inspector: the k-th shown member is the k-th row, boxed whole. A row is a widget named
// `value`, with the caption named `name` just before it when it has one (a slider with a range
// carries its caption itself).
std::vector<core::Rect> inspector_rows(const std::vector<std::pair<std::string, core::Rect>>& named)
{
    std::vector<core::Rect> rows;
    std::optional<core::Rect> caption;
    for (const auto& [name, rect] : named) {
        if (name == "name") {
            caption = rect;
            continue;
        }
        rows.push_back(caption ? united(*caption, rect) : rect);
        caption.reset();
    }
    return rows;
}

// The UI as PRISM renders it, sized to nest in the diagram (on its own it has only a viewBox and
// would stretch to fill its parent).
std::string ui_svg(const render::SceneSnapshot& snap, float height)
{
    std::string svg = render::to_svg(snap, kUiWidth, height);
    if (const auto at = svg.find("<svg"); at != std::string::npos) svg.erase(0, at);
    svg.replace(0, 4, fmt::format(R"svg(<svg width="{}" height="{}")svg", kUiWidth, height));
    return svg;
}

struct Mapping {
    int line;           // index into the code lines
    core::Rect widget;  // in the UI's own coordinates
};

struct Picture {
    std::string title;
    std::string caption;
    std::string alt;
    std::vector<CodeLine> code{};   // filled by render_scene
    std::vector<Mapping> maps{};
    std::string ui{};
    float ui_height = 0.f;
};

std::string diagram(const Picture& p)
{
    std::size_t widest = 0;
    for (const auto& line : p.code) widest = std::max(widest, line.text.size());
    const float code_width = std::max(420.f, float(widest) * kCharWidth + 28.f);
    const float code_top = kMargin + kTitleHeight;
    const float code_height = float(p.code.size()) * kLineHeight + 24.f;
    const float ui_left = kMargin + code_width + kGap;
    const float ui_top = code_top;
    const float width = ui_left + kUiWidth + kMargin;
    const float height = code_top + std::max(code_height, p.ui_height) + kMargin + 28.f;
    const float cycle = kSecondsPerField * float(p.maps.size());
    const float share = 100.f / float(p.maps.size());

    std::ostringstream o;
    o << fmt::format(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {0} {1}" width="{0}" height="{1}" role="img" aria-label="{2}">)svg",
                     width, height, escaped(p.alt))
      << "\n<style>\n"
      << fmt::format("  .hl {{ opacity: 0; animation: walk {}s infinite; }}\n", cycle)
      << fmt::format("  @keyframes walk {{ 0% {{ opacity: 0 }} 3% {{ opacity: 1 }} {:.2f}% {{ opacity: 1 }} {:.2f}% {{ opacity: 0 }} 100% {{ opacity: 0 }} }}\n",
                     share - 3.f, share)
      << "  @media (prefers-reduced-motion: reduce) { .hl { animation: none; opacity: 0.55; } }\n"
         "</style>\n";
    o << fmt::format(R"svg(<rect width="{}" height="{}" fill="#ffffff"/>)svg", width, height) << "\n";
    o << fmt::format(R"svg(<text x="{}" y="{}" font-family="system-ui, sans-serif" font-size="18" font-weight="600" fill="#1f2328">{}</text>)svg",
                     kMargin, kMargin + 18.f, escaped(p.title)) << "\n";
    o << R"svg(<marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="#d1242f"/></marker>)svg"
      << "\n";

    o << fmt::format(R"svg(<rect x="{}" y="{}" width="{}" height="{}" rx="8" fill="#f6f8fa" stroke="#d0d7de"/>)svg",
                     kMargin, code_top, code_width, code_height) << "\n";
    for (std::size_t i = 0; i < p.code.size(); ++i)
        o << fmt::format(R"svg(<text x="{}" y="{}" font-family="ui-monospace, Menlo, Consolas, monospace" font-size="{}" fill="#1f2328" xml:space="preserve">{}</text>)svg",
                         kMargin + 14.f, code_top + 18.f + float(i) * kLineHeight, kCodeFont, escaped(p.code[i].text))
          << "\n";

    o << fmt::format(R"svg(<rect x="{}" y="{}" width="{}" height="{}" rx="8" fill="none" stroke="#d0d7de"/>)svg",
                     ui_left - 1.f, ui_top - 1.f, kUiWidth + 2.f, p.ui_height + 2.f) << "\n";
    o << fmt::format(R"svg(<g transform="translate({},{})">)svg", ui_left, ui_top) << p.ui << "</g>\n";

    for (std::size_t i = 0; i < p.maps.size(); ++i) {
        const auto& m = p.maps[i];
        const float line_y = code_top + 5.f + float(m.line) * kLineHeight;
        const float wx = ui_left + m.widget.origin.x.raw(), wy = ui_top + m.widget.origin.y.raw();
        const float ww = m.widget.extent.w.raw(), wh = m.widget.extent.h.raw();
        const float x0 = kMargin + code_width - 6.f, y0 = line_y + kLineHeight / 2.f;
        const float x1 = wx - 6.f, y1 = wy + wh / 2.f;
        o << fmt::format(R"svg(<g class="hl" style="animation-delay: {:.2f}s">)svg", kSecondsPerField * float(i))
          << fmt::format(R"svg(<rect x="{}" y="{}" width="{}" height="{}" rx="4" fill="#d1242f" fill-opacity="0.08" stroke="#d1242f" stroke-width="2"/>)svg",
                         kMargin + 6.f, line_y, code_width - 12.f, kLineHeight)
          << fmt::format(R"svg(<rect x="{}" y="{}" width="{}" height="{}" rx="4" fill="#d1242f" fill-opacity="0.08" stroke="#d1242f" stroke-width="2"/>)svg",
                         wx - 3.f, wy - 3.f, ww + 6.f, wh + 6.f)
          << fmt::format(R"svg(<path d="M{},{} C{},{} {},{} {},{}" fill="none" stroke="#d1242f" stroke-width="2" marker-end="url(#arrow)"/>)svg",
                         x0, y0, x0 + kGap / 2.f, y0, x1 - kGap / 2.f, y1, x1, y1)
          << "</g>\n";
    }

    o << fmt::format(R"svg(<text x="{}" y="{}" font-family="system-ui, sans-serif" font-size="13" fill="#57606a">{}</text>)svg",
                     kMargin, height - kMargin + 4.f, escaped(p.caption)) << "\n";
    o << "</svg>\n";
    return o.str();
}

// The height the UI's widgets take: its lowest widget's bottom, plus a margin.
float content_height(const render::SceneSnapshot& snap)
{
    float bottom = 0.f;
    for (const auto& [id, r] : snap.geometry) bottom = std::max(bottom, r.origin.y.raw() + r.extent.h.raw());
    return bottom + 12.f;
}

// One scene: a model, the code lines that declare its shown members, and how to find each
// member's widget box. Fails loudly when the two do not line up, rather than draw a wrong arrow.
// The UI panel takes the height its widgets need, so a theme with more spacing cannot cut one off.
template <class Model>
bool render_scene(Model& model, const std::string& source, const std::string& scene,
                  bool by_row, Picture picture, const std::string& out_path)
{
    picture.code = read_scene(source, scene);
    app::WidgetTree tree{model};
    const auto fonts = backends::headless_text_measurer();
    tree.set_text_measurer(fonts.get());
    tree.set_theme(ui::light_theme());   // beside the light code panel
    const auto probe = tree.build_snapshot(kUiWidth, 4000.f, 1);
    if (!probe) {
        std::cerr << scene << ": no snapshot\n";
        return false;
    }
    const float ui_height = content_height(*probe);
    tree.mark_dirty_by_id(tree.root().id);
    const auto snap = tree.build_snapshot(kUiWidth, ui_height, 2);
    if (!snap) {
        std::cerr << scene << ": no snapshot\n";
        return false;
    }
    std::vector<std::pair<std::string, core::Rect>> named;
    collect_named(tree.root(), *snap, named);
    const auto rows = inspector_rows(named);

    std::size_t row = 0;
    for (int i = 0; i < int(picture.code.size()); ++i) {
        const auto& member = picture.code[i].member;
        if (member.empty()) continue;
        if (by_row) {
            if (row >= rows.size()) {
                std::cerr << scene << ": fewer rows than members (" << rows.size() << "); named:";
                for (const auto& [name, rect] : named)
                    std::cerr << " " << name << "@" << rect.origin.y.raw();
                std::cerr << "\n";
                return false;
            }
            picture.maps.push_back({i, rows[row++]});
            continue;
        }
        const auto it = std::ranges::find_if(named, [&](const auto& n) { return n.first == member; });
        if (it == named.end()) {
            std::cerr << scene << ": no widget named " << member << "\n";
            return false;
        }
        picture.maps.push_back({i, it->second});
    }
    picture.ui = ui_svg(*snap, ui_height);
    picture.ui_height = ui_height;
    std::ofstream(out_path) << diagram(picture);
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <source.cpp> <plain.svg> <annotations.svg> <widgets.svg>\n";
        return 1;
    }
    const std::string source = argv[1];

    core::Shared<Profile> profile{Profile{}};
    inspector::Inspector<Profile> profile_view{profile};
    core::Shared<Bench> bench{Bench{}};
    inspector::Inspector<Bench> bench_view{bench};
    Player player;

    const bool ok =
        render_scene(profile_view, source, "plain", true,
                     {.title = "A struct is a UI: each field becomes a widget",
                      .caption = "No view code: PRISM reads the members through C++26 reflection. Strings and numbers show as text.",
                      .alt = "A plain C++ struct and the UI PRISM makes from it: the string, the int and the double show their values as text, the bool becomes a checkbox, the enum a dropdown."},
                     argv[2])
        && render_scene(bench_view, source, "annotations", true,
                        {.title = "Annotations change how a member is shown",
                         .caption = "label renames the caption, range and unit make a slider, readonly makes a checkbox not editable, section adds a header, skip hides it.",
                         .alt = "A struct with annotations: label renames a caption, range and unit turn a double into a slider, readonly makes a checkbox not editable, section adds a header, skip hides a member."},
                        argv[3])
        && render_scene(player, source, "widgets", false,
                        {.title = "Or pick the widget yourself: a Field holds it",
                         .caption = "A Field of a widget type is that widget: a label, a slider, a checkbox, a button.",
                         .alt = "A model of Fields holding widget types: a heading label, a slider, a checkbox and a button, each pointing at the widget it becomes."},
                        argv[4]);
    return ok ? 0 : 1;
}
#else
// Reflection-only: placeholders keep build_by_default working on other toolchains.
int main(int argc, char* argv[])
{
    for (int i = 2; i < argc; ++i) std::ofstream(argv[i]) << "<svg xmlns=\"http://www.w3.org/2000/svg\"/>\n";
    return argc < 5 ? 1 : 0;
}
#endif

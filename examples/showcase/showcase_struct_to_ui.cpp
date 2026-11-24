// The "a struct is a UI" diagram (doc/diagrams/struct-to-ui.svg): the struct's source on the left,
// the UI PRISM makes from it on the right, and, field by field, a box on the source line, a box
// on its widget and an arrow between them. The widget boxes come from the snapshot's own
// geometry, so the picture cannot promise a mapping PRISM does not make. Animated with CSS only
// (a highlight walks the fields), which GitHub and the manual both play; with reduced motion,
// every mapping shows at once.
//
// Usage: showcase_struct_to_ui <output.svg> <this source file>
// The source panel is read from this file, between the BEGIN/END markers, so the code shown is
// the code compiled.
#include "showcase_common.hpp"

#if __cpp_impl_reflection
#include <prism/app/widget_tree.hpp>
#include <prism/widgets/inspector.hpp>

#include <fmt/format.h>

#include <regex>
#include <sstream>
#include <vector>

using namespace prism;

// BEGIN struct
enum class Theme { Light, Dark, System };

struct Profile {
    std::string name = "Ada Lovelace";
    int age = 36;
    bool newsletter = true;
    Theme theme = Theme::Dark;
    double volume = 0.7;
};
// END struct

namespace {

constexpr float kUiWidth = 300.f;
constexpr float kUiHeight = 285.f;
constexpr float kCodeWidth = 420.f;
constexpr float kGap = 90.f;            // between the code panel and the UI: room for the arrows
constexpr float kMargin = 24.f;
constexpr float kTitleHeight = 40.f;
constexpr float kLineHeight = 20.f;
constexpr float kCodeFont = 13.f;
constexpr float kSecondsPerField = 1.6f;

struct CodeLine {
    std::string text;
    std::string field;   // the member a Field line declares, else empty
};

std::vector<CodeLine> read_struct(const std::string& path)
{
    std::ifstream in(path);
    std::vector<CodeLine> lines;
    bool inside = false;
    const std::regex member{R"(^\s+[\w:<>]+\s+(\w+)\s*=)"};
    for (std::string line; std::getline(in, line);) {
        if (line.find("// END struct") != std::string::npos) break;
        if (inside) {
            std::smatch m;
            lines.push_back({line, std::regex_search(line, m, member) ? m[1].str() : std::string{}});
        }
        if (line.find("// BEGIN struct") != std::string::npos) inside = true;
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
        default: out += c;
        }
    }
    return out;
}

void collect_named(const ui::WidgetNode& node, std::vector<std::pair<std::string, ui::WidgetId>>& out)
{
    if (!node.name.empty() && !node.is_container) out.emplace_back(node.name, node.id);
    for (const auto& child : node.children) collect_named(child, out);
}

std::optional<core::Rect> rect_of(const render::SceneSnapshot& snap, ui::WidgetId id)
{
    for (const auto& [wid, rect] : snap.geometry)
        if (wid == id) return rect;
    return std::nullopt;
}

// The UI as PRISM renders it, without its own <?xml?> prolog, to nest in the diagram.
std::string ui_svg(const render::SceneSnapshot& snap)
{
    std::string svg = render::to_svg(snap, kUiWidth, kUiHeight);
    if (const auto at = svg.find("<svg"); at != std::string::npos) svg.erase(0, at);
    // Its own viewBox and no size: nested as is, it would stretch to fill the diagram.
    svg.replace(0, 4, fmt::format(R"svg(<svg width="{}" height="{}")svg", kUiWidth, kUiHeight));
    return svg;
}

struct Mapping {
    int line;           // index into the code lines
    core::Rect widget;  // in the UI's own coordinates
};

std::string diagram(const std::vector<CodeLine>& code, const std::vector<Mapping>& maps,
                    const std::string& ui)
{
    const float code_top = kMargin + kTitleHeight;
    const float code_height = float(code.size()) * kLineHeight + 24.f;
    const float ui_left = kMargin + kCodeWidth + kGap;
    const float ui_top = code_top;
    const float width = ui_left + kUiWidth + kMargin;
    const float height = code_top + std::max(code_height, kUiHeight) + kMargin + 28.f;
    const float cycle = kSecondsPerField * float(maps.size());

    std::ostringstream o;
    o << fmt::format(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {0} {1}" width="{0}" height="{1}" role="img" aria-label="Each field of a C++ struct becomes a widget: a string a text field, an int a number field, a bool a checkbox, an enum a dropdown, a double a number field.">)svg",
                     width, height);
    o << "\n<style>\n"
         "  .hl { opacity: 0; animation: walk " << cycle << "s infinite; }\n"
         "  @keyframes walk { 0% { opacity: 0 } 3% { opacity: 1 } "
      << fmt::format("{:.2f}", 100.f / float(maps.size()) - 3.f) << "% { opacity: 1 } "
      << fmt::format("{:.2f}", 100.f / float(maps.size())) << "% { opacity: 0 } 100% { opacity: 0 } }\n"
         "  @media (prefers-reduced-motion: reduce) { .hl { animation: none; opacity: 0.55; } }\n"
         "</style>\n";
    o << fmt::format(R"svg(<rect width="{}" height="{}" fill="#ffffff"/>)svg", width, height) << "\n";
    o << fmt::format(R"svg(<text x="{}" y="{}" font-family="system-ui, sans-serif" font-size="18" font-weight="600" fill="#1f2328">A struct is a UI: each field becomes a widget</text>)svg",
                     kMargin, kMargin + 18.f) << "\n";
    o << fmt::format(R"svg(<marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="#d1242f"/></marker>)svg") << "\n";

    // The source panel.
    o << fmt::format(R"svg(<rect x="{}" y="{}" width="{}" height="{}" rx="8" fill="#f6f8fa" stroke="#d0d7de"/>)svg",
                     kMargin, code_top, kCodeWidth, code_height) << "\n";
    for (std::size_t i = 0; i < code.size(); ++i)
        o << fmt::format(R"svg(<text x="{}" y="{}" font-family="ui-monospace, Menlo, Consolas, monospace" font-size="{}" fill="#1f2328" xml:space="preserve">{}</text>)svg",
                         kMargin + 14.f, code_top + 18.f + float(i) * kLineHeight, kCodeFont, escaped(code[i].text))
          << "\n";

    // The UI, as PRISM renders it.
    o << fmt::format(R"svg(<rect x="{}" y="{}" width="{}" height="{}" rx="8" fill="none" stroke="#d0d7de"/>)svg",
                     ui_left - 1.f, ui_top - 1.f, kUiWidth + 2.f, kUiHeight + 2.f) << "\n";
    o << fmt::format(R"svg(<g transform="translate({},{})">)svg", ui_left, ui_top) << ui << "</g>\n";

    // One highlight per field: its line, its widget, the arrow between them.
    for (std::size_t i = 0; i < maps.size(); ++i) {
        const auto& m = maps[i];
        const float line_y = code_top + 5.f + float(m.line) * kLineHeight;
        const float wx = ui_left + m.widget.origin.x.raw(), wy = ui_top + m.widget.origin.y.raw();
        const float ww = m.widget.extent.w.raw(), wh = m.widget.extent.h.raw();
        o << fmt::format(R"svg(<g class="hl" style="animation-delay: {:.2f}s">)svg", kSecondsPerField * float(i));
        o << fmt::format(R"svg(<rect x="{}" y="{}" width="{}" height="{}" rx="4" fill="#d1242f" fill-opacity="0.08" stroke="#d1242f" stroke-width="2"/>)svg",
                         kMargin + 6.f, line_y, kCodeWidth - 12.f, kLineHeight);
        o << fmt::format(R"svg(<rect x="{}" y="{}" width="{}" height="{}" rx="4" fill="#d1242f" fill-opacity="0.08" stroke="#d1242f" stroke-width="2"/>)svg",
                         wx - 3.f, wy - 3.f, ww + 6.f, wh + 6.f);
        const float x0 = kMargin + kCodeWidth - 6.f, y0 = line_y + kLineHeight / 2.f;
        const float x1 = wx - 6.f, y1 = wy + wh / 2.f;
        o << fmt::format(R"svg(<path d="M{},{} C{},{} {},{} {},{}" fill="none" stroke="#d1242f" stroke-width="2" marker-end="url(#arrow)"/>)svg",
                         x0, y0, x0 + kGap / 2.f, y0, x1 - kGap / 2.f, y1, x1, y1);
        o << "</g>\n";
    }

    o << fmt::format(R"svg(<text x="{}" y="{}" font-family="system-ui, sans-serif" font-size="13" fill="#57606a">No view code: PRISM reads the members through C++26 reflection and picks a widget for each type.</text>)svg",
                     kMargin, height - kMargin + 4.f) << "\n";
    o << "</svg>\n";
    return o.str();
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <output.svg> <source.cpp>\n";
        return 1;
    }
    const auto code = read_struct(argv[2]);
    core::Shared<Profile> profile{Profile{}};
    inspector::Inspector<Profile> model{profile};
    app::WidgetTree tree{model};
    const auto fonts = backends::headless_text_measurer();
    tree.set_text_measurer(fonts.get());
    tree.set_theme(ui::light_theme());   // beside the light code panel
    const auto snap = tree.build_snapshot(kUiWidth, kUiHeight, 1);
    if (!snap) {
        std::cerr << "no snapshot\n";
        return 1;
    }
    std::vector<std::pair<std::string, ui::WidgetId>> named;
    collect_named(tree.root(), named);

    // The Inspector shows one row per member, in order, each a caption (`name`) and its widget
    // (`value`). The k-th member line maps to the k-th row, boxed whole: caption and widget.
    std::vector<core::Rect> captions, widgets;
    for (const auto& [name, id] : named) {
        const auto rect = rect_of(*snap, id);
        if (!rect) continue;
        (name == "name" ? captions : widgets).push_back(*rect);
    }
    std::vector<Mapping> maps;
    std::size_t row = 0;
    for (int i = 0; i < int(code.size()); ++i) {
        if (code[i].field.empty()) continue;
        if (row >= captions.size() || row >= widgets.size()) {
            std::cerr << "fewer inspector rows than members (" << captions.size() << ")\n";
            return 1;
        }
        const auto& a = captions[row];
        const auto& b = widgets[row];
        const float x0 = std::min(a.origin.x.raw(), b.origin.x.raw());
        const float y0 = std::min(a.origin.y.raw(), b.origin.y.raw());
        const float x1 = std::max(a.origin.x.raw() + a.extent.w.raw(), b.origin.x.raw() + b.extent.w.raw());
        const float y1 = std::max(a.origin.y.raw() + a.extent.h.raw(), b.origin.y.raw() + b.extent.h.raw());
        maps.push_back({i, core::Rect{core::Point{core::X{x0}, core::Y{y0}},
                                      core::Size{core::Width{x1 - x0}, core::Height{y1 - y0}}}});
        ++row;
    }
    std::ofstream(argv[1]) << diagram(code, maps, ui_svg(*snap));
    return 0;
}
#else
// Reflection-only: a placeholder keeps build_by_default working on other toolchains.
int main(int argc, char* argv[])
{
    if (argc < 2) return 1;
    std::ofstream(argv[1]) << "<svg xmlns=\"http://www.w3.org/2000/svg\"/>\n";
    return 0;
}
#endif

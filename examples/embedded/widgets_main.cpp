// widgets, desktop: the widget demo a board runs (widgets_app.hpp) as an app with a plain main().
// A board compiles this same file; its firmware supplies the port before main() (platform/run.hpp).
#include <embedded/widgets_app.hpp>
#include <prism/app/run.hpp>

int main()
{
    prism_examples::WidgetsModel model;
    return prism::app::run(model, {.title = "Widgets"});
}

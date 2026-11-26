// The audio scope as an app: the same main() a desktop build runs. Nothing here knows it may be a
// board; the port it links supplies the display, the input events, and the capture device the
// scope records from (audio/backend.hpp). Without one, the scope shows its sweep.

#include <embedded/audio_scope.hpp>

#include <prism/app/run.hpp>

int main()
{
    prism_examples::Scope scope;
    return prism::app::run(scope.model, {.title = "Scope"},
                           [&scope](prism::app::AppContext &ctx) { scope.attach(ctx); });
}

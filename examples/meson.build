# Under emscripten the examples build as web pages (name.html + .js/.wasm/.data), and the
# README SVG captures -- a desktop run of each example -- are skipped.
is_wasm = host_machine.system() == 'emscripten'
example_suffix = is_wasm ? 'html' : []
example_shell = is_wasm ? files('web/shell.html') : []
example_link_args = is_wasm ? ['--shell-file', meson.current_source_dir() / 'web' / 'shell.html'] : []

subdir('model_notes')
subdir('widget_gallery')
subdir('perf_lab')
subdir('model_system_monitor')   # /proc on Linux, a simulated machine elsewhere
subdir('model_lab_bench')
subdir('model_synth_rack')       # real audio through SDL3
subdir('model_data_studio')      # SQLite, from WrapDB
subdir('model_task_runner')
subdir('crdt_pair')              # two windows, two Doc replicas, one model
subdir('crdt_hub')               # one window, its Doc served to the browser peer
subdir('tour')                   # five short programs, one idea each
if not is_wasm
  subdir('showcase')             # headless SVG capture tools, not apps
endif

# The board apps as the desktop apps they already are: the same models and views, one main() each
# that calls app::run. Nothing in them knows it may be a board; a port supplies the rest.
executable('widgets', 'embedded/widgets_main.cpp',
  include_directories : include_directories('.'),
  dependencies : [prism_dep],
  name_suffix : example_suffix,
  link_args : example_link_args,
  link_depends : example_shell,
)

executable('scope', 'embedded/scope_main.cpp',
  include_directories : include_directories('.'),
  dependencies : [prism_dep, prism_embedded_apps_dep],
  name_suffix : example_suffix,
  link_args : example_link_args,
  link_depends : example_shell,
)

executable('tone', 'embedded/tone_main.cpp',
  include_directories : include_directories('.'),
  dependencies : [prism_dep],
  name_suffix : example_suffix,
  link_args : example_link_args,
  link_depends : example_shell,
)

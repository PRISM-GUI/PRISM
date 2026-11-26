# crdt_twins: two Mixers in one window, each its own Doc, synced through the y-protocols messages
# (crdt::SyncSession). examples/ is on the include path, so the model include is the same line a
# board compiles. One source file serves the desktop window and the browser page.
if prism_crdt_dep.found()
  executable('crdt_twins', 'main.cpp',
    include_directories : include_directories('..'),
    dependencies : [prism_dep, prism_crdt_dep],
    name_suffix : example_suffix,
    link_args : example_link_args,
    link_depends : example_shell,
  )
endif

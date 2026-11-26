# crdt_hub: one window whose Doc a hub serves. crdt::Hub lands in prism-crdt only when prism-net is
# there, so that is the condition. examples/ is on the include path, so the model include is the
# same line a board compiles.
if prism_crdt_dep.found() and prism_net_dep.found()
  executable('crdt_hub', 'main.cpp',
    include_directories : include_directories('..'),
    dependencies : [prism_dep, prism_crdt_dep],
    name_suffix : example_suffix,
    link_args : example_link_args,
    link_depends : example_shell,
  )
endif

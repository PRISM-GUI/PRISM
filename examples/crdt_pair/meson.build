# Two windows of one model, each its own Doc replica (doc/integrations/crdt.md). Built only when
# the crdt flavor is: it needs cargo, and a target that can run it.
if prism_crdt_dep.found()
  executable('crdt_pair', 'main.cpp',
    dependencies : [prism_dep, prism_crdt_dep],
    name_suffix : example_suffix,
    link_args : example_link_args,
    link_depends : example_shell,
  )
endif

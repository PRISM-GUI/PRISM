# One model, three ways of meeting it: here the two windows wired in-process (desktop) and, under
# emscripten, the browser page itself. Neither needs prism-net: the hub the page joins lives in
# examples/crdt_hub.
if prism_crdt_dep.found()
  crdt_pair_sources = host_machine.system() == 'emscripten' ? 'web_main.cpp' : 'main.cpp'
  executable('crdt_pair', crdt_pair_sources,
    dependencies : [prism_dep, prism_crdt_dep],
    name_suffix : example_suffix,
    link_args : example_link_args,
    link_depends : example_shell,
  )
endif

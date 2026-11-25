# One model, and however many peers: the desktop pair (two windows wired in-process, or --hub one
# window serving the browser page) and, under emscripten, the page itself. On a desktop the --hub
# mode is what needs prism-net, so without it there is no example to build; the page has its own
# client and no server.
crdt_pair_peer = host_machine.system() == 'emscripten' or prism_net_dep.found()
if prism_crdt_dep.found() and crdt_pair_peer
  crdt_pair_sources = host_machine.system() == 'emscripten' ? 'web_main.cpp' : 'main.cpp'
  executable('crdt_pair', crdt_pair_sources,
    dependencies : [prism_dep, prism_crdt_dep],
    name_suffix : example_suffix,
    link_args : example_link_args,
    link_depends : example_shell,
  )
endif

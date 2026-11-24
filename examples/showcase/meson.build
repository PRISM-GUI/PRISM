# A showcase SVG is re-made when its program changes and also when PRISM's libraries do: a change
# to a widget's drawing lands in libprism, which the program is not relinked against.
showcase_libs = [prism_lib]
if is_variable('prism_software_backend_lib')
  showcase_libs += prism_software_backend_lib
endif

showcases = {
  'counter':  'showcase_counter.cpp',
  'slider':   'showcase_slider.cpp',
  'layout':   'showcase_layout.cpp',
  'canvas':   'showcase_canvas.cpp',
  'plot':     'showcase_plot.cpp',
  'inspector': 'showcase_inspector.cpp',
  'tree':     'showcase_tree.cpp',
  'table':    'showcase_table.cpp',
  'tabs':     'showcase_tabs.cpp',
  'dialog':   'showcase_dialog.cpp',
  'form':     'showcase_form.cpp',
  'first_app': 'showcase_first_app.cpp',
  'derived': 'showcase_derived.cpp',
  'views': 'showcase_views.cpp',
  'buttons': 'showcase_buttons.cpp',
  'text_inputs': 'showcase_text_inputs.cpp',
  'pickers': 'showcase_pickers.cpp',
  'labels': 'showcase_labels.cpp',
  'code': 'showcase_code.cpp',
  'rich_text': 'showcase_rich_text.cpp',
  'menus': 'showcase_menus.cpp',
  'containers': 'showcase_containers.cpp',
  'split': 'showcase_split.cpp',
  'list': 'showcase_list.cpp',
  'dock': 'showcase_dock.cpp',
  'list_tabs': 'showcase_list_tabs.cpp',
  'popover': 'showcase_popover.cpp',
  'enabled': 'showcase_enabled.cpp',
  'theme_light': 'showcase_theme_light.cpp',
  'theme_dark': 'showcase_theme_dark.cpp',
  'node_board': 'showcase_node_board.cpp',
}

foreach name, src : showcases
  exe = executable('showcase_' + name, src,
    dependencies : [prism_dep],
  )

  custom_target('svg_' + name,
    output : name + '.svg',
    command : [exe, '@OUTPUT@'],
    depends : [exe] + showcase_libs,
    build_by_default : true,
  )
endforeach

# The "a struct is a UI" diagram: it reads its own source for the code panel.
struct_to_ui_exe = executable('showcase_struct_to_ui', 'showcase_struct_to_ui.cpp',
  dependencies : [prism_dep],
)
custom_target('svg_struct_to_ui',
  output : ['struct_to_ui.svg', 'struct_annotations.svg', 'struct_widgets.svg'],
  command : [struct_to_ui_exe, files('showcase_struct_to_ui.cpp'), '@OUTPUT0@', '@OUTPUT1@', '@OUTPUT2@'],
  depends : [struct_to_ui_exe] + showcase_libs,
  build_by_default : true,
)

# The modern look (spike): one program, both palettes.
modern_exe = executable('showcase_modern', 'showcase_modern.cpp', dependencies : [prism_dep])
foreach variant : ['dark', 'light']
  custom_target('svg_modern_' + variant,
    output : 'modern_' + variant + '.svg',
    command : [modern_exe, '@OUTPUT@', variant],
    depends : [modern_exe] + showcase_libs,
    build_by_default : true,
  )
endforeach

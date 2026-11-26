# The shared board apps -- the widget demo (widgets_app.hpp) and the audio scope (audio_scope,
# spectrum, sample_source, synthetic_source), each with the plain main() of any PRISM app
# (widgets_main.cpp, scope_main.cpp) -- moved from prism-stm32, so every board
# repository builds the same sources (doc/design/esp32-port.md, "Shared apps move into PRISM").
# prism_embedded_apps_dep carries the include directory examples/ itself (the apps include each
# other as <embedded/audio_scope.hpp>), PRISM's core and CMSIS-DSP, whose arm_rfft_fast_f32 is
# the scope's FFT.
#
# Entered outside the sdl block in meson.build, so it runs on every target, bare metal ('none')
# included.
cmsis_dsp_dep = subproject('cmsis-dsp').get_variable('cmsis_dsp_dep')

# The audio registry (audio::Input, audio::Output and their devices) lives with SDL's backend
# where there is one, in libprism where there is not (src/meson.build): the scope records through
# audio::Input either way, so a desktop build needs that library and a bare-metal one does not.
prism_embedded_apps_deps = [prism_core_dep, cmsis_dsp_dep]
if sdl_enabled
  prism_embedded_apps_deps += prism_software_backend_dep
endif

# The apps' .cpp files, as a static library (a board links it into its firmware). Dependencies, as
# prism-stm32's host scope_lib had them: PRISM's core (the widget demo and the scope build their
# views on it) and CMSIS-DSP (spectrum.cpp's FFT). The scope's source is an audio::Input, so it
# needs the audio registry above. Nothing depends on SDL at the source level, so this builds on a
# bare-metal ('none') target too.
prism_embedded_apps_inc = include_directories('..')
prism_embedded_apps_lib = static_library('prism_embedded_apps',
  files('spectrum.cpp', 'synthetic_source.cpp', 'audio_scope.cpp'),
  include_directories : prism_embedded_apps_inc,
  dependencies : prism_embedded_apps_deps,
)

prism_embedded_apps_dep = declare_dependency(
  link_with : prism_embedded_apps_lib,
  include_directories : prism_embedded_apps_inc,
  dependencies : prism_embedded_apps_deps,
)

# The apps' main()s, which a port compiles unchanged: a subproject's files may only be reached
# through a variable it exports (`prism.get_variable('prism_embedded_app_mains')['scope']`).
prism_embedded_app_mains = {
  'widgets' : files('widgets_main.cpp'),
  'scope' : files('scope_main.cpp'),
  'tone' : files('tone_main.cpp'),
}

# Yrs' C ABI (yffi, from crates.io), built by cargo from the crate's own Cargo.lock. Not on
# WrapDB: bump by hand (version, hash) together with libyrs.h, which the crate does not ship.
project('yffi', 'c', version : '0.28.2', meson_version : '>= 1.5')

cargo = find_program('cargo')

# A browser build is a cross build for cargo too: wasm with shared memory needs atomics, and its
# std has to be rebuilt (build.py runs -Zbuild-std), which only a nightly toolchain with rust-src
# can do. Meson has no toolchain knob for cargo, so the cross file carries it: rust_target names
# the Rust target (empty: the native or browser default), rust_toolchain the rustup toolchain that
# has it (empty: the default one).
rust_target = meson.get_external_property('rust_target', '')
rust_toolchain = meson.get_external_property('rust_toolchain', '')

wasm_rustflags = '-C target-feature=+atomics,+bulk-memory,+mutable-globals'
yrs_args = []
yrs_env = {'CARGO' : cargo.full_path()}
if rust_target != ''
  yrs_args = ['--target', rust_target]
  # The atomics RUSTFLAGS are the browser's alone: a board's std needs -Zbuild-std and nothing else.
  if host_machine.system() == 'emscripten'
    yrs_args += ['--rustflags', wasm_rustflags]
  endif
  if rust_toolchain != ''
    yrs_env += {'RUSTUP_TOOLCHAIN' : rust_toolchain}
  endif
elif host_machine.system() == 'emscripten'
  yrs_args = ['--target', 'wasm32-unknown-emscripten', '--rustflags', wasm_rustflags]
  yrs_env = {'CARGO' : cargo.full_path(), 'RUSTUP_TOOLCHAIN' : 'nightly'}
endif

# Always stale: cargo, not Meson, knows which crates changed.
yrs_lib = custom_target('yrs',
  output : 'libyrs.a',
  command : [find_program('python3'), files('build.py'), meson.current_source_dir() / 'Cargo.toml',
             '@PRIVATE_DIR@', '@OUTPUT@'] + yrs_args,
  env : yrs_env,
  build_always_stale : true,
  install : true,
  install_dir : get_option('libdir'),
)

# A static Rust archive does not carry the platform's threads, libdl and libm that its std needs.
cc = meson.get_compiler('c')
yffi_dep = declare_dependency(
  link_with : yrs_lib,
  # System: libyrs.h is generated C (empty structs, C-only idioms) that clang warns about in C++.
  include_directories : include_directories('.', is_system : true),
  dependencies : [dependency('threads'), cc.find_library('dl', required : false),
                  cc.find_library('m', required : false)],
)
meson.override_dependency('yffi', yffi_dep)

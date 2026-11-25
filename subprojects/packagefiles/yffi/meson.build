# Yrs' C ABI (yffi, from crates.io), built by cargo from the crate's own Cargo.lock. Not on
# WrapDB: bump by hand (version, hash) together with libyrs.h, which the crate does not ship.
project('yffi', 'c', version : '0.28.2', meson_version : '>= 1.5')

cargo = find_program('cargo')
# Always stale: cargo, not Meson, knows which crates changed.
yrs_lib = custom_target('yrs',
  output : 'libyrs.a',
  command : [find_program('python3'), files('build.py'), meson.current_source_dir() / 'Cargo.toml',
             '@PRIVATE_DIR@', '@OUTPUT@'],
  env : {'CARGO' : cargo.full_path()},
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

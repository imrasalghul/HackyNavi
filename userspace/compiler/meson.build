executable(
  'hackynavi-aco', 'compiler.c',
  include_directories: [inc_include, inc_src, inc_amd, inc_amd_common, inc_compiler, inc_spirv],
  dependencies: [idep_aco, idep_vtn, idep_nir, idep_mesautil, idep_amd_generated_headers, dep_thread, dep_m],
  link_with: [libamd_common, libamdgpu_addrlib],
  install: false,
)

shared_library(
  'hackynavi_gfx11', 'compiler.c',
  include_directories: [inc_include, inc_src, inc_amd, inc_amd_common, inc_compiler, inc_spirv],
  dependencies: [idep_aco, idep_vtn, idep_nir, idep_mesautil, idep_amd_generated_headers, dep_thread, dep_m],
  link_with: [libamd_common, libamdgpu_addrlib],
  c_args: ['-DHN_GFX11_SHARED'],
  install: false,
)

# Vulkan wrapper: a layered Vulkan driver on top of the system / custom
# (libadrenotools) Android Vulkan driver, with the common Mesa WSI for X11.

wrapper_icd = custom_target(
  'wrapper_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'wrapper_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.3', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', vulkan_icd_lib_path,
    '--icd-filename', libname_prefix + 'vulkan_wrapper.' + libname_suffix,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install_tag : 'runtime',
  install : true,
)

wrapper_entrypoints = custom_target(
  'wrapper_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['wrapper_entrypoints.h', 'wrapper_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'wrapper',
    '--beta', with_vulkan_beta.to_string(),
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

wrapper_trampolines = custom_target(
  'wrapper_trampolines',
  input : [files('vk_wrapper_trampolines_gen.py'), vk_api_xml],
  output : ['wrapper_trampolines.c', 'wrapper_trampolines.h'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@',
    '--out-c', '@OUTPUT0@', '--out-h', '@OUTPUT1@',
    '--beta', with_vulkan_beta.to_string(),
  ],
  depend_files : vk_dispatch_trampolines_gen_depend_files,
)

wrapper_features = custom_target(
  'wrapper_features',
  input : [files('vk_wrapper_features_gen.py'), vk_api_xml],
  output : ['wrapper_features.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@',
    '--out-c', '@OUTPUT0@',
    '--beta', with_vulkan_beta.to_string(),
  ],
  depend_files : vk_physical_device_features_gen_depend_files,
)

adrenotools_subproj = subproject('libadrenotools')
adrenotools_dep = declare_dependency(
  include_directories : adrenotools_subproj.get_variable('adrenotools_inc'),
  link_with : adrenotools_subproj.get_variable('adrenotools_lib'),
)

wrapper_files = files(
  'graphicsenv_hook.cpp',
  'spirv_patcher.cpp',
  'wrapper_bcn.c',
  'wrapper_device.c',
  'wrapper_device_memory.c',
  'wrapper_instance.c',
  'wrapper_log.c',
  'wrapper_physical_device.c',
)

libvulkan_wrapper = shared_library(
  'vulkan_wrapper',
  [wrapper_entrypoints, wrapper_trampolines, wrapper_features, wrapper_files],
  include_directories : [inc_include, inc_src],
  dependencies : [
    idep_vulkan_lite_runtime,
    idep_vulkan_util,
    idep_vulkan_wsi,
    idep_mesautil,
    adrenotools_dep,
    dep_android,
    dep_dl,
    vulkan_wsi_deps,
  ],
  gnu_symbol_visibility : 'hidden',
  link_args : [ld_args_build_id, ld_args_bsymbolic, ld_args_gc_sections],
  install : true,
)

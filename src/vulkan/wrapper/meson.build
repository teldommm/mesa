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

# WFG frame generation (wrapper_wfg.c, LICENSE.wfg): its shaders, each compiled into a
# header wrapper_wfg.c includes.  [header variable, source, defines]
wfg_shaders = [
  ['dis_luma_r16_comp', 'dis_luma_r16.comp', []],
  ['dis_luma_r32_comp', 'dis_luma_r32.comp', []],
  ['dis_gradient_comp', 'dis_gradient.comp', []],
  ['dis_inverse_search_comp', 'dis_inverse_search.comp', []],
  ['dis_propagate_comp', 'dis_propagate.comp', []],
  ['dis_densify_comp', 'dis_densify.comp', []],
  ['dis_fullscreen_vert', 'dis_fullscreen.vert', []],
  ['dis_interpolate_frag', 'dis_interpolate.frag', []],
  ['dis_hist_comp', 'dis_hist.comp', []],
  ['dis_hist_r16_comp', 'dis_hist.comp', ['-DDIS_HIST_R16']],
  ['dis_side_comp', 'dis_side.comp', []],
  ['dis_flow_pack_comp', 'dis_flow_pack.comp', []],
  ['dis_splat_comp', 'dis_splat.comp', []],
  ['dis_fill_comp', 'dis_fill.comp', []],
  ['dis_vr_prep_comp', 'dis_vr_prep.comp', []],
  ['dis_vr_d1_comp', 'dis_vr_d1.comp', []],
  ['dis_vr_d2_comp', 'dis_vr_d2.comp', []],
  ['dis_vr_w_comp', 'dis_vr_w.comp', []],
  ['dis_vr_coef_comp', 'dis_vr_coef.comp', []],
  ['dis_vr_sor_comp', 'dis_vr_sor.comp', []],
  ['dis_vr_add_comp', 'dis_vr_add.comp', []],
  ['dis_fb_poly_comp', 'dis_fb_poly.comp', []],
  ['dis_fb_hsum_comp', 'dis_fb_hsum.comp', []],
  ['dis_fb_vsolve_comp', 'dis_fb_vsolve.comp', []],
]

wfg_spv_header = files('wfg_spv_header.py')
wfg_shader_headers = []
foreach s : wfg_shaders
  wfg_shader_headers += custom_target(
    s[0] + '.spv.h',
    input : files('wfg_shaders/' + s[1]),
    output : s[0] + '.spv.h',
    command : [prog_python, wfg_spv_header, prog_glslang, '@INPUT@', '@OUTPUT@', s[0]] + s[2],
  )
endforeach

wrapper_files = files(
  'graphicsenv_hook.cpp',
  'spirv_patcher.cpp',
  'wrapper_bcn.c',
  'wrapper_device.c',
  'wrapper_device_memory.c',
  'wrapper_fg.c',
  'wrapper_instance.c',
  'wrapper_log.c',
  'wrapper_physical_device.c',
  'wrapper_wfg.c',
)

libvulkan_wrapper = shared_library(
  'vulkan_wrapper',
  [wrapper_entrypoints, wrapper_trampolines, wrapper_features, wrapper_files,
   wfg_shader_headers],
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

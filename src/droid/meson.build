droid_sink = library('droid-sink' + droid_module_suffix,
  ['droid-sink.c', 'droid-sink.h'],
  dependencies : libdroid_util_dep,
  pic : true,
  include_directories : configinc,
  install : true,
  install_dir : modlibexecdir,
  install_rpath : rpath_dirs,
)

droid_sink_dep = declare_dependency(
  link_with : droid_sink,
  compile_args : pa_c_args + ['-DPULSEAUDIO_VERSION=@0@'.format(pa_version_major)],
  dependencies : libdroid_util_dep,
  include_directories : [configinc],
)

droid_source = library('droid-source' + droid_module_suffix,
  ['droid-source.c', 'droid-source.h'],
  dependencies : libdroid_util_dep,
  pic : true,
  install : true,
  install_dir : modlibexecdir,
  install_rpath : rpath_dirs,
)

droid_source_dep = declare_dependency(
  link_with : droid_source,
  compile_args : pa_c_args + ['-DPULSEAUDIO_VERSION=@0@'.format(pa_version_major)],
  dependencies : libdroid_util_dep,
  include_directories : [configinc],
)

droid_module_name_suffix = droid_module_suffix.underscorify()

module_sink = shared_module('module-droid-sink' + droid_module_suffix,
  'module-droid-sink.c',
  name_prefix : '',
  c_args : '-DPA_MODULE_NAME=module_droid_sink' + droid_module_name_suffix,
  dependencies : [droid_sink_dep, libdroid_util_dep],
  install : true,
  install_dir : modlibexecdir,
  install_rpath : rpath_dirs,
)

module_sink = shared_module('module-droid-source' + droid_module_suffix,
  'module-droid-source.c',
  name_prefix : '',
  c_args : '-DPA_MODULE_NAME=module_droid_source' + droid_module_name_suffix,
  dependencies : [droid_source_dep, libdroid_util_dep],
  install : true,
  install_dir : modlibexecdir,
  install_rpath : rpath_dirs,
)

module_card_sources = ['module-droid-card.c']
module_card_extra_deps = []

module_card_sources += 'droid-extcon.c'
module_card_extra_deps += libudev_dep

module_sink = shared_module('module-droid-card' + droid_module_suffix,
  module_card_sources,
  name_prefix : '',
  c_args : '-DPA_MODULE_NAME=module_droid_card' + droid_module_name_suffix,
  dependencies : [droid_sink_dep, droid_source_dep, libdroid_util_dep] + module_card_extra_deps,
  install : true,
  install_dir : modlibexecdir,
  install_rpath : rpath_dirs,
)

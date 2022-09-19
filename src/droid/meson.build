droid_sink = library('droid-sink',
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

droid_source = library('droid-source',
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

module_sink = shared_module('module-droid-sink',
  'module-droid-sink.c',
  name_prefix : '',
  c_args : '-DPA_MODULE_NAME=module_droid_sink',
  dependencies : [droid_sink_dep, libdroid_util_dep],
  install : true,
  install_dir : modlibexecdir,
  install_rpath : rpath_dirs,
)

module_sink = shared_module('module-droid-source',
  'module-droid-source.c',
  name_prefix : '',
  c_args : '-DPA_MODULE_NAME=module_droid_source',
  dependencies : [droid_source_dep, libdroid_util_dep],
  install : true,
  install_dir : modlibexecdir,
  install_rpath : rpath_dirs,
)

module_sink = shared_module('module-droid-card',
  'module-droid-card.c',
  name_prefix : '',
  c_args : '-DPA_MODULE_NAME=module_droid_card',
  dependencies : [droid_sink_dep, droid_source_dep, libdroid_util_dep],
  install : true,
  install_dir : modlibexecdir,
  install_rpath : rpath_dirs,
)

project('simple-cam', 'c', 'cpp',
	default_options : [
		'werror=true',
		'warning_level=2',
		'cpp_std=c++17',
	])

drm_inc = include_directories('/usr/include/libdrm/')

# simple-cam.cpp is the fully commented application
src_files = files([
	'simple-cam.cpp',
	'event_loop.cpp',
])

# Point your PKG_CONFIG_PATH environment variable to the
# libcamera install path libcamera.pc file ($prefix/lib/pkgconfig/libcamera.pc)
deps = [
      dependency('libcamera', required : true),
      dependency('libevent_pthreads'),
]

cpp_arguments = [ '-Wno-unused-parameter', ]

add_project_arguments(cpp_arguments, language : 'cpp')

# simple-cam executable
simple_cam = executable('simple-cam', src_files,
                        dependencies : deps, include_directories: [drm_inc])

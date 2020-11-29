project(
	'repros',
	'c',
	version: '1.3.1',
	license: 'MIT',
	meson_version: '>=0.48.0',
	default_options: [
		'c_std=c11',
		'warning_level=2',
	],
)

cc = meson.get_compiler('c')

cairo = dependency('cairo')
realtime = cc.find_library('rt')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
wayland_protos = dependency('wayland-protocols')
xkbcommon = dependency('xkbcommon')

subdir('protocol')

executable(
	'main',
	files([
		'main.c',
	]),
	dependencies: [
		cairo,
		client_protos,
		realtime,
		wayland_client,
		wayland_cursor,
		xkbcommon,
	],
	install: true,
)

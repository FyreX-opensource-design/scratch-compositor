project(
  'stackcomp',
  'c',
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=false',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')
add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

wlroots = dependency('wlroots-0.19')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
wayland_protocols = dependency('wayland-protocols', version: '>=1.32')
wayland_scanner = find_program('wayland-scanner')

xdg_shell_xml = wayland_protocols.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
xdg_shell_server_h = custom_target(
  'xdg-shell-protocol',
  output: 'xdg-shell-protocol.h',
  input: xdg_shell_xml,
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

layer_shell_xml = files('protocols/wlr-layer-shell-unstable-v1.xml')
layer_shell_server_h = custom_target(
  'wlr-layer-shell-protocol',
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  input: layer_shell_xml,
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

executable(
  'stackcomp',
  'src/main.c',
  'src/config.c',
  xdg_shell_server_h,
  layer_shell_server_h,
  dependencies: [wlroots, wayland_server, xkbcommon, m_dep],
  install: true,
)

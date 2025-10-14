project('fnott', 'c',
        version: '1.8.0',
        license: 'MIT',
        meson_version: '>=0.59.0',
        default_options: [
          'c_std=c18',
          'warning_level=1',
          'werror=true',
          'b_ndebug=if-release'])

is_debug_build = get_option('buildtype').startswith('debug')
cc = meson.get_compiler('c')

add_project_arguments(
  ['-D_GNU_SOURCE=200809L'] +
  (is_debug_build
   ? ['-D_DEBUG']
   : [cc.get_supported_arguments('-fno-asynchronous-unwind-tables')]),
  language: 'c')

if cc.has_function('memfd_create',
                   args: ['-D_GNU_SOURCE=200809L'],
                   prefix: '#include <sys/mman.h>')
  add_project_arguments('-DMEMFD_CREATE', language: 'c')
endif

# Compute the relative path used by compiler invocations.
source_root = meson.current_source_dir().split('/')
build_root = meson.global_build_root().split('/')
relative_dir_parts = []
i = 0
in_prefix = true
foreach p : build_root
  if i >= source_root.length() or not in_prefix or p != source_root[i]
    in_prefix = false
    relative_dir_parts += '..'
  endif
  i += 1
endforeach
i = 0
in_prefix = true
foreach p : source_root
  if i >= build_root.length() or not in_prefix or build_root[i] != p
    in_prefix = false
    relative_dir_parts += p
  endif
  i += 1
endforeach
relative_dir = join_paths(relative_dir_parts) + '/'

if cc.has_argument('-fmacro-prefix-map=/foo=')
  add_project_arguments('-fmacro-prefix-map=@0@='.format(relative_dir), language: 'c')
endif

math = cc.find_library('m', required : false)

threads = dependency('threads')
libepoll = dependency('epoll-shim', required: false)
pixman = dependency('pixman-1')
png = dependency('libpng')
wayland_protocols = dependency('wayland-protocols', version: '>=1.32')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
dbus = dependency('dbus-1')
fontconfig = dependency('fontconfig')
system_nanosvg = cc.find_library('nanosvg', required: get_option('system-nanosvg'))
system_nanosvgrast = cc.find_library('nanosvgrast', required: get_option('system-nanosvg'))

tllist = dependency('tllist', version: '>=1.0.1', fallback: 'tllist')
fcft = dependency('fcft', version: ['>=3.0.0', '<4.0.0'], fallback: 'fcft')

wayland_protocols_datadir = wayland_protocols.get_variable('pkgdatadir')

wscanner = dependency('wayland-scanner', native: true)
wscanner_prog = find_program(
  wscanner.get_variable('wayland_scanner'), native: true)

wl_xmls = [
  'external/wlr-layer-shell-unstable-v1.xml',
  'external/idle.xml',
  wayland_protocols_datadir + '/stable/xdg-shell/xdg-shell.xml',
  wayland_protocols_datadir + '/unstable/xdg-output/xdg-output-unstable-v1.xml',
  wayland_protocols_datadir + '/staging/ext-idle-notify/ext-idle-notify-v1.xml',
  wayland_protocols_datadir / 'stable/viewporter/viewporter.xml',
  wayland_protocols_datadir / 'staging/fractional-scale/fractional-scale-v1.xml',
  wayland_protocols_datadir / 'unstable/tablet/tablet-unstable-v2.xml',  # required by cursor-shape-v1
  wayland_protocols_datadir / 'staging/cursor-shape/cursor-shape-v1.xml',
  wayland_protocols_datadir / 'staging/xdg-activation/xdg-activation-v1.xml',
]

wl_proto_headers = []
wl_proto_src = []
foreach prot : wl_xmls
  wl_proto_headers += custom_target(
    prot.underscorify() + '-client-header',
    output: '@BASENAME@.h',
    input: prot,
    command: [wscanner_prog, 'client-header', '@INPUT@', '@OUTPUT@'])

  wl_proto_src += custom_target(
    prot.underscorify() + '-private-code',
    output: '@BASENAME@.c',
    input: prot,
    command: [wscanner_prog, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

env = find_program('env', native: true)
generate_version_sh = files('generate-version.sh')
version = custom_target(
  'generate_version',
  build_always_stale: true,
  output: 'version.h',
  command: [env, 'LC_ALL=C', generate_version_sh, meson.project_version(), '@CURRENT_SOURCE_DIR@', '@OUTPUT@'])

if system_nanosvg.found() and system_nanosvgrast.found()
  nanosvg = declare_dependency(
    dependencies: [system_nanosvg, system_nanosvgrast, math]
  )
  svg_lib = 'nanosvg (system)'
else
  nanosvg = declare_dependency(
    sources: ['nanosvg.c', '3rd-party/nanosvg/src/nanosvg.h',
              'nanosvgrast.c', '3rd-party/nanosvg/src/nanosvgrast.h'],
    include_directories: '.',
    dependencies: math)
  svg_lib = 'nanosvg (bundled)'
endif

executable(
  'fnott',
  'main.c',
  'char32.c', 'char32.h',
  'config.c', 'config.h',
  'ctrl.c', 'ctrl.h',
  'dbus.c', 'dbus.h',
  'fdm.c', 'fdm.h',
  'icon.c', 'icon.h',
  'log.c', 'log.h',
  'notification.c', 'notification.h',
  'png.c', 'png-fnott.h',
  'shm.c', 'shm.h',
  'spawn.c', 'spawn.h',
  'svg.c', 'svg.h',
  'stride.h',
  'tokenize.c', 'tokenize.h',
  'uri.c', 'uri.h',
  'wayland.c', 'wayland.h',
  'xdg.c', 'xdg.h',
  wl_proto_src + wl_proto_headers, version,
  dependencies: [threads, libepoll, pixman, png, wayland_client, wayland_cursor, dbus, fontconfig,
                tllist, fcft, nanosvg],
  install: true)

executable(
  'fnottctl',
  'fnottctl.c',
  'ctrl-protocol.h',
  'log.c', 'log.h',
  version,
  install: true)

install_data('fnott.desktop', install_dir: join_paths(get_option('datadir'), 'applications'))

scdoc = dependency('scdoc', native: true, required: get_option('docs'))
if scdoc.found()
  install_data('fnott.ini', install_dir: join_paths(get_option('sysconfdir'), 'xdg', 'fnott'))
  install_data(
    'LICENSE', 'README.md',
    install_dir: join_paths(get_option('datadir'), 'doc', 'fnott'))
  subdir('doc')
endif

subdir('completions')
subdir('dbus')
subdir('systemd')

summary(
  {
    'SVG': svg_lib,
    'Documentation': scdoc.found(),
  }
)

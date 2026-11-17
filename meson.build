project('NetworkManager-shadowsocks', 'c', version: '0.1.0',
  meson_version: '>=0.61', default_options: ['c_std=gnu11', 'warning_level=2'])
libnm = dependency('libnm', version: '>=1.20')
gio = dependency('gio-unix-2.0', version: '>=2.66')
json = dependency('json-glib-1.0', version: '>=1.6')
gtk = dependency('gtk+-3.0', version: '>=3.22')
gtk4 = dependency('gtk4', version: '>=4.0')
gmodule = dependency('gmodule-2.0')
deps = [libnm, gio, json]
inc = include_directories('src')
conf = configuration_data()
conf.set_quoted('SS_VERSION', meson.project_version())
conf.set_quoted('GETTEXT_PACKAGE', 'NetworkManager-shadowsocks')
conf.set_quoted('SS_LOCALEDIR', get_option('prefix') / get_option('localedir'))
conf.set_quoted('SS_IP', get_option('ip_binary'))
conf.set_quoted('SS_LOCAL', get_option('sslocal_binary'))
conf.set_quoted('SS_REGIONAL_BUNDLED', get_option('prefix') / get_option('datadir') / 'NetworkManager-shadowsocks/china')
conf.set_quoted('SS_REGIONAL_STATE', get_option('prefix') / get_option('localstatedir') / 'lib/NetworkManager-shadowsocks/china')
configure_file(output: 'config.h', configuration: conf)
core = static_library('ss-core', ['src/profile.c', 'src/plugin.c', 'src/routes.c', 'src/regional.c', 'src/i18n.c', 'src/nm-config.c'],
  dependencies: deps, include_directories: inc, pic: true)
coredep = declare_dependency(link_with: core, dependencies: deps, include_directories: inc)
subdir('po')
locale_test = executable('test-i18n', 'tests/test-i18n.c', dependencies: coredep)
foreach locale_case : [['english', 'C.UTF-8', 'en', 'english'],
                      ['simplified-chinese', 'zh_CN.UTF-8', 'zh_CN', 'translated'],
                      ['language-override', 'en_US.UTF-8', 'zh_CN', 'translated'],
                      ['missing-translation', 'C.UTF-8', 'fr', 'english'],
                      ['posix-locale', 'C', 'zh_CN', 'english']]
  test('i18n-' + locale_case[0], locale_test,
    args: [meson.current_build_dir() / 'po', locale_case[3]],
    env: ['LC_ALL=' + locale_case[1], 'LANGUAGE=' + locale_case[2]], depends: translations[0])
endforeach
test('translation-format-check', find_program('msgfmt'),
  args: ['--check', '--check-format', '-o', '/dev/null', meson.project_source_root() / 'po/zh_CN.po'])
executable('nm-shadowsocks-service', ['src/service.c', 'src/runtime.c'],
  dependencies: coredep, install: true, install_dir: get_option('libexecdir'))
editor = shared_module('nm-vpn-plugin-shadowsocks', 'src/editor-plugin.c',
  dependencies: [coredep, gmodule, meson.get_compiler('c').find_library('dl')],
  install: true, install_dir: get_option('libdir') / 'NetworkManager')
editor3 = shared_module('nm-vpn-plugin-shadowsocks-editor', 'src/editor.c',
  dependencies: [coredep, gtk], install: true, install_dir: get_option('libdir') / 'NetworkManager')
editor4 = shared_module('nm-gtk4-vpn-plugin-shadowsocks-editor', 'src/editor.c',
  dependencies: [coredep, gtk4], install: true, install_dir: get_option('libdir') / 'NetworkManager')
cli = executable('nm-shadowsocks', 'src/cli.c', dependencies: coredep, install: true)
auth_dialog = executable('nm-shadowsocks-auth-dialog', 'src/auth-dialog.c',
  dependencies: coredep, install: true, install_dir: get_option('libexecdir'))
python = import('python').find_installation('python3')
test('auth-dialog', python, args: [files('tests/test-auth-dialog.py'), auth_dialog.full_path(),
  meson.project_build_root() / 'nm-shadowsocks-service.name'])
assert(python.language_version().version_compare('>=3.9'), 'The dataset updater requires Python >= 3.9')
updater_conf = configuration_data()
updater_conf.set('PYTHON', python.full_path())
updater_conf.set('LOCALEDIR', get_option('prefix') / get_option('localedir'))
updater_conf.set('STATE_DIR', get_option('prefix') / get_option('localstatedir') / 'lib/NetworkManager-shadowsocks/china')
subdir('tools')
test('python-updater', python, args: [files('tests/test-updater.py')],
  env: ['SS_TEST_UPDATER=' + updater_path, 'SS_TEST_LOCALEDIR=' + meson.project_build_root() / 'po'],
  depends: translations[0])
regional_test = executable('test-regional', 'tests/test-regional.c', dependencies: coredep,
  c_args: ['-DSOURCE_DIR="' + meson.project_source_root() + '"', '-DUPDATER="' + updater_path + '"'])
test('regional-data', regional_test)
unit = executable('test-core', 'tests/test-core.c', dependencies: coredep)
test('core', unit)
editor_test = executable('test-editor', 'tests/test-editor.c', dependencies: [coredep, gtk, gmodule],
  c_args: ['-DEDITOR_PATH="' + editor.full_path() + '"', '-DSOURCE_DIR="' + meson.project_source_root() + '"',
           '-DTEST_LOCALEDIR="' + meson.project_build_root() / 'po' + '"'])
test('editor-module', editor_test, depends: [editor, editor3])
editor4_test = executable('test-editor-gtk4', 'tests/test-editor.c', dependencies: [coredep, gtk4, gmodule],
  c_args: ['-DEDITOR_PATH="' + editor.full_path() + '"', '-DSOURCE_DIR="' + meson.project_source_root() + '"',
           '-DTEST_LOCALEDIR="' + meson.project_build_root() / 'po' + '"'])
test('editor-module-gtk4', editor4_test, depends: [editor, editor4])
live_test = executable('test-routes-live', 'tests/test-routes-live.c', dependencies: coredep,
  c_args: ['-DSOURCE_DIR="' + meson.project_source_root() + '"'])
backend_test = executable('test-backend-live', 'tests/test-backend-live.c', dependencies: coredep)
if get_option('network_tests')
  test('routes-in-network-namespace', find_program('tests/run-netns.sh'), args: [live_test.full_path()])
  test('backend-in-network-namespace', find_program('tests/run-netns.sh'), args: [backend_test.full_path()])
endif
vpnconf = configuration_data()
vpnconf.set('LIBEXECDIR', get_option('prefix') / get_option('libexecdir'))
vpnconf.set('EDITOR', get_option('prefix') / get_option('libdir') / 'NetworkManager' / 'libnm-vpn-plugin-shadowsocks.so')
vpn_dir = get_option('vpn_service_dir')
if vpn_dir == ''
  vpn_dir = libnm.get_variable(pkgconfig: 'vpnservicedir', default_value: get_option('libdir') / 'NetworkManager/VPN')
endif
configure_file(input: 'data/nm-shadowsocks-service.name.in', output: 'nm-shadowsocks-service.name',
  configuration: vpnconf, install: true, install_dir: vpn_dir)
install_data('data/org.freedesktop.NetworkManager.shadowsocks.conf',
  install_dir: get_option('datadir') / 'dbus-1/system.d')
install_data('examples/profile.json', 'examples/regional-example.cidr', install_dir: get_option('datadir') / 'doc/NetworkManager-shadowsocks/examples')
install_data('README.md', 'README-zh_CN.md', 'DESIGN.md', install_dir: get_option('datadir') / 'doc/NetworkManager-shadowsocks')
install_data('data/china/china.cidr', 'data/china/china-ipv4.cidr', 'data/china/china-ipv6.cidr',
  'data/china/china.json', 'data/china/README.md', install_dir: get_option('datadir') / 'NetworkManager-shadowsocks/china')

# Config generator for the isolated end-to-end TUN test; never installed.
backend_config_test = executable('test-backend-config', 'tests/test-backend-config.c', dependencies: coredep)
if get_option('network_tests')
  test('tun-forwarding-in-network-namespaces', find_program('tests/run-netns.sh'),
    args: [python.full_path(), meson.project_source_root() / 'tests/test-tun-live.py',
           cli.full_path(), backend_config_test.full_path()],
    depends: [cli, backend_config_test])
endif

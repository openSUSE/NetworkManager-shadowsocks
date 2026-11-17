updater = configure_file(input: 'update-routes.py', output: 'nm-shadowsocks-update-routes',
  configuration: updater_conf, install: true, install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
updater_path = meson.current_build_dir() / 'nm-shadowsocks-update-routes'

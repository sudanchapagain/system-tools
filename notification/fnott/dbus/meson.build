conf_data = configuration_data()
conf_data.set('bindir', get_option('prefix') / get_option('bindir'))
configure_file(
  input: 'fnott.service.in',
  output: '@BASENAME@',
  configuration: conf_data,
  install_dir: dbus.get_variable(
    pkgconfig: 'session_bus_services_dir',
    default_value: get_option('datadir') / 'dbus-1/services',
  ),
)

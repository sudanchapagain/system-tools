systemd = dependency('systemd', required: false)
custom_systemd_units_dir = get_option('systemd-units-dir')

if systemd.found() or custom_systemd_units_dir != ''
  configuration = configuration_data()
  configuration.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))

  if (custom_systemd_units_dir == '')
    systemd_units_dir = systemd.get_variable('systemduserunitdir')
  else
    systemd_units_dir = join_paths(get_option('prefix'), 'lib/systemd/user')
  endif

  configure_file(
    configuration: configuration,
    input: 'fnott.service.in',
    output: '@BASENAME@',
    install_dir: systemd_units_dir
    )
endif

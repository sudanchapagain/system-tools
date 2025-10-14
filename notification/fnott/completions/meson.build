zsh_install_dir = join_paths(get_option('datadir'), 'zsh/site-functions')
fish_install_dir = join_paths(get_option('datadir'), 'fish', 'vendor_completions.d')

install_data('zsh/_fnott', install_dir: zsh_install_dir)
install_data('zsh/_fnottctl', install_dir: zsh_install_dir)

install_data('fish/fnott.fish', install_dir: fish_install_dir)
install_data('fish/fnottctl.fish', install_dir: fish_install_dir)

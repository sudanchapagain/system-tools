set -l commands dismiss actions dismiss-with-default-action list pause unpause quit

complete -c fnottctl
complete -c fnottctl -f

complete -c fnottctl -s v -l version -d "show the version number and quit"
complete -c fnottctl -s h -l help -d "show help message and quit"

complete -c fnottctl -n "not __fish_seen_subcommand_from $commands" -a "$commands"
complete -c fnottctl -n "__fish_seen_subcommand_from dismiss" -a "all (fnottctl list | cut -d ':' -f 1)"
complete -c fnottctl -n "__fish_seen_subcommand_from dismiss-with-default-action" -a "(fnottctl list | cut -d ':' -f 1)"
complete -c fnottctl -n "__fish_seen_subcommand_from actions" -a "(fnottctl list | cut -d ':' -f 1)"

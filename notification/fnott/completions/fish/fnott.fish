complete -c fnott

complete -c fnott -f
complete -c fnott -r -s c -l config                               -d "path to configuration file (XDG_CONFIG_HOME/fnott/fnott.ini)"
complete -c fnott -x -s p -l print-pid                            -d "print PID to this file or FD when up and running"
complete -c fnott -x -s l -l log-colorize  -a "never always auto" -d "enable or disable colorization of log output on stderr"
complete -c fnott    -s s -l log-no-syslog                        -d "disable syslog logging"
complete -c fnott    -s v -l version                              -d "show the version nuber and quit"
complete -c fnott    -s h -l help                                 -d "show help message and quit"

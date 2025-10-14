# Frequently Asked Questions

## Starship is doing something unexpected, how can I debug it?

You can enable the debug logs by using the `STARSHIP_LOG` env var. These logs
can be very verbose so it is often useful to use the `module` command if you are
trying to debug a particular module, for example, if you are trying to debug
the `rust` module you could run the following command to get the trace
logs and output from the module.

```sh
env STARSHIP_LOG=trace starship module rust
```

If starship is being slow you can try using the `timings` command to see if
there is a particular module or command that is to blame.

```sh
env STARSHIP_LOG=trace starship timings
```

This will output the trace log and a breakdown of all modules that either took
more than 1ms to execute or produced some output.

Finally if you find a bug you can use the `bug-report` command to create a
GitHub issue.

```sh
starship bug-report
```

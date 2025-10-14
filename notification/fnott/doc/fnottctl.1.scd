fnottctl(1)

# NAME
fnottctl - utility to interact with *fnott*(1)

# SYNOPSIS
*fnottctl* *dismiss* [_id_]++
*fnottctl* *dismiss-with-default-action* [_id_]++
*fnottctl* *actions* [_id_]++
*fnottctl* *list*++
*fnottctl* *pause*++
*fnottctl* *unpause*++
*fnottctl* *quit*++
*fnottctl* *--version*

# OPTIONS

*id*
	The notification ID to dismiss or show actions for. Use *fnottctl
	list* to see the IDs of currently active notifications.

*-v*,*--version*
	Show the version number and quit

# DESCRIPTION

*fnottctl* is used to interact (dismiss notifications, show and select
action for notifications) with *fnott*(1).

The most common operation is *fnottctl dismiss*. This will dismiss the
highest priority notification. You might want to bind this to a
keyboard shortcut in your Wayland compositor configuration. This is
the same as right clicking the notification.

To see, and select between, actions associated with the notification,
use *fnottctl actions*. This requires a dmenu-like utility to have
been configured in *fnott.ini*(5).

Finally, you can trigger the notification's default action (an action
named *default*) and dismiss it simultaneously with
*dismiss-with-default-action*. This is the same as left clicking the
notification.

You can optionally specify a notification ID, to dismiss (or show
actions for) a specific notification instead of the highest priority
one.

For _dismiss_, there is also the special ID _all_ which will, not
unsurprisingly, dismiss *all* notifications.

# SEE ALSO

*fnott*(1), *fnott.ini*(5)

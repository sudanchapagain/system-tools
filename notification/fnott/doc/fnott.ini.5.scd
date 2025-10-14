fnott(5)

# NAME
fnott - configuration file

# DESCRIPTION

*fnott* uses the standard _unix configuration format_, with section
based key/value pairs. The default (global) section is unnamed
(i.e. not prefixed with a _[section]_). However it can also be
explicitly named _[main]_, say if it needs to be reopened after any
of the other sections.

fnott will search for a configuration file in the following locations,
in this order:

- *XDG_CONFIG_HOME/fnott/fnott.ini* (defaulting to
   *~/.config/fnott/fnott.ini* if unset)
- *XDG_CONFIG_DIRS/fnott/fnott.ini* (defaulting to
   */etc/xdg/fnott/fnott.ini* if unset)

# SECTION: default

## Global options

*output*
	The output to display notifications on. If left unspecified, the
	compositor will choose one for us.
	
	Note that if you *do not* specify an output, and the output chosen
	by the compositor is scaled, then each new notification will flash
	a low-res frame before re-rendering with the correct scale
	factor. This is because fnott has no way of knowing what the scale
	factor is until *after* the notification has been mapped
	(i.e. shown).
	
	Default: _unspecified_.
	
	In Sway, you can use *swaymsg -t get_outputs* to get a list of
	available outputs.

*min-width*
	Minimum notification width, in pixels. Default: _0_.

*max-width*
	Maximum notification width, in pixels. 0 means unlimited. Note
	that fnott will automatically word-wrap the notification text if
	set to a non-zero value. Default: _0_.

*max-height*
	Maximum notification height, in pixels. 0 means
	unlimited. Default: _0_.

*icon-theme*
	Icon theme to use when a notification has requested an
	non-embedded icon. Default: _default_.

*max-icon-size*
	Maximum icon size, in pixels. Icons larger than this will be
	scaled down. A value of 0 disables icons. Default: _32_.

*stacking-order*
	How to stack multiple notifications.
	
		- *bottom-up* : oldest, highest priority furthest away from
		  the anchor point.
		- *top-down*: oldest, highest priority at the anchor point.
	
	Thus, if the notifications are anchored at the top, *bottom-up*
	will have the most recent notification in the upper corner, while
	the oldest notification is in the bottom of the stack.
	
	Default: _bottom-up_.

*anchor*
	Where notifications are positioned: *top-left*, *top-right*,
	*bottom-left*, *bottom-right* or *center*. Default: _top-right_.

*edge-margin-vertical*
	Vertical margin, in pixels, between the screen edge (top or
	bottom, depending on anchor pointer) and notifications. Default:
	_10_.

*edge-margin-horizontal*
	Horizontal margin, in pixels, between the screen edge (left or
	right, depending on anchor pointer) and notifications. Default:
	_10_.

*notification-margin*
	Margin between notifications. Default: _10_.

*selection-helper*
	Command (and optionally arguments) to execute to display actions
	and let the user select an action to run.
	
	The utility should accept (action) entries to display on stdin
	(newline separated), and write the selected entry on
	stdout.
	
	It is strongly recommended that you enable
	*selection-helper-uses-null-separator* (see below), if your helper
	of choice supports it.
	
	Default: _dmenu_.

*selection-helper-uses-null-separator*
	Boolean. When enabled, the action strings passed to the selection
	helper will be NULL separated, instead of newline
	separated.
	
	Note that you may also have to adjust *selection-helper*, to
	ensure the selection helper parses the action strings
	correctly. For example, instead of *fuzzel --dmenu*, you would
	have to use *fuzzel --dmenu0*.
	
	Default: _no_.

*play-sound*
	Command to execute to play notification sounds. _${filename}_ will
	be expanded to the path to the audio file to play. Default: _aplay
	${filename}_.

*scaling-filter*

	Which scaling filter to use when scaling non-SVG notification
	images.
	
	Possible values are:
	- none
	- nearest
	- bilinear
	- cubic
	- lanczos3
	
	*none* is the fastest, but also looks the worst. *lanczos3*
	produces the best result, but is also, by far, the slowest option.
	
	Default: _lanczos3_

## Per-urgency default options

These options can also be specified in an _urgency_ section, in which
case they override the values specified in the default section.

*layer*
	Layer on which notifications will appear: *background*, *bottom*,
	*top* or *overlay*. Default: _top_.

*background*
	Background color of the notification, in RGBA format. Default:
	_3f5f3fff_.

*border-color*
	Border color of the notification, in RGBA format. Default:
	_909090ff_.

*border-radius*
	Corner radius on the border in pixels. Default: _0_.

*border-size*
	Border size, in pixels. Default: _1_.

*padding-vertical*
	Vertical padding, in pixels, between the notification edge (top or
	bottom) and notification text. Default: _20_.

*padding-horizontal*
	Horizontal padding, in pixels, between the notification edge (left
	or right) and notification text. Default: _20_.

*dpi-aware*
	Boolean.
	
	When set to *yes*, fonts are sized using the monitor's DPI, making
	a font of a given size have the same physical size, regardless of
	monitor.
	
	In this mode, the monitor's scaling factor is ignored; doubling
	the scaling factor will *not* double the font size.
	
	When set to *no*, the monitor's DPI is ignored. The font is
	instead sized using the monitor's scaling factor; doubling the
	scaling factor *does* double the font size.
	
	Note that this option typically does not work with bitmap fonts,
	which only contains a pre-defined set of sizes, and cannot be
	dynamically scaled. Whichever size (of the available ones) that
	best matches the DPI or scaling factor, will be used.
	
	Also note that if the font size has been specified in pixels
	(*:pixelsize=*_N_, instead of *:size=*_N_), DPI scaling
	(*dpi-aware=yes*) will have no effect (the specified pixel size
	will be used as is). But, if the monitor's scaling factor is used
	to size the font (*dpi-aware=no*), the font's pixel size will be
	multiplied with the scaling factor.
	
	Default: _no_

*title-font*
	Font to use for the application title, in fontconfig format (see
	*FONT FORMAT*). Default: _sans serif_.

*title-color*
	Text color to use for the application title, in RGBA
	format. Default: _ffffffff_.

*title-format*
	Template string for the title portion of the notification (see
	*FORMAT STRINGS*). Default: _<i>%a%A</i>_.

*summary-font*
	Font to use for the summary, in fontconfig format (see *FONT
	FORMAT*). Default: _sans serif_.

*summary-color*
	Text color to use for the summary, in RGBA format. Default:
	_ffffffff_.

*summary-format*
	Template string for the summary portion of the notification (see
	*FORMAT STRINGS*). Default: _<b>%s\\n</b>_.

*body-font*
	Font to use for the text body, in fontconfig format (see *FONT
	FORMAT*). Default: _sans serif_.

*body-color*
	Text color to use for the text body, in RGBA format. Default:
	_ffffffff_.

*body-format*
	Template string for the body portion of the notification (see
	*FORMAT STRINGS*). Default: _%b_.

*progress-bar-height*
	Height, in pixels, of progress bars (rendered when a notification
	has an _int:value_ hint). Default: _20_.

*progress-color*
	Color, in RGBA format, of progress indicator. Default: _ffffffff_.

*progress-style*
	Defines the style of the progress indicator in notifications.
	
	Possible values:
	- *bar*: Displays a separate progress bar.
	- *background*: Fills the notification background with the progress color.

	Default: _bar_.

*max-timeout*
	Time limit, in seconds, before notifications are automatically
	dismissed. Applications can provide their own timeout when they
	create a notification. This option can be used to limit that
	timeout. A value of 0 disables the limit. Default: _0_.

*default-timeout*
	Time, in seconds, before notifications are automatically dismissed
	if the notifying application does not specify a timeout. A value
	of 0 disables the timeout. I.e. if the application does not
	provide a timeout, the notification is never automatically
	dismissed (unless *max-timeout* has been set). Default: _0_.

*idle-timeout*
	Time, in seconds, that you must be idle to prevent any
	notification from being dismissed. A value of 0 disables
	the timeout. Default: _0_.

*sound-file*
	Absolute path to audio file to play when a notification is
	received. If unset, no sound is played. Default: _unset_.

*icon*
	Icon to use when none is provided by the notifications
	themselves. Can be either an absolute path, or a name (without
	extension).
	
	In the latter case, it will be searched for in the selected icon
	theme, using the fallback rules defined by the XDG icon theme
	specification.
	
	Default: _unset_.

# SECTION: low

This section allows you to override the options listed under
*per-urgency default options* for _low_ priority notifications.

By default, the following options are already overridden:

- *background*: _2b2b2bff_
- *title-color* _888888ff_
- *summary-color*: _888888ff_
- *body-color*: _888888ff_

# SECTION: normal

This section allows you to override the options listed under
*per-urgency default options* for _normal_ priority notifications.

By default, the following options are already overridden: _none_.

# SECTION: critical

This section allows you to override the options listed under
*per-urgency default options* for _critical_ priority notifications.

By default, the following options are already overridden:

- *background*: _6c3333ff_


# FONT FORMAT

The font is specified in FontConfig syntax. That is, a colon-separated
list of font name and font options.

_Examples_:
- Dina:weight=bold:slant=italic
- Courier New:size=12

# FORMAT STRINGS

The *title-format*, *summary-format* and *body-format* options allow
you to configure what to display for the corresponding portion of the
notification.

They are strings with placeholders that are expanded with attributes
from the notification:

- *%a*  application name
- *%s*  notification summary
- *%b*  notification body text
- *%A*  action indicator ('\*' if actions are present, empty string otherwise)
- *%%*  a literal '%'
- *\\n*  a literal newline

Also supported are the following markup tags:

- *<b>*  bold
- *<i>*  italic
- *<u>*  underline


# SEE ALSO

*fnott*(1)

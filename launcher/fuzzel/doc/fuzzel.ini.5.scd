fuzzel.ini(5)

# NAME
fuzzel.ini - configuration file for *fuzzel*(1)


# DESCRIPTION

*fuzzel* uses the standard _unix configuration format_, with section
based key/value pairs. The default section is usually unnamed,
i.e. not prefixed with a _[section]_. However it can also be
explicitly named _[main]_, say if it needs to be reopened after any of
the other sections.

fuzzel will search for a configuration file in the following locations,
in this order:

	- *XDG_CONFIG_HOME/fuzzel/fuzzel.ini* (defaulting to
	  *~/.config/fuzzel/fuzzel.ini* if unset)
	- *XDG_CONFIG_DIRS/fuzzel/fuzzel.ini* (defaulting to
	  */etc/xdg/fuzzel/fuzzel.ini* if unset)

An example configuration file containing all options with their default value
commented out will usually be installed to */etc/xdg/fuzzel/fuzzel.ini*.

# SECTION: main

*include*
	Absolute path to configuration file to import.

	The import file has its own section scope. I.e. the including
	configuration is still in the default section after the include,
	regardless of which section the included file ends in.

		- The path must be an absolute path, or start with *~/*.
		- Multiple include directives are allowed, but only one path per
		  directive.
		- Nested imports are allowed.

	Default: _not set_.

*namespace*
	Namespace for the spawned layer shell surface. Useful for blocking
	fuzzel out from screencasts in your compositor if it shows sensitive
	information, such as passwords.

	Default: _launcher_

*output*
	Output (monitor) to display on. You can list the available outputs
	with *wlr-randr* or with Sway using *swaymsg -t get_outputs*.

	Example: _DP-1_

	Default: Not set-- let the compositor choose output.

*font*
	Comma separated list of fonts to use, in fontconfig format. That
	is, a font name followed by a list of colon-separated
	options. Most noteworthy is *:size=n*, which is used to set the
	font size. Note that the font size is also affected by the
	*dpi-aware* option. See *FONT CONFIG* below for details.

	Examples:
		- Dina:weight=bold:slant=italic
		- Courier New:size=12
		- Fantasque Sans Mono:fontfeatures=ss01

	Default: _monospace_.

*use-bold*
	Boolean. When set, fuzzel may use bold fonts. For example, for the
	currently selected item. Default: _no_

*dpi-aware*
	*auto*, *yes*, or *no*.

	When set to *yes*, fonts are sized using the monitor's DPI, making
	a font of a given size have the same physical size, regardless of
	monitor.

	In this mode, the monitor's scaling factor is ignored; doubling
	the scaling factor will *not* double the font size.

	When set to *no*, the monitor's DPI is ignored. The font is
	instead sized using the monitor's scaling factor; doubling the
	scaling factor *does* double the font size.

	Finally, if set to *auto*, fonts will be sized using the monitor's
	DPI if _all_ monitors have a scaling factor of 1. If at least one
	monitor as a scaling factor larger than 1 (regardless of whether
	the fuzzel window is mapped on that monitor or not), fonts will be
	scaled using the scaling factor.

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

	Default: _auto_.

*gamma-correct*
	Boolean. When enabled, fuzzel will do gamma-correct blending in
	linear color space. This is how font glyphs are supposed to be
	rendered, but since nearly no applications or toolkits are doing
	it on Linux, the result may not look like you are used to.

	Compared to the default (disabled), bright glyphs on a dark
	background will appear thicker, and dark glyphs on a light
	background will appear thinner.

	FreeType can limit the effect of the latter, with a technique
	called stem darkening. It is only available for CFF fonts
	(OpenType, .otf) and disabled by default (in FreeType). You can
	enable it by setting the environment variable
	*FREETYPE_PROPERTIES="cff:no-stem-darkening=0"* before starting
	fuzzel.

	Also be aware that many fonts have been developed on systems that
	do not do gamma-correct blending, and may therefore look thicker
	than intended when rendered with gamma-correct blending, since the
	font designer set the font weight based on incorrect rendering.

	In order to represent colors faithfully, higher precision image
	buffers are required. For this reason, fuzzel uses 16-bit image
	buffers (instead of 8-bit) when gamma-correct blending is enabled.

	It is not possible to enable gamma-correct blending in cairo
	enabled builds of fuzzel, since cairo does not support 16-bit
	image buffers.

*placeholder*
	Text to display as placeholder in the input box. Default: empty.

*prompt*
	String to use as input prompt. Note that trailing spaces are
	trimmed, unless the string is quoted. Default: _"> "_.

*icon-theme*
	Icon theme. Note that the name is case sensitive. Default:
	_default_.

*icons-enabled*
	Boolean. When enabled, application icons (from the selected
	*icon-theme*) will be rendered. Default: _yes_.

*hide-before-typing*
	Boolean. When enabled, application list will be hidden until the first
	letter is typed. When the prompt is cleared again, the list will appear.
	Default: _no_.

*fields*
	Comma separated list of XDG Desktop entry fields to match
	against.

	Even though fuzzel only displays the application names,
	matching can (and by default is) be done against other fields as
	well. Supported fields are:

	- filename
	- name
	- generic
	- exec
	- categories
	- keywords
	- comment

	"filename" is the ".desktop" file name.

	Default: _filename,name,generic_.

*password-character*
	Default character to use with the command line option
	*--password*. Note that this can be overridden on the command
	line, by explicitly specifying the character with
	*--password=X*. Default: _\*_.

*match-mode*
	One of *exact*, *fzf* and *fuzzy*.

	*exact*: what you type is matched exactly against each field in
	the *fields* list.

	*fzf*: fzf-style matching, where substrings from what you have
	typed are matched against each field in the *fields* list.

	*fuzzy*: levenshtein-based fuzzy matching of what you have typed
	against each field in the *fields* list.

	Default: _fzf_

*sort-result*
	Boolean. Whether to sort the result or not. Disabling sorting
	means no comparisons will be made between match
	results. I.e. launch count, match length, etc will be
	ignored. This effectively disables the cache (though, the cache is
	still updated on exit).

	Disabling this can improve search latency when searching very
	large data sets.

	Default: _yes_

*match-counter*
	Boolean. When enabled, the match count and total count is
	displayed on the right-hand side of the input prompt. Disabling
	the counter also improves performance when loading a very large
	number of dmenu entries from STDIN.

	Default: _no_.

*filter-desktop*
	Boolean. Filter desktop files based on the value of
	XDG_CURRENT_DESKTOP. Default: _no_.

*show-actions*
	Boolean. Some desktop files define "actions", in addition to the
	application itself. Examples are "new window", "preferences",
	etc. When this option is enabled, those actions will be listed by
	fuzzel. Default: _no_.

*terminal*
	terminal command to use when launching 'terminal' programs.

	The command may contain the placeholder _{cmd}_ which will be
	replaced with the actual command to be run. When _{cmd}_ is present,
	the command is **only** substituted in those locations and is **not**
	appended at the end.

	Examples:
	- _foot -a '{cmd}' -T '{cmd}' {cmd}_ sets app-id and title to the
	  command name, then executes it
	- _xterm -e {cmd}_ executes the command in xterm
	- _xterm -e_ (no {cmd}) uses old behavior: appends command after
	  terminal args

	Default: _$TERMINAL -e_.

*list-executables-in-path*
	Boolean. When enabled, the list will include executables
	from the PATH environment variable. Default: _no_.

*launch-prefix*
	Prefix to add before argv of executed program.
	If set, fuzzel will pass the Desktop File ID of the chosen application
	(see the Desktop Entry specification) in the FUZZEL_DESKTOP_FILE_ID
	environment variables.
	Default: _not set_.

*anchor*
	Set window anchor, i.e. where on screen the window will be
	displayed.  You can choose one from:

	- top-left
	- top
	- top-right
	- left
	- center
	- right
	- bottom-left
	- bottom
	- bottom-right

	Default: _center_

*x-margin*
	Horizontal margin away from the anchor point in pixels. Default: _0_.

	Note: this option has no effect when anchor=center, top or bottom.

*y-margin*
	Vertical margin away from the anchor point in pixels. Default: _0_.

	Note: this option has no effect when anchor=center, left or right.

*lines*
	Number of matches to show. Default: _15_.

*hide-prompt*
	Boolean. When set, hides the prompt line, making the window smaller.
	Input is still accepted though the prompt line is not displayed.
	Cannot be used with *prompt-only*. Default: _no_.

*minimal-lines*
	Boolean. Adjust the number of lines to display to the minimum of
	_lines_ and the number of input lines. Only effective in dmenu
	mode. This allows fuzzel to size itself to the actual number of
	input lines rather than always showing the full _lines_
	rows. Default: _no_.

*width*
	Window width, in characters. Margins and borders not included. For
	variable width fonts this is an estimate, as the exact number of
	characters that will fit depends on which characters are being
	displayed. Default: _30_.

*tabs*
	Number of spaces a tab is expanded to. Default: _8_.

*horizontal-pad*
	Horizontal padding, in pixels, between border and
	content. Default: _40_.

*vertical-pad*
	Vertical padding, in pixels, between border and content. Default:
	_8_.

*inner-pad*
	Vertical padding between prompt and match list, in
	pixels. Default: _0_.

*image-size-ratio*
	The ratio of the large image displayed when there are only a "few"
	matches, compared to the full window size. Set to 1 to disable the
	the large images. Default: _0.5_.

*line-height*
	Override line height from font metrics. Default: use font metrics.

*letter-spacing*
	Additional letter spacing. Negative values are allowed. Default:
	_0_.

*layer*
	Which layer to render the fuzzel window on. Valid values are *top*
	and *overlay*.

	*top* renders above normal windows, but typically below fullscreen
	windows and lock screens.

	*overlay* renders on top of both normal windows and fullscreen
	windows. Note that the order is undefined if several windows use
	the same layer. Since e.g. lock screens typically use *overlay*,
	that means fuzzel may or may not appear on top of a lock screen.

	Default: _overlay_

*keyboard-focus*
	Keyboard focusing mode, valid modes are *exclusive* and *on-demand*.

	*exclusive* locks keyboard focus to fuzzel, receiving all keyboard
	input until closed.

	*on-demand* makes fuzzel focus like a regular window, allowing other
	windows to take keyboard focus by e.g. clicking on them.
	By default switching keyboard focus will immediately close fuzzel,
	using *exit-on-keyboard-focus-loss=no* allows interacting with
	other windows while fuzzel is open.

	Default: _exclusive_

*exit-on-keyboard-focus-loss*
	Boolean. If true, exit when the fuzzel window loses keyboard focus.

	Setting this to false can be useful on compositors where enabling
	"focus-follows-mouse" causes fuzzel to exit as soon as the mouse
	is moved over another window. Sway (<= 1.7) exhibits this
	behavior, for example.

	Default: _yes_

*render-workers*
	Number of threads to use for rendering. Set to 0 to disable
	multithreading. Default: the number of available logical CPUs
	(including SMT). Note that this is not always the best value. In
	some cases, the number of physical _cores_ is better.

	In case you have a ridiculous amount of cores and/or threads,
	consider limiting the number of *render-workers*, since fuzzel
	cannot parallelize more than the number of displayed entries.

*match-workers*
	Number of threads to use for matching. Set to 0 to disable
	multithreading. Default: the number of available logical CPUs
	(including SMT). Note that this is not always the best value. In
	some cases, the number of physical _cores_ is better.

	In case you have a ridiculous amount of cores and/or threads,
	consider limiting the number of *match-workers*, since fuzzel
	cannot parallelize more than the number of available entries.

*delayed-filter-ms*
	Time, in milliseconds, to delay refiltering when there are more
	matches than *delayed-filter-limit*. Default: _300_.

*delayed-filter-limit*
	When there are more matches than this, switch from immediate
	refiltering to delayed refiltering (see
	*delayed-filter-ms*). Default: _20000_.

*scaling-filter*
	Scaling filter to use when down scaling PNGs. One of *none*,
	*nearest*, *bilinear*, *box*, *linear*, *cubic*, *lanczos2*,
	*lanczos3* and *lanczos3-stretched*.

	*none*, *nearest* and *bilinear* are the fastest options, but also
	have the worst looking results.

	*cubic*, *lanczos2*, *lanczos3* and *lanczos3-stretched* are the
	slowest options, with the best looking results.

	*box*, and to some degree, *linear*, are in the middle,
	performance wise, while in most cases still being just as, or
	close to, as good as *cubic* and the lanczos filters.

	Default: _box_

*cache*
	Specify the default cache location. If unset,
	*$XDG_CACHE_HOME/fuzzel* will be used. Set to */dev/null* to
	disable caching. Default: _not set_.

*auto-select*
	Boolean. When one entry is left, automatically select it.
	Default: _no_.

*enable-mouse*
	Boolean. Enable mouse input.
	Default: _yes_.

# SECTION: colors

All color values are in RGBA.

*background*
	Background color. Default: _fdf6e3ff_.

*text*
	Text (foreground) color of unselected entries. Default:
	_657b83ff_.

*prompt*
	Text (foreground) color of prompt character(s). Default:
	_586e75ff_.

*placeholder*
	Text (foreground) color of the placeholder string. Default:
	_93a1a1ff_.

*input*
	Text (foreground) color of input string. Default: _657b83ff_.

*match*
	Text (foreground) color of the matched substring. Default:
	_cb4b16ff_.

*selection*
	Background color of the selected entry. Default: _eee8d5ff_.

*selection-text*
	Text (foreground) color of the selected entry. Default:
	_586e75ff_.

*selection-match*
	Text (foreground) color of the matched substring of the
	selected entry. Default: _cb4b16ff_.

*counter*=_HEX_
	The color of the match count stats printed at the right-hand side
	of the input prompt. See *COLORS*. Default: _93a1a1ff_.

*border*
	Border color. Default: _002b36ff_.


# SECTION: border

*width*
	Width of the border, in pixels. Default: _1_.

*radius*
	Amount of corner "roundness". Default: _10_.

*selection-radius*
	Amount of corner "roundness" for the selected entry. Default: _0_.


# SECTION: dmenu

*mode*
	One of *text* or *index*. Determines what fuzzel prints on stdout
	when an entry is selected: *text* prints the entry itself, *index*
	prints the index of the selected entry. Index values start at zero
	for the first entry. Default: _text_.

*exit-immediately-if-empty*
	Boolean. If enabled, fuzzel will not run at all (i.e. it will not
	open a window, and will not print anything on stdout) if there's
	nothing on stdin. Default: _no_.


# SECTION: key-bindings

This section lets you override the default key bindings.

The general format is _action=combo1...comboN_. That is, each action
may have one or more key combinations, space separated. Each
combination is in the form _mod1+mod2+key_. The names of the modifiers
and the key *must* be valid XKB key names.

Note that if *Shift* is one of the modifiers, the _key_ *must not* be
in upper case. For example, *Control+Shift+V* will never trigger, but
*Control+Shift+v* will.

Note that *Alt* is usually called *Mod1*.

*xkbcli interactive-wayland* can be useful for finding keysym names.

A key combination can only be mapped to *one* action. Lets say you
want to bind *Control+k* to *next*. Since this is the default shortcut
for *delete-line-forward*, you must unmap the default binding. This can be
done by setting _action=none_; e.g. *delete-line-forward=none*.

*cancel*
	Quit fuzzel without executing anything. Default: _Control+g
	Control+c Control+bracketleft Escape_.

*execute*
	Execute the currently selected entry. Or, in dmenu mode, print the
	selected entry on stdout. Default: _Return KP_Enter Control+y_.

*execute-or-next*
	If there is a single match, execute it. Or, in dmenu mode, print
	the selected entry on stdout. If there are more than one match,
	select the next entry. Wraps around when the last entry has been
	reached. Default: _Tab_.

*execute-input*
	Execute the raw input as is, regardless of whether it matches
	anything in the list or not. In dmenu mode, prints the selected
	entry on stdout instead. Default: _Shift+Return Shift+KP_Enter_.

*cursor-left*
	Moves the cursor one **character** to the left. Default: _Left
	Control+b_.

*cursor-left-word*
	Moves the cursor one **word** to the left. Default: _Control+Left
	Mod1+b_.

*cursor-right*
	Moves the cursor one **character** to the right. Default: _Right
	Control+f_.

*cursor-right-word*
	Moves the cursor one **word** to the right. Default:
	_Control+Right Mod1+f_.

*cursor-home*
	Moves the cursor to the beginning of the input. Default: _Home
	Control+a_.

*cursor-end*
	Moves the cursor to the end of the input. Default: _End
	Control+e_.

*delete-input*
	There is no standalone binding to clear the input. Use
	*cursor-home* (_Control+a_) to go into the beginning of the input,
	followed the *delete-line-forward* binding
	(_Control+k_). Alternatively, if you are at the end of the input,
	use *delete-line-backward* (_Control+u_). These are alternatives
	to using "select all" to highlight text (not supported by fuzzel)
	and then delete it.

*delete-line-backward*
	Deletes everything before the cursor. Default: _Control+u_.

*delete-line-forward*
	Deletes everything after the cursor. Default: _Control+k_.

*delete-prev*
	Deletes the **character before** the cursor. Default: _BackSpace Control+h_.

*delete-prev-word*
	Deletes the **word before** the cursor. Default: _Mod1+BackSpace
	Control+BackSpace Control+w_.

*delete-next*
	Deletes the **character after** the cursor. Default: _Delete_.

*delete-next-word*
	Deletes the **word after** the cursor. Default: _Mod1+d
	Control+Delete_.

*delete-line-forward*
	Deletes everything after the cursor. Default: _Control+k_.

*delete-line*
	Deletes the entire input. Default: _Control+Shift+BackSpace_.

*insert-selected*
	Copies the selected entry to the prompt, replacing the current
	prompt. In application mode, the *Exec* line is inserted, and in
	dmenu mode the entry itself is used. Default: _Control+Tab_.

*expunge*
	Removes the currently selected entry from the cache (of most
	commonly launched applications). Default: _Shift+Delete_.

*clipboard-paste*
	Pastes from the _clipbard_. Default: _Control+v_ _XF86Paste_.

*primary-paste*
	Pastes from the _primary selection_. This action is also bound to
	the middle mouse button (this is currently not configurable).
	Default: _Shift+Insert_ _Shift+KP_Insert_.

*prev*
	Select the previous entry. Does *not* wrap around when the first
	entry has been reached. Default: _Up Control+p_.

*prev-with-wrap*
	Select the previous entry. Wraps around when the first entry has
	been reached. Default: _ISO_Left_Tap_. (Shift+Tab)

*prev-page*
	Switch to the previous page. Default: _PageUp KP_PageUp_.

*next*
	Select the next entry. Does *not* wrap around when the last entry
	has been reached. Default: _Down Control+n_.

*next-with-wrap*
	Select the next entry. Wraps around when the last entry has
	been reached. Default: _unbound_.

*next-page*
	Switch to the next page. Default: _Page_Down KP_Page_Down_.

*first*
	Select the first entry, on the first page. Default:
	_Control+Home_.

*last*
	Select the last entry, on the last page. Default: _Control+End_.

*custom-1*, *custom-2*, ..., *custom-19*
	Execute the currently selected entry, then exit with a non-zero
	exit code. *custom-1* exits with exit code 10, *custom-2* with 11,
	*custom-3* with 12, and so on.

	Default:
		- custom-1: _Mod1+1_
		- custom-2: _Mod1+2_
		- custom-3: _Mod1+3_
		- custom-4: _Mod1+4_
		- custom-5: _Mod1+5_
		- custom-6: _Mod1+6_
		- custom-7: _Mod1+7_
		- custom-8: _Mod1+8_
		- custom-9: _Mod1+9_
		- custom-10: _Mod1+10_
		- custom-11: _Mod1+11_
		- custom-12: _Mod1+12_
		- custom-13: _Mod1+13_
		- custom-14: _Mod1+14_
		- custom-15: _Mod1+15_
		- custom-16: _Mod1+16_
		- custom-17: _Mod1+17_
		- custom-18: _Mod1+18_
		- custom-19: _Mod1+19_

# FONT CONFIG

Fontconfig uses a flexible string format for specifying fonts that
allows you to define font families, styles, weights, and various
properties. The basic syntax follows this pattern:

	"Family Name:style=Style:weight=Weight:size=Size"

Common Properties:

- *family* - Font family name
- *style* - italic, oblique, roman
- *weight* - light, normal, bold, black (or numeric values 100-900)
- *size* - Point size
- *slant* - italic, oblique, roman
- *width* - condensed, normal, expanded


Fontconfig supports fallback fonts. You can specify multiple font
families separated by commas. The fallback fonts are searched in the
order they appear. If a glyph cannot be found in any of the fallback
fonts, the dynamic fallback list from fontconfig (for the primary font)
is searched. Ex:

	"My Custom Font, DejaVu Sans:weight=bold, sans-serif"

Fontconfig always returns something unless you don't have any fonts installed,
regardless of what font name you give it. You can check fonts that would be matched
with `fc-match -s YourFontName`.

The font library Fuzzel uses, fcft, will search the explicit fallbacks whenever a glyph
cannot be found in the primary font. When it cannot find a glyph in the explicit
fallbacks, then it searches the fontconfig fallbacks of the first font in Fuzzel's list.

This design allows a custom fallback sequence as well as custom font options for each
fallback.

# SEE ALSO

- *fuzzel*(1)
- *https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html*
- *https://codeberg.org/dnkl/fuzzel*

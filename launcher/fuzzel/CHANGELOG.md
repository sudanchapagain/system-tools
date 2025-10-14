# Changelog

* [Unreleased](#unreleased)
* [1.13.1](#1-13-1)
* [1.13.0](#1-13-0)
* [1.12.0](#1-12-0)
* [1.11.1](#1-11-1)
* [1.11.0](#1-11-0)
* [1.10.2](#1-10-2)
* [1.10.1](#1-10-1)
* [1.10.0](#1-10-0)
* [1.9.2](#1-9-2)
* [1.9.1](#1-9-1)
* [1.9.0](#1-9-0)
* [1.8.2](#1-8-2)
* [1.8.1](#1-8-1)
* [1.8.0](#1-8-0)
* [1.7.0](#1-7-0)
* [1.6.5](#1-6-5)
* [1.6.4](#1-6-4)
* [1.6.3](#1-6-3)
* [1.6.2](#1-6-2)
* [1.6.1](#1-6-1)
* [1.6.0](#1-6-0)
* [1.5.4](#1-5-4)
* [1.5.3](#1-5-3)
* [1.5.2](#1-5-2)
* [1.5.1](#1-5-1)
* [1.5.0](#1-5-0)
* [1.4.2](#1-4-2)
* [1.4.1](#1-4-1)


## Unreleased
### Added

* Faster, more accurate SVG rendering with `resvg` backend. It does
  not require cairo and supports gamma correction. Packagers are
  advised to prefer this backend. The librsvg and nanosvg backends may
  be removed in a future release ([#650][650]).
* `--match-nth=N` command line option, allowing dmenu entries to be
  matched against text other than what is being displayed
  ([#589][589]).
* `--only-match` command line option, preventing the user from
  executing "custom" input in dmenu mode ([#651][651]).

[650]: https://codeberg.org/dnkl/fuzzel/issues/650
[589]: https://codeberg.org/dnkl/fuzzel/issues/589
[651]: https://codeberg.org/dnkl/fuzzel/issues/651

### Changed
### Deprecated
### Removed
### Fixed

* `--with-nth` and `--accept-nth` crashing if a column is empty
  (e.g. `\tsecond column`, or `first\t\tthird`) ([#658][658])
* Regression: `--accept-nth=0` and `--with-nth=0` not being treated as
  if not specified at all ([#670][670]).
* Improved scrolling with mouse wheel and touchpad ([#673][673]).

[658]: https://codeberg.org/dnkl/fuzzel/issues/658
[670]: https://codeberg.org/dnkl/fuzzel/issues/670
[673]: https://codeberg.org/dnkl/fuzzel/issues/673


### Security
### Contributors

 * Mark Stosberg

## 1.13.1

### Fixed

* Regression: input not being returned in dmenu mode ([#647][647])
* Regression: `--password` not working ([#653][653]).
* Regression: Part of the background transparency disappearing after
  typing ([#652][652]).

[647]: https://codeberg.org/dnkl/fuzzel/issues/647
[653]: https://codeberg.org/dnkl/fuzzel/issues/653
[652]: https://codeberg.org/dnkl/fuzzel/issues/652

### Contributors

 * Mark Stosberg
 * rehanzo

## 1.13.0

### Added

* Add support for fallback icons in dmenu mode using comma-separated
  values in the icon metadata ([#504][504]).
* `--namespace` command line option ([#512][512]).
* Gamma-correct blending. Disabled by default. Enable with the new
  `--gamma-correct` command line option, or by setting
  `gamma-correct-blending=yes` in `fuzzel.ini`. Note that
  gamma-correct blending **is not supported in cairo enabled builds of
  fuzzel**, due to the lack of 16-bit image buffers in cairo
  ([#546][546]).
* `scaling-filter` option (and the corresponding `--scaling-filter`
  command line option). This option defines which scaling filter
  fuzzel uses when down scaling PNGs ([#553][553]).
* `--minimal-lines` command line and config option, causing fuzzel to
  adjust the number of displayed lines to the minimum of `--lines` and
  the number of input lines ([#144][144])
* Touchscreen support ([#594][594])
* `{cmd}` placeholder support in the `terminal` option. When `{cmd}` is
  present in the terminal command string, it will be replaced with the
  application command instead of appending the command at the end. Useful
  for setting the title or app_id. ([#600][600])
* `--hide-prompt` command line option to hide the prompt line, reducing
   window size while still accepting input. This cannot be used
  simultaneously with `--prompt-only`.i  ([#597][597])
* `--auto-select` option. When one entry is left, automatically select
  it. ([#411][411])
* `selection-radius` option and `--selection-radius` flag to set the border
  radius of the selected entry
* `enable-mouse` option and `--no-mouse` flag to disable mouse input

[411]: https://codeberg.org/dnkl/fuzzel/issues/411
[504]: https://codeberg.org/dnkl/fuzzel/pulls/504
[512]: https://codeberg.org/dnkl/fuzzel/pulls/512
[546]: https://codeberg.org/dnkl/fuzzel/issues/546
[553]: https://codeberg.org/dnkl/fuzzel/issues/553
[144]: https://codeberg.org/dnkl/fuzzel/issues/144
[594]: https://codeberg.org/dnkl/fuzzel/issues/594
[597]: https://codeberg.org/dnkl/fuzzel/issues/597
[600]: https://codeberg.org/dnkl/fuzzel/issues/600


### Changed

* Scrolling speed is now line-by-line instead of page-by-page
  ([#586][586])
* Number of render workers is now automatically limited to the number
  of displayed entries.
* wayland-protocols >= 1.41 is now required.
* pixman >= 0.46.0 is now required.
* fcft >= 3.3.1 is now required.
* In application mode (not dmenu), fuzzel now ignores icon theme
  directories whose `Context` field is not `Application` or
  `Apps`, or `Legacy`. This is for performance reasons ([#553][553])
* Default down scaling filter changed from `lanczos3` to `box`
  ([#553][553]).
* Use the width of 'o' instead of 'W' when calculating the window
  width ([#560][560]).
* Default icon theme from `hicolor` to `default`.
* Error out (with an appropriate error message) if the command line we
  attempt to execute has non-specification-compliant quoting.
* Input text now "scrolls" with the cursor, if the string is longer
  than the available space ([#378][378]).
* When using the mouse, the currently selected item is only changed
  when the mouse is over an item. Before this release, the X
  coordinate was ignored ([#606][606]).

[560]: https://codeberg.org/dnkl/fuzzel/issues/560
[378]: https://codeberg.org/dnkl/fuzzel/issues/378
[606]: https://codeberg.org/dnkl/fuzzel/issues/606


### Fixed

* Crash when `--match-workers >= 100` ([#507][507]).
* Keyboard modifiers not being reset on keyboard leave events.
* `--with-nth`: non-selected columns still being matched.
* Match highlighting being offset when `--with-nth` is used
  ([#551][551]).
* Too wide PNGs bleeding into the text ([#313][313]).
* Lookup of icon themes in `~/.icons` and `/usr/share/pixmans`.
* Missing shell completions for `--launch-prefix`.
* Missing shell completions for `--prompt-only`.
* Empty window being displayed despite `--no-run-if-empty` being used.
* Fix border width and scaling for mixed-DPI environments ([#595][595])
* Some icons not found on FreeBSD (`/usr/local/share/pixmaps` not
  searched) ([#622][622])
* Segfault when "execute" action is pressed before there's any item in the
  list. ([#638][638])

[507]: https://codeberg.org/dnkl/fuzzel/issues/507
[551]: https://codeberg.org/dnkl/fuzzel/issues/551
[313]: https://codeberg.org/dnkl/fuzzel/issues/313
[586]: https://codeberg.org/dnkl/fuzzel/issues/586
[595]: https://codeberg.org/dnkl/fuzzel/issues/595
[622]: https://codeberg.org/dnkl/fuzzel/issues/622
[638]: https://codeberg.org/dnkl/fuzzel/issues/638


### Contributors

* alex-huff
* bbb651
* e-tho
* frosty
* Gingeh
* Guillaume Outters
* Hubert Hirtz
* joeledwardson
* Konstantin Pospelov
* Mark Stosberg
* Ronan Pigott
* wispl


## 1.12.0

### Added

* Nanosvg updated to ea6a6aca009422bba0dbad4c80df6e6ba0c82183
* `--select-index` command line option ([#447][447]).
* `--with-nth=N` command line option ([#438][438]).
* `--accept-nth=N` command line option ([#493][493]).
* `keyboard-focus=exclusive|on-demand` command line and config option
  ([#495][495]).
* New key binding: `delete-line` corresponding to C-S-Backspace in Emacs,
  Which deletes the entire input ([#472][472]).

[447]: https://codeberg.org/dnkl/fuzzel/issues/447
[438]: https://codeberg.org/dnkl/fuzzel/issues/438
[493]: https://codeberg.org/dnkl/fuzzel/pulls/493
[495]: https://codeberg.org/dnkl/fuzzel/pulls/495
[472]: https://codeberg.org/dnkl/fuzzel/pulls/472

### Changed

* Take the matched sub-string's position into account when sorting
  matches; entries where the matched sub-string is closer to the
  beginning of the entry is sorted before entries where the sub-string
  is closer to the end of the entry ([#419][419]).
* "Too large" PNG icons are now downscaled using a less precise
  scaling filter, to improve performance ([#439][439]).
* Icon themes: 'scaled' directories are now ignored ([#459][459]).
* Border radius limited by `max(horizontal-pad, vertical-pad)`,
  instead of `min(horizontal-pad, vertical-pad)` ([#436][436]).
* `stdout` and `stderr` of the launched application are now preserved,
  instead of being redirected to `/dev/null` ([#480][480]).
* Version 5 of the `wl_seat` interface is now required.
* The `pixmaps` sub-directory is no longer searched for in every
  `XDG_DATA_DIR`; only `/usr/share/pixmaps`.

[419]: https://codeberg.org/dnkl/fuzzel/issues/419
[439]: https://codeberg.org/dnkl/fuzzel/issues/439
[459]: https://codeberg.org/dnkl/fuzzel/issues/459
[436]: https://codeberg.org/dnkl/fuzzel/issues/436
[480]: https://codeberg.org/dnkl/fuzzel/issues/480


### Fixed

* Custom line heights smaller than the font's actual line height
  causing visual glitches in the prompt.
* Wayland protocol error: _"layer surface has never been configured"_
  ([#426][426]).
* Entries with `NoDisplay=True` being included in the total (entry)
  count.
* Crash if compositor sends a key event without having sent a keymap
  event before that ([#482][482]).
* Filenames matching up to the first period only ([#487][487]).
* `wl_seat_release()` used while only binding version 4 of the seat
  interface ([#498][498]).
* Double close of an FD, causing occasional failures to start fuzzel
  ([#497][497]).
* `$HOME/.icons` not searched.

[426]: https://codeberg.org/dnkl/fuzzel/issues/426
[482]: https://codeberg.org/dnkl/fuzzel/issues/482
[487]: https://codeberg.org/dnkl/fuzzel/issues/487
[498]: https://codeberg.org/dnkl/fuzzel/issues/498
[497]: https://codeberg.org/dnkl/fuzzel/issues/497


### Contributors

* Ashish SHUKLA
* bbb651
* Christian Fillion
* Craig Barnes
* Mark Stosberg
* Peter Colberg
* Sertonix
* Wekuz


## 1.11.1

### Added

* New option `--hide-before-typing` for application launcher mode hides
  the list until the first character is typed ([#351][351])


### Changed

* Logging no longer reports "aborted" if dmenu mode is called with no
  input ([#406][406]).


### Fixed

* Crash when the border size is larger than the border radius
  ([#401][401]).
* Regression: cursor not centered when using a custom line height
  ([#403][403]).
* Regression: selected background not rendered on top of the regular
  background ([#405][405]).
* Regression: Supporting `XDG_CONFIG_HOME` as absolute path but not
  relative should be supported ([#408][408])

[351]: https://codeberg.org/dnkl/fuzzel/issues/351
[401]: https://codeberg.org/dnkl/fuzzel/issues/401
[403]: https://codeberg.org/dnkl/fuzzel/issues/403
[405]: https://codeberg.org/dnkl/fuzzel/issues/405
[406]: https://codeberg.org/dnkl/fuzzel/issues/406
[408]: https://codeberg.org/dnkl/fuzzel/issues/408


### Contributors

* @ever
* @ingolemo
* @otaj
* @thoralf
* @tranzystorekk
* Mark Stosberg


## 1.11.0

### Added

* Nanosvg updated to 93ce879dc4c04a3ef1758428ec80083c38610b1f
* New options `--x-margin` `--y-margin` which place the launcher some
  distance away from the anchor point, in pixels. Default to 0
  ([#344][344]).
* Support for the `StartupNotify` key in `.desktop` files.
* Log output now respects the [`NO_COLOR`](http://no-color.org/)
  environment variable.
* Rounded corners without cairo. With this, the **only** time cairo is
  needed is when you want to use the librsvg SVG backend. For nanosvg
  builds, there is no longer any need to link against cairo.
* Support for linking against a system provided nanosvg library. See
  the new `-Dsystem-nanosvg` meson option. Default's to `disabled`
  (i.e. use the bundled version).
* Mouse support. Left click selects/launches, right click quits
  fuzzel, wheel scrolls (a page at at time). This is, for the time
  being, not configurable ([#131][131])
* `--cache=PATH` command line option, and `cache` option to
  `fuzzel.ini`, allowing a custom cache location to be used
  ([#189][189], [#219][219]).
* `expunge` key binding, to remove an entry from the cache. Mapped to
  shift+delete by default.
* `--prompt-color` and `colors.prompt`, controlling the
  text/foreground color of the prompt ([#365][365]).
* `--input-color` and `colors.input`, controlling the text/foreground
  color of the input string ([#365][365]).
* Much improved performance with large amounts of input ([#305][305]).
* Improved rendering performance, by using threads. See the new
  `--render-workers` command line option, and the new `render-workers`
  option in `fuzzel.ini`.
* dmenu mode: `--prompt-only=PROMPT` command line option
  ([#276][276]).
* dmenu mode: start rendering input before STDIN has been closed.
* `--match-workers` command line option and the new `match-workers`
  option in `fuzzel.ini`.
* `delayed-filter-ms`, `delayed-filter-limit` options to `fuzzel.ini`,
  and `--delayed-filter-ms` and `--delayed-filter-limit` command line
  options.
* Match count can now printed at the right-hand side of the input
  prompt. It is disabled by default, and can be enabled with either
  the `--counter` command line option, or the `match-counter` setting
  in `fuzzel.ini`.
* `--counter-color` and `colors.counter`, controlling the color of the
  match count. The default value is `93a1a1ff` (_base1_ in the
  solarized palette).
* The currently selected entry may now be rendered with a bold
  font. It is disabled by default, and can be enabled either via the
  new `--use-bold` command line option, or by setting `use-bold=yes`,
  in `fuzzel.ini`.
* `sort-result` option to `fuzzel.ini`, and `--no-sort` command line
  option.
* Placeholder text (for the input box), controlled by the new
  `--placeholder`, `--placeholder-color` command line options, and the
  `placeholder` and `colors.placeholder` options in `fuzzel.ini`
  ([#188][188]).
* `--search=TEXT` command line option, allowing you to "pre-filter"
  the result ([#][274][274]).
* Support for pasting text into fuzzel. Both the regular clipboard,
  and the primary clipboard are supported. See the new
  `clipboard-paste` and `primary-paste` key bindings ([#200][200]).
* `hide-before-typing` command line and config option, hiding the
  application list until the first letter is typed. ([#351][351]).

[344]: https://codeberg.org/dnkl/fuzzel/issues/344
[131]: https://codeberg.org/dnkl/fuzzel/issues/131
[189]: https://codeberg.org/dnkl/fuzzel/issues/189
[219]: https://codeberg.org/dnkl/fuzzel/issues/219
[365]: https://codeberg.org/dnkl/fuzzel/issues/365
[305]: https://codeberg.org/dnkl/fuzzel/issues/305
[276]: https://codeberg.org/dnkl/fuzzel/issues/276
[188]: https://codeberg.org/dnkl/fuzzel/issues/188
[274]: https://codeberg.org/dnkl/fuzzel/issues/274
[200]: https://codeberg.org/dnkl/fuzzel/issues/200
[351]: https://codeberg.org/dnkl/fuzzel/issues/351


### Changed

* The cache now stores desktop file **IDs** instead of the application
  titles. This way, we do not store multiple cache entries with the
  same value (title) ([#339][339]).
* Always fallback on the icon theme `hicolor`.
* Quitting without executing an entry in dmenu mode now exits with
  code 2 instead of 1 ([#353][353]).
* The default `layer` is now `overlay` instead of `top`. This means
  fuzzel now renders on top of fullscreen windows by default
  ([#81][81]).
* `--no-fuzzy` option has been replaced with `--match-mode`.
  Default match mode is `fzf`. Other options are `fuzzy` and `exact`. See
  `man fuzzel.ini` for details.
  ([#305][305]).
* Default text color of the prompt and the selected entry to
  `586e75ff` (_base01_ in the solarized palette).
* Initial application sorting is now done case insensitive.
* Background is no longer transparent by default. You can change this
  by setting `colors.background`.
* Search string is now font shaped, improving support for e.g Arabic
  ([#398][398]).

[339]: https://codeberg.org/dnkl/fuzzel/issues/339
[353]: https://codeberg.org/dnkl/fuzzel/issues/353
[81]: https://codeberg.org/dnkl/fuzzel/issues/81
[398]: https://codeberg.org/dnkl/fuzzel/issues/398


### Fixed

* PNG images being way too dark.
* Crash when the cache contains strings that are not valid in the
  current locale ([#337][337]).
* Crash when `tabs` (in `fuzzel.ini`) is set to `0` ([#348][348]).
* Crash while loading the cache, when `--list-executables-in-path` is
  used ([#356][356])
* Rounding of window size when fractional scaling is used.
* Dmenu mode failing with _"failed to read from stdin: Resource
  temporarily unavailable"_.
* First frame flickers when fractional scaling is used.
* Borders, padding etc not updated on scale changes when
  `dpi-aware=yes`. This mostly affected setups using fractional
  scaling, but all setups were affected in one way or another.
* Mouse selection not working correctly (wrong item selected) when
  `dpi-aware=yes` and desktop scaling was enabled.

[337]: https://codeberg.org/dnkl/fuzzel/issues/337
[348]: https://codeberg.org/dnkl/fuzzel/issues/348
[356]: https://codeberg.org/dnkl/fuzzel/issues/356


### Contributors

* Aaron Lichtman
* alz
* Jordi Estrada
* Leonardo Hernández Hernández
* Mark Stosberg
* Ronan Pigott
* triallax


## 1.10.2

### Fixed

* Crash when `terminal=yes` (in `fuzzel.ini`), and a `.desktop` file
  has `Terminal=True` but no `Exec` key ([#331][331]).
* `--anchor=center` not working on some compositors ([#330][330]).

[331]: https://codeberg.org/dnkl/fuzzel/issues/331
[330]: https://codeberg.org/dnkl/fuzzel/issues/330


## 1.10.1

### Fixed

* Crash when executing the command line as is, i.e. when there is no
  matching entry.
* Crash when parsing a `.desktop` file with lines beginning with
  whitespace ([#328][328], [#329][329]).


[328]: https://codeberg.org/dnkl/fuzzel/issues/328
[329]: https://codeberg.org/dnkl/fuzzel/issues/329


## 1.10.0

### Added

* Support for the `cursor-shape-v1` Wayland protocol.
* New option `--anchor` allows setting the window position anchor
  (i.e. where on the screen the window should generally be
  positioned), such as `top`, `top-left`, `bottom-right`, `center`,
  etc. Defaults to `center` ([#130][130]).
* `--check-config` command line option ([#264][264]).
* New key binding: `execute-input` (mapped to shift+return by
  default). This key binding executes the raw input as is, regardless
  of whether it matches anything in the list or not ([#252][252]).
* `--select=STRING` command line option. Selects the first entry that
  matches the given string ([#237][237]).
* `include=<path>` option to `fuzzel.ini` ([#205][205]).
* New option `--list-executables-in-path` add executables presents in
  the $PATH variable to the list ([#284][284]).
* New key binding: `delete-line-backward` which corresponds to C-u in bash.
  Also renames `delete-line` to `delete-line-forward` ([#307][307]).
* The ID of the selected `.desktop` file, and executed command line
  are now logged, at info level ([#302][302]).
* Support for `wp_fractional_scale_v1` (i.e. true fractional scaling)
  ([#320][320]).

[130]: https://codeberg.org/dnkl/fuzzel/issues/130
[264]: https://codeberg.org/dnkl/fuzzel/issues/264
[252]: https://codeberg.org/dnkl/fuzzel/issues/252
[237]: https://codeberg.org/dnkl/fuzzel/issues/237
[205]: https://codeberg.org/dnkl/fuzzel/issues/205
[284]: https://codeberg.org/dnkl/fuzzel/pulls/284
[307]: https://codeberg.org/dnkl/fuzzel/pulls/307
[302]: https://codeberg.org/dnkl/fuzzel/issues/302
[320]: https://codeberg.org/dnkl/fuzzel/issues/320


### Changed

* Minimum required version of _wayland-protocols_ is now 1.32
* Selection color is now painted over background color ([#255][255]).
* Exact matches (of the application title) are now sorted first
  ([#259][259]).
* Set default log level to warning ([#266][266]).
* Rename `delete-line` binding to `delete-line-forward` ([#307][307]).
* `password-character` can now be set to the empty string
  ([#263][263]).

[255]: https://codeberg.org/dnkl/fuzzel/issues/255
[259]: https://codeberg.org/dnkl/fuzzel/issues/259
[266]: https://codeberg.org/dnkl/fuzzel/pulls/266
[263]: https://codeberg.org/dnkl/fuzzel/issues/263


### Fixed

* Ignore whitespace in `.desktop` files’ key and name values
  ([#248][248]).
* Desktop entries with `NoDisplay=true` being ignored unless they also
  had `Name` and `Exec` set ([#253][253]).
* Crash when compositor sends a `keyboard::modifiers` event without
  first sending a `keyboard::keymap` event (with a valid keymap)
  ([#293][293]).
* Quoted empty (`""`) arguments being ignored ([#285][285]).
* Log-level not respected by syslog.

[248]: https://codeberg.org/dnkl/fuzzel/issues/248
[253]: https://codeberg.org/dnkl/fuzzel/issues/253
[293]: https://codeberg.org/dnkl/fuzzel/issues/293
[285]: https://codeberg.org/dnkl/fuzzel/issues/285


### Contributors

* Alexander Orzechowski
* Grzegorz Szymaszek
* Gurvan
* Jakub Fišer
* Mark Stosberg
* Mehrad Mahmoudian
* otaj
* Sertonix
* tet
* Thomas Voss
* Zi How Poh


## 1.9.2

### Added

* Added a new option `--filter-desktop` which toggles filtering of desktop
  entries based on the OnlyShowIn and NotShowIn keys. Filtering is based on the
  value of $XDG\_CURRENT\_DESKTOP according to desktop-entry spec. Filtering is
  off by default. To disable filtering set in the config from the command line,
  use --filter-desktop=no


### Changed

* Output scaling is now applied to the border radius ([#236][236]).

[236]: https://codeberg.org/dnkl/fuzzel/issues/236


### Fixed

* Last line sometimes not being rendered ([#234][234]).
* `key-bindings.cursor-right-word` not being recognized as a valid
  action.
* `password-character` being set in `fuzzel.ini` incorrectly enabling
  password mode ([#241][241]).
* Added missing zsh+fish completions for `--password`.

[234]: https://codeberg.org/dnkl/fuzzel/issues/234
[241]: https://codeberg.org/dnkl/fuzzel/issues/241


### Contributors

* complex2liu
* Mark Stosberg
* Ronan Pigott


## 1.9.1

### Fixed

* Regression: default font size was unintentionally changed from 12pt
  in 1.8.2, to 8pt in 1.9.0. The old default of 12pt has now been
  restored.
* Regression: crash when pressing Enter and the match list is empty (e.g. when
  trying to execute a command line) ([#222][222]).

[222]: https://codeberg.org/dnkl/fuzzel/issues/222


## 1.9.0

### Added

* Add support for startup notifications via xdg activation ([#195][195])
* Convert tabs to spaces when rendering ([#137][137]).
* `--dmenu0` command line option. Like `--dmenu`, but input is NUL
  separated instead of newline separated ([#197][197]).
* Support for localized strings. If you want the old behavior, run
  `fuzzel` with `LC_MESSAGES=C` ([#199][199]).
* Export `FUZZEL_DESKTOP_FILE_ID` environment variable when setting
  the `--launch-prefix` in order to pass the Desktop File ID to the
  launch prefix ([#110][110]).
* New key bindings: `[key-bindings].first` and `[key-bindings].last`,
  bound to `Control+Home` and `Control+End` by default ([#210][210]).
* New key binding: `[key-bindings].insert-selected`, bound to
  `Control+Tab` by default. It replaces the current prompt (filter)
  with the selected item ([#212][212]).

[195]: https://codeberg.org/dnkl/fuzzel/pulls/195
[137]: https://codeberg.org/dnkl/fuzzel/issues/137
[197]: https://codeberg.org/dnkl/fuzzel/issues/197
[199]: https://codeberg.org/dnkl/fuzzel/issues/199
[110]: https://codeberg.org/dnkl/fuzzel/issues/110
[210]: https://codeberg.org/dnkl/fuzzel/issues/210
[212]: https://codeberg.org/dnkl/fuzzel/issues/212


### Changed

* Better verification of color values specified on the command line
  ([#194][194]).
* When determining initial font size, do FontConfig config
  substitution if the user-provided font pattern has no {pixel}size
  option ([#1287][foot-1287]).

[194]: https://codeberg.org/dnkl/fuzzel/issues/194
[foot-1287]: https://codeberg.org/dnkl/foot/issues/1287


### Fixed

* Update nanosvg to f0a3e10. Fixes rendering of certain SVG icons
  ([#190][190]).
* Not being able to input numbers using the keypad ([#192][192]).
* Absolute path PNG icons not being loaded ([#214][214]).

[190]: https://codeberg.org/dnkl/fuzzel/issues/190
[192]: https://codeberg.org/dnkl/fuzzel/issues/192
[214]: https://codeberg.org/dnkl/fuzzel/issues/214


### Contributors

* Mark Stosberg
* Max Gautier
* Ronan Pigott


## 1.8.2

### Added

* Fish completions ([#176][176])

[176]: https://codeberg.org/dnkl/fuzzel/issues/176


### Fixed

* Unsupported icon formats not being skipped when loading application
  icons.
* Wrong size of PNG icons selected ([#182][182])

[182]: https://codeberg.org/dnkl/fuzzel/issues/182


## 1.8.1

### Fixed

* Regression: not able to input text with modifiers (e.g. Shift)
  pressed ([#177][177]).

[177]: https://codeberg.org/dnkl/fuzzel/issues/177


## 1.8.0

### Added

* Support for file based configuration ([#3][3]).
* Customizable key bindings ([#117][117]).
* "Custom" key bindings (like Rofi’s `kb-custom-N` key
  bindings).
* If `argv[0]` is _dmenu_, fuzzel now starts in dmenu mode
  ([#107][107]).
* `--password=[CHARACTER]` command line option. Intended to be used
  with “password input”; all typed text is rendered as _CHARACTER_,
  defaulting to `*` if _CHARACTER_ is omitted ([#108][108]).
* `Ctrl+y` binding to execute selected entry.
* `Ctrl+j`/`Ctrl+k` binding to move to the next/previous item
  ([#120][120]).
* Escape sequences in `Exec` arguments are now supported.
* Quoted environment variables in `Exec` arguments are now supported ([#143][143]).
* Multiple space-separated search words can now be entered at the prompt.
* `-M,--selection-match-color`, that lets you configure the color of
  matched substrings of the currently selected item
* New config option `image-size-ratio`, allowing you to control the
  size of the large image displayed when there are only a “few”
  matches.
* Support for icons in dmenu mode, using Rofi’s extended dmenu
  protocol ([#166][166]).
* `--layer` command line option, allowing you to choose which layer to
  render the fuzzel window on (`top` or `overlay`) ([#81][81]).
* `--no-exit-on-keyboard-focus-loss` command line option
  (`exit-on-keyboard-focus-loss` config option) ([#128][128]).

[3]: https://codeberg.org/dnkl/fuzzel/issues/3
[117]: https://codeberg.org/dnkl/fuzzel/issues/117
[107]: https://codeberg.org/dnkl/fuzzel/issues/107
[108]: https://codeberg.org/dnkl/fuzzel/issues/108
[120]: https://codeberg.org/dnkl/fuzzel/issues/120
[143]: https://codeberg.org/dnkl/fuzzel/issues/143
[166]: https://codeberg.org/dnkl/fuzzel/issues/166
[81]: https://codeberg.org/dnkl/fuzzel/issues/81
[128]: https://codeberg.org/dnkl/fuzzel/issues/128


### Changed

* `-i` is now **ignored**. This is to increase compatibility with
  other similar utilities. To set the icon theme, either use the long
  option (`--icon-theme=THEME`), or set it in the configuration file
  (default: `$XDG_CONFIG_HOME/fuzzel/fuzzel.ini`) ([#149][149]).
* Minimum required meson version is now 0.58.
* libpng warnings are now routed through fuzzel’s logging
  ([#101][101]).
* Nanosvg is now the default SVG backend. librsvg is still supported,
  and can be used by setting the `-Dsvg-backend=librsvg` meson option.
* It is no longer necessary to close stdin when using fuzzel in dmenu
  mode, as long as `--no-run-if-empty` is **not** being used
  ([#106][106]).
* Improved performance of initial rendering of icons ([#124][124]).
* `--terminal` now defaults to `$TERMINAL -e`.
* Font shaping is now applied to the prompt
* The large image displayed when there are only a “few” matches is now
  smaller by default.
* Swapped meaning of the command line options `-p` and `-P`; `-p` is
  now the short option for `--prompt` ([#146][146]).
* Do not add icon-sized padding on the left size in dmenu mode
  ([#158][158]).
* Color config values are now allowed to be prefixed with `#`
  ([#160][160]).


[149]: https://codeberg.org/dnkl/fuzzel/issues/149
[101]: https://codeberg.org/dnkl/fuzzel/issues/101
[106]: https://codeberg.org/dnkl/fuzzel/issues/106
[124]: https://codeberg.org/dnkl/fuzzel/issues/124
[146]: https://codeberg.org/dnkl/fuzzel/issues/146
[158]: https://codeberg.org/dnkl/fuzzel/issues/158
[160]: https://codeberg.org/dnkl/fuzzel/issues/160


### Fixed

* User `.desktop` entries with `NoDisplay=true` not overriding system
  entries ([#114][114]).
* Icon lookup is now better at following the XDG specification.
* Backspace removes not only the previous character, but also
  everything **after** the cursor.
* Crash on exit in dmenu mode when selection list is empty.
* Keypad `enter` not executing the selected entry ([#138][138])

[114]: https://codeberg.org/dnkl/fuzzel/issues/114
[138]: https://codeberg.org/dnkl/fuzzel/issues/138


### Contributors

* Chinmay Dalal
* Matthew Toohey
* Michael Yang
* Eyeoglu


## 1.7.0

### Added

* `-F,--fields=FIELDS` command line option, allowing you to select
  which XDG Desktop Entry fields to match against
  ([#63](https://codeberg.org/dnkl/fuzzel/issues/63)).
* Support for desktop entry actions
  ([#71](https://codeberg.org/dnkl/fuzzel/issues/71)).
* Fuzzy matching. This is enabled by default, but can be disabled with
  `--no-fuzzy`. When enabled, the fuzziness can be adjusted with
  `--fuzzy-max-length-discrepancy` and `--fuzzy-max-distance`
  ([#56](https://codeberg.org/dnkl/fuzzel/issues/56)).
* `--index` (dmenu mode only): print selected entry’s index instead of
  its text ([#88](https://codeberg.org/dnkl/fuzzel/issues/88)).
* `--log-level=info|warning|error|none` command line option
  ([#34](https://codeberg.org/dnkl/fuzzel/issues/34)).
* `--log-no-syslog` command line option.
* `--log-colorize=auto|never|always` command line option.


### Changed

* Fuzzel now refuses to start if there is another fuzzel instance
  running ([#57](https://codeberg.org/dnkl/fuzzel/issues/57)).
* Treat "Apps" as valid context for applications to support more
  icon themes (for example, Faenza)
* The `Name` entry of the desktop files are no longer used as unique
  identifiers. Instead, we now generate the “desktop file ID”
  according to the XDG desktop entry specification, and use that as ID
  ([#68](https://codeberg.org/dnkl/fuzzel/issues/68)).
* All XDG data directories are now searched when loading an
  icon. Previously, only XDG data directories where the theme
  directory contained an `index.theme` file were searched
  ([#62](https://codeberg.org/dnkl/fuzzel/issues/62)).
* Pressing Tab when there is a single match now executes it
  ([#77](https://codeberg.org/dnkl/fuzzel/issues/77)).
* Use a lock file instead of parsing `/proc` to prevent multiple
  fuzzel instances from running at the same time
  ([#84](https://codeberg.org/dnkl/fuzzel/issues/84)).
* The application list is now populated in a separate thread, in
  parallel to initializing the GUI. This reduces the risk of missing
  keyboard input ([#82](https://codeberg.org/dnkl/fuzzel/issues/82)).
* Icons are now loaded in a thread. This allows us to display the
  application list quickly (initially without icons, if loading them
  takes “too” long).
* Fuzzel now exits with exit code 0 when the non-dmenu launcher is
  aborted (no application has been launched) by the user
  ([#98](https://codeberg.org/dnkl/fuzzel/issues/98)).


### Fixed

* Long entries overrunning the right side padding
  ([#80](https://codeberg.org/dnkl/fuzzel/issues/80)).
* Tab and Shift+Tab not wrapping around
  ([#78](https://codeberg.org/dnkl/fuzzel/issues/78)).
* Visual glitches in the corners, when using rounded corners
  ([#90](https://codeberg.org/dnkl/fuzzel/issues/90)).
* Regression: `--dmenu --lines=0` crashing
  ([#92](https://codeberg.org/dnkl/fuzzel/issues/92)).


### Contributors

* yangyingchao
* ReplayCoding


## 1.6.5

### Added

* `--dpi-aware=no|yes|auto` command line option.
* Multi-page view ([#42](https://codeberg.org/dnkl/fuzzel/issues/42)).


### Removed

* Misleading error message about a non-existing cache file
  ([#59](https://codeberg.org/dnkl/fuzzel/issues/59)).


### Fixed

* Window quickly resized when launched
  ([#21](https://codeberg.org/dnkl/fuzzel/issues/21)).
* Layer surface being committed before configure event has been ack:ed.


## 1.6.4

### Added

* Support for [nanosvg](https://github.com/memononen/nanosvg) as an
  alternative SVG backend. Nanosvg is bundled with fuzzel and has
  **no** additional dependencies. This means you can now have SVGs
  without depending on Cairo.


### Changed

* Meson option `-Denable-png` replaced with `-Dpng-backend=none|libpng`.
* Meson option `-Denable-svg` replaced with `-Dsvg-backend=none|librsvg|nanosvg
* fcft >= 3.0 is now required.
* `-f,--font` now supports explicit font fallbacks.


### Fixed

* Graphical corruption triggered by the “gerbview” SVG icon, from
  KiCAD ([#47](https://codeberg.org/dnkl/fuzzel/issues/47)).
* SVG icons containing multiple icons not being limited to the main
  icon ([#48](https://codeberg.org/dnkl/fuzzel/issues/48)).


## 1.6.3

### Added

* `-P,--prompt` command line option, allowing you to set a custom
  prompt.


### Changed

* `-f,--font` now supports explicit font fallbacks.


### Fixed

* Removed usage of deprecated function `rsvg_handle_get_dimensions()`
  when building against recent versions of librsvg
  ([#45](https://codeberg.org/dnkl/fuzzel/issues/45)).


### Contributors

* [bapt](https://codeberg.org/bapt)


## 1.6.2

### Added

* `-s,--selection-text-color` command line option, that lets you
  configure the foreground/text color of the currently selected item
  ([#37](https://codeberg.org/dnkl/fuzzel/issues/37)).


### Changed

* Use `rsvg_handle_render_document()` instead of
  `rsvg_handle_render_cairo()` on libsrvg >= 2.46, since the latter
  has been deprecated ([#32](https://codeberg.org/dnkl/fuzzel/issues/32)).


### Fixed

* Icons not being searched for in all icon theme instances
* Crash when XKB compose file is missing
  ([#35](https://codeberg.org/dnkl/fuzzel/issues/35)).


## 1.6.1

### Fixed

* Wrong font being used for some entries if guessing monitor fuzzel
  will appear on, and guessing wrong
  ([#31](https://codeberg.org/dnkl/fuzzel/issues/31)).


## 1.6.0

### Added

* Text shaping support ([#20](https://codeberg.org/dnkl/fuzzel/issues/20)).
* Option for vertical padding between prompt and match list.


### Changed

* fcft >= 2.4.0 is now required.
* In dmenu mode, fuzzel now prints the keyboard input as is, if it
  does not match any of the items
  ([#23](https://codeberg.org/dnkl/fuzzel/issues/23)).
* The `.desktop` filename is now also matched against
  ([#25](https://codeberg.org/dnkl/fuzzel/issues/25)).


### Fixed

* Set initial subpixel mode correctly when there is only one monitor.
* Crash when `~/.cache/fuzzel` contained invalid/corrupt entries.


### Contributors

* [loserMcloser](https://codeberg.org/loserMcloser)


## 1.5.4

### Fixed

* Icon size calculation with scaling factors > 1


## 1.5.3

### Fixed

* Compilation when both PNGs and SVGs have been disabled.


## 1.5.2

### Changed

* Maximum icon height reduced, from the `line height`, to the `line
  height` minus the font's `descent`. This ensures a margin between
  icons.


### Fixed

* Crash when compositor provided bad monitor geometry data
  ([#17](https://codeberg.org/dnkl/fuzzel/issues/17)).


## 1.5.1

### Fixed

* Regression: border not being rendered when `--border-radius=0`, or
  if fuzzel was built without cairo
  ([#15](https://codeberg.org/dnkl/fuzzel/issues/15)).


## 1.5.0

### Added

* meson option `-Denable-svg=[auto|enabled|disabled]`. When disabled,
  _librsvg_ is no longer a dependency and SVG icons are
  disabled. Default: `auto`.
* meson option `-Denable-png=[auto|enabled|disabled]`. When disabled,
  _libpng_ is no longer a dependency and PNG icons are
  disabled. Default: `auto`.
* meson option `-Denable-cairo=[auto|enabled|disabled]`. When
  disabled, fuzzel will not be able to draw rounded corners, nor
  support SVGs (regardless of what `-Denable-svg` is set to)
  ([#10](https://codeberg.org/dnkl/fuzzel/issues/10)).
* `-I,--no-icons` command line option; disables all icons
  ([#12](https://codeberg.org/dnkl/fuzzel/issues/12))
* FreeBSD port.
* `-x,--horizontal-pad` and `-y,--vertical-pad` command line options
  ([#12](https://codeberg.org/dnkl/fuzzel/issues/12)).
* `--line-height` and `-letter-spacing` command line options
  ([#12](https://codeberg.org/dnkl/fuzzel/issues/12)).


### Changed

* PNGs are now loaded and rendered with _libpng_ instead of _cairo_.


### Fixed

* Wrong colors when not fully opaque.
* Rendering of SVGs containing multiple icons.
* Font being incorrectly scaled on rotated monitors.
* PPI being calculated incorrectly.
* Crash on keyboard input when repeat rate was zero (i.e. no repeat).


### Contributors

* [magenbluten](https://codeberg.org/magenbluten)
* jbeich


## 1.4.2

### Fixed

* Subpixel antialiasing was not applied correctly on opaque
  backgrounds.


## 1.4.1

### Fixed

* Incorrect extension for man pages.

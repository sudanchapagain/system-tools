# Changelog

* [Unreleased](#unreleased)
* [1.8.0](#1-8-0)
* [1.7.1](#1-7-1)
* [1.7.0](#1-7-0)
* [1.6.0](#1-6-0)
* [1.5.0](#1-5-0)
* [1.4.1](#1-4-1)
* [1.4.0](#1-4-0)
* [1.3.0](#1-3-0)
* [1.2.1](#1-2-1)
* [1.2.0](#1-2-0)
* [1.1.2](#1-1-2)
* [1.1.1](#1-1-1)
* [1.1.0](#1-1-0)
* [1.0.1](#1-0-1)
* [1.0.0](#1-0-0)


## Unreleased
### Added
### Changed
### Deprecated
### Removed
### Fixed
### Security
### Contributors


## 1.8.0

### Added

* `progress-style` configuration option with possible values of `bar`
  (previous behavior) and `background` which shows the progress as the
  background color.
* Fish shell completions.


### Changed

* Default icon theme from `hicolor` to `default`.


### Fixed

* Crash when `max-icon-size=0` ([#145][145]).
* Parse sections in order. This fixes a bug where sections before the
  main section get ignored. ([#137][137]).
* `fnottctl pause|unpause` exiting with an error.
* Lookup of icon themes in `~/.icons` and `/usr/share/pixmaps`.

[145]: https://codeberg.org/dnkl/fnott/issues/145
[137]: https://codeberg.org/dnkl/fnott/issues/137


### Contributors

* e-tho
* ldev


## 1.7.1

### Added

* `anchor` can now be set to `center`.
* Nanosvg updated to ea6a6aca009422bba0dbad4c80df6e6ba0c82183


### Fixed

* Messages sent directly after starting fnott (for example, when fnott
  is auto-activated by D-Bus) not processed until more D-Bus messages
  are received later.


## 1.7.0

### Added

* Log output now respects the [`NO_COLOR`](http://no-color.org/)
  environment variable.
* `border-radius` configuration option (yes, this means fnott now
  supports rounded corners).
* Support for linking against a system provided nanosvg library. See
  the new `-Dsystem-nanosvg` meson option. Defaults to `disabled`
  (i.e. use the bundled version).
* Support for the `x-canonical-private-synchronous` hint.
* XDG activation support; when triggering an action, fnott attempts to
  retrieve an XDG activation token. This will only succeed if the
  cursor is inside the notification window. The token is then
  signaled over the D-Bus _Notifications_ interface.
* `fnottctl dismiss-with-default-action`.
* Implemented the `org.freedesktop.DBus.Introspectable`
  interface. This fixes an issue where e.g. `gdbus` was not able to
  create, or close, notifications.


### Changed

* Left clicking a notification now triggers the default action, if
  any, in addition to dismissing the notification. Right click to
  dismiss the notification without trigger the default action.
* `STRING:image-path` hint that points to either a non-existing file,
  or an invalid image, will now be ignored (instead of removing the
  notification's icon).
* All notifications are now dismissed
  (i.e. `org.freedesktop.Notifications.NotificationClosed` is
  signaled) when fnott exits.


### Fixed

* `reason` in the `NotificationClosed` signal being off-by-one.
* Icons loaded via `image-data` hints being too dark.
* Not all data being read from the action selection helper, under
  certain circumstances.


### Contributors

* Evyatar Stalinsky
* ldev


## 1.6.0

### Added

* `selection-helper-uses-null-separator=yes|no` to `fnott.ini`. This
  can be used to e.g. improve handling of action strings with newlines
  in them ([#113][113]).
* `scaling-filter` to `fnott.ini`, allowing you to choose which
  scaling filter to use when scaling non-SVG notification images. The
  default is `lanczos3` ([#103][103]).
* Nanosvg updated to 93ce879dc4c04a3ef1758428ec80083c38610b1f
* D-Bus service file for starting automatically ([#102][102])
* Systemd unit file ([#48][48])

[113]: https://codeberg.org/dnkl/fnott/issues/113
[103]: https://codeberg.org/dnkl/fnott/issues/103
[102]: https://codeberg.org/dnkl/fnott/pulls/102
[48]: https://codeberg.org/dnkl/fnott/issues/48


### Changed

* `layer` is now per-urgency ([#112][112]).

[112]: https://codeberg.org/dnkl/fnott/issues/112


### Fixed

* Selected action not being recognized correctly ([#114][114]).
* PNG images being way too dark.
* Regression: notification replacement (e.g. `notify-send -r NNNN`)
  not working correctly; the notification was resized, but the content
  was not updated ([#126][126]).

[114]: https://codeberg.org/dnkl/fnott/issues/114
[126]: https://codeberg.org/dnkl/fnott/issues/126


### Contributors

* Mark Stosberg
* Max Gautier
* sewn


## 1.5.0

### Added

* `docs` meson option ([#111][111])
* `pause` and `unpause` commands to temporarily disable or re-enable
  notifications ([#20][20]).
* Support for fractional scaling (`wp_fractional_scale_v1`).
* Support for preferred buffer scale (`wl_compositor` >= 6).
* Support for server side cursors (`wp_cursor_shape_v1`).

[20]: https://codeberg.org/dnkl/fnott/issues/20


### Changed

* Fnott now requires wayland-protocols >= 1.32.
* Minimum required meson version is now 0.59 ([#111][111]).
* Move example config to `<sysconfdir>/xdg/fnott.ini` ([#111][111]).
* `dpi-aware` no longer accepts the value `auto`, and the default
  value is now `no`.

[111]: https://codeberg.org/dnkl/fnott/pulls/111

### Removed

* `dpi-aware=auto`


### Contributors

* Lukas Wurzinger
* Marcin Puc
* polykernel
* sewn


## 1.4.1

### Fixed

* Compilation errors with clang 15.x ([#96][96])
* Notifications initially positioned outside the screen not being
  visible after being moved up in the notification stack.

[96]: https://codeberg.org/dnkl/fnott/issues/96


## 1.4.0

### Added

* `idle-timeout` option to specify the amount of time you need to be
  idle before notifications are prevented from timing out ([#16][16]).
* `icon` option, to specify icon to use when none is provided by the
  notification itself ([#82][82]).
* Support for `image-path` hints ([#84][84]).
* `dpi-aware=no|yes|auto` option ([#80][80]).

[16]: https://codeberg.org/dnkl/fnott/issues/16
[82]: https://codeberg.org/dnkl/fnott/issues/82
[84]: https://codeberg.org/dnkl/fnott/issues/84
[80]: https://codeberg.org/dnkl/fnott/issues/80


### Changed

* Default value of `max-width` and `max-height` is now `0`
  (unlimited).
* When determining initial font size, do FontConfig config
  substitution if the user-provided font pattern has no {pixel}size
  option ([#1287][foot-1287]).

[foot-1287]: https://codeberg.org/dnkl/foot/issues/1287


### Fixed

* file:// URIs, in icon paths ([#84][84])
* _Replace ID_ being ignored if there were no prior notification with
  that ID.
* Wayland protocol violation when output scaling was enabled.
* Notification expiration (timeout) and dismissal are now deferred
  while the action selection helper is running ([#90][90]).

[84]: https://codeberg.org/dnkl/fnott/issues/84
[90]: https://codeberg.org/dnkl/fnott/issues/90


### Contributors

* Leonardo Hernández Hernández


## 1.3.0

### Added

* Support for a “progress” hints, `notify-send -h int:value:20 ...`,
  ([#51][51]).
* `title-format`, `summary-format` and `body-format` options, allowing
  you to customize the rendered strings. In this release, the `%a`,
  `%s`, `%b` and `%%` formatters, as well as `\n`, are
  recognized. ([#39][39]).
* Added configuration option `layer` to specify the layer on which notifications
  are displayed. Values include `background`, `top`, `bottom`, and `overlay`
  ([#71][71]).

[51]: https://codeberg.org/dnkl/fnott/issues/51
[39]: https://codeberg.org/dnkl/fnott/issues/39
[71]: https://codeberg.org/dnkl/fnott/issues/71


### Changed

* Minimum required meson version is now 0.58.
* Notification text is now truncated instead of running into, and
  past, the vertical padding ([#52][52]).
* All color configuration options have been changed from (A)RGB
  (i.e. ARGB, where the alpha component is optional), to RGBA. This
  means **all** color values **must** be specified with 8 digits
  ([#47][47]).

[52]: https://codeberg.org/dnkl/fnott/issues/52
[47]: https://codeberg.org/dnkl/fnott/issues/47


### Removed

* `$XDG_CONFIG_HOME/fnottrc` and `~/.config/fnottrc`. Use
  `$XDG_CONFIG_HOME/fnott/fnott.ini` (defaulting to
  `~/.config/fnott/fnott.ini`) instead ([#7][7]).

[7]: https://codeberg.org/dnkl/fnott/issues/7


### Fixed

* Scale not being applied to the notification’s size when first
  instantiated ([#54][54]).
* Fallback to `/etc/xdg` if `XDG_CONFIG_DIRS` is unset.
* Icon lookup is now better at following the XDG specification
  ([#64][64]).
* Setting `max-width` and/or `max-height` to 0 no longer causes fnott
  to crash. Instead, a zero max-width/height means there is no limit
  ([#66][66]).

[54]: https://codeberg.org/dnkl/fnott/issues/54
[64]: https://codeberg.org/dnkl/fnott/issues/64
[66]: https://codeberg.org/dnkl/fnott/issues/66


### Contributors

* bagnaram
* Humm
* Leonardo Hernández Hernández
* Mark Stosberg
* merkix


## 1.2.1

### Fixed

* Crash when receiving notification with inline image data
  ([#44][44]).

[44]: https://codeberg.org/dnkl/fnott/issues/44


## 1.2.0

### Added

* Configurable padding of notification text. New `fnottrc` options:
  `padding-vertical` and `padding-horizontal` ([#35][35]).

[35]: https://codeberg.org/dnkl/fnott/issues/35


### Changed

* Default padding is now fixed at 20, instead of depending on the font
  size. This is due to the new `padding-horizontal|vertical` options.


### Fixed

* `fnottctl actions` exiting without receiving a reply.
* Fnott is now much better at surviving monitors being disabled and
  re-enabled ([#25][25]).
* Wrong font being used when the body and summary (or title and body,
  or title and summary) is set to the same text ([#36][36]).
* Fnott no longer allocates the vertical padding space between summary
  and body text, if the body text is empty ([#41][41]).

[25]: https://codeberg.org/dnkl/fnott/issues/25
[36]: https://codeberg.org/dnkl/fnott/issues/36
[41]: https://codeberg.org/dnkl/fnott/issues/41


### Contributors

* fauxmight
* Rishabh Das


## 1.1.2

### Fixed

* `max-timeout` not having any effect when the timeout is 0
  ([#32][32]).

[32]: https://codeberg.org/dnkl/fnott/issues/32


## 1.1.1

### Added

* `default-timeout` option, to adjust the timeout when applications
  ask us to pick the timeout ([#27][27]).
* `max-timeout` option ([#29][29]).

[27]: https://codeberg.org/dnkl/fnott/issues/27
[29]: https://codeberg.org/dnkl/fnott/issues/29


### Changed

* Updated nanosvg to ccdb1995134d340a93fb20e3a3d323ccb3838dd0
  (20210903).


### Removed

* `timeout` option (replaced with `max-timeout`, [#29][29]).


### Fixed

* Icons not being searched for in all icon theme instances
  ([#17][17]).
* fnott crashing when a notification was received while no monitor was
  attached to the wayland session.
* Wrong colors in (semi-)transparent areas of SVG icons.

[17]: https://codeberg.org/dnkl/fnott/issues/17


### Contributors

* Julian Scheel
* polykernel
* Stanislav Ochotnický


## 1.1.0

### Added

* Configurable minimal width of notifications. New `fnottrc` option:
  `min-width`
* Configurable anchor point and margins. New `fnottrc` options:
  `anchor=top-left|top-right|bottom-left|bottom-right`,
  `edge-margin-vertical`, `edge-margin-horizontal` and
  `notification-margin` ([#4][4]).
* `-c,--config=PATH` command line option ([#10][10]).
* Text shaping support ([#13][13]).
* `play-sound` to `fnott.ini`, specifying the command to execute to
  play a sound ([#12][12]).
* `sound-file`, a per-urgency option in `fnott.ini`, specifying the
  path to an audio file to play when a notification is received
  ([#12][12]).

[4]: https://codeberg.org/dnkl/fnott/issues/4
[10]: https://codeberg.org/dnkl/fnott/issues/10
[13]: https://codeberg.org/dnkl/fnott/issues/13
[12]: https://codeberg.org/dnkl/fnott/issues/12


### Changed

* Fnott now searches for its configuration in
  `$XDG_DATA_DIRS/fnott/fnott.ini`, if no configuration is found in
  `$XDG_CONFIG_HOME/fnott/fnott.ini` or in `$XDG_CONFIG_HOME/fnottrc`
  ([#7][7]).
* Assume a DPI of 96 if the monitor’s DPI is 0 (seen on certain
  emulated displays).
* There is now an empty line between the ‘summary’ and ‘body’.

[7]: https://codeberg.org/dnkl/fnott/issues/7


### Deprecated

* `$XDG_CONFIG_HOME/fnottrc` and `~/.config/fnottrc`. Use
  `$XDG_CONFIG_HOME/fnott/fnott.ini` (defaulting to
  `~/.config/fnott/fnott.ini`) instead ([#7][7]).

[7]: https://codeberg.org/dnkl/fnott/issues/7


### Removed

* `margin` option from `fnottrc`


### Fixed

* Notification sometimes not being rendered with the correct subpixel
  mode, until updated.


### Contributors

- yyp (Alexey Yerin)
- Julian Scheel


## 1.0.1

### Added

* `timeout` option to `fnottrc`. This option can be set on a
  per-urgency basis. If both the user has set a timeout, and the
  notification provides its own timeout, the shortest one is used
  ([#2][2]).
* FreeBSD port ([#1][1]).

[1]: https://codeberg.org/dnkl/fnott/issues/1


### Fixed

* PPI being incorrectly calculated.
* Crash due to bug in Sway-1.5 when a notification is dismissed,
  either with `fnottctl` or through its timeout, while the cursor is
  above it.


### Contributors

* jbeich


## 1.0.0

Initial release - no changelog. Rough list of features:

* Application title, summary and body fonts can be configured individually
* Icon support, both inline and name referenced (PNG + SVG).
* Actions (requires a dmenu-like utility to display and let user
  select action - e.g. [fuzzel](https://codeberg.org/dnkl/fuzzel))
* Urgency (custom colors and fonts for different urgency levels)
* Markup (**bold**, _italic_ and underline)
* Timeout (notification is automatically dismissed)

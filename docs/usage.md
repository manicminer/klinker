# Using Klinker

## Command line

    klinker [--settings] [--choose] [--config FILE] [link...]
    klinker [--config FILE] --list-browsers

| Option | Effect |
|--------|--------|
| *(no links)* | Opens the settings window. |
| `link...` | Handles each link in turn: preprocessing, then the first matching rule, otherwise the chooser. |
| `--settings` | Opens the settings window (together with any links). |
| `--choose` | Always shows the chooser, ignoring routing rules. Useful for testing rules or picking a browser once. |
| `--config FILE` | Uses `FILE` instead of `~/.config/klinker/config.json`; the default file is then neither read nor written. |
| `--list-browsers` | Prints the browsers in chooser order, one per line: ID, name, command and `ok`, `hidden` or the problems (tab-separated); no display needed. Cannot be combined with links, `--settings` or `--choose`. |
| `-h`, `--help` / `-v`, `--version` | Prints help or the version without needing a display. |

Links must be `http` or `https` URLs with a host, `file` URLs, or paths of
existing files (turned into `file://` URLs). Links are passed on exactly as
received, except that plain spaces become `%20` (KDE passes links with `%20`
decoded to spaces); bytes that are not valid UTF-8 are percent-encoded rather
than replaced. Tabs, line breaks and other control characters are refused.
Unusable arguments are reported on standard error with exit status 2
before any window opens.

Exit status is 0 otherwise, including when the chooser is cancelled.

## The chooser

The chooser is a horizontal strip of icons by default. *Settings → Appearance*
chooses the icon size (24–128 pixels) and whether names are shown under the
icons, or switches it to a vertical list. The window widens to show every icon;
a few icons are centred rather than stretched. Only when they do not fit on the
screen does the strip scroll, and only sideways (scroll bar, mouse wheel or
keyboard). On a screen too low for a whole row of the chosen size, the icons
are shown smaller instead; the setting is kept. Without names, hovering an icon shows
the browser's name and number, and screen readers announce both.

| Key | Action |
|-----|--------|
| 1–9 | Open the browser shown at that position (after filtering) |
| ↑ ↓ (vertical) or ← → (horizontal), Enter or Space | Select and open |
| Home, End | First or last browser |
| Letters | Filter the list by name; digits then type into the filter |
| ↓ in the filter | Back to the list, where 1–9 work again |
| Esc | Clear the filter, or cancel |

Clicking a browser opens it. *Details* shows the link as received and what
happened to it (tracking parameters removed, redirects followed, or why that
failed). The copy button copies the link that would be opened.

If a browser cannot be started, the chooser stays open with the error so you
can pick another one or open *Settings*.

With *Close the chooser when it loses focus* (off by default), switching to
another application cancels the link as Esc does, also while redirects are
still being resolved; nothing is opened later. Whether the focus really went to
another application is decided a moment (150 ms) after the chooser loses it; a
link that becomes ready meanwhile waits, and a rule only opens it if the focus
came back or stayed within Klinker. Moving between the chooser's own
controls, its copy buttons and menus, or opening the settings from it does not
count. The chooser only closes once it has actually had the focus, so a chooser
that never received it (for example because the compositor kept the focus
elsewhere) stays open. When several links were given, cancelling one, by Esc,
*Cancel* or losing the focus, moves on to the next, whose chooser takes the
focus again. When no browser is listed (none
detected, or all hidden) the chooser says so and offers the settings.

While redirects are being resolved the list is disabled; *Skip* stops the check
and uses the link as received (with tracking parameters removed if that is
enabled).

Opening the settings from the chooser hides the chooser until the settings
close; saved changes (order, hidden browsers, names, appearance) are shown when
it returns, resized on the screen it was on.
While the settings are open no link is routed or opened, even if its redirects
finish resolving meanwhile; a link that becomes ready while the settings are
open is routed with the saved settings once the window closes. Saving different
privacy settings starts the current link again from the link as received, so
what is shown, routed and opened always reflects the new settings. Links still
waiting their turn use them as well. A link that was already ready in the
chooser when the settings opened, and is not started again, is not routed
again: the chooser returns, and changed rules take effect from the next link.

## Settings

**Browsers** lists the browsers in chooser order. Detected browsers come from
desktop files in `~/.local/share/applications`, `/usr/local/share`,
`/usr/share` and the Flatpak export directories.

- *Add…* / *Edit…*: name, program (in `PATH` or a full path), arguments one per
  line, and an icon name or image file. `{url}` in an argument is replaced by
  the link; without it the link is added as the last argument. Missing programs
  give a warning; commands that would pass the link to a shell as code are
  refused. So are commands that would run Klinker itself (`klinker`); its own
  desktop file is never listed.
- *Hide* / *Show*: hidden browsers are left out of the chooser but rules can
  still use them.
- *Move Up* / *Move Down*: the order of the chooser and of its number keys.
- *Reset*: forget your changes to a detected browser.
- *Remove…*: a removed detected browser does not come back on rescans; rules
  that used it show an error until they get another browser.
- *Rescan*: look for newly installed or removed browsers, keeping your order
  and changes.

**Routing Rules**: add, remove, reorder and edit rules. Each rule has an
optional name, an enabled switch, a match type, a pattern and a target browser
(hidden browsers are marked; a missing one is shown as *Missing browser*
instead of silently picking another). Problems are shown next to each rule.
An enabled rule with an invalid pattern or without a browser cannot be saved;
disable it to keep it for later. *Test a link* shows which rule would handle a
link (redirects are not followed when testing).

**Appearance**: horizontal icons or vertical list, the icon size and names of
the horizontal strip (kept, but unused, while the list is vertical), and
*Close the chooser when it loses focus*.

**Privacy**: the two opt-in switches described in the README.

**About** shows the configuration file in use.

*Apply* and *OK* check the rules and save atomically. *Apply* is only
available while there are unsaved changes and keeps the window open; *OK*
saves what is shown even without changes and closes it. If the file was changed
by someone else since it was loaded, or cannot be parsed, nothing is written
and the reason is shown; *Reload from Disk* discards your unsaved changes and
loads the file again. *Cancel* discards the changes made since the last
successful *Apply*, including a reload that was not followed by a successful
save; anything already applied stays saved and in use. This invocation keeps
using the settings it started with or last applied, and saving later is again
refused rather than overwriting the newer file.

## Troubleshooting

- Qt may send warnings to the systemd journal instead of the terminal. Run with
  `QT_FORCE_STDERR_LOGGING=1` to see them.
- A browser that is not listed: check that its desktop file lists
  `x-scheme-handler/http` or `x-scheme-handler/https` in `MimeType`, then
  *Rescan*. Or add it manually.
- To start over, move `~/.config/klinker/config.json` away; Klinker then
  uses the defaults.

## Development

All tests:

    ctest --test-dir build --output-on-failure

The GUI tests (`test_placement`, `test_chooser`, `test_settings`,
`test_application`) run with `QT_QPA_PLATFORM=offscreen` and isolated XDG
directories. Launch tests start `klinker_capture_fixture`, which only records its
arguments. Focus changes to another application are reported to the offscreen
platform the way a platform plugin would (`QWindowSystemInterface`), so these
tests never involve real windows, the desktop or the clipboard.

Screenshots of the chooser and every settings page in Breeze Light and Breeze
Dark are written when `KLINKER_SCREENSHOT_DIR` is set:

    KLINKER_SCREENSHOT_DIR="$PWD/build/screenshots" ctest --test-dir build -R 'test_(chooser|settings|application)'

The README screenshots in `docs/screenshots/` come from these tests run in a
private D-Bus session with the KDE platform theme, which Oxygen needs to
draw disabled buttons dimmed; they were then optimised losslessly and renamed:

    dbus-run-session -- env -u DISPLAY -u WAYLAND_DISPLAY QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME=kde \
        KLINKER_SCREENSHOT_DIR="$PWD/build/screenshots" build/tests/test_chooser screenshots horizontalOxygenScreenshots
    dbus-run-session -- env -u DISPLAY -u WAYLAND_DISPLAY QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME=kde \
        KLINKER_SCREENSHOT_DIR="$PWD/build/screenshots" build/tests/test_settings appearanceScreenshot

| File | Taken as |
|------|----------|
| `chooser-horizontal.png` | `chooser-horizontal-names-oxygen-dark.png` |
| `chooser-horizontal-icons.png` | `chooser-horizontal-icons-oxygen-dark.png` |
| `chooser-vertical.png` | `chooser-light.png` |
| `settings-appearance.png` | `settings-appearance-horizontal-oxygen-dark.png` |

Wayland smoke tests of the layer-shell chooser in a private, headless KWin with
its own D-Bus session (nothing appears on your desktop):

    tests/wayland-smoke.sh build/tests/test_chooser waylandLayerShellPopup waylandFocusLoss waylandOwnPopupKeepsTheChooser

or as a CTest entry with `-DKLINKER_WAYLAND_TESTS=ON`, which runs all three.
`waylandFocusLoss` and `waylandOwnPopupKeepsTheChooser` start a second
application, `klinker_focus_fixture` (a plain window), to take the keyboard focus
for real; they refuse to run anywhere but in that private session. To run only
the placement test against the running Plasma session instead (a chooser
appears briefly on the active screen):

    QT_QPA_PLATFORM=wayland build/tests/test_chooser waylandLayerShellPopup

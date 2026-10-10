# Klinker — implementation plan

A native KDE Plasma URL handler: when a link is opened, either route it to a
browser by rule or show a chooser. Inspired in functionality (not code or
assets) by Switchyard.

Stack: C++20, Qt 6 (Core/Network/Widgets/Test), KDE Frameworks 6, plain CMake
+ Ninja (ECM is not required). Tests use QtTest and never launch real browsers
or modify the user's desktop configuration.

## Milestones

1. **Foundation (this milestone)** — static library `klinker_core` with no GUI or
   networking:
   - `urlcleaner` — opt-in removal of known tracking query parameters, operating
     on the raw URL string so every other octet is preserved.
   - `rules` — rule model, host canonicalisation, domain / subdomain / glob /
     POSIX ERE matching, validation and first-match routing.
   - `execcommand` — desktop-entry `Exec` tokenising and field-code conversion,
     `{url}` substitution into an argv array, one-argument-per-line editing
     format, command validation (missing executable, self recursion, shell
     interpolation).
   - `browserdiscovery` — XDG desktop-entry scan for HTTP(S) handlers.
   - `browsercatalog` — merges detected browsers with user settings (order,
     overrides, hide, remove, reset, manual entries).
   - `config` — versioned JSON config, validation, atomic and conflict-aware
     saving.
2. **Networking** — async redirect unwrapping (≤ 3 hops, HEAD with bounded GET
   fallback, scheme/credential/downgrade guards, cleaning at every hop) and the
   URL pipeline (clean → resolve → clean → route). Integration tests against a
   local deterministic HTTP server; launch tests through a capture executable.
3. **GUI and desktop integration** — borderless chooser (palette-driven,
   number-key shortcuts, scrolling list), settings pages (Browsers, Rules,
   Privacy), placement utilities (cursor/active screen, clamping, Wayland
   layer-shell or centred fallback), `.desktop` file, icon, opt-in install
   instructions, README.
4. **Review and visual verification** on KDE Wayland plus offscreen palette
   tests.

## Milestone 1 design decisions

- **URL preservation.** Nothing in the core round-trips a URL through `QUrl`
  for output. Tracking removal splits the raw string at the first `?` and `#`,
  drops only matching `&`-separated components and rejoins the rest
  verbatim, empty components included (`?&utm_source=x&` becomes `?&`); the
  `?` goes only when no component is left. With nothing to remove the input
  string is returned unchanged.
- **Tracking list** (exact, case-sensitive, after percent-decoding the name):
  `utm_*`, `gclid`, `dclid`, `gbraid`, `wbraid`, `gclsrc`, `fbclid`,
  `msclkid`, `mc_cid`, `mc_eid`, `yclid`, `twclid`, `ttclid`, `igshid`,
  `_hsenc`, `_hsmi`, `mkt_tok`. Generic names such as `ref` or `id` are kept.
  Only `http`/`https` URLs are touched; queries inside fragments are not.
- **Host extraction** is a small WHATWG-style authority parser rather than
  `QUrl::host()`, so routing sees the same host the browser will: credentials
  and port are dropped, `\` is a path separator for special schemes (so
  `http://evil.example\@good.example/` routes as `evil.example`), the host is
  percent-decoded, converted to ACE (IDN) and lower-cased, and a trailing dot
  is ignored.
- **Numeric hosts** of http(s), ftp and ws(s) URLs follow the WHATWG IPv4
  parser: a host whose last label is a number must be an IPv4 address and is
  written in dotted decimal (`127.1`, `0x7f000001` and `2130706433` are all
  `127.0.0.1`); otherwise it has no canonical host. Numeric domain patterns
  are canonicalised the same way, and invalid ones are rejected.
- **Domain rules** compare canonical hosts exactly. **Subdomain rules match
  the apex too**: `example.com` matches `example.com` and `a.b.example.com`
  but not `notexample.com`. Patterns are bare host names (no scheme, port,
  path or wildcards).
- **Glob rules** match the whole URL string, case-sensitively. `*` matches any
  sequence including `/`, `?` matches one character, `\` escapes the next
  character. There are no bracket classes.
- **Regex rules** are POSIX ERE compiled with `regcomp(REG_EXTENDED |
  REG_NOSUB)` and searched unanchored against the UTF-8 URL; anchor with `^`
  and `$` as needed.
- **Routing**: the first rule that is enabled, valid, targets an existing
  browser and matches wins. Invalid rules (bad pattern, missing target) are
  skipped and reported per rule; they never crash or route.
  Hidden browsers remain valid targets; removed or uninstalled ones do not.
- **Discovery** scans `$XDG_DATA_HOME` then `$XDG_DATA_DIRS`, appending the
  user and system Flatpak export directories at lowest precedence when they
  are not already listed. The first file for a desktop ID wins, including a
  `Hidden=true` file masking lower ones. Entries must be `Type=Application`,
  not `NoDisplay`, pass `TryExec`, list `x-scheme-handler/http` or
  `x-scheme-handler/https`, be shown in `$XDG_CURRENT_DESKTOP` per
  `OnlyShowIn`/`NotShowIn`, have a parseable `Exec`, and must not be this
  application (by desktop ID or executable, including behind `env`).
  Entries whose effective program cannot be determined (e.g. `env -S`) could
  be this application, so they are skipped with a warning.
- **Exec conversion** follows the Desktop Entry Specification quoting rules.
  `%u %U %f %F` become `{url}`, `%i` becomes `--icon <Icon>`, `%c` the name,
  `%k` the file path, `%%` a literal `%`; deprecated codes are dropped.
  Wrappers such as `env` and `flatpak run` are kept as the argv prefix.
- **Launch commands** are `program` + argument list. Every argument containing
  `{url}` has it substituted; if none does, the URL is appended as a final
  argument. The URL is always contained within argv elements, never passed
  through a shell. Validation fails closed: it rejects a command whose
  effective program cannot be determined (`env -S`/`--split-string`, any
  `env` option it does not know, no program, `{url}` as the program), any
  shell given a command string (an option containing `c` or `C`, such as
  `-c`, `-ec`, `-C` or `--command`, wherever `{url}` is or whether the URL is
  appended), and a shell argument with `{url}` embedded inside it. A shell
  running a script file with `{url}` as a separate argument is allowed.
- **Browser settings** are stored as one ordered array. Detected entries store
  only overrides that differ from the desktop file; `reset` clears them.
  Removing a detected browser leaves a `removed` tombstone so a rescan does
  not bring it back; removing a manual browser deletes it. Newly detected
  browsers are appended after the configured order, sorted by name.
- **Config** lives at `$XDG_CONFIG_HOME/klinker/config.json`
  (schema in [configuration.md](configuration.md)). Saving takes a lock file,
  refuses to write a configuration that loading would reject (the existing
  file is kept), refuses to replace a file that is malformed or changed since
  it was loaded, and writes atomically with `QSaveFile`. No URL history is stored.

## Risks carried forward

- Non-UTF-8 bytes in a URL passed on the command line are decoded by Qt before
  the core sees them; milestone 3 should read `argv` carefully.
- Localised `Name[xx]` handling relies on KConfig defaults and is not covered
  by tests.
- Commands behind `env -S` are rejected rather than parsed, and shells cannot
  be given a command string at all, even one that only uses `"$1"`. Use a
  script file or the program directly.
- Hosts that Qt's IDNA conversion rejects (e.g. labels with a leading hyphen)
  have no canonical host, so domain rules never match them; this fails closed
  to the chooser.
- `validateCommand` is advisory in milestone 1. Milestone 2's launcher must
  call it (and `buildLaunchCommand`'s refusal of option-like URLs) right before
  every launch, including for routed and hidden browsers.
- User-visible messages are plain English strings; they need wrapping for
  translation (KI18n) when the GUI lands.

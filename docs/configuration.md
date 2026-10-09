# Configuration file

kde-browser-switcher keeps its settings in a single human-editable JSON file:

    $XDG_CONFIG_HOME/kde-browser-switcher/config.json

(`~/.config/kde-browser-switcher/config.json` when `XDG_CONFIG_HOME` is unset.)
A missing file means "all defaults". The application never stores visited URLs
or any history.

## Example

```json
{
    "version": 1,
    "browsers": [
        { "id": "firefox.desktop" },
        { "id": "chromium.desktop", "hidden": true },
        { "id": "google-chrome.desktop", "removed": true },
        {
            "id": "org.mozilla.firefox.desktop",
            "name": "Firefox (Work)",
            "arguments": ["-P", "work", "{url}"]
        },
        {
            "id": "manual-3f2a9c1d",
            "manual": true,
            "name": "Tor Browser",
            "program": "/opt/tor-browser/start-tor-browser",
            "arguments": ["{url}"],
            "icon": "torbrowser"
        }
    ],
    "rules": [
        {
            "name": "Work",
            "enabled": true,
            "type": "subdomain",
            "pattern": "corp.example",
            "browser": "org.mozilla.firefox.desktop"
        },
        {
            "enabled": true,
            "type": "glob",
            "pattern": "https://github.com/my-org/*",
            "browser": "chromium.desktop"
        },
        {
            "enabled": false,
            "type": "regex",
            "pattern": "^https?://(www\\.)?youtube\\.com/",
            "browser": "firefox.desktop"
        }
    ],
    "privacy": {
        "stripTrackingParameters": false,
        "unwrapRedirects": false
    }
}
```

## Top level

| Key        | Type    | Default | Meaning |
|------------|---------|---------|---------|
| `version`  | integer | —       | Required. Must be `1`. Files from a newer version are refused, not overwritten. |
| `browsers` | array   | `[]`    | Browser order and per-browser settings. |
| `rules`    | array   | `[]`    | Routing rules, evaluated top to bottom. |
| `privacy`  | object  | all off | URL preprocessing switches. |

## `browsers`

The array order is the order of the chooser. Installed browsers that are not
listed (for example, ones installed later) are appended after the listed ones,
sorted by name.

| Key         | Type             | Meaning |
|-------------|------------------|---------|
| `id`        | string           | Required and unique. For detected browsers, the desktop file ID (e.g. `firefox.desktop`). |
| `manual`    | boolean          | `true` for a browser added by hand. Manual entries require `program`. |
| `hidden`    | boolean          | Hide from the chooser. Rules may still target a hidden browser. |
| `removed`   | boolean          | Detected browsers only: never show or route to it, even after a rescan. |
| `name`      | string           | Display name. For detected browsers, overrides the desktop file. |
| `icon`      | string           | Icon name or absolute path. |
| `program`   | string           | Executable name (looked up in `PATH`) or absolute path. |
| `arguments` | array of strings | One element per argument. See below. |

For detected browsers, any of `name`, `icon`, `program` and `arguments` that is
absent comes from the desktop file, so later updates to the desktop file still
apply. "Reset to defaults" in the settings removes these keys.

### Arguments and `{url}`

The URL is passed as an argument, never through a shell. Every argument that
contains `{url}` has it replaced by the URL (so `--app={url}` works); if no
argument contains `{url}`, the URL is appended as the last argument. Commands
are rejected if they would start kde-browser-switcher itself, if it is not clear
which program they run (for example `env -S …`, or an `env` option the
application does not know), or if they could hand the URL to a shell as code.
Shells (`sh`, `bash`, `zsh`, `fish`, …) are never given a command string: any
`-c`, `-C` or `--command` style option is rejected, even
`sh -c 'exec firefox "$1"' sh {url}`. To use a shell, put the commands in a
script file and pass `{url}` as a separate argument:
`sh /path/to/script.sh {url}`.

Desktop file `Exec` lines are converted on detection: `%u`, `%U`, `%f` and `%F`
become `{url}`, `%i` becomes `--icon <icon>`, `%c` the name, `%k` the desktop
file path and `%%` a literal `%`. Wrappers such as `env VAR=value browser` or
`flatpak run …` are kept as they are.

## `rules`

The first rule that is enabled, valid and matches decides the browser. If no
rule matches, the chooser is shown. A rule with an invalid pattern or a target
browser that no longer exists is skipped and reported in the settings; it never
routes.

| Key       | Type    | Default | Meaning |
|-----------|---------|---------|---------|
| `name`    | string  | `""`    | Optional label. |
| `enabled` | boolean | `true`  | Disabled rules are kept but ignored. |
| `type`    | string  | —       | Required: `domain`, `subdomain`, `glob` or `regex`. |
| `pattern` | string  | —       | Required. Meaning depends on `type`. |
| `browser` | string  | `""`    | Target browser `id`. |

Rule types:

- **`domain`** — the URL's host equals `pattern`, case-insensitively.
  Credentials, port, path, query and fragment are ignored, international
  domain names match their `xn--` form and a trailing dot is ignored.
  Numeric hosts are read the way browsers read them: `127.1`, `0x7f000001`
  and `2130706433` are all `127.0.0.1`, for the URL and the pattern alike.
  The pattern is a bare host name or IPv4 address: no scheme, port, path or
  wildcards.
- **`subdomain`** — like `domain`, but also matches any subdomain on a dot
  boundary. `example.com` matches `example.com` itself and `a.b.example.com`,
  but not `notexample.com`.
- **`glob`** — matched against the whole URL, case-sensitively. `*` matches any
  run of characters including `/`, `?` matches one character and `\` makes the
  next character literal. There are no `[...]` classes.
- **`regex`** — a POSIX extended regular expression (`regcomp` with
  `REG_EXTENDED`) searched anywhere in the URL. Use `^` and `$` to anchor.
  Remember that JSON needs backslashes doubled (`"\\."` for a literal dot).

Rules are evaluated against the URL after the optional privacy preprocessing
below.

## `privacy`

Both switches are off by default.

| Key                       | Meaning |
|---------------------------|---------|
| `stripTrackingParameters` | Remove known tracking query parameters (`utm_*`, `gclid`, `dclid`, `gbraid`, `wbraid`, `gclsrc`, `fbclid`, `msclkid`, `mc_cid`, `mc_eid`, `yclid`, `twclid`, `ttclid`, `igshid`, `_hsenc`, `_hsmi`, `mkt_tok`) from http(s) URLs. Names are compared exactly after percent-decoding. Everything else in the URL is kept byte for byte, including empty query components (`?&utm_source=x&` becomes `?&`); the `?` is dropped only when the query held nothing but tracking parameters. |
| `unwrapRedirects`         | Follow up to three HTTP redirects in the background before routing. **Privacy notice:** this contacts the link's server before you choose a browser. |

## Errors and safety

- If the file cannot be parsed, the application uses defaults, shows the error
  (including the line number for JSON syntax errors) and refuses to save until
  the file is fixed. It never overwrites a malformed file.
- Unknown keys are ignored with a warning, which helps to spot typos.
- Before saving, the new contents are checked with the same rules as loading
  (for example, a manual browser needs a `program` and every `id` must be
  unique). If the check fails, nothing is written and the existing file is
  kept.
- Saving writes a temporary file and atomically renames it into place, holding
  `config.json.lock` meanwhile. If the file changed on disk since it was loaded
  (for example, edited by hand or saved by another window), saving is refused
  so that the other change is not lost; reload and try again.

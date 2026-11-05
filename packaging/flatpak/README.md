# Klinker Flatpak

`io.github.manicminer.Klinker`, not on Flathub. Built here **unsigned**; signed and published only
by the shared repository [manicminer/flatpak](https://github.com/manicminer/flatpak) at
<https://manicminer.github.io/flatpak/> (remote `manicminer`, branch `stable`, x86_64). Installing
and the permissions are described in the [main README](../../README.md#flatpak).

## Files

| File | Purpose |
|---|---|
| `io.github.manicminer.Klinker.json` | manifest: KDE 6.11 runtime, LayerShellQt 6.8.2 (not in the runtime), Klinker |
| `build.sh` | builds a source tree into an unsigned repo + `repo.tar` + `SOURCE.json` (+ bundle with `--bundle-gpg-key`) |
| `render-manifest.py`, `source-json.py`, `lib.sh` | helpers: source path and tests in the manifest, the build record for the publisher (source tag/commit, packaging commit and file digests, rendered manifest digest, runtime/SDK commits), checks of refs and exports |
| `test-installed.sh` | installs a build into a scratch installation and tests it with stand-in browsers (below) |
| `ci-install-deps.sh` | CI tooling |
| `manicminer-flatpak.gpg` | the repository's public key (as `keys/` of manicminer/flatpak), for release bundles |

## Sandbox design

The browsers Klinker offers are the host's, so the sandboxed application works on the host
(`src/core/hostsystem.cpp`; native builds are unchanged):

- **Finding browsers**: desktop entries are read through read-only views of the host's
  application directories: `host-os` (the host's `/usr`, under `/run/host`), the per-user
  `applications` and `icons`, the per-user and system Flatpak exports together with the
  installations they link into, and snap's desktop directory. The runtime's own `/usr` is never
  scanned.
- **Checking programs**: `TryExec`, the program of each command and its availability are looked
  up on the host, in one `flatpak-spawn --host sh -c …` per scan with the names as positional
  parameters, never as script text. The same query resolves each program's symlinks on the host
  (`readlink -f`), so an alias such as `~/bin/browser` is known by what it runs. The runtime's `/usr/bin` and the host's absolute symlinks
  (e.g. into `/opt`) would give wrong answers inside the sandbox.
- **Starting browsers**: `HostCommand` of `org.freedesktop.Flatpak.Development` on the session
  bus (what `flatpak-spawn --host` uses), in `$HOME`, with the same argument vector as natively, no
  shell, the host session's environment, standard input from `/dev/null` and Klinker's output. It
  answers only once the host has executed the program, so a browser that cannot be started is
  reported as natively instead of counting as started. The checks before that are the native ones:
  commands that hand the link to a shell as code or that would run Klinker are refused, including
  `flatpak run io.github.manicminer.Klinker`, its exported wrapper and host symlinks to either or
  to a native `klinker`. If host commands are not allowed, launching fails with a message instead
  of silently.
- **Paths**: `%k` and file links are passed as host paths; a program chosen with *Browse…*
  (a document portal path) is turned into its host path through the Documents portal.
- **Never the OpenURI portal**: it would hand the link to the default browser, which is Klinker.
- **Placement**: `org.kde.KWin` for the active output; LayerShellQt is built into the app.
  In a nested KWin 6.8 the sandboxed client (security context) is offered `zwlr_layer_shell_v1`
  and gets its layer surface, and `activeOutputName` answers.

`org.freedesktop.Flatpak` lets the application run any command on the host as the user, so the
sandbox does not protect the system from Klinker; the read-only filesystem grants only make
discovery work.

## Local build and tests

```sh
flatpak --user install flathub org.flatpak.Builder org.kde.Sdk//6.11 org.kde.Platform//6.11
dbus-run-session -- packaging/flatpak/build.sh --source . --out /tmp/klinker-flatpak --run-tests
FLATPAK_USER_DIR=/tmp/klinker-flatpak-test packaging/flatpak/test-installed.sh --repo /tmp/klinker-flatpak/repo
```

`build.sh` uses org.flatpak.Builder from the default per-user installation, the same as CI; for a
separate installation point `HOME` at a private directory. `--run-tests` runs the whole test suite
inside the build (offscreen; the tests unset `FLATPAK_ID`, and the sandbox tests simulate the host
with stand-ins for `flatpak-spawn` and `HostCommand`).

`test-installed.sh` needs a scratch installation with `org.kde.Platform` (never your own; it
refuses the default one), re-runs itself with a private HOME, runtime directory and D-Bus session
and no display, and withdraws the host's system directories from the app for its runs. It checks
the exported desktop file (Flatpak removes `TryExec`, which would hide the entry on the host) and
read-only filesystem permissions, lists browsers found through the host (a program outside the
sandbox's view, one in the host's `PATH`, a symlinked per-user Flatpak export; a missing `TryExec`
and Klinker itself left out), and routes a link by a rule to a stand-in browser that records the
argument vector it was started with on the host. It then checks that a symlink to Klinker's
exported wrapper, outside the sandbox's view, is refused as Klinker itself, and that a browser the
host cannot execute keeps the link in Klinker instead of counting as started. Keep `TMPDIR` short: helper sockets are created
below it.

## Releases

Bump the version in `CMakeLists.txt` and add a `<release>` to
`data/io.github.manicminer.Klinker.metainfo.xml`, then tag the commit on main `v<version>` and push
the tag. The workflow in the tagged commit builds, tests and attaches the bundle to a GitHub
Release; nothing is requested from the publisher, whose scheduled *Publish* finds the successful
run and publishes it, normally within 15-30 minutes. `build.sh --tag` refuses a tag that does not
match the version in the source.

*Flatpak* can also be run by hand on main with `source_tag` set, to rebuild an existing tag with
main's packaging (for example after a runtime update). Tags before v0.4.0 have no Flatpak support
and are refused: the sandbox integration is part of the application, not of the packaging, so a
release without it cannot be published. Tags are never moved; a tag whose source cannot be built is
superseded by a new one.

The publisher checks ancestry itself: the tag's commit and the packaging commit (the run's head)
must be the head of main or its ancestors. Keep main protected (no force push or deletion).

#!/usr/bin/env bash
# End-to-end test of an installed build, offline and synthetic:
#
#   1. installs the build (unsigned, test only) into a SCRATCH per-user installation;
#   2. checks the exported desktop file (Exec runs the app with the links, no TryExec that would
#      hide it on the host) and that every filesystem permission is read-only;
#   3. with stand-in browsers on the "host" (this private environment), checks that the sandboxed
#      Klinker finds them through the host: a TryExec and an Exec in a directory the sandbox
#      cannot see, a program found in the host's PATH, a per-user Flatpak export (a symlink into
#      the installation), a missing TryExec hidden, and its own `flatpak run` refused;
#   4. routes a link by a rule and checks that the stand-in received it, as one argument, started
#      on the host with HostCommand;
#   5. failure paths: an alias of the exported wrapper that only the host can see is refused as
#      Klinker itself, and a browser the host cannot execute keeps the link in Klinker instead of
#      counting as started.
#
# The sandbox sees no host system directories here (host-os and /var/lib/flatpak|snapd are
# withdrawn for the runs), so no installed application or real browser is read or started. It
# re-runs itself with a private HOME, runtime directory and D-Bus session and no display (keep
# TMPDIR short: the session's helper sockets are created below it):
#   FLATPAK_USER_DIR=<scratch> test-installed.sh --repo DIR
# The scratch installation needs the app's runtime (org.kde.Platform).
set -euo pipefail
if [[ -z "${KLINKER_TEST_ISOLATED:-}" ]]; then
    [[ -n "${FLATPAK_USER_DIR:-}" && "$(realpath -m "$FLATPAK_USER_DIR")" != "$(realpath -m "${XDG_DATA_HOME:-$HOME/.local/share}/flatpak")" ]] \
        || { echo "set FLATPAK_USER_DIR to a scratch installation; this test never uses your own" >&2; exit 1; }
    h="$(mktemp -d "${TMPDIR:-/tmp}/klinker-test.XXXXXX")"; mkdir -m 700 "$h/run" "$h/host-bin"; rc=0
    # The host's PATH (for flatpak-spawn --host, through the private bus) starts with host-bin.
    env -i PATH="$h/host-bin:$PATH" TMPDIR="$h" HOME="$h" XDG_RUNTIME_DIR="$h/run" LANG=C.UTF-8 KLINKER_TEST_ISOLATED=1 \
        FLATPAK_USER_DIR="$FLATPAK_USER_DIR" dbus-run-session -- bash "$0" "$@" || rc=$?
    rm -rf "$h"; exit "$rc"
fi
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$here/lib.sh"

repo=""
while (($#)); do
    case "$1" in
        --repo) repo="$2"; shift 2 ;;
        *) die "unknown argument: $1" ;;
    esac
done
[[ -d "$repo" ]] || die "usage: FLATPAK_USER_DIR=<scratch> $0 --repo DIR"
repo="$(cd "$repo" && pwd -P)"
remote="klinker-installed-test"
cleanup() {
    flatpak --user uninstall -y --noninteractive "$APP_ID//$BRANCH" >/dev/null 2>&1 || true
    flatpak --user remote-delete --force "$remote" >/dev/null 2>&1 || true
}
trap cleanup EXIT
pass=0
ok() { pass=$((pass + 1)); printf 'ok   %s\n' "$1"; }
klinker() { # runs the app with the host's system directories withdrawn
    timeout "${run_timeout:-60}" flatpak --user run --nofilesystem=host-os --nofilesystem=/var/lib/flatpak/app \
        --nofilesystem=/var/lib/flatpak/exports/share --nofilesystem=/var/lib/snapd/desktop \
        --env=QT_QPA_PLATFORM=offscreen "$@"
}

# 1. Install (local, unsigned test repository; the published one is always signed).
flatpak --user remote-delete --force "$remote" >/dev/null 2>&1 || true
flatpak --user remote-add --no-gpg-verify "$remote" "file://$repo"
flatpak --user install -y --noninteractive "$remote" "$APP_ID//$BRANCH" >/dev/null
ok "installed $APP_ID from the build"

# 2. Exported desktop file and permissions.
desktop="$FLATPAK_USER_DIR/exports/share/applications/$APP_ID.desktop"
[[ -f "$desktop" ]] || die "no exported $APP_ID.desktop"
exec_line="$(sed -n 's/^Exec=//p' "$desktop" | head -n1)"
[[ "$exec_line" == *"--command=klinker"* && "$exec_line" == *" $APP_ID "* && "$exec_line" == *"%U"* ]] \
    || die "exported Exec does not run klinker with the links: $exec_line"
! grep -q '^TryExec=' "$desktop" || die "exported desktop file keeps TryExec, which hides it on the host"
grep -qx 'MimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;' "$desktop" || die "exported MimeType changed"
ok "exported desktop file: Exec=$exec_line, no TryExec"
perms="$(flatpak --user info --show-permissions "$APP_ID//$BRANCH")"
filesystems="$(sed -n 's/^filesystems=//p' <<<"$perms")"
for fs in ${filesystems//;/ }; do
    [[ "$fs" == *:ro ]] || die "writable filesystem permission: $fs"
done
grep -qx 'org.freedesktop.Flatpak=talk' <<<"$perms" || die "no org.freedesktop.Flatpak: browsers could not be started on the host"
! grep -Eq '^sockets=.*(session-bus|system-bus)' <<<"$perms" || die "whole-bus access: $perms"
ok "filesystem permissions read-only ($filesystems); host commands through org.freedesktop.Flatpak"

# 3. Stand-in browsers on the host. host-only/ is not visible in the sandbox.
mkdir -p "$HOME/host-only" "$HOME/.local/share/applications"
cat >"$HOME/host-only/browser" <<EOF
#!/bin/sh
printf '%s\0' "\$@" >"$HOME/host-only/launched.tmp" && mv "$HOME/host-only/launched.tmp" "$HOME/host-only/launched"
EOF
chmod +x "$HOME/host-only/browser"
cp "$HOME/host-only/browser" "$HOME/host-bin/klinker-test-path-browser"
entry() { # file name exec [extra]
    printf '[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nMimeType=x-scheme-handler/http;x-scheme-handler/https;\n%s' \
        "${1%.desktop}" "$2" "${3:-}" >"$HOME/.local/share/applications/$1"
}
entry klinker-test-host.desktop "$HOME/host-only/browser --test-flag %u" "TryExec=$HOME/host-only/browser"$'\n'
entry klinker-test-path.desktop "klinker-test-path-browser %u" "TryExec=klinker-test-path-browser"$'\n'
entry klinker-test-missing.desktop "$HOME/host-only/browser %u" "TryExec=$HOME/host-only/missing"$'\n'
entry klinker-test-self.desktop "flatpak run --branch=stable $APP_ID %U"
# A per-user Flatpak browser as the host exports it: a relative symlink into ~/.local/share/flatpak/app.
exported="$HOME/.local/share/flatpak/app/org.example.KlinkerTest/current/active/export/share/applications"
mkdir -p "$exported" "$HOME/.local/share/flatpak/exports/share/applications"
printf '[Desktop Entry]\nType=Application\nName=Exported\nExec=flatpak run org.example.KlinkerTest %%u\nMimeType=x-scheme-handler/https;\n' \
    >"$exported/org.example.KlinkerTest.desktop"
ln -s ../../../app/org.example.KlinkerTest/current/active/export/share/applications/org.example.KlinkerTest.desktop \
    "$HOME/.local/share/flatpak/exports/share/applications/org.example.KlinkerTest.desktop"
visible="$(klinker --command=sh "$APP_ID//$BRANCH" -c 'test -e "$1" && echo visible || echo hidden' sh "$HOME/host-only/browser")"
[[ "$visible" == hidden ]] || die "the stand-in is visible in the sandbox ($visible); the test would not prove host lookups"

listing="$(klinker "$APP_ID//$BRANCH" --list-browsers)"
ids="$(cut -f1 <<<"$listing" | sort | tr '\n' ' ')"
[[ "$ids" == "klinker-test-host.desktop klinker-test-path.desktop org.example.KlinkerTest.desktop " ]] \
    || die "browsers found: $ids"$'\n'"$listing"
grep -qP '^klinker-test-host\.desktop\t[^\t]*\t\Q'"$HOME"'\E/host-only/browser --test-flag \{url\}\tok$' <<<"$listing" \
    || die "host browser not usable: $listing"
grep -qP '^klinker-test-path\.desktop\t.*\tok$' <<<"$listing" || die "PATH browser not usable: $listing"
ok "browsers found through the host: TryExec/Exec outside the sandbox, in the host's PATH and a per-user Flatpak export; missing and self left out"

# 4. A link routed by a rule starts the stand-in on the host with the link as one argument.
config="$HOME/.var/app/$APP_ID/config/klinker"
mkdir -p "$config"
cat >"$config/config.json" <<'EOF'
{"version": 1, "rules": [{"enabled": true, "type": "domain", "pattern": "example.org", "browser": "klinker-test-host.desktop"}]}
EOF
url="https://example.org/test?q=%22x%22;\$(id)&r='y'#\`z\`"
klinker "$APP_ID//$BRANCH" "$url" || die "klinker failed to route the link"
for _ in $(seq 1 50); do
    [[ -f "$HOME/host-only/launched" ]] && break
    sleep 0.2
done
[[ -f "$HOME/host-only/launched" ]] || die "the stand-in browser was not started"
mapfile -d '' -t argv <"$HOME/host-only/launched"
[[ "${#argv[@]}" == 2 && "${argv[0]}" == "--test-flag" && "${argv[1]}" == "$url" ]] \
    || die "the stand-in received: $(printf '[%s] ' "${argv[@]}")"
ok "routed link started the host browser with HostCommand, link intact as one argument"

# 5. Failure paths. The alias is a symlink in host-only/ (hidden from the sandbox) to the wrapper
# Flatpak exports for Klinker; the broken browser is executable but its interpreter does not exist.
wrapper="$FLATPAK_USER_DIR/exports/bin/$APP_ID"
[[ -x "$wrapper" ]] || die "no exported wrapper $wrapper"
ln -s "$wrapper" "$HOME/host-only/alias-browser"
printf '#!/nonexistent/interpreter\n' >"$HOME/host-only/broken-browser"
chmod +x "$HOME/host-only/broken-browser"
rm -f "$HOME/host-only/launched"
cat >"$config/config.json" <<EOF
{"version": 1,
 "browsers": [{"id": "alias", "manual": true, "name": "Alias", "program": "$HOME/host-only/alias-browser", "arguments": ["{url}"]},
              {"id": "broken", "manual": true, "name": "Broken", "program": "$HOME/host-only/broken-browser", "arguments": ["{url}"]}],
 "rules": [{"enabled": true, "type": "domain", "pattern": "example.org", "browser": "broken"}],
 "chooser": {"closeOnFocusLoss": false}}
EOF
listing="$(klinker "$APP_ID//$BRANCH" --list-browsers)"
grep -qP '^alias\t.*\tThis command would open Klinker itself\.' <<<"$listing" || die "alias of the exported wrapper not refused: $listing"
grep -qP '^broken\t.*\tok$' <<<"$listing" || die "broken browser not listed as usable: $listing"
ok "an alias of Klinker's exported wrapper outside the sandbox is refused as Klinker itself"
# Success would end Klinker at once; a failed start shows the chooser with the link, which stays
# open (closeOnFocusLoss off) until the timeout.
rc=0
run_timeout=15 klinker "$APP_ID//$BRANCH" "$url" || rc=$?
[[ "$rc" == 124 ]] || die "klinker ended ($rc) although the broken browser could not be started"
[[ ! -e "$HOME/host-only/launched" ]] || die "a browser was started"
ok "a browser the host cannot execute is reported, the link is kept"

printf '\n%d checks passed\n' "$pass"

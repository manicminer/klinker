#!/bin/sh
# Runs Wayland-only GUI test functions inside a private, headless KWin with its
# own D-Bus session, so nothing appears on or changes the running desktop.
#
#   tests/wayland-smoke.sh BUILD/tests/test_chooser [TEST FUNCTIONS...]
#
# Without test functions it runs waylandLayerShellPopup. Exit status 77 means
# kwin_wayland or dbus-run-session is not installed.
set -eu

test_binary=$1
shift
[ $# -gt 0 ] || set -- waylandLayerShellPopup

for tool in kwin_wayland dbus-run-session; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "wayland-smoke: $tool not found, skipping" >&2
        exit 77
    fi
done

runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/kbs-wayland-XXXXXX")
chmod 700 "$runtime_dir"
trap 'rm -rf "$runtime_dir"' EXIT

export KBS_SMOKE_RUNTIME_DIR="$runtime_dir"
export KBS_SMOKE_TEST_BINARY="$test_binary"

# Services the nested session activates inherit its output, so that goes to a
# log file; only the test output is printed.
status=0
dbus-run-session -- sh -eu -c '
    export XDG_RUNTIME_DIR="$KBS_SMOKE_RUNTIME_DIR"
    unset DISPLAY WAYLAND_DISPLAY
    kwin_wayland --virtual --no-lockscreen --no-global-shortcuts --no-kactivities \
        --width 1600 --height 900 --output-count "${KBS_SMOKE_OUTPUTS:-1}" --socket kbs-smoke &
    kwin=$!
    trap "kill $kwin 2>/dev/null; wait $kwin 2>/dev/null || true" EXIT
    tries=0
    until [ -S "$XDG_RUNTIME_DIR/kbs-smoke" ]; do
        tries=$((tries + 1))
        if [ $tries -gt 100 ] || ! kill -0 $kwin 2>/dev/null; then
            echo "wayland-smoke: kwin_wayland did not start" >"$XDG_RUNTIME_DIR/test.log"
            exit 1
        fi
        sleep 0.1
    done
    WAYLAND_DISPLAY=kbs-smoke QT_QPA_PLATFORM=wayland QT_FORCE_STDERR_LOGGING=1 \
        "$KBS_SMOKE_TEST_BINARY" "$@" >"$XDG_RUNTIME_DIR/test.log" 2>&1
' kbs-smoke "$@" >"$runtime_dir/session.log" 2>&1 </dev/null || status=$?

if [ -s "$runtime_dir/test.log" ]; then
    cat "$runtime_dir/test.log"
else
    cat "$runtime_dir/session.log" >&2
fi
exit $status

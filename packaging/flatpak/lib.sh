# shellcheck shell=bash
# Shared by the packaging scripts; sourced, not run.

APP_ID="io.github.manicminer.Klinker"
BRANCH="stable"
# The shared manicminer Flatpak repository (published by github.com/manicminer/flatpak).
REPO_URL="https://manicminer.github.io/flatpak/repo/"
RUNTIME_REPO="https://dl.flathub.org/repo/flathub.flatpakrepo"

die() {
    printf '%s: error: %s\n' "${0##*/}" "$*" >&2
    exit 1
}

# Release tags: v<major>.<minor>.<patch>, the version in CMakeLists.txt.
validate_tag() {
    [[ "$1" =~ ^v[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}$ ]]
}

# The version a source tree declares: project(klinker VERSION x.y.z ...).
source_version() {
    sed -n 's/^project(klinker VERSION \([0-9.]*\) .*/\1/p' "$1/CMakeLists.txt"
}

# The refs a build may contain.
allowed_ref() {
    local ref="$1" arch="$2"
    [[ "$ref" == "app/$APP_ID/$arch/$BRANCH" || "$ref" == "runtime/$APP_ID.Locale/$arch/$BRANCH" ]]
}

# The exported files of the app commit: the desktop file, icon and metainfo, under the app ID only.
check_exports() {
    local repo="$1" arch="$2" ref listing unexpected desktop
    ref="app/$APP_ID/$arch/$BRANCH"
    listing="$(ostree --repo="$repo" ls -R "$ref" /export | awk '$1 !~ /^d/ {print $5}')"
    grep -qx "/export/share/applications/$APP_ID.desktop" <<<"$listing" || die "desktop file not exported"
    grep -qx "/export/share/icons/hicolor/scalable/apps/$APP_ID.svg" <<<"$listing" || die "icon not exported"
    unexpected="$(grep -v -E "^/export/share/(applications|icons/hicolor/[^/]+/apps|metainfo)/${APP_ID//./\\.}\.[a-z.]+$" <<<"$listing" || true)"
    [[ -z "$unexpected" ]] || die "unexpected exports: $unexpected"
    desktop="$(ostree --repo="$repo" cat "$ref" "/export/share/applications/$APP_ID.desktop")"
    grep -qx 'Exec=klinker %U' <<<"$desktop" || die "desktop file does not run klinker with the links"
    grep -qx 'MimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;' <<<"$desktop" || die "desktop file claims other types"
}

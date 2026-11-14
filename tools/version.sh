#!/bin/sh
# The project's version, kept the same everywhere it is written down.
#   tools/version.sh               print it (and fail if the files disagree)
#   tools/version.sh set X.Y.Z     write X.Y.Z everywhere
#   tools/version.sh check [vX.Y.Z]  fail if the files disagree, or differ from a tag
# Releases: tools/version.sh set X.Y.Z, add the CHANGELOG.md section, commit,
# then `git tag vX.Y.Z && git push origin vX.Y.Z` (.github/workflows/release.yml).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# every file and the version it holds
versions() {
    printf 'CMakeLists.txt %s\n' "$(sed -n 's/^project(typex2_recomp VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
    printf 'launcher/package.json %s\n' "$(sed -n 's/^  "version": "\([0-9.]*\)",$/\1/p' launcher/package.json)"
    printf 'launcher/package-lock.json %s\n' "$(sed -n '3s/^  "version": "\([0-9.]*\)",$/\1/p' launcher/package-lock.json)"
    printf 'launcher/src-tauri/tauri.conf.json %s\n' "$(sed -n 's/^  "version": "\([0-9.]*\)",$/\1/p' launcher/src-tauri/tauri.conf.json)"
    printf 'launcher/src-tauri/Cargo.toml %s\n' "$(sed -n 's/^version = "\([0-9.]*\)"$/\1/p' launcher/src-tauri/Cargo.toml | head -1)"
    printf 'launcher/src-tauri/Cargo.lock %s\n' "$(sed -n '/^name = "typex2-launcher"$/{n;s/^version = "\([0-9.]*\)"$/\1/p;}' launcher/src-tauri/Cargo.lock)"
    printf 'tools/libretro/typex2_libretro.info %s\n' "$(sed -n 's/^display_version = "\([0-9.]*\)"$/\1/p' tools/libretro/typex2_libretro.info)"
}

check() {
    v=$(versions | awk 'NR == 1 {print $2}')
    bad=$(versions | awk -v v="$v" '$2 != v')
    if [ -n "$bad" ]; then
        echo "version mismatch (CMakeLists.txt has $v):" >&2
        echo "$bad" >&2
        exit 1
    fi
    if [ -n "${1:-}" ] && [ "$1" != "v$v" ]; then
        echo "tag $1 does not match the version in the files ($v)" >&2
        exit 1
    fi
    echo "$v"
}

# in place, with BSD and GNU sed alike
edit() {
    sed -i.bak "$1" "$2" && rm -f "$2.bak"
}

case "${1:-}" in
"") check ;;
check) check "${2:-}" ;;
set)
    new=${2:?usage: tools/version.sh set X.Y.Z}
    echo "$new" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo "not X.Y.Z: $new" >&2; exit 1; }
    edit "s/^project(typex2_recomp VERSION [0-9.]*/project(typex2_recomp VERSION $new/" CMakeLists.txt
    edit "s/^  \"version\": \"[0-9.]*\",$/  \"version\": \"$new\",/" launcher/package.json
    edit "3s/^  \"version\": \"[0-9.]*\",$/  \"version\": \"$new\",/" launcher/package-lock.json
    # the root package's entry in package-lock.json ("packages" -> "")
    edit "/^    \"\": {$/,/^    }/s/^      \"version\": \"[0-9.]*\",$/      \"version\": \"$new\",/" launcher/package-lock.json
    edit "s/^  \"version\": \"[0-9.]*\",$/  \"version\": \"$new\",/" launcher/src-tauri/tauri.conf.json
    # the first version line is the [package] one (sed has no portable "first match only")
    awk -v v="$new" '!done && /^version = "[0-9.]*"$/ {print "version = \"" v "\""; done = 1; next} {print}' \
        launcher/src-tauri/Cargo.toml > launcher/src-tauri/Cargo.toml.new
    mv launcher/src-tauri/Cargo.toml.new launcher/src-tauri/Cargo.toml
    edit "/^name = \"typex2-launcher\"$/{n;s/^version = \"[0-9.]*\"$/version = \"$new\"/;}" launcher/src-tauri/Cargo.lock
    edit "s/^display_version = \"[0-9.]*\"$/display_version = \"$new\"/" tools/libretro/typex2_libretro.info
    check
    ;;
*) echo "usage: tools/version.sh [set X.Y.Z | check [vX.Y.Z]]" >&2; exit 2 ;;
esac

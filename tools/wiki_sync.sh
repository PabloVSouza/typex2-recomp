#!/bin/sh
# Publish docs/ to the GitHub wiki (a separate git repo, <origin>.wiki.git).
# docs/ is the source of truth; the wiki is a mirror. Page links drop their
# .md suffix (wiki URLs have none) and links into the repository become
# absolute GitHub URLs.
# usage: tools/wiki_sync.sh [commit message]
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
origin=$(git -C "$root" remote get-url origin)
wiki=${origin%.git}.wiki.git
blob=$(echo "${origin%.git}" | sed 's|^git@github.com:|https://github.com/|')/blob/main
msg=${1:-"Sync from docs/ at $(git -C "$root" rev-parse --short HEAD)"}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if ! git clone -q "$wiki" "$tmp/wiki" 2>/dev/null; then
    echo "wiki_sync: cannot clone $wiki" >&2
    echo "  enable the wiki and create its first page once in the web UI, then rerun" >&2
    exit 1
fi
find "$tmp/wiki" -maxdepth 1 -name '*.md' -delete
for f in "$root"/docs/*.md; do  # includes _Sidebar.md (wiki navigation)
    sed -e 's|](\.\./\([^)]*\))|]('"$blob"'/\1)|g' \
        -e 's|](\([A-Za-z0-9-]*\)\.md\(#[^)]*\)\{0,1\})|](\1\2)|g' "$f" > "$tmp/wiki/$(basename "$f")"
done
cd "$tmp/wiki"
git add -A
if git diff --cached --quiet; then
    echo "wiki_sync: wiki already up to date"
    exit 0
fi
git commit -q -m "$msg"
git push -q
echo "wiki_sync: pushed to $wiki"

#!/bin/sh
# tools/discord_release.sh vX.Y.Z[-rc.N]: posts a release to the Discord server's #releases channel,
# from the version's section of CHANGELOG.md (an -rc tag uses its version's section).
# DISCORD_RELEASES_WEBHOOK is the channel's webhook URL (a repository secret; release.yml calls this
# after publishing, discord-release.yml posts one again). DRY_RUN=1 prints the post instead.
#
# The post, as the bloodborne-native releases are: "Version X is out", the summary, What's new (one
# bullet per **Label:** paragraph or list), a link to docs/Known-Issues.md (known issues are not in the
# notes), the download, how to update, the community
# line. Plain text: Discord keeps the notes' line breaks, so lines are joined, links and bold dropped.
set -eu
tag=${1:?usage: discord_release.sh vX.Y.Z}
repo=PabloVSouza/typex2-recomp
v=${tag#v}
v=${v%%-*}
cd "$(dirname "$0")/.."
# the icon, pinned to a commit: Discord keeps an image by its URL
icon="https://raw.githubusercontent.com/$repo/6348b03/launcher/src-tauri/icons/128x128%402x.png"

# The notes: docs/releases/<tag>.md when it exists (written for players: its summary and What's new,
# up to its first ## section), else the version's CHANGELOG.md section.
mode=changelog
if [ -f "docs/releases/$tag.md" ]; then
    mode=notes
    notes=$(awk 'NR > 1 && /^## / {exit} NR > 1 && $0 != "What'"'"'s new:"' "docs/releases/$tag.md")
else
    notes=$(awk -v v="$v" '/^## / {on = index($0, "## " v " ") == 1; next} on' CHANGELOG.md)
fi
[ -n "$notes" ] || { echo "no notes for $tag (docs/releases/$tag.md or CHANGELOG.md)" >&2; exit 1; }

version=$(printf '%s' "${tag#v}" | sed 's/-rc\./ RC /')
body=$(printf '%s\n' "$notes" | awk -v repo="$repo" -v version="$version" -v mode="$mode" -v url="https://github.com/$repo/releases/tag/$tag" '
    function plain(s) { gsub(/\*\*/, "", s); s = gensub_links(s); return s }
    function gensub_links(s,   out, i) { # [text](url) -> text
        out = ""
        while (match(s, /\[[^]]*\]\([^)]*\)/)) {
            i = index(substr(s, RSTART), "](")
            out = out substr(s, 1, RSTART - 1) substr(s, RSTART + 1, i - 2)
            s = substr(s, RSTART + RLENGTH)
        }
        return out s
    }
    # paragraphs and list items, each joined into one line
    /^[[:space:]]*$/ { flush(); next }
    mode == "notes" && /^- / { flush(); cur = "TOP " substr($0, 3); next }    # a point
    mode == "notes" && /^ +- / { flush(); sub(/^ +- /, ""); cur = "SUB " $0; next } # a sub-point
    /^- / { flush(); cur = $0; next }
    { sub(/^[[:space:]]+/, ""); cur = cur == "" ? $0 : cur " " $0 }
    function flush() { if (cur != "") items[n++] = cur; cur = "" }
    END {
        flush()
        print "Version " version " is out 🕹️"
        new = ""; summary = ""
        for (i = 0; i < n; i++) {
            s = items[i]
            if (s ~ /^\*\*Known issues:\*\*/) continue # the page below has them
            if (s ~ /^TOP /) { new = new "\n• " plain(substr(s, 5)); continue }
            if (s ~ /^SUB /) { new = new "\n   ◦ " plain(substr(s, 5)); continue }
            if (s ~ /^- /) { new = new "\n   ◦ " plain(substr(s, 3)); continue }
            if (s ~ /^\*\*/) { new = new "\n• " plain(s); continue }
            if (new == "") summary = summary (summary == "" ? "" : "\n\n") plain(s)
            else new = new "\n" plain(s)
        }
        if (summary != "") print "\n" summary
        if (new != "") print "\nWhat'"'"'s new" new
        print "\nKnown issues: https://github.com/" repo "/blob/main/docs/Known-Issues.md"
        print "\nDownload: " url
        if (mode == "notes")
            print "\nTo update: install the new launcher (your library and settings stay), rebuild your games, then send new game packs or the RetroArch core to Android. Steps on the release page. Bring your own games: none are included."
        else
            print "\nInstall over the previous version: your game folders, builds and settings are kept. Bring your own games: none are included."
    }')

# Discord's limit is 2000 characters; the release page has the rest
if [ "$(printf '%s' "$body" | wc -m)" -gt 2000 ]; then
    body="$(printf '%s' "$body" | awk 'BEGIN {n = 0} {n += length($0) + 1; if (n > 1850) exit; print}')
…

Full notes: https://github.com/$repo/releases/tag/$tag"
fi

json=$(jq -n --arg content "$body" --arg avatar "$icon" \
    '{username: "Type X Recomp", avatar_url: $avatar, content: $content, allowed_mentions: {parse: []}}')
if [ "${DRY_RUN:-}" = 1 ]; then
    printf '%s\n' "$body"
    exit 0
fi
[ -n "${DISCORD_RELEASES_WEBHOOK:-}" ] || { echo "DISCORD_RELEASES_WEBHOOK is not set: not posted"; exit 0; }
printf '%s' "$json" | curl -sS -f -o /dev/null -w 'Posted to Discord (%{http_code})\n' \
    -H 'Content-Type: application/json' -H 'User-Agent: typex2-recomp release' \
    --data @- "$DISCORD_RELEASES_WEBHOOK?wait=true"

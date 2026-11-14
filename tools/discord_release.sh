#!/bin/sh
# tools/discord_release.sh vX.Y.Z[-rc.N]: posts a release to the Discord server's #releases channel,
# from the version's section of CHANGELOG.md (an -rc tag uses its version's section).
# DISCORD_RELEASES_WEBHOOK is the channel's webhook URL (a repository secret; release.yml calls this
# after publishing, discord-release.yml posts one again). DRY_RUN=1 prints the post instead.
#
# The post, as the bloodborne-native releases are: "Version X is out", the summary, What's new (one
# bullet per **Label:** paragraph or list), Known issues, the download, how to update, the community
# line. Plain text: Discord keeps the notes' line breaks, so lines are joined, links and bold dropped.
set -eu
tag=${1:?usage: discord_release.sh vX.Y.Z}
repo=PabloVSouza/typex2-recomp
v=${tag#v}
v=${v%%-*}
cd "$(dirname "$0")/.."
# the icon, pinned to a commit: Discord keeps an image by its URL
icon="https://raw.githubusercontent.com/$repo/6348b03/launcher/src-tauri/icons/128x128%402x.png"

notes=$(awk -v v="$v" '/^## / {on = index($0, "## " v " ") == 1; next} on' CHANGELOG.md)
[ -n "$notes" ] || { echo "CHANGELOG.md has no section for $v" >&2; exit 1; }

version=$(printf '%s' "${tag#v}" | sed 's/-rc\./ RC /')
body=$(printf '%s\n' "$notes" | awk -v version="$version" -v url="https://github.com/$repo/releases/tag/$tag" '
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
    /^- / { flush(); cur = $0; next }
    { sub(/^[[:space:]]+/, ""); cur = cur == "" ? $0 : cur " " $0 }
    function flush() { if (cur != "") items[n++] = cur; cur = "" }
    END {
        flush()
        print "Version " version " is out 🕹️"
        new = ""; known = ""; summary = ""
        for (i = 0; i < n; i++) {
            s = items[i]
            if (s ~ /^\*\*Known issues:\*\*/) { sub(/^\*\*Known issues:\*\* */, "", s); known = plain(s); continue }
            if (s ~ /^- /) { new = new "\n   ◦ " plain(substr(s, 3)); continue }
            if (s ~ /^\*\*/) { new = new "\n• " plain(s); continue }
            if (new == "") summary = summary (summary == "" ? "" : "\n\n") plain(s)
            else new = new "\n" plain(s)
        }
        if (summary != "") print "\n" summary
        if (new != "") print "\nWhat'"'"'s new" new
        if (known != "") print "\nKnown issues\n" known
        print "\nDownload: " url
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

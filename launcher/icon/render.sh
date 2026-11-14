#!/bin/sh
# Renders the pixel-art icon (icon.awk, 32x32) to the PNGs the app and the Discord server use,
# with nearest-neighbour scaling (needs awk and ffmpeg), then the launcher's icon set:
#   launcher/icon/render.sh   -> launcher/icon/app-1024.png, discord-640.png, src-tauri/icons/*
set -eu
D=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
LC_ALL=C awk -v MODE=app -v SCREEN=triwin -f "$D/icon.awk" > "$T/app.pam"
LC_ALL=C awk -v MODE=discord -v SCREEN=triwin -f "$D/icon.awk" > "$T/discord.ppm"
ffmpeg -loglevel error -y -i "$T/app.pam" -vf scale=1024:1024:flags=neighbor "$D/app-1024.png"
ffmpeg -loglevel error -y -i "$T/discord.ppm" -vf scale=640:640:flags=neighbor "$D/discord-640.png"
cd "$D/.." && npx tauri icon "$D/app-1024.png"
# tauri's resampling softens pixel art: the sizes that are whole multiples of 32, nearest-neighbour
cd "$D/../src-tauri/icons"
for s in 32 64 128; do ffmpeg -loglevel error -y -i "$T/app.pam" -vf scale=$s:$s:flags=neighbor ${s}x${s}.png; done
ffmpeg -loglevel error -y -i "$T/app.pam" -vf scale=256:256:flags=neighbor 128x128@2x.png
ffmpeg -loglevel error -y -i "$T/app.pam" -vf scale=512:512:flags=neighbor icon.png
# Android's adaptive icon shows only the middle two thirds of its foreground: the art padded to
# 54x54 (54 divides every density's 108..432 px, so the pixels stay square)
LC_ALL=C awk -v MODE=art -v PAD=11 -f "$D/icon.awk" -v SCREEN=triwin > "$T/fg.ppm"
for d in mdpi:108 hdpi:162 xhdpi:216 xxhdpi:324 xxxhdpi:432; do
    ffmpeg -loglevel error -y -i "$T/fg.ppm" -vf scale=${d#*:}:${d#*:}:flags=neighbor android/mipmap-${d%%:*}/ic_launcher_foreground.png
done
# Android's adaptive icon: the icon's own navy behind it (not white), and the same set for the
# standalone app's APK (tools/android/res, compiled by tools/android/package.cmake)
sed -i.bak 's/#FFFFFF/#1B1E3E/' android/values/ic_launcher_background.xml && rm -f android/values/ic_launcher_background.xml.bak
rm -rf "$D/../../tools/android/res" && cp -R android "$D/../../tools/android/res"

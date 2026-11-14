#!/bin/sh
# Stages the kit the release app carries (src-tauri/kit, bundled as a resource):
#   kit/bin     x2recomp and x2-player from a CMake build
#   kit/lib     their libraries that are not part of the OS (SDL3, FFmpeg, capstone, ...)
#   kit/source  the source tree exports build from (CMake files, runtime, tools/android, tools/libretro)
# Only this project's code and those libraries: never game files or generated code.
#   launcher/scripts/stage-kit.sh [build dir]   (default: build)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$ROOT/build}
KIT=$ROOT/launcher/src-tauri/kit
EXE=
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=.exe ;; esac

rm -rf "$KIT"
mkdir -p "$KIT/bin" "$KIT/lib" "$KIT/source"
cp "$BUILD/recompiler/x2recomp$EXE" "$KIT/bin/"
[ -f "$BUILD/x2-player$EXE" ] && cp "$BUILD/x2-player$EXE" "$KIT/bin/"

# the source tree: tracked files only
cd "$ROOT"
git ls-files CMakeLists.txt runtime tools/android tools/libretro | while read -r f; do
    mkdir -p "$KIT/source/$(dirname "$f")"
    cp "$f" "$KIT/source/$f"
done

case "$(uname -s)" in
Darwin)
    # copy every non-system dylib the binaries need (recursively) and load it from kit/lib
    queue=$(ls "$KIT"/bin/*)
    while [ -n "$queue" ]; do
        next=
        for f in $queue; do
            for dep in $(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -v -e '^/usr/lib/' -e '^/System/' -e '^@'); do
                name=$(basename "$dep")
                case "$f" in
                    "$KIT"/bin/*) install_name_tool -change "$dep" "@executable_path/../lib/$name" "$f" ;;
                    *) install_name_tool -change "$dep" "@loader_path/$name" "$f" ;;
                esac
                if [ ! -f "$KIT/lib/$name" ]; then
                    cp "$dep" "$KIT/lib/$name"
                    chmod u+w "$KIT/lib/$name"
                    install_name_tool -id "@loader_path/$name" "$KIT/lib/$name"
                    next="$next $KIT/lib/$name"
                fi
            done
        done
        queue=$next
    done
    # the edits invalidate signatures: sign again (ad hoc; the app bundle is signed over it)
    for f in "$KIT"/lib/* "$KIT"/bin/*; do codesign --force --sign - "$f" 2>/dev/null; done
    ;;
Linux)
    # libraries outside the base system next to the binaries (RPATH $ORIGIN/../lib)
    for f in "$KIT"/bin/*; do
        ldd "$f" | awk '/=> \// {print $3}' | grep -v -E '/(libc|libm|libdl|libpthread|librt|libstdc\+\+|libgcc_s|ld-linux|libGL|libEGL|libGLX|libGLdispatch|libX[a-z0-9]*|libxcb[a-z0-9-]*|libwayland[a-z-]*|libasound|libpulse[a-z-]*|libdbus-1|libudev|libz)\.so' | while read -r dep; do
            cp -L "$dep" "$KIT/lib/"
        done
        patchelf --set-rpath '$ORIGIN/../lib' "$f" 2>/dev/null || echo "patchelf missing: $f keeps its RPATH"
    done
    ;;
MINGW*|MSYS*)
    # MSYS2 CLANG64: the import library game DLLs link against, and the DLLs the binaries
    # load that are not Windows' own, beside them (where Windows looks first)
    cp "$BUILD/libx2-player.dll.a" "$KIT/bin/"
    queue=$(ls "$KIT"/bin/*.exe)
    while [ -n "$queue" ]; do
        next=
        for f in $queue; do
            for dep in $(ldd "$f" | awk '/=> \// {print $3}' | grep -i -v '^/c/windows/'); do
                name=$(basename "$dep")
                if [ ! -f "$KIT/bin/$name" ]; then
                    cp "$dep" "$KIT/bin/$name"
                    next="$next $KIT/bin/$name"
                fi
            done
        done
        queue=$next
    done
    ;;
esac
echo "kit staged in $KIT:"
du -sh "$KIT"/bin "$KIT"/lib "$KIT"/source

#!/bin/sh
# Lint our own sources (never games/*/gen).
#   tools/lint.sh format [base]   clang-format check of lines changed since <base> (default HEAD)
#   tools/lint.sh fix [base]      apply clang-format to those lines
#   tools/lint.sh tidy [files]    clang-tidy (bug-finding checks in .clang-tidy); needs build/compile_commands.json
# LLVM tools come from PATH or Homebrew's llvm (brew install llvm).
set -e
cd "$(dirname "$0")/.."
LLVM=$(brew --prefix llvm 2>/dev/null)/bin
PATH="$PATH:$LLVM"
mode=${1:-format}
[ $# -gt 0 ] && shift

case $mode in
format | fix)
    base=${1:-HEAD}
    files=$(git diff --name-only "$base" -- '*.c' '*.h' '*.cpp' '*.hpp' | grep -v '^games/' || true)
    [ -z "$files" ] && { echo "lint: no changed C/C++ files since $base"; exit 0; }
    if [ "$mode" = fix ]; then
        git clang-format --force --binary "$(command -v clang-format)" "$base" -- $files
    else
        out=$(git clang-format --binary "$(command -v clang-format)" --diff "$base" -- $files || true)
        case $out in
        "" | *"no modified files"* | *"did not modify"*) echo "lint: format ok" ;;
        *) echo "$out"; echo "lint: run tools/lint.sh fix"; exit 1 ;;
        esac
    fi
    ;;
tidy)
    [ -f build/compile_commands.json ] || { echo "lint: configure build/ first (cmake -S . -B build)"; exit 1; }
    if [ $# -eq 0 ]; then
        set -- $(git ls-files 'runtime/src/*.c' 'recompiler/src/*.cpp' 'tools/*.c' 'tools/*.cpp' | grep -v platform_libretro_stub)
    fi
    clang-tidy -p build --quiet "$@"
    ;;
*)
    echo "usage: tools/lint.sh format|fix [base] | tidy [files]"; exit 2 ;;
esac

#!/bin/sh
# Translate every shader in a folder to GLSL (tests/sm_glsl_test.c) and compile each result to
# SPIR-V with glslangValidator; prints the compile failures and a count. The GLSL and SPIR-V go
# to the output folder.
#   usage: tools/sm_glsl_test.sh <shader folder> <output folder> [--opts]
cd "$(dirname "$0")/.." || exit 1
DIR=${1:?shader folder}
OUT=${2:?output folder}
command -v glslangValidator >/dev/null || { echo "glslangValidator is missing (brew install glslang)"; exit 1; }
mkdir -p build "$OUT" || exit 1
rm -f "$OUT"/*.vert "$OUT"/*.frag "$OUT"/*.spv "$OUT"/*.log
clang -O1 -Wall -I runtime/portable runtime/portable/sm_glsl.c tests/sm_glsl_test.c -o build/sm_glsl_test || exit 1
build/sm_glsl_test "$DIR" "$OUT" $3 | tail -20
# each source with and without the user clip planes
ls "$OUT" | grep -E '\.(vert|frag)$' | sed "s|^|$OUT/|" | xargs -P 8 -n 1 sh -c '
    glslangValidator -V --target-env vulkan1.1 -o "$1.spv" "$1" >"$1.log" 2>&1 || echo "FAIL $1"
    case "$1" in *.vert)
        glslangValidator -V --target-env vulkan1.1 -DSM_NO_CLIP_DISTANCE -o "$1.noclip.spv" "$1" >>"$1.log" 2>&1 ||
            echo "FAIL $1 (SM_NO_CLIP_DISTANCE)";;
    esac' sh | sort
echo "sources $(ls "$OUT" | grep -cE '\.(vert|frag)$'), SPIR-V $(ls "$OUT" | grep -c '\.spv$')"

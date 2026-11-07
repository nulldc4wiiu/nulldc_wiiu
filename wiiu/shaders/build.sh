#!/usr/bin/env bash
# Recompiles the GX2 shader pairs with CafeGLSL's desktop compiler (scratch/cafeglsl/linux-x86_64 in the
# wiiu-cemu-hb workspace, from github.com/Exzap/CafeGLSL v1.0.0-rc1). Run from the workspace root:
#   projects/nulldc_wiiu/wiiu/shaders/build.sh
set -euo pipefail
cd "$(dirname "$0")"
for name in blit geo; do
  docker run --rm --platform linux/amd64 -v "$PWD/../../../..:/w" -w "/w/projects/nulldc_wiiu/wiiu/shaders" debian:bookworm \
    /w/scratch/cafeglsl/linux-x86_64/bin/glslcompiler -vs $name.vert -ps $name.frag -o $name.gsh
done

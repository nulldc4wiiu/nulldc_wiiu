#!/bin/sh
# Recompiles the GX2 shader pairs (blit, geo) into the .gsh files the build embeds. Only needed after editing a
# .vert or .frag: the compiled .gsh files are checked in. Needs CafeGLSL's desktop compiler
# (github.com/Exzap/CafeGLSL, v1.0.0-rc1 or later; Linux x86-64):
#   wiiu/shaders/build.sh /path/to/glslcompiler
set -eu
GLSLC="${1:?usage: build.sh /path/to/glslcompiler}"
cd "$(dirname "$0")"
for name in blit geo; do
  "$GLSLC" -vs $name.vert -ps $name.frag -o $name.gsh
done

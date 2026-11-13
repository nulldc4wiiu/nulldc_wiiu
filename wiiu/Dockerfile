# Build environment for nullDC (Wii U).
#
# devkitPro's devkitPPC image already ships the Wii U toolchain (wut, wut-tools, wuhbtool) and cmake/ninja;
# the build needs nothing else.
#
# From the repo root:
#   docker build -t nulldc-wiiu wiiu
#   docker run --rm -v "$PWD":/src nulldc-wiiu wiiu/build-wiiu.sh
# devkitPPC image of 2026-10-16, the toolchain v0.800-U1 was built with
FROM devkitpro/devkitppc@sha256:d50f8a886f2ce1d3c22685a774afb0845379876b47d9b41a9b729f4f63a54813

WORKDIR /src

# Build environment for nullDC (Wii U).
#
# devkitPro's devkitPPC image already ships the Wii U toolchain (wut, wut-tools, wuhbtool) and cmake/ninja;
# the build needs nothing else.
#
# From the repo root:
#   docker build -t nulldc-wiiu wiiu
#   docker run --rm -v "$PWD":/src nulldc-wiiu wiiu/build-wiiu.sh
FROM devkitpro/devkitppc:latest

WORKDIR /src

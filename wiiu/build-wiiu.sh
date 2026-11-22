#!/bin/sh
# Build nullDC (Wii U). From the repo root, inside the Docker image from wiiu/Dockerfile:
#   docker build -t nulldc-wiiu wiiu
#   docker run --rm -v "$PWD":/src nulldc-wiiu wiiu/build-wiiu.sh
# Output: build-wiiu/nulldc.wuhb, plus wiiu/release/ = the files to publish
# (nulldc.wuhb + LICENSE-nullDC4Wii.txt + THIRD-PARTY-NOTICES.txt + SHA256SUMS).
set -e
cd "$(dirname "$0")/.."

echo ">> building nulldc.wuhb"
/opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S wiiu -B build-wiiu -G Ninja -DCMAKE_BUILD_TYPE=Release
cp apps/apps/nulldc4wii/data/fsca-table.bin build-wiiu/content/fsca-table.bin
ninja -C build-wiiu

echo ">> assembling wiiu/release/"
rm -rf wiiu/release && mkdir wiiu/release
cp build-wiiu/nulldc.wuhb build-wiiu/content/LICENSE-nullDC4Wii.txt build-wiiu/content/THIRD-PARTY-NOTICES.txt wiiu/release/
( cd wiiu/release && sha256sum nulldc.wuhb LICENSE-nullDC4Wii.txt THIRD-PARTY-NOTICES.txt > SHA256SUMS )

echo "built: build-wiiu/nulldc.wuhb"
cat wiiu/release/SHA256SUMS

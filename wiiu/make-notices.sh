#!/bin/sh
# Write the license files that ship with the app into OUTDIR (wiiu/CMakeLists.txt runs this at configure time
# and bundles OUTDIR into the wuhb; wiiu/build-wiiu.sh copies them to wiiu/release/):
#   LICENSE-nullDC4Wii.txt   nullDC4Wii's license, verbatim (LICENSE)
#   THIRD-PARTY-NOTICES.txt  everything else in the wuhb, with its license text (from wiiu/licenses/)
# Fails if any text is missing.
# usage: wiiu/make-notices.sh OUTDIR
set -e
OUT="${1:?usage: make-notices.sh OUTDIR}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
mkdir -p "$OUT"

cp "$ROOT/LICENSE" "$OUT/LICENSE-nullDC4Wii.txt"

N="$OUT/THIRD-PARTY-NOTICES.txt"
cat > "$N" <<'EOF'
nullDC (Wii U) - third-party notices

nullDC4Wii itself is covered by LICENSE-nullDC4Wii.txt. The app also contains
the following software and font, each under its own license, reproduced below.

  wut (Wii U toolchain) ..... zlib license
  Roboto font ............... SIL Open Font License 1.1

The GX2 shaders were compiled with CafeGLSL, a build tool; none of it is in the app.
EOF

add() {   # add NAME FILE
    [ -f "$2" ] || { echo "make-notices: missing license text for $1: $2" >&2; exit 1; }
    printf '\n\n==============================================================================\n%s\n==============================================================================\n\n' "$1" >> "$N"
    cat "$2" >> "$N"
}

add "wut"         "$HERE/licenses/wut-LICENSE.txt"
add "Roboto font" "$HERE/licenses/Roboto-OFL.txt"

echo "notices: $OUT/LICENSE-nullDC4Wii.txt $N"

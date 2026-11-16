#!/bin/sh
# devkitPro's makefiles break on paths containing spaces, so build in a
# space-free temp directory and copy the result back.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
# VARIANT=old builds the OLD RELEASE variant (game's own Android controller
# support only, no asset cache) into sixguns-old.nro.
VARIANT="${VARIANT:-}"
BUILD_DIR="${TMPDIR:-/tmp}/sixguns-publish-build${VARIANT:+-$VARIANT}"
DEFS=""
[ "$VARIANT" = "old" ] && DEFS="-DSTOCK_INPUT -DNO_ASSET_CACHE"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"

mkdir -p "$BUILD_DIR"
if rsync --version >/dev/null 2>&1; then
  rsync -a --delete --exclude build --exclude '*.nro' --exclude '*.elf' --exclude '*.nacp' \
    "$HERE/source" "$HERE/Makefile" "$BUILD_DIR/"
else
  # No working rsync (e.g. devkitPro's MSYS2 on Windows): replace the sources,
  # keep the build/ objects so make stays incremental. -p keeps timestamps.
  rm -rf "$BUILD_DIR/source"
  cp -Rp "$HERE/source" "$BUILD_DIR/"
  cp -p "$HERE/Makefile" "$BUILD_DIR/"
  rm -f "$BUILD_DIR"/source/._* 2>/dev/null || true   # macOS AppleDouble files
fi
[ -f "$HERE/icon.jpg" ] && cp "$HERE/icon.jpg" "$BUILD_DIR/"

make -C "$BUILD_DIR" DEFINES="$DEFS" "$@"
OUT="$HERE/sixguns${VARIANT:+-$VARIANT}.nro"
if [ -f "$BUILD_DIR/sixguns.nro" ]; then
  cp "$BUILD_DIR/sixguns.nro" "$OUT" && echo "Built $OUT"
  # keep this build's ELF for tools/scripts/prof_symbolize.py (profiler addresses)
  ID="$( (shasum -a 256 "$OUT" 2>/dev/null || sha256sum "$OUT") | cut -c1-16)"
  mkdir -p "$HERE/build-elf" && cp "$BUILD_DIR/sixguns.elf" "$HERE/build-elf/sixguns-$ID.elf" && echo "ELF kept as port/build-elf/sixguns-$ID.elf (for symbolising crash addresses)"
fi

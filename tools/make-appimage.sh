#!/bin/sh
# ============================================================================
#  Stellarbox Windesktop — construcción del AppImage
#  ---------------------------------------------------------------------------
#  Reutiliza tools/bundle.sh (todas las librerías + plugins en el paquete) y
#  monta encima un AppDir con AppRun, .desktop e icono. Necesita appimagetool.
#
#  Uso:
#      sh tools/make-appimage.sh [carpeta-destino] [opciones de bundle.sh]
#      sh tools/make-appimage.sh dist
#
#  Variables de entorno:
#      APPIMAGETOOL=/ruta/a/appimagetool   (por defecto: appimagetool del PATH)
#      APP_NAME=stellarbox-windesktop
#      APP_ICON=icons/configuracion/desktop.svg
# ============================================================================

set -eu

PROG=$(basename "$0")
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC_DIR=$(dirname "$HERE")

APP_NAME=${APP_NAME:-stellarbox-windesktop}
APP_ICON_REL=${APP_ICON:-icons/configuracion/desktop.svg}
APP_VERSION=${APP_VERSION:-0.2.0}

DEST_DIR=${1:-}
if [ -n "$DEST_DIR" ]; then
    shift
else
    DEST_DIR="$SRC_DIR/dist"
fi
mkdir -p "$DEST_DIR"
DEST_DIR=$(cd "$DEST_DIR" && pwd)

die() { printf '%s: error: %s\n' "$PROG" "$*" >&2; exit 1; }

# 1) localizar appimagetool --------------------------------------------------
APPIMAGETOOL=${APPIMAGETOOL:-$(command -v appimagetool 2>/dev/null || true)}
if [ -z "$APPIMAGETOOL" ]; then
    for c in "$HOME/.local/bin/appimagetool" /usr/local/bin/appimagetool /usr/bin/appimagetool; do
        [ -x "$c" ] && { APPIMAGETOOL=$c; break; }
    done
fi
[ -n "$APPIMAGETOOL" ] || die "falta appimagetool (descárgalo de https://github.com/AppImage/AppImageKit/releases)"

# 2) localizar el binario ya compilado ---------------------------------------
BIN=""
for cand in "$SRC_DIR/build/stellarbox-windesktop" \
            "$SRC_DIR/build-check/stellarbox-windesktop" \
            "$SRC_DIR/build-release/stellarbox-windesktop"; do
    if [ -x "$cand" ]; then BIN=$cand; break; fi
done
if [ -z "$BIN" ]; then
    BIN=$(find "$SRC_DIR" -maxdepth 3 -type f -name stellarbox-windesktop \
          -perm -u+x -not -path '*/portable*' -not -path '*/dist/*' 2>/dev/null | head -1 || true)
fi
[ -n "$BIN" ] || die "no se encuentra el binario compilado (cmake --build build)"

# 3) empaquetar --------------------------------------------------------------
BUNDLE_DIR="$DEST_DIR/AppDir"
APPDIR="$DEST_DIR/$APP_NAME.AppImage"

printf '==> 1/4 Empaquetando librerías en AppDir…\n'
sh "$HERE/bundle.sh" "$BIN" "$BUNDLE_DIR" --icons "$SRC_DIR/icons" \
    --qss "$SRC_DIR/packaging/stellarbox.qss" "$@"

# 4) metadatos de AppImage ---------------------------------------------------
printf '==> 2/4 Añadiendo AppRun y .desktop…\n'
LAUNCHER="$BUNDLE_DIR/$(basename "$BIN").sh"
[ -f "$LAUNCHER" ] || die "bundle.sh no generó el lanzador $LAUNCHER"
cp -f "$LAUNCHER" "$BUNDLE_DIR/AppRun"
chmod +x "$BUNDLE_DIR/AppRun"
cp -f "$SRC_DIR/packaging/$APP_NAME.desktop" "$BUNDLE_DIR/$APP_NAME.desktop"

printf '==> 3/4 Generando el icono…\n'
SVG="$SRC_DIR/$APP_ICON_REL"
PNG="$BUNDLE_DIR/$APP_NAME.png"
rm -f "$PNG"
if [ -f "$SVG" ]; then
    if command -v rsvg-convert >/dev/null 2>&1; then
        rsvg-convert -w 256 -h 256 "$SVG" -o "$PNG" 2>/dev/null || true
    fi
    if [ ! -s "$PNG" ] && command -v convert >/dev/null 2>&1; then
        convert -background none -resize 256x256 "$SVG" "$PNG" 2>/dev/null || true
    fi
    if [ ! -s "$PNG" ] && command -v inkscape >/dev/null 2>&1; then
        inkscape --export-type=png --export-filename="$PNG" -w 256 -h 256 "$SVG" \
            >/dev/null 2>&1 || true
    fi
fi
if [ -s "$PNG" ]; then
    ln -sf "$APP_NAME.png" "$BUNDLE_DIR/.DirIcon"
else
    printf '%s: aviso: no se pudo rasterizar %s; se usará el icono genérico\n' \
        "$PROG" "$APP_ICON_REL" >&2
fi

# 5) construir ----------------------------------------------------------------
printf '==> 4/4 Empaquetando con appimagetool…\n'
rm -f "$APPDIR"
cd "$DEST_DIR"
# APPIMAGE_EXTRACT_AND_RUN=1 permite construir en contenedores sin FUSE.
APPIMAGE_EXTRACT_AND_RUN=1 "$APPIMAGETOOL" --no-appstream "$BUNDLE_DIR" "$APPDIR"

printf '\nAppImage listo: %s (%s)\n' "$APPDIR" "$(du -h "$APPDIR" | cut -f1)"
exit 0
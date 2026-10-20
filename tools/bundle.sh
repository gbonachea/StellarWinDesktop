#!/bin/sh
# ============================================================================
#  Stellarbox Windesktop — empaquetado portátil
#  ---------------------------------------------------------------------------
#  Copia el binario junto a TODAS las librerías compartidas de las que
#  depende (Qt 6, xcb, fontconfig, harfbuzz, icu, ...) y a los plugins de Qt,
#  de forma que la carpeta generada funcione en cualquier Linux x86_64 /
#  aarch64 sin instalar nada en el sistema.
#
#  Estructura generada:
#
#      portable/
#        stellarbox-windesktop          <- binario (RPATH = $ORIGIN/lib)
#        stellarbox-windesktop.sh       <- lanzador (LD_LIBRARY_PATH + QT_PLUGIN_PATH)
#        qt.conf                        <- rutas relativas de Qt (plugins/)
#        icons/configuracion/*.svg
#        stellarbox.qss                 <- tema por defecto (buscado junto al
#                                          binario por la propia aplicación)
#        lib/libQt6*.so.6 ...           <- dependencias resueltas con ldd
#        plugins/platforms/libqxcb.so   <- plugins de Qt (plataforma, imágenes…)
#        plugins/xcbglintegrations/…
#
#  Uso:
#      sh tools/bundle.sh <binario> <carpeta-destino> [opciones]
#
#      --icons <dir>     copia <dir> como icons/ dentro del paquete
#                        (por defecto: icons/ junto al binario)
#      --qss <fichero>   copia <fichero> como stellarbox.qss dentro del paquete
#                        (el tema por defecto que carga la aplicación)
#      --keep <soname>   no empaqueta esa librería (repetible); se añade a
#                        la lista de "siempre del sistema".
#      --no-strip        no quita los símbolos de depuración al copiar.
#      --no-verify       no ejecuta la comprobación final de dependencias.
#
#  Variables de entorno equivalentes:
#      BUNDLE_KEEP_SYSTEM="libdbus-1.so.3"   no empaqueta lo indicado
#      BUNDLE_QT_PLUGINS=/ruta/a/plugins     usa otro árbol de plugins de Qt
#      BUNDLE_IMAGEFORMATS="libqsvg.so"      solo esos formatos de imagen
#                                            (por defecto: todos)
#      BUNDLE_STRIP=0                        no quita símbolos
#      BUNDLE_PLATFORMTHEMES=1               incluye platformthemes/libqgtk3.so
#                                            (arrastra libgtk-3, +30 MB)
#      BUNDLE_WAYLAND=1                       incluye platforms/libqwayland-*
#      BUNDLE_TLS=1                           incluye tls/libqcertonlybackend.so
#      BUNDLE_OPENSSL=1                       añade tls/libqopensslbackend.so
#                                            (arrastra libssl/libcrypto)
#      BUNDLE_GENERIC=1                       incluye plugins/generic/*
#
#  Requisitos: coreutils, patchelf y ldd de glibc.
#  Compatible con /bin/sh (dash): no usa arrays ni extensiones de bash.
# ============================================================================

set -eu

PROG=$(basename "$0")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/stellarbox-bundle.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM

die()  { printf '%s: error: %s\n' "$PROG" "$*" >&2; exit 1; }
warn() { printf '%s: aviso: %s\n' "$PROG" "$*" >&2; }
info() { printf '%s\n' "$*"; }

# --- argumentos --------------------------------------------------------------
SRC_BIN=""
OUT_DIR=""
ICONS_DIR=""
QSS_FILE=""
DO_STRIP=${BUNDLE_STRIP:-1}
DO_VERIFY=1
KEEP_SYSTEM=${BUNDLE_KEEP_SYSTEM:-}

while [ $# -gt 0 ]; do
    case "$1" in
        --icons)     ICONS_DIR=${2:-}; shift 2 ;;
        --qss)       QSS_FILE=${2:-}; shift 2 ;;
        --keep)      KEEP_SYSTEM="$KEEP_SYSTEM ${2:-}"; shift 2 ;;
        --no-strip)  DO_STRIP=0; shift ;;
        --no-verify) DO_VERIFY=0; shift ;;
        -h|--help)   sed -n '2,45p' "$0"; exit 0 ;;
        -*)          die "opción desconocida: $1" ;;
        *)           if [ -z "$SRC_BIN" ]; then SRC_BIN=$1
                  elif [ -z "$OUT_DIR" ]; then OUT_DIR=$1
                  else die "argumento de más: $1 (solo <binario> <destino>)"
                  fi
                  shift ;;
    esac
done

[ -n "$SRC_BIN" ] || die "falta el binario de entrada"
[ -n "$OUT_DIR" ] || die "falta la carpeta de destino"
[ -f "$SRC_BIN" ] || die "no existe el binario: $SRC_BIN"
[ -x "$SRC_BIN" ] || die "el binario no es ejecutable: $SRC_BIN"

for cmd in ldd patchelf readlink; do
    command -v "$cmd" >/dev/null 2>&1 || die "falta la herramienta: $cmd"
done

SRC_BIN=$(readlink -f "$SRC_BIN")
BIN_NAME=$(basename "$SRC_BIN")
SRC_DIR=$(dirname "$SRC_BIN")

# --- política de empaquetado -------------------------------------------------
# Nunca se empaqueta el núcleo de glibc (lo aporta el sistema anfitrión: si se
# incluye una glibc distinta aparecen errores del tipo GLIBC_2.xx not found).
# Todo lo demás, incluidas libstdc++ y libgcc_s, sí se copia: así el paquete
# también funciona en distribuciones más antiguas que la de compilación.
SYSTEM_LIBS="libc.so.6 libm.so.6 libmvec.so.1 libdl.so.2 libpthread.so.0 librt.so.1 \
librt.so.1 libresolv.so.2 libcrypt.so.1 libutil.so.1 libanl.so.1 libnsl.so.1 \
libthread_db.so.1 ld-linux-x86-64.so.2 ld-linux-aarch64.so.2 ld-linux-armhf.so.3 \
linux-vdso.so.1"

is_system_lib() {
    for s in $SYSTEM_LIBS $KEEP_SYSTEM; do
        [ "$s" = "$1" ] && return 0
    done
    return 1
}

# --- preparación de la carpeta ------------------------------------------------
[ "$OUT_DIR" != "/" ] || die "destino '/' no permitido"
mkdir -p "$OUT_DIR" 2>/dev/null || die "no se puede crear el destino: $OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
# El destino se borra entero, así que no puede contener al binario de entrada
case "$SRC_BIN" in
    "$OUT_DIR"/*) die "el destino contiene el binario de entrada: $OUT_DIR" ;;
esac
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/lib" "$OUT_DIR/plugins"
OUT_BIN="$OUT_DIR/$BIN_NAME"

cp -f "$SRC_BIN" "$OUT_BIN"
chmod u+w "$OUT_BIN"

if [ -z "$ICONS_DIR" ] && [ -d "$SRC_DIR/icons" ]; then
    ICONS_DIR="$SRC_DIR/icons"
fi
if [ -n "$ICONS_DIR" ] && [ -d "$ICONS_DIR" ]; then
    cp -r "$ICONS_DIR" "$OUT_DIR/icons"
    info "iconos      -> $OUT_DIR/icons"
fi

# Tema por defecto: la aplicación lo busca junto al binario (stellarbox.qss)
if [ -z "$QSS_FILE" ] && [ -f "$SRC_DIR/stellarbox.qss" ]; then
    QSS_FILE="$SRC_DIR/stellarbox.qss"
fi
if [ -n "$QSS_FILE" ]; then
    [ -f "$QSS_FILE" ] || die "no existe el QSS: $QSS_FILE"
    cp -f "$QSS_FILE" "$OUT_DIR/stellarbox.qss"
    info "tema        -> $OUT_DIR/stellarbox.qss"
fi

# --- localizar el árbol de plugins de Qt --------------------------------------
QT_PLUGINS_SRC=${BUNDLE_QT_PLUGINS:-}
if [ -z "$QT_PLUGINS_SRC" ]; then
    for q in qmake6 qmake; do
        command -v "$q" >/dev/null 2>&1 || continue
        candidate=$("$q" -query QT_INSTALL_PLUGINS 2>/dev/null || true)
        if [ -n "$candidate" ] && [ -d "$candidate" ]; then
            QT_PLUGINS_SRC=$candidate
            break
        fi
    done
fi
if [ -z "$QT_PLUGINS_SRC" ] || [ ! -d "$QT_PLUGINS_SRC" ]; then
    # Alternativa: deducirlo de donde vive libQt6Core (Debian: <prefix>/qt6/plugins)
    qtcore=$(ldd "$SRC_BIN" 2>/dev/null | awk '/libQt6Core\.so/ {print $3; exit}')
    if [ -n "${qtcore:-}" ]; then
        qtdir=$(dirname "$qtcore")
        for candidate in "$qtdir/qt6/plugins" "$qtdir/plugins"; do
            if [ -d "$candidate" ]; then QT_PLUGINS_SRC=$candidate; break; fi
        done
    fi
fi
[ -n "$QT_PLUGINS_SRC" ] && [ -d "$QT_PLUGINS_SRC" ] \
    || die "no se encuentra el directorio de plugins de Qt (usa BUNDLE_QT_PLUGINS=...)"
info "plugins Qt  -> $QT_PLUGINS_SRC"

# --- estado de la resolución de dependencias ---------------------------------
QUEUE="$WORK/queue"
NEXT="$WORK/queue.next"
SEEN="$WORK/seen"
MISSING="$WORK/missing"
LDD_OUT="$WORK/ldd.out"
: > "$SEEN"; : > "$MISSING"; : > "$QUEUE"

lib_count=0

strip_maybe() {
    [ "$DO_STRIP" = "1" ] || return 0
    command -v strip >/dev/null 2>&1 || return 0
    strip --strip-unneeded "$1" >/dev/null 2>&1 || true
}

# Las librerías viven todas en lib/, así que "$ORIGIN" basta entre ellas.
# --force-rpath escribe DT_RPATH (no DT_RUNPATH) para que cada librería
# resuelva también las dependencias de sus vecinas sin LD_LIBRARY_PATH.
patch_lib() {
    patchelf --remove-rpath "$1" >/dev/null 2>&1 || true
    patchelf --force-rpath --set-rpath '$ORIGIN' "$1" >/dev/null 2>&1 \
        || warn "no se pudo ajustar el RPATH de $(basename "$1")"
    strip_maybe "$1"
}

# Un plugin vive en plugins/<subdir>/…: desde ahí hay que subir un nivel por
# cada componente de <subdir> más uno (salir de plugins/), es decir
# plugins/platforms/libqxcb.so -> "$ORIGIN/../../lib".
patch_plugin() {
    plugin=$1
    rel=${plugin#"$OUT_DIR/plugins/"}
    depth=$(dirname "$rel" | tr '/' '\n' | grep -c . || true)
    depth=$((depth + 1))
    up=""
    i=0
    while [ "$i" -lt "$depth" ]; do up="../$up"; i=$((i + 1)); done
    patchelf --remove-rpath "$plugin" >/dev/null 2>&1 || true
    patchelf --force-rpath --set-rpath "\$ORIGIN/${up}lib:\$ORIGIN" "$plugin" \
        >/dev/null 2>&1 || warn "no se pudo ajustar el RPATH de $(basename "$plugin")"
    strip_maybe "$plugin"
}

copy_plugin() {
    src=$1
    subdir=$(dirname "${src#"$QT_PLUGINS_SRC/"}")
    dst="$OUT_DIR/plugins/$subdir/$(basename "$src")"
    mkdir -p "$(dirname "$dst")"
    cp -f "$src" "$dst"
    chmod u+w "$dst"
    patch_plugin "$dst"
    printf '%s\n' "$dst" >> "$QUEUE"
    info "  plugin plugins/$subdir/$(basename "$src")"
}

copy_if_exists() { [ -f "$1" ] && copy_plugin "$1" || true; }

# Analiza un ELF y copia/encola todo lo que aparezca en ldd como "no nativo".
enqueue_deps() {
    target=$1
    if ! ldd "$target" > "$LDD_OUT" 2>"$WORK/ldd.err"; then
        warn "ldd no pudo analizar $(basename "$target")"
        return 0
    fi
    while IFS= read -r line; do
        case "$line" in
            *'=>'*)
                path=${line#*'=> '}
                path=${path%%(*}
                path=$(printf '%s' "$path" | tr -d ' \t')
                [ -n "$path" ] || continue
                [ -f "$path" ] || continue
                name=$(basename "$path")
                is_system_lib "$name" && continue
                case "$path" in "$OUT_DIR"/*) continue ;; esac
                real=$(readlink -f "$path")
                grep -qxF "$real" "$SEEN" 2>/dev/null && continue
                printf '%s\n' "$real" >> "$SEEN"
                printf '%s\n' "$real" >> "$NEXT"
                # DT_NEEDED referencia el SONAME (p. ej. libQt6Core.so.6), así que
                # hay que guardar el fichero con ese nombre aunque en el sistema
                # sea un symlink a libQt6Core.so.6.4.2.
                dest="$OUT_DIR/lib/$name"
                if [ -e "$dest" ]; then
                    [ "$(readlink -f "$dest")" = "$real" ] \
                        || warn "colisión de nombres en lib/: $name ($real)"
                    continue
                fi
                cp -f "$real" "$dest"
                chmod u+w "$dest"
                patch_lib "$dest"
                lib_count=$((lib_count + 1))
                if [ "$(basename "$real")" != "$name" ]; then
                    # El nombre exacto del sistema, por si alguien lo referencia
                    ln -sf "$name" "$OUT_DIR/lib/$(basename "$real")"
                fi
                ;;
            *'not found'*)
                printf '%s\n' "$(printf '%s' "$line" | tr -d ' \t')" >> "$MISSING"
                ;;
        esac
    done < "$LDD_OUT"
}

# El binario principal no vive en lib/: se parchea con su propia ruta.
patchelf --remove-rpath "$OUT_BIN" >/dev/null 2>&1 || true
patchelf --force-rpath --set-rpath '$ORIGIN/lib:$ORIGIN' "$OUT_BIN" >/dev/null 2>&1 \
    || die "patchelf no pudo preparar el RPATH de $OUT_BIN"
strip_maybe "$OUT_BIN"
printf '%s\n' "$OUT_BIN" >> "$QUEUE"

# --- plugins de Qt que se empaquetan ------------------------------------------
info "copiando plugins Qt:"
copy_if_exists "$QT_PLUGINS_SRC/platforms/libqxcb.so"         # plataforma X11/XWayland
copy_if_exists "$QT_PLUGINS_SRC/platforms/libqoffscreen.so"   # arranque sin pantalla
copy_if_exists "$QT_PLUGINS_SRC/platforms/libqminimal.so"     # arranque sin pantalla
if [ "${BUNDLE_WAYLAND:-0}" = "1" ]; then
    for p in "$QT_PLUGINS_SRC"/platforms/libqwayland-*.so; do copy_if_exists "$p"; done
fi
for p in "$QT_PLUGINS_SRC"/xcbglintegrations/*.so; do copy_if_exists "$p"; done
case "${BUNDLE_IMAGEFORMATS:-}" in
    "")     for p in "$QT_PLUGINS_SRC"/imageformats/*.so; do copy_if_exists "$p"; done ;;
    ON|OFF|YES|NO|TRUE|FALSE)
            # Por defecto se empaquetan todos los formatos; se ignoran valores
            # booleanos (p. ej. si la variable llega desde una opción de CMake).
            warn "BUNDLE_IMAGEFORMATS=\"$BUNDLE_IMAGEFORMATS\" no es una lista: se usan todos"
            for p in "$QT_PLUGINS_SRC"/imageformats/*.so; do copy_if_exists "$p"; done ;;
    *)      # Lista explícita para adelgazar, p. ej. "libqsvg.so libqjpeg.so"
            for name in $BUNDLE_IMAGEFORMATS; do
                [ -f "$QT_PLUGINS_SRC/imageformats/$name" ] \
                    || warn "no existe el plugin imageformats/$name"
                copy_if_exists "$QT_PLUGINS_SRC/imageformats/$name"
            done ;;
esac
for p in "$QT_PLUGINS_SRC"/iconengines/*.so;       do copy_if_exists "$p"; done
if [ "${BUNDLE_TLS:-0}" = "1" ]; then
    copy_if_exists "$QT_PLUGINS_SRC/tls/libqcertonlybackend.so"
    if [ "${BUNDLE_OPENSSL:-0}" = "1" ]; then
        copy_if_exists "$QT_PLUGINS_SRC/tls/libqopensslbackend.so"
    fi
else
    warn "tls/ omitido: el escritorio no usa red (ahorra libcurl/libssl, unos 15 MB)."
    warn "  Actívalo con BUNDLE_TLS=1 (más BUNDLE_OPENSSL=1 para HTTPS) si hace falta."
fi
if [ "${BUNDLE_PLATFORMTHEMES:-0}" = "1" ]; then
    for p in "$QT_PLUGINS_SRC"/platformthemes/*.so; do copy_if_exists "$p"; done
else
    warn "platformthemes/ (tema nativo GTK) omitido: se usa el estilo Fusion."
    warn "  Actívalo con BUNDLE_PLATFORMTHEMES=1 si quieres diálogos nativos."
fi
if [ "${BUNDLE_GENERIC:-0}" = "1" ]; then
    for p in "$QT_PLUGINS_SRC"/generic/*.so; do copy_if_exists "$p"; done
fi

# --- resolución recursiva (ldd en cascada) ------------------------------------
info "resolviendo dependencias con ldd…"
while [ -s "$QUEUE" ]; do
    : > "$NEXT"
    while IFS= read -r target; do
        [ -n "$target" ] || continue
        enqueue_deps "$target"
    done < "$QUEUE"
    mv "$NEXT" "$QUEUE"
done
[ "$lib_count" -gt 0 ] || die "no se copió ninguna librería: ¿el binario es ELF dinámico?"

if [ -s "$MISSING" ]; then
    warn "dependencias ausentes en el sistema de compilación (no se empaquetan):"
    while IFS= read -r m; do warn "  $m"; done < "$MISSING"
fi

# --- qt.conf y lanzador --------------------------------------------------------
cat > "$OUT_DIR/qt.conf" <<'EOF'
# Rutas relativas al ejecutable: así Qt encuentra los plugins del paquete
# aunque el sistema tenga otra versión de Qt instalada.
[Paths]
Prefix = .
Plugins = plugins
EOF

# El heredoc va SIN comillas para poder incrustar $BIN_NAME; el resto se
# escapa para que lo interprete el lanzador, no este script.
cat > "$OUT_DIR/$BIN_NAME.sh" <<EOF
#!/bin/sh
# Lanzador portátil: fija las rutas y arranca el gestor de escritorio.
# El binario ya lleva RPATH=\$ORIGIN/lib, pero LD_LIBRARY_PATH y QT_PLUGIN_PATH
# cubren además las librerías que se abren con dlopen() en tiempo de ejecución.
HERE=\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)
export LD_LIBRARY_PATH="\$HERE/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="\$HERE/plugins\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}"
exec "\$HERE/$BIN_NAME" "\$@"
EOF
chmod +x "$OUT_DIR/$BIN_NAME.sh"

# --- comprobación final ---------------------------------------------------------
if [ "$DO_VERIFY" = "1" ]; then
    info "verificando el paquete…"
    raw="$WORK/ldd-all.txt"; left="$WORK/left.txt"; : > "$raw"; : > "$left"
    for target in "$OUT_BIN" "$OUT_DIR"/plugins/*/*.so; do
        [ -f "$target" ] || continue
        ldd "$target" 2>/dev/null | awk '/=>/ {print $3}' >> "$raw" || true
    done
    # Se normalizan las rutas porque ldd devuelve "..././lib/…" al usar $ORIGIN
    while IFS= read -r p; do
        rp=$(readlink -f "$p" 2>/dev/null || printf '%s' "$p")
        case "$rp" in "$OUT_DIR/lib/"*) continue ;; esac
        printf '%s\n' "$rp" >> "$left"
    done < "$raw"
    sort -u "$left" > "$left.sorted" && mv "$left.sorted" "$left"
    if [ -s "$left" ]; then
        info "siguen resolviéndose desde el sistema (esperado: núcleo de glibc):"
        while IFS= read -r l; do printf '  %s\n' "$l"; done < "$left"
    else
        info "todas las dependencias (salvo glibc) salen de lib/: OK"
    fi
    [ -f "$OUT_DIR/plugins/platforms/libqxcb.so" ] \
        || warn "falta platforms/libqxcb.so: el paquete NO arrancará bajo X11"
fi

size=$(du -sh "$OUT_DIR" 2>/dev/null | cut -f1)
info ""
info "Paquete portable listo: $OUT_DIR ($size)"
info "  $lib_count librerías en lib/ · ejecuta ./$BIN_NAME o ./$BIN_NAME.sh"
exit 0
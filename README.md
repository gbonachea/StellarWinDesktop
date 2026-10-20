# Stellarbox Windesktop

Escritorio gráfico dentro de una ventana: fondo de pantalla, iconos del
directorio configurado, menú contextual, arrastrar-y-soltar y panel de widgets.
Escrito en Qt 6 (Widgets) + libxcb.

El objetivo de este repositorio es poder compilarlo y usarlo **en cualquier
Linux sin instalar nada**: el paquete portátil lleva dentro el binario, todas
las librerías compartidas y los plugins de Qt que necesita.

---

## 1. Compilar

Requisitos (solo para compilar; el paquete final no necesita nada de esto):

| Paquete | Para qué |
|---|---|
| `cmake` ≥ 3.16, `g++` con C++17 | compilación |
| `qt6-base-dev` | Qt 6 Widgets/Gui/Core |
| `libxcb1-dev` (`xcb-proto`) | `xcb_connect` / `_NET_WM_WINDOW_TYPE_DESKTOP` |
| `patchelf` | reescribir RPATH al empaquetar |

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/stellarbox-windesktop
```

### Uso del escritorio

* **Arrastrar iconos**: se agarran y siguen al puntero; al soltarlos quedan
  justo donde el cursor y el escritorio recuerda esa posición por monitor.
* **Menú contextual** (clic derecho): sobre un icono permite abrir, renombrar,
  copiar/cortar o enviar a la papelera; sobre el fondo, crear elementos,
  pegar, cambiar el fondo de pantalla, abrir Configuración y **Ordenar los
  iconos**.
* **Ordenar los iconos**: submenú para recolocar los iconos en la rejilla por
  **Nombre**, **Tamaño**, **Tipo** o **Fecha de modificación**, con la opción
  **Orden inverso**. Los iconos del sistema (Equipo, Usuario, Red, Papelera)
  quedan siempre primero y las carpetas delante de los archivos. La elección
  se recuerda en `~/.config/stellarbox/settings.ini`.

---

## 2. Carpeta portátil (lo que buscas)

```sh
cmake --build build --target portable
```

Genera `build/portable/`, autocontenida:

```
portable/
├── stellarbox-windesktop          binario (RPATH = $ORIGIN/lib)
├── stellarbox-windesktop.sh       lanzador (LD_LIBRARY_PATH + QT_PLUGIN_PATH)
├── qt.conf                        rutas relativas de Qt → plugins/
├── stellarbox.qss                 tema por defecto del escritorio
├── icons/configuracion/*.svg      iconos de la ventana de configuración
├── lib/                           76 librerías: Qt 6, xcb, fontconfig, freetype,
│                                  harfbuzz, icu, png, zlib… (todo con $ORIGIN)
└── plugins/
    ├── platforms/libqxcb.so       plataforma X11/XWayland (+ offscreen, minimal)
    ├── xcbglintegrations/         integración GL de XCB
    ├── imageformats/              svg, jpeg, png, webp, tiff, ico…
    └── iconengines/libqsvgicon.so
```

Para usarla, copie la carpeta donde quiera y ejecute:

```sh
./stellarbox-windesktop          # el binario ya busca lib/ por $ORIGIN
./stellarbox-windesktop.sh       # equivalente, útil para tareas de escritorio
```

Para distribuirla:

```sh
tar -cJf stellarbox-windesktop-portable.tar.xz -C build portable
```

**Cómo funciona**

* `bundle.sh` recorre `ldd` en cascada sobre el binario y cada plugin, copia
  cada `.so` a `lib/` usando su **SONAME** (`libQt6Core.so.6`, no
  `libQt6Core.so.6.4.2`) y le pone `RPATH=$ORIGIN` con `patchelf`. Con
  `DT_RPATH` las librerías también se resuelven entre sí sin `LD_LIBRARY_PATH`.
* `qt.conf` con `[Paths] Plugins = plugins` hace que Qt busque los plugins junto
  al ejecutable, ignorando el Qt que tenga instalado el sistema.
* Se empaqueta todo salvo el núcleo de glibc (`libc`, `libm`, `libdl`,
  `libpthread`, `ld-linux`, …): meter una glibc ajena produce errores
  `version 'GLIBC_2.xx' not found`. `libstdc++` y `libgcc_s` **sí** se copian, así
  que el paquete funciona en distribuciones más antiguas que la de compilación.

**Opciones** (todo con `-D` de CMake al configurar, o como variables de entorno
delante del script):

| Opción | Efecto |
|---|---|
| `-DSTELLARBOX_BUNDLE_TLS=ON` | añade `tls/` → arrastra `libcurl`, `libgnutls`… (+15 MB) |
| `-DSTELLARBOX_BUNDLE_OPENSSL=ON` | añade `tls/libqopensslbackend.so` (con `TLS=ON`) |
| `-DSTELLARBOX_BUNDLE_PLATFORMTHEMES=ON` | añade `platformthemes/libqgtk3.so` → diálogos nativos GTK (+30 MB) |
| `-DSTELLARBOX_BUNDLE_WAYLAND=ON` | añade `platforms/libqwayland-*.so` |
| `-DSTELLARBOX_BUNDLE_IMAGEFORMATS="libqsvg.so libqjpeg.so"` | solo esos formatos de imagen |

Sin CMake:

```sh
BUNDLE_TLS=1 sh tools/bundle.sh build/stellarbox-windesktop ~/Portable
```

**Al tamaño** (~80 MB) casi todo es Qt 6 (21 MB) + `libicudata` (30 MB de datos
de Unicode). Si la máquina destino ya tiene ICU, se puede dejar que lo use el
sistema:

```sh
sh tools/bundle.sh build/stellarbox-windesktop ~/Portable \
    --keep libicuuc.so.74 --keep libicui18n.so.74 --keep libicudata.so.74
```

Eso baja el paquete a **~45 MB**, a cambio de que el sistema tenga libicu (casi
todas las distribuciones la tienen). Con `--no-strip` se ahorra algo más.

> **Importante:** para la máxima compatibilidad, compile el paquete en la
> distribución **más antigua** que vaya a usar (p. ej. Debian 11 / Ubuntu 20.04,
> glibc 2.31). Si compila en una máquina con glibc muy reciente, las librerías
> del paquete pedirán símbolos que en un sistema antiguo no existen. Lo mismo
> con la arquitectura: el paquete es para x86_64; para arm64 hay que compilar en
> arm64 (los nombres y la estructura del empaquetado son los mismos).

---

## 3. Lo que el paquete **no** puede incluir

Son partes del sistema, no bibliotecas:

* el kernel y el **núcleo de glibc** (`libc.so.6`, `libm.so.6`, el cargador
  `ld-linux`) — por diseño, ver arriba;
* el **servidor gráfico X11**: el escritorio se apoya en XCB y en el tipo de
  ventana `_NET_WM_WINDOW_TYPE_DESKTOP`. En una sesión Wayland funciona a través
  de XWayland, que viene instalado en GNOME/KDE;
* los **controladores gráficos** que abren `libGL`/`libEGL` con `dlopen`
  (`/usr/lib/x86_64-linux-gnu/dri`): están en el sistema;
* programas que invoca como ayuda: `xrandr` (para enumerar/aplicar resoluciones;
  si no está, usa la información de la pantalla que da Qt) y el gestor de
  archivos del usuario (`QDesktopServices`).

Nada de eso se puede empaquetar: viene del sistema anfitrión.

---

## 4. Instalación classic (opcional)

```sh
sudo cmake --install build          # binario en /usr/local/bin,
                                    # iconos en share/stellarbox/icons,
                                    # entrada de menú en share/applications
```

Los iconos del diálogo de configuración se buscan, en este orden, en
`icons/configuracion` junto al ejecutable y luego en
`/usr/share/stellarbox/icons/configuracion`, por eso funcionan igual en el
paquete portátil y en el instalado. La configuración del usuario sigue en
`~/.config/stellarbox/settings.ini` (no se toca el home del usuario).

---

## 5. AppImage (opcional, no necesario)

Si algún día quieres un único fichero en vez de una carpeta:

```sh
cmake --build build --target appimage     # build/dist/stellarbox-windesktop.AppImage
```

Es el mismo paquete portátil montado en un `AppDir` y comprimido con
`appimagetool` (https://github.com/AppImage/AppImageKit/releases); reutiliza
`tools/bundle.sh`, no duplica la lógica.

---

## 6. Temas (hojas de estilo QSS)

La interfaz (diálogos, menús, configuración) se puede reestilizar con cualquier
hoja **QSS** de Qt 6. El escritorio —iconos y widgets— se pinta a mano con
`QPainter`, así que además deriva sus colores del tema activo para no
desentonar.

**De dónde se carga la hoja de estilo**, en este orden:

1. la variable de entorno `STELLARBOX_QSS=/ruta/tema.qss`;
2. `Appearance/Qss` en `~/.config/stellarbox/settings.ini` (lo fija el diálogo);
3. `stellarbox.qss` junto al ejecutable (el paquete portátil y AppImage llevan
   su propio tema), o `../share/stellarbox/stellarbox.qss` respecto a él (la
   instalación normal, p. ej. `/usr/local/bin` → `/usr/local/share`);
4. `/usr/share/stellarbox/stellarbox.qss` y
   `/usr/local/share/stellarbox/stellarbox.qss`.

El repositorio incluye ese tema por defecto en
[`packaging/stellarbox.qss`](packaging/stellarbox.qss): es el tema oscuro de
siempre más un bloque `@stellarbox` con el cristal translúcido y esquinas algo
más redondeadas. Se instala en `share/stellarbox/` y se copia junto al binario
al empaquetar (`tools/bundle.sh --qss`).

**Cómo se adapta el escritorio**

* Sin QSS se conserva el aspecto original (paneles oscuros translúcidos, texto
  blanco, barras verde/azul/cian/naranja).
* Con un QSS activo:
  * los **colores de selección y resaltado** de los iconos se toman del rol
    `Highlight` de la paleta;
  * si el tema redefine también los colores base (reglas `*`), se usan sus
    colores de fondo y texto; si no, se mantienen los originales, pensados para
    verse sobre el fondo de escritorio;
  * la **barra lateral** de Configuración deja que el propio QSS pinte su fondo.

**Ajuste fino opcional** — un bloque JSON en el propio `.qss` (los colores
admiten `#rgb`, `#rrggbb` y `#aarrggbb`):

```css
/* @stellarbox {
     "accent":          "#93ba84",   // acentos + selección de una vez
     "cpuColor":        "#93ba84",
     "memColor":        "#339dd6",
     "netColor":        "#66d9e8",
     "diskColor":       "#e67e22",
     "selectionColor":  "#93ba84",
     "selectionAlpha":  115,         // opacidad de la selección (0-255)
     "hoverColor":      "#ffffff",
     "textColor":       "#ffffff",
     "panelColor":      "#14181c",
     "panelTitleColor": "#bec8d2",
     "barBgColor":      "#ffffff",
     "iconRadius":      8,           // redondeo de la celda de un icono
     "panelRadius":     14,          // redondeo de los paneles de widget
     "barRadius":       4,
     "panelBlur":       14 } */       // redondeo de las barras;
                                      // cristal esmerilado (0 = sin cristal)

QWidget { background: #1d1d2a; color: #e6e6f0; }
QPushButton { border-radius: 9px; }
/* … resto del tema … */
```

Todas las claves son opcionales: las que no aparezcan se derivan del tema como
se ha descrito. Con los radios del ejemplo se consiguen esquinas más redondeadas
y con `panelColor`/`accent` paneles y selección a juego con el tema. La selección
de los iconos se dibuja translúcida (alfa 115) para no tapar el fondo de
escritorio; con `#aarrggbb` o `selectionAlpha` se controla su opacidad.

**Cristal esmerilado de los widgets.** Por defecto (desde la versión con
`panelBlur`) los paneles del reloj, sistema, red y discos no son un color plano:
dibujan difuminada la parte del fondo de pantalla que tienen detrás, como si
fueran de cristal esmerilado. El desenfoque lo hace la propia aplicación sobre
el fondo (no necesita un compositor ni transparencia real de ventana), se
calcula una sola vez y se reutiliza, así que no afecta al rendimiento de las
actualizaciones por segundo. Sobre el cristal se pinta el `panelColor`: con los
colores por defecto su opacidad se limita a 140 para que el desenfoque se note,
pero si el tema fija `panelColor` (con alfa `#aarrggbb` o sin ella) se respeta
tal cual. `"panelBlur": 0` desactiva el cristal (panel plano) y el radio del
desenfoque se ajusta con `panelBlur` (píxeles).

---

## 7. Estructura del proyecto

| Ruta | Contenido |
|---|---|
| `stellarbox-windesktop.cpp` | todo el código (una sola unidad de traducción) |
| `CMakeLists.txt` | compilación, objetivos `portable` y `appimage` |
| `tools/bundle.sh` | empaquetador portátil (ldd + patchelf + qt.conf) |
| `tools/make-appimage.sh` | envuelve el anterior en un AppImage |
| `packaging/*.desktop` | entrada de menú / metadatos del AppImage |
| `packaging/stellarbox.qss` | tema por defecto (incluye el bloque `@stellarbox`) |
| `icons/configuracion/*.svg` | iconos de la ventana de configuración |
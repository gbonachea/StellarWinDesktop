# ============================================================================
#  Stellarbox Windesktop 0.2.0 — reconstrucción source-level del binario
#  Requiere: Qt 6 (Widgets), libxcb (xcb-proto + xcb)
#
#  Compilación normal:
#      cmake -B build && cmake --build build
#
#  Paquete autocontenido (una carpeta con el binario, sus librerías y los
#  plugins de Qt; no depende de nada instalado en el sistema):
#      cmake --build build --target portable     -> build/portable/
#
#  Opcional, un único fichero que reutiliza el paquete portátil anterior:
#      cmake --build build --target appimage     -> build/dist/*.AppImage
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(Stellarbox-Windesktop VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Rutas relativas a $ORIGIN: el binario busca sus librerías junto a sí mismo
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Core)
find_library(XCB_LIBRARY xcb REQUIRED)

add_executable(stellarbox-windesktop stellarbox-windesktop.cpp)

target_include_directories(stellarbox-windesktop PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(stellarbox-windesktop PRIVATE
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
    ${XCB_LIBRARY}
)

set_target_properties(stellarbox-windesktop PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    # Al instalar (o al empaquetar) las librerías van en <carpeta>/lib
    INSTALL_RPATH "$ORIGIN/lib:$ORIGIN"
)

# Copia la carpeta de iconos y el tema por defecto junto al ejecutable (para
# que las pruebas desde build/ encuentren icons/configuracion/*.svg y
# stellarbox.qss a través de applicationDirPath())
add_custom_command(TARGET stellarbox-windesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/icons ${CMAKE_BINARY_DIR}/icons
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/packaging/stellarbox.qss
            ${CMAKE_BINARY_DIR}/stellarbox.qss
    COMMENT "Copiando iconos y tema por defecto a ${CMAKE_BINARY_DIR}"
)

install(TARGETS stellarbox-windesktop RUNTIME DESTINATION bin)
install(DIRECTORY icons/ DESTINATION share/stellarbox/icons)
install(FILES packaging/stellarbox.qss DESTINATION share/stellarbox)
install(FILES packaging/stellarbox-windesktop.desktop
        DESTINATION share/applications)

# ============================================================================
#  Empaquetado portátil
# ----------------------------------------------------------------------------
#  tools/bundle.sh copia el binario con todas sus dependencias compartidas y los
#  plugins de Qt dentro de una misma carpeta (lib/, plugins/, qt.conf), ajustando
#  los RPATH a $ORIGIN para que no dependa de nada instalado en el sistema.
# ============================================================================
option(STELLARBOX_BUNDLE_PLATFORMTHEMES
       "Incluir platformthemes/libqgtk3.so en el paquete (arrastra libgtk-3)" OFF)
option(STELLARBOX_BUNDLE_WAYLAND
       "Incluir los plugins de plataforma de Wayland" OFF)
option(STELLARBOX_BUNDLE_TLS
       "Incluir los plugins TLS de Qt (arrastran libcurl/libssl, +15 MB)" OFF)
option(STELLARBOX_BUNDLE_OPENSSL
       "Incluir tls/libqopensslbackend.so (requiere STELLARBOX_BUNDLE_TLS)" OFF)
set(STELLARBOX_BUNDLE_IMAGEFORMATS "" CACHE STRING
    "Lista separada por espacios de plugins de imagen a incluir (vacío = todos)")

# Traduce las opciones de CMake en variables de entorno para bundle.sh
set(STELLARBOX_BUNDLE_ENV "")
foreach(_opt
        "STELLARBOX_BUNDLE_PLATFORMTHEMES;BUNDLE_PLATFORMTHEMES"
        "STELLARBOX_BUNDLE_WAYLAND;BUNDLE_WAYLAND"
        "STELLARBOX_BUNDLE_TLS;BUNDLE_TLS"
        "STELLARBOX_BUNDLE_OPENSSL;BUNDLE_OPENSSL"
        "STELLARBOX_BUNDLE_IMAGEFORMATS;BUNDLE_IMAGEFORMATS")
    list(GET _opt 0 _cmake_var)
    list(GET _opt 1 _env_var)
    list(APPEND STELLARBOX_BUNDLE_ENV "${_env_var}=${${_cmake_var}}")
endforeach()

add_custom_target(portable
    COMMAND ${CMAKE_COMMAND} -E env ${STELLARBOX_BUNDLE_ENV}
            sh ${CMAKE_SOURCE_DIR}/tools/bundle.sh
                $<TARGET_FILE:stellarbox-windesktop>
                ${CMAKE_BINARY_DIR}/portable
                --icons ${CMAKE_SOURCE_DIR}/icons
                --qss ${CMAKE_SOURCE_DIR}/packaging/stellarbox.qss
    DEPENDS stellarbox-windesktop
    COMMENT "Empaquetando la versión portátil en ${CMAKE_BINARY_DIR}/portable"
    VERBATIM
)

add_custom_target(appimage
    COMMAND ${CMAKE_COMMAND} -E env ${STELLARBOX_BUNDLE_ENV}
            sh ${CMAKE_SOURCE_DIR}/tools/make-appimage.sh
                ${CMAKE_BINARY_DIR}/dist
    DEPENDS stellarbox-windesktop
    COMMENT "Generando el AppImage en ${CMAKE_BINARY_DIR}/dist"
    VERBATIM
)

# Objetivo de verificación sin X: STELLARBOX_CONFIG_SNAP=1 genera capturas del
# diálogo de Configuración y del escritorio en /tmp/opencode.
add_executable(config-snap EXCLUDE_FROM_ALL stellarbox-windesktop.cpp)
target_compile_definitions(config-snap PRIVATE STELLARBOX_SNAPSHOT_TEST=1)
target_include_directories(config-snap PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(config-snap PRIVATE
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
    ${XCB_LIBRARY}
)
set_target_properties(config-snap PROPERTIES AUTOMOC ON)
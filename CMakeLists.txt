# ============================================================================
#  Stellarbox Windesktop 0.2.0 — reconstrucción source-level del binario
#  Requiere: Qt 6 (Widgets), libxcb (xcb-proto + xcb)
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(Stellarbox-Windesktop VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
)

# Copia la carpeta de iconos junto al ejecutable (para que las prueban desde
# build/ encuentren icons/configuracion/*.svg a través de applicationDirPath())
add_custom_command(TARGET stellarbox-windesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/icons ${CMAKE_BINARY_DIR}/icons
    COMMENT "Copiando iconos a ${CMAKE_BINARY_DIR}/icons"
)

install(TARGETS stellarbox-windesktop RUNTIME DESTINATION bin)
install(DIRECTORY icons/ DESTINATION share/stellarbox/icons)

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
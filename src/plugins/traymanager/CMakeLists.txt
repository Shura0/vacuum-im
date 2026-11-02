project(traymanager)

set(PLUGIN_NAME "traymanager")
set(PLUGIN_DISPLAY_NAME "Tray manager")
set(${PLUGIN_NAME}_IS_ESSENTIAL YES)

include("traymanager.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

if (IS_ENABLED AND UNIX AND NOT APPLE)
	find_package(Qt6DBus REQUIRED)
	target_link_libraries(${PLUGIN_NAME} Qt6::DBus)
endif ()

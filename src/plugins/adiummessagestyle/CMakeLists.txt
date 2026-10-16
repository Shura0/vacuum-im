project(adiummessagestyle)

set(PLUGIN_NAME "adiummessagestyle")
set(PLUGIN_DISPLAY_NAME "Adium message style")

# The plugin is not ported to Qt 6 yet (it depends on Qt WebEngine), so it is
# disabled by default. Enable it explicitly with -DPLUGIN_adiummessagestyle=ON
# once the WebEngine port is complete.
set(PLUGIN_adiummessagestyle NO CACHE BOOL "Enable adiummessagestyle plugin")

include("adiummessagestyle.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

if (IS_ENABLED)
	find_package(Qt6WebEngineWidgets REQUIRED)
	target_link_libraries(${PLUGIN_NAME} Qt6::WebEngineWidgets)
	if (APPLE)
		file(COPY "${CMAKE_SOURCE_DIR}/resources/adiummessagestyles"
			DESTINATION "${CMAKE_BINARY_DIR}/${INSTALL_RESOURCES}")
	else (APPLE)
		install(DIRECTORY "${CMAKE_SOURCE_DIR}/resources/adiummessagestyles"
			DESTINATION "${INSTALL_RESOURCES}"
			COMPONENT ${PLUGIN_NAME})
	endif (APPLE)
endif (IS_ENABLED)

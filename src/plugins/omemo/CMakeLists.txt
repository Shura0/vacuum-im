project(omemo)

set(PLUGIN_NAME "omemo")
set(PLUGIN_DISPLAY_NAME "OMEMO")
# set(PLUGIN_DEPENDENCIES messagewidgets) # used only in CPack
# set(${PLUGIN_NAME}_IS_ESSENTIAL YES)   # only for plugins the client cannot start without

include("omemo.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

if (PLUGIN_omemo)
	find_package(OpenSSL REQUIRED COMPONENTS Crypto)

	target_link_libraries(${PLUGIN_NAME} omemo-c OpenSSL::Crypto)

	# Keep the vendored C library out of the plugin's exported symbol table.
	if(UNIX AND NOT APPLE)
		target_link_options(${PLUGIN_NAME} PRIVATE "-Wl,--exclude-libs,ALL")
	endif()
endif()

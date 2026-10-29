project(vimplugin)

set(PLUGIN_NAME "vimplugin")
set(PLUGIN_DISPLAY_NAME "Vim mode")
set(PLUGIN_DEPENDENCIES messagewidgets)	 # used only in CPack

include("vimplugin.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

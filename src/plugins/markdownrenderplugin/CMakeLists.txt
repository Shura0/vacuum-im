project(markdownrenderplugin)

set(PLUGIN_NAME "markdownrenderplugin")
set(PLUGIN_DISPLAY_NAME "Markdown Render")
set(PLUGIN_DEPENDENCIES messageprocessor) # used only in CPack
# set(${PLUGIN_NAME}_IS_ESSENTIAL YES)   # only for plugins the client cannot start without

include("markdownrenderplugin.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

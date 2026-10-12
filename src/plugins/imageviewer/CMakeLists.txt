project(imageviewer)

set(PLUGIN_NAME "imageviewer")
set(PLUGIN_DISPLAY_NAME "Image viewer")
set(PLUGIN_DEPENDENCIES messagewidgets) # used only in CPack

include("imageviewer.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

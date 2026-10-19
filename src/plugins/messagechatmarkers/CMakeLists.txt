project(messagechatmarkers)

set(PLUGIN_NAME "messagechatmarkers")
set(PLUGIN_DISPLAY_NAME "Chat Markers")
set(PLUGIN_DEPENDENCIES stanzaprocessor messageprocessor servicediscovery messagewidgets optionsmanager) # used only in CPack

include("messagechatmarkers.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

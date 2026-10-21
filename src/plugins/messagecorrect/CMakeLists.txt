project(messagecorrect)

set(PLUGIN_NAME "messagecorrect")
set(PLUGIN_DISPLAY_NAME "Message Correct")
set(PLUGIN_DEPENDENCIES stanzaprocessor messageprocessor servicediscovery messagewidgets optionsmanager) # used only in CPack

include("messagecorrect.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

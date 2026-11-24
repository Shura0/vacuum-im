project(messagedisplayedsync)

set(PLUGIN_NAME "messagedisplayedsync")
set(PLUGIN_DISPLAY_NAME "Message Displayed Sync")
set(PLUGIN_DEPENDENCIES stanzaprocessor messageprocessor servicediscovery messagewidgets optionsmanager xmppstreams pepmanager) # used only in CPack

include("messagedisplayedsync.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

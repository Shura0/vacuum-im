project(messagereceipts)

set(PLUGIN_NAME "messagereceipts")
set(PLUGIN_DISPLAY_NAME "Message Receipts")
set(PLUGIN_DEPENDENCIES xmppstreams stanzaprocessor messageprocessor servicediscovery messagewidgets) # used only in CPack

include("messagereceipts.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")
project(mamarchive)

set(PLUGIN_NAME "mamarchive")
set(PLUGIN_DISPLAY_NAME "MAM Message Archive")
set(PLUGIN_DEPENDENCIES messagearchiver stanzaprocessor servicediscovery) # used only in CPack

include("mamarchive.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

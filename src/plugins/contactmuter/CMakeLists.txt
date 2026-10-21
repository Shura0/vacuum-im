project(contactmuter)

set(PLUGIN_NAME "contactmuter")
set(PLUGIN_DISPLAY_NAME "Contact Muter")
# set(PLUGIN_DEPENDENCIES messagewidgets) # used only in CPack
# set(${PLUGIN_NAME}_IS_ESSENTIAL YES)   # only for plugins the client cannot start without

include("contactmuter.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

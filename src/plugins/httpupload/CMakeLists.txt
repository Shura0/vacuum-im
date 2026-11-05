project(httpupload)

set(PLUGIN_NAME "httpupload")
set(PLUGIN_DISPLAY_NAME "HTTP Upload")
set(PLUGIN_DESCRIPTION "Uploads files to an HTTP server using XEP-0363 and sends the link")
set(PLUGIN_DEPENDENCIES stanzaprocessor servicediscovery urlprocessor messagewidgets messageprocessor multiuserchat notifications rostersview) # used only in CPack

include("httpupload.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

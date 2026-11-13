project(jingle)

set(PLUGIN_NAME "jingle")
set(PLUGIN_DISPLAY_NAME "Jingle (Voice Calls)")
set(PLUGIN_DEPENDENCIES stanzaprocessor xmppstreams servicediscovery optionsmanager) # used only in CPack
set(PLUGIN_DESCRIPTION "Jingle (Voice and Video) calls")

include("jingle.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

if (IS_ENABLED)
	find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
	target_link_libraries(${PLUGIN_NAME} OpenSSL::SSL OpenSSL::Crypto srtp2)

	find_package(Qt6Multimedia REQUIRED)
	target_link_libraries(${PLUGIN_NAME} Qt6::Multimedia)
endif (IS_ENABLED)

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

	#Echo cancellation is optional: without the library the plugin builds and
	#calls work, just without echo control (the user is warned in the log).
	find_package(PkgConfig QUIET)
	if (PkgConfig_FOUND)
		pkg_check_modules(WEBRTC_APM QUIET webrtc-audio-processing-1)
	endif()
	if (WEBRTC_APM_FOUND)
		message(STATUS "jingle: echo cancellation through ${WEBRTC_APM_LIBRARIES}")
		target_compile_definitions(${PLUGIN_NAME} PRIVATE HAVE_WEBRTC_AEC)
		target_include_directories(${PLUGIN_NAME} PRIVATE ${WEBRTC_APM_INCLUDE_DIRS})
		target_compile_options(${PLUGIN_NAME} PRIVATE ${WEBRTC_APM_CFLAGS_OTHER})
		target_link_directories(${PLUGIN_NAME} PRIVATE ${WEBRTC_APM_LIBRARY_DIRS})
		target_link_libraries(${PLUGIN_NAME} ${WEBRTC_APM_LIBRARIES})
		target_link_options(${PLUGIN_NAME} PRIVATE ${WEBRTC_APM_LDFLAGS_OTHER})
	else()
		message(STATUS "jingle: webrtc-audio-processing not found, echo cancellation disabled")
	endif()

	find_package(Qt6DBus QUIET)
	if (NOT Qt6DBus_FOUND)
		message(STATUS "jingle: Qt6 DBus not found, screen sharing disabled")
	elseif (PkgConfig_FOUND)
		pkg_check_modules(GSTREAMER QUIET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
		if (GSTREAMER_FOUND)
			message(STATUS "jingle: screen sharing through xdg-desktop-portal and GStreamer")
			target_compile_definitions(${PLUGIN_NAME} PRIVATE HAVE_SCREENSHARE)
			target_include_directories(${PLUGIN_NAME} PRIVATE ${GSTREAMER_INCLUDE_DIRS})
			target_compile_options(${PLUGIN_NAME} PRIVATE ${GSTREAMER_CFLAGS_OTHER})
			target_link_directories(${PLUGIN_NAME} PRIVATE ${GSTREAMER_LIBRARY_DIRS})
			target_link_libraries(${PLUGIN_NAME} Qt6::DBus ${GSTREAMER_LIBRARIES})
			target_link_options(${PLUGIN_NAME} PRIVATE ${GSTREAMER_LDFLAGS_OTHER})
		else()
			message(STATUS "jingle: GStreamer not found, screen sharing disabled")
		endif()
	endif()

	if (APPLE)
		file(COPY "${CMAKE_SOURCE_DIR}/resources/sounds"
			DESTINATION "${CMAKE_BINARY_DIR}/${INSTALL_RESOURCES}")
	else (APPLE)
		install(DIRECTORY "${CMAKE_SOURCE_DIR}/resources/sounds"
			DESTINATION "${INSTALL_RESOURCES}"
			COMPONENT ${PLUGIN_NAME})
	endif (APPLE)
endif (IS_ENABLED)

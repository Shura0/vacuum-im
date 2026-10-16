if (INSTALL_SDK)
	install(FILES "config.cmake" "translations.cmake"
		DESTINATION "${INSTALL_INCLUDES}/make"
		COMPONENT sdk)
endif (INSTALL_SDK)

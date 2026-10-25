if (NOT SYSTEM_ZLIB_FOUND)
	add_subdirectory(zlib)
endif (NOT SYSTEM_ZLIB_FOUND)

if (NOT SYSTEM_MINIZIP_FOUND)
	add_subdirectory(minizip)
endif (NOT SYSTEM_MINIZIP_FOUND)

if (NOT SYSTEM_IDN_FOUND)
	add_subdirectory(idn)
endif (NOT SYSTEM_IDN_FOUND)

if (NOT SYSTEM_QTLOCKEDFILE_FOUND)
	add_subdirectory(qtlockedfile)
endif (NOT SYSTEM_QTLOCKEDFILE_FOUND)

if (NOT SYSTEM_QXTGLOBALSHORTCUT_FOUND)
	add_subdirectory(qxtglobalshortcut)
endif (NOT SYSTEM_QXTGLOBALSHORTCUT_FOUND)

add_subdirectory(idle)

# libomemo-c is only needed by the omemo plugin.  The plugin option is a
# normal CMake cache variable, so -DPLUGIN_omemo=OFF skips both the library
# and the plugin even though this directory is processed first.
if(NOT DEFINED PLUGIN_omemo)
	set(PLUGIN_omemo ON)
endif()
if(PLUGIN_omemo)
	add_subdirectory(protobuf-c)
	add_subdirectory(libomemo-c)
endif()

include("${CMAKE_SOURCE_DIR}/src/make/config.cmake")

project(protobuf-c C)

add_library(protobuf-c STATIC protobuf-c.c)

target_include_directories(protobuf-c
	PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
	PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/protobuf-c")

set_target_properties(protobuf-c PROPERTIES
	POSITION_INDEPENDENT_CODE ON
	C_VISIBILITY_PRESET hidden
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/libs")

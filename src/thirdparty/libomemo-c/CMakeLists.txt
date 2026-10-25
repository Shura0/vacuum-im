include("${CMAKE_SOURCE_DIR}/src/make/config.cmake")

project(omemo-c C)

include(CheckSymbolExists)
include(TestBigEndian)

check_symbol_exists(memset_s "string.h" HAVE_MEMSET_S)
test_big_endian(WORDS_BIGENDIAN)

file(GLOB_RECURSE OMEMO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
# curve.c calls all_fast_tests(); the slow tests are standalone and not needed.
list(FILTER OMEMO_SOURCES EXCLUDE REGEX "/ed25519/tests/internal_slow_tests\\.c$")

add_library(omemo-c STATIC ${OMEMO_SOURCES})

target_include_directories(omemo-c
	PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
	PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src/curve25519"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/curve25519/ed25519"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/curve25519/ed25519/nacl_includes"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/curve25519/ed25519/additions"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/curve25519/ed25519/additions/generalized"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/curve25519/ed25519/sha512")

if(HAVE_MEMSET_S)
	target_compile_definitions(omemo-c PRIVATE HAVE_MEMSET_S=1)
endif()

if(WORDS_BIGENDIAN)
	target_compile_definitions(omemo-c PRIVATE WORDS_BIGENDIAN)
endif()

target_link_libraries(omemo-c PUBLIC protobuf-c)

if(UNIX)
	target_link_libraries(omemo-c PUBLIC m)
endif()

set_target_properties(omemo-c PROPERTIES
	POSITION_INDEPENDENT_CODE ON
	C_VISIBILITY_PRESET hidden
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/libs")

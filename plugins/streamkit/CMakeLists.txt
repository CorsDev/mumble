# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_library(streamkit SHARED
	"streamkit.cpp"
	"Pages.h"
)

find_package("nlohmann_json" REQUIRED)
find_package("Threads" REQUIRED)

# cpp-httplib is header-only
target_include_directories(streamkit SYSTEM PRIVATE "${3RDPARTY_DIR}/cpp-httplib")

target_link_libraries(streamkit PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

if(WIN32)
	target_link_libraries(streamkit PRIVATE ws2_32)

	# cpp-httplib refuses to build for anything older than Windows 10, while the project as a whole still targets an
	# older version
	get_directory_property(DIRECTORY_DEFINITIONS COMPILE_DEFINITIONS)
	list(FILTER DIRECTORY_DEFINITIONS EXCLUDE REGEX "^_WIN32_WINNT=")
	set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${DIRECTORY_DEFINITIONS}")
	target_compile_definitions(streamkit PRIVATE "_WIN32_WINNT=0x0A00")
endif()

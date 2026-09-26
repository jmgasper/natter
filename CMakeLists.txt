cmake_minimum_required(VERSION 3.16)
project(natter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(NATTER_BUILD_TESTS "Build the unit and end-to-end tests" ON)
option(NATTER_BUILD_CLI "Build natter-cli" ON)
option(NATTER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(CURL REQUIRED)
find_package(OpenSSL 3 REQUIRED)
find_package(Threads REQUIRED)

add_library(natter_core STATIC
	src/core/credentials.cpp
	src/core/emoji.cpp
	src/core/events.cpp
	src/core/http.cpp
	src/core/mrkdwn.cpp
	src/core/models.cpp
	src/core/realtime.cpp
	src/core/session.cpp
	src/core/store.cpp
	src/core/util.cpp
	src/core/web_api.cpp
	src/core/websocket.cpp
	src/core/ws_frame.cpp
)
target_include_directories(natter_core
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/vendor
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_compile_definitions(natter_core PRIVATE NATTER_VERSION="${PROJECT_VERSION}")
target_link_libraries(natter_core PUBLIC CURL::libcurl OpenSSL::SSL OpenSSL::Crypto
	Threads::Threads)
if(HAIKU)
	# BSD sockets live in libnetwork on Haiku.
	target_link_libraries(natter_core PUBLIC network)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(natter_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
	if(NATTER_WARNINGS_AS_ERRORS)
		target_compile_options(natter_core PRIVATE -Werror)
	endif()
endif()

if(NATTER_BUILD_CLI)
	add_executable(natter-cli src/cli/main.cpp)
	target_link_libraries(natter-cli PRIVATE natter_core)
	if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
		target_compile_options(natter-cli PRIVATE -Wall -Wextra -Wno-unused-parameter)
	endif()
endif()

if(NATTER_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
	enable_testing()
	add_subdirectory(tests)
endif()

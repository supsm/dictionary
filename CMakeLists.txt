cmake_minimum_required(VERSION 3.22)

project(dictionary CXX)

option(USE_ASAN "Use address sanitizer" FALSE)
option(BUILD_TESTS TRUE)

if (BUILD_TESTS)
	add_subdirectory(tests)
endif()

find_package(FLTK 1.4 REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(dictionary "src/main.cpp")
add_executable(save_words "src/save_words.cpp")

if (NOT $<CONFIG:Debug>)
	set_target_properties(dictionary PROPERTIES WIN32_EXECUTABLE TRUE MACOSX_BUNDLE TRUE)

	include(CheckIPOSupported)
	check_ipo_supported(RESULT result OUTPUT output)
	if (result)
		set_target_properties(dictionary PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
	else()
		message(WARNING "IPO is not supported. Error: ${output}")
	endif()
endif()

target_compile_features(dictionary PUBLIC cxx_std_23)
set_target_properties(dictionary PROPERTIES CXX_EXTENSIONS FALSE)
target_compile_features(save_words PUBLIC cxx_std_23)
set_target_properties(save_words PROPERTIES CXX_EXTENSIONS FALSE)

if (MSVC)
	target_compile_options(dictionary PRIVATE /W4)
else()
	target_compile_options(dictionary PRIVATE -Wall -Wextra -pedantic)
endif()

set_target_properties(dictionary PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_include_directories(dictionary PUBLIC "include")
target_link_libraries(dictionary PUBLIC fltk::fltk)
target_include_directories(dictionary PUBLIC ${OPENSSL_INCLUDE_DIR})
target_link_libraries(dictionary PUBLIC ${OPENSSL_LIBRARIES})

target_include_directories(save_words PUBLIC "include")
target_include_directories(save_words PUBLIC ${OPENSSL_INCLUDE_DIR})
target_link_libraries(save_words PUBLIC ${OPENSSL_LIBRARIES})

if (USE_ASAN)
	target_compile_options(dictionary PRIVATE -fsanitize=address)
	target_link_options(dictionary PRIVATE -fsanitize=address)
endif()

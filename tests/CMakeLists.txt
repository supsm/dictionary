add_subdirectory(lib/Catch2)

add_executable(tests test_sdict.cpp)
target_include_directories(tests PUBLIC ../src)
target_compile_features(tests PUBLIC cxx_std_23)
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain)

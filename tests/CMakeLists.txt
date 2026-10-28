add_executable(fo4mx_tests
    TestMain.cpp
    test_catalogue.cpp
    test_formats.cpp
    test_merge.cpp
    test_pipeline.cpp
)
target_link_libraries(fo4mx_tests PRIVATE fo4mx_core)
target_include_directories(fo4mx_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(fo4mx_tests PRIVATE FO4MX_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
fo4mx_set_warnings(fo4mx_tests)
add_test(NAME fo4mx_tests COMMAND fo4mx_tests)

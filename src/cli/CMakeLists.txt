add_executable(fo4mx main.cpp)
target_link_libraries(fo4mx PRIVATE fo4mx_core)
fo4mx_set_warnings(fo4mx)
install(TARGETS fo4mx RUNTIME DESTINATION bin)

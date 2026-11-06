add_executable(fo4mx main.cpp)
target_link_libraries(fo4mx PRIVATE fo4mx_core)
fo4mx_set_warnings(fo4mx)
install(TARGETS fo4mx RUNTIME DESTINATION bin)

# The FormID reference (Editor IDs of the base-game masters) next to the executable, as packaged.
add_custom_command(TARGET fo4mx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/resources/Fallout4_reference_editorids.bin $<TARGET_FILE_DIR:fo4mx>)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Sql)

set(CMAKE_AUTOMOC ON)

# The application's code as a library, so the GUI tests drive the same pages the app shows.
add_library(fo4mx_gui STATIC
    AppController.cpp
    MainWindow.cpp
    SetupDialog.cpp
    Theme.cpp
    Format.cpp
    widgets/GraphView.cpp
    widgets/Widgets.cpp
    models/SqlModel.cpp
    models/GroupedSqlModel.cpp
    pages/Page.hpp
    pages/AssetsPage.cpp
    pages/AutoMergerPage.cpp
    pages/DiagnosticsPage.cpp
    pages/ChangesPage.cpp
    pages/CompilationPage.cpp
    pages/ConflictsPage.cpp
    pages/DashboardPage.cpp
    pages/DependenciesPage.cpp
    pages/PluginsPage.cpp
    pages/ReportsPage.cpp
    pages/SettingsPage.cpp
    pages/ValidationPage.cpp
)
target_include_directories(fo4mx_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fo4mx_gui PUBLIC fo4mx_core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql)
fo4mx_set_warnings(fo4mx_gui)

add_executable(Fallout4MultiplexerRenewed WIN32 main.cpp)
target_link_libraries(Fallout4MultiplexerRenewed PRIVATE fo4mx_gui)
set_target_properties(Fallout4MultiplexerRenewed PROPERTIES OUTPUT_NAME "Fallout4MultiplexerRenewed")
fo4mx_set_warnings(Fallout4MultiplexerRenewed)

install(TARGETS Fallout4MultiplexerRenewed RUNTIME DESTINATION bin)

# The FormID reference (Editor IDs of the base-game masters) next to the executable, as packaged.
add_custom_command(TARGET Fallout4MultiplexerRenewed POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/resources/Fallout4_reference_editorids.bin $<TARGET_FILE_DIR:Fallout4MultiplexerRenewed>)

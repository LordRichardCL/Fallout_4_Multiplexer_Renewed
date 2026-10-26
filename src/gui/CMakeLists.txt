find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Sql)

set(CMAKE_AUTOMOC ON)

add_executable(Fallout4MultiplexerRenewed WIN32
    main.cpp
    AppController.cpp
    MainWindow.cpp
    Theme.cpp
    Format.cpp
    widgets/GraphView.cpp
    widgets/Widgets.cpp
    models/SqlModel.cpp
    pages/Page.hpp
    pages/AssetsPage.cpp
    pages/CompilationPage.cpp
    pages/ConflictsPage.cpp
    pages/DashboardPage.cpp
    pages/DependenciesPage.cpp
    pages/PluginsPage.cpp
    pages/ReportsPage.cpp
    pages/SettingsPage.cpp
    pages/ValidationPage.cpp
)

target_include_directories(Fallout4MultiplexerRenewed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Fallout4MultiplexerRenewed PRIVATE fo4mx_core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql)
set_target_properties(Fallout4MultiplexerRenewed PROPERTIES OUTPUT_NAME "Fallout4MultiplexerRenewed")
fo4mx_set_warnings(Fallout4MultiplexerRenewed)

install(TARGETS Fallout4MultiplexerRenewed RUNTIME DESTINATION bin)

# MultiplexerLoadMonitor.dll (Windows only): an F4SE plugin that only observes the game's start-up and
# loads, and logs and shows which stage and file it last saw. Independent of the rest of the project,
# static C runtime, like the Diagnostic Recorder and the archive loader.
add_library(MultiplexerLoadMonitor SHARED
    LoadMonitor.cpp
    MonitorCore.hpp
    ../recorder/F4SEPluginAbi.hpp
)
target_compile_features(MultiplexerLoadMonitor PRIVATE cxx_std_20)
target_compile_definitions(MultiplexerLoadMonitor PRIVATE UNICODE _UNICODE)
target_link_libraries(MultiplexerLoadMonitor PRIVATE shell32 ole32 uuid user32 gdi32)
set_target_properties(MultiplexerLoadMonitor PROPERTIES
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(MultiplexerLoadMonitor PRIVATE /W4 /EHsc)
endif()

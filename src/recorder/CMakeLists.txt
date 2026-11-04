# MultiplexerDiagnosticRecorder.dll: the optional F4SE plugin (Windows only). It is independent of
# the rest of the project (no Qt, spdlog or SQLite) and links the C runtime statically, so the DLL
# needs nothing but Windows' own libraries next to the game.
add_library(MultiplexerDiagnosticRecorder SHARED
    Recorder.cpp
    F4SEPluginAbi.hpp
)
target_compile_features(MultiplexerDiagnosticRecorder PRIVATE cxx_std_20)
target_compile_definitions(MultiplexerDiagnosticRecorder PRIVATE UNICODE _UNICODE)
target_link_libraries(MultiplexerDiagnosticRecorder PRIVATE version shell32 ole32 uuid)
set_target_properties(MultiplexerDiagnosticRecorder PROPERTIES
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(MultiplexerDiagnosticRecorder PRIVATE /W4 /EHsc)
endif()

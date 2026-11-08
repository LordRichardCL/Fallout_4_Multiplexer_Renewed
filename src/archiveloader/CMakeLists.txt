# MultiplexerArchiveLoader.dll: proof of concept (Windows only). Supplies the full SResourceArchiveList2
# to the game, which reads INI values into a 1,024-byte buffer and cuts longer lists. Independent of the
# rest of the project, static C runtime, like the Diagnostic Recorder.
add_library(MultiplexerArchiveLoader SHARED
    ArchiveLoader.cpp
    ../recorder/F4SEPluginAbi.hpp
)
target_compile_features(MultiplexerArchiveLoader PRIVATE cxx_std_20)
target_compile_definitions(MultiplexerArchiveLoader PRIVATE UNICODE _UNICODE)
target_link_libraries(MultiplexerArchiveLoader PRIVATE shell32 ole32 uuid)
set_target_properties(MultiplexerArchiveLoader PROPERTIES
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(MultiplexerArchiveLoader PRIVATE /W4 /EHsc)
endif()

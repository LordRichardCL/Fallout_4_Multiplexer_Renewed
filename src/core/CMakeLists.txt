include(${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake)
fo4mx_embed_text(${PROJECT_SOURCE_DIR}/resources/record_catalogue.csv
                 ${CMAKE_CURRENT_BINARY_DIR}/generated/RecordCatalogueData.cpp kRecordCatalogue)

add_library(fo4mx_core STATIC
    util/Compression.cpp
    util/IniFile.cpp
    util/Json.cpp
    util/Log.cpp
    util/Platform.cpp
    util/Text.cpp
    esp/FieldDecoder.cpp
    esp/PluginFile.cpp
    esp/PluginHeader.cpp
    esp/PluginWriter.cpp
    esp/Record.cpp
    esp/RecordCatalogue.cpp
    esp/RecordDefinitions.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/RecordCatalogueData.cpp
    esp/StringTables.cpp
    ba2/Ba2Archive.cpp
    vfs/GameInstall.cpp
    vfs/VirtualFileSystem.cpp
    assets/AssetIndex.cpp
    assets/AssetPreview.cpp
    assets/AssetScanner.cpp
    engine/BuildCache.cpp
    engine/AssetTracer.cpp
    engine/ChangeDetector.cpp
    engine/CompilationEngine.cpp
    engine/ConflictEngine.cpp
    engine/DependencyEngine.cpp
    engine/Diagnosis.cpp
    engine/Exporter.cpp
    engine/FieldModel.cpp
    engine/MergeEngine.cpp
    engine/Normalizer.cpp
    engine/Pipeline.cpp
    engine/PluginScanner.cpp
    engine/Session.cpp
    engine/ValidationEngine.cpp
    db/Database.cpp
    db/DatabaseWriter.cpp
    report/ReportGenerator.cpp
)

target_include_directories(fo4mx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(fo4mx_core PUBLIC cxx_std_20)
target_link_libraries(fo4mx_core PUBLIC ZLIB::ZLIB SQLite::SQLite3 spdlog::spdlog)
find_package(Threads REQUIRED)
target_link_libraries(fo4mx_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(fo4mx_core PUBLIC version shell32 ole32 advapi32)
endif()
target_compile_definitions(fo4mx_core PUBLIC FO4MX_VERSION="${PROJECT_VERSION}")
fo4mx_set_warnings(fo4mx_core)

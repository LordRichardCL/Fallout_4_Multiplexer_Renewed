include(${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake)
fo4mx_embed_text(${PROJECT_SOURCE_DIR}/resources/record_catalogue.csv
                 ${CMAKE_CURRENT_BINARY_DIR}/generated/RecordCatalogueData.cpp kRecordCatalogue)

# Code fingerprints of the build cache's components (cmake/CacheComponents.cmake): run at every build that
# changes a core file, and written only when a fingerprint changes.
file(GLOB_RECURSE FO4MX_CORE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp ${CMAKE_CURRENT_SOURCE_DIR}/*.inc)
set(FO4MX_CACHE_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/CacheFingerprints.hpp)
string(JOIN " " FO4MX_TOOLCHAIN ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${CMAKE_BUILD_TYPE})
add_custom_command(
    OUTPUT ${FO4MX_CACHE_HEADER}
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${FO4MX_CACHE_HEADER}
            -DLIST=${PROJECT_SOURCE_DIR}/cmake/CacheComponents.cmake "-DTOOLCHAIN=${FO4MX_TOOLCHAIN}"
            -P ${PROJECT_SOURCE_DIR}/cmake/CacheFingerprint.cmake
    DEPENDS ${FO4MX_CORE_SOURCES} ${PROJECT_SOURCE_DIR}/resources/record_catalogue.csv
            ${PROJECT_SOURCE_DIR}/cmake/CacheComponents.cmake ${PROJECT_SOURCE_DIR}/cmake/CacheFingerprint.cmake
    COMMENT "Build cache code fingerprints"
    VERBATIM)

add_library(fo4mx_core STATIC
    ${FO4MX_CACHE_HEADER}
    engine/CacheVersions.cpp
    diagnostics/RecorderLog.cpp
    diagnostics/RecorderPackage.cpp
    util/Compression.cpp
    util/Zip.cpp
    util/IniFile.cpp
    util/Json.cpp
    util/Log.cpp
    util/Platform.cpp
    util/Text.cpp
    util/FileTransaction.cpp
    esp/FieldDecoder.cpp
    esp/FieldValues.cpp
    esp/GroupLayout.cpp
    esp/LayoutCheck.cpp
    esp/PluginFile.cpp
    esp/PluginHeader.cpp
    esp/PluginWriter.cpp
    esp/PrevisFile.cpp
    esp/Record.cpp
    esp/RecordCatalogue.cpp
    esp/RecordDefinitions.cpp
    esp/SubrecordOrder.cpp
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
    engine/AutoMerger.cpp
    engine/ChangeDetector.cpp
    engine/CleaningEngine.cpp
    engine/CompilationEngine.cpp
    engine/PrevisCheck.cpp
    engine/CompilationResult.cpp
    engine/ConflictEngine.cpp
    engine/DependencyEngine.cpp
    engine/Diagnosis.cpp
    engine/Exporter.cpp
    engine/PluginIni.cpp
    engine/Redirection.cpp
    engine/SwitchBack.cpp
    engine/SupportBundle.cpp
    engine/BuildComparison.cpp
    engine/SlotUsage.cpp
    diagnostics/CrashLog.cpp
    diagnostics/PapyrusLog.cpp
    diagnostics/SaveGame.cpp
    diagnostics/StartupCheck.cpp
    assets/BcDecode.cpp
    engine/FieldModel.cpp
    engine/MergeEngine.cpp
    engine/Mo2Support.cpp
    engine/Normalizer.cpp
    engine/OutputRuns.cpp
    engine/OverwrittenChanges.cpp
    engine/BuildRecord.cpp
    engine/EarlierBuilds.cpp
    engine/Pipeline.cpp
    engine/PluginScanner.cpp
    engine/InactivePreview.cpp
    engine/PluginSummary.cpp
    engine/Provenance.cpp
    engine/Compatibility.cpp
    engine/PatchPlan.cpp
    engine/PostCompilation.cpp
    engine/ReferenceIndex.cpp
    engine/ReferenceScanner.cpp
    engine/Session.cpp
    engine/ValidationEngine.cpp
    engine/VortexSupport.cpp
    db/Database.cpp
    db/DatabaseWriter.cpp
    papyrus/Pex.cpp
    report/ReportGenerator.cpp
    reference/FormIdReference.cpp
)

target_include_directories(fo4mx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(fo4mx_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(fo4mx_core PUBLIC cxx_std_20)
target_link_libraries(fo4mx_core PUBLIC ZLIB::ZLIB SQLite::SQLite3 spdlog::spdlog)
find_package(Threads REQUIRED)
target_link_libraries(fo4mx_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(fo4mx_core PUBLIC version shell32 ole32 advapi32)
endif()
target_compile_definitions(fo4mx_core PUBLIC FO4MX_VERSION="${PROJECT_VERSION}" FO4MX_HOTFIX=${FO4MX_HOTFIX})
fo4mx_set_warnings(fo4mx_core)

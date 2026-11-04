#pragma once

// The small part of the F4SE plugin interface the Diagnostic Recorder uses, declared here so the
// recorder needs no F4SE source. Layouts and values follow F4SE's PluginAPI.h as published by the
// F4SE team (checked against F4SE 0.6.23, the last Old Gen release, and 0.7.9, Next Gen): the
// interface passed to a plugin, the messaging interface and its message numbers, the plugin
// information filled in by F4SEPlugin_Query (0.6.x), and the version data 0.7.x reads from
// F4SEPlugin_Version. Nothing here touches game memory: the recorder uses no game addresses and
// no game structures.

#include <cstdint>

namespace f4se_abi {

using PluginHandle = std::uint32_t;
constexpr PluginHandle kPluginHandleInvalid = 0xFFFFFFFF;

enum : std::uint32_t {
    kInterfaceInvalid = 0,
    kInterfaceMessaging = 1,
};

// Version numbers as F4SE packs them: major << 24 | minor << 16 | build << 4 | sub.
constexpr std::uint32_t packVersion(std::uint32_t major, std::uint32_t minor, std::uint32_t build, std::uint32_t sub = 0)
{
    return ((major & 0xFF) << 24) | ((minor & 0xFF) << 16) | ((build & 0xFFF) << 4) | (sub & 0xF);
}

struct PluginInfo {
    std::uint32_t infoVersion; // 1
    const char* name;
    std::uint32_t version;
};

struct F4SEInterface {
    std::uint32_t f4seVersion;
    std::uint32_t runtimeVersion;
    std::uint32_t editorVersion;
    std::uint32_t isEditor;
    void* (*QueryInterface)(std::uint32_t id);
    PluginHandle (*GetPluginHandle)();
    std::uint32_t (*GetReleaseIndex)();
    const PluginInfo* (*GetPluginInfo)(const char* name); // F4SE 0.6.22+
    const char* (*GetSaveFolderName)();                     // F4SE 0.7.1+ only: check f4seVersion first
};

struct F4SEMessagingInterface {
    struct Message {
        const char* sender;
        std::uint32_t type;
        std::uint32_t dataLen;
        void* data;
    };
    using EventCallback = void (*)(Message* msg);

    // Messages F4SE itself sends (sender "F4SE").
    enum : std::uint32_t {
        kPostLoad = 0,      // every plugin has loaded
        kPostPostLoad = 1,
        kPreLoadGame = 2,   // data: save file path (dataLen characters)
        kPostLoadGame = 3,  // data: bool, the save loaded
        kPreSaveGame = 4,
        kPostSaveGame = 5,
        kDeleteGame = 6,    // data: save file path
        kInputLoaded = 7,
        kNewGame = 8,
        kGameLoaded = 9,    // the game finished its first load (sent once)
        kGameDataReady = 10 // data: bool, false before the plugins load, true when they have
    };

    std::uint32_t interfaceVersion;
    bool (*RegisterListener)(PluginHandle listener, const char* sender, EventCallback handler);
    bool (*Dispatch)(PluginHandle sender, std::uint32_t messageType, void* data, std::uint32_t dataLen,
                     const char* receiver);
};

// Read by F4SE 0.7.x from the exported F4SEPlugin_Version symbol.
struct F4SEPluginVersionData {
    enum : std::uint32_t { kVersion = 1 };
    enum : std::uint32_t {
        kAddressIndependenceSignatures = 1 << 0, // no hard-coded addresses
    };
    enum : std::uint32_t {
        kStructureIndependenceNoStructs = 1 << 0, // no game structures
    };

    std::uint32_t dataVersion;
    std::uint32_t pluginVersion;
    char name[256];
    char author[256];
    std::uint32_t addressIndependence;
    std::uint32_t structureIndependence;
    std::uint32_t compatibleVersions[16];
    std::uint32_t seVersionRequired;
    std::uint32_t reservedNonBreaking;
    std::uint32_t reservedBreaking;
    std::uint8_t reserved[512];
};

static_assert(sizeof(F4SEPluginVersionData) == 4 + 4 + 256 + 256 + 4 + 4 + 64 + 4 + 4 + 4 + 512,
              "F4SEPluginVersionData must match F4SE's layout");

} // namespace f4se_abi

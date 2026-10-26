# Fallout 4 Multiplexer Renewed

**A Fallout 4 mod compilation, conflict resolution and load order intelligence platform.**

Author: Romewolf

Fallout 4 Multiplexer Renewed is a compiler for large modded Fallout 4 installations. It analyses every plugin, asset and dependency, works out the final winning state of the mod collection, and compiles that state into a configurable set of generated output plugins (10–20 by default). Those plugins contain fully materialised Bethesda records and open in xEdit, FO4Edit and the Creation Kit like any normal plugin.

It is **not** a mod manager, load order sorter, Bash Patch replacement, merge patch generator, runtime virtualisation layer or F4SE runtime solution.

## Target platform

- **Primary runtime:** Fallout 4 1.10.163 (Old Gen)
- **Future profiles:** Next Gen and later Bethesda releases
- **OS:** Windows 10 / 11, 64-bit only
- **Mod managers:** Mod Organizer 2 (priority 1, including the MO2 VFS), Vortex, standalone installs

## Technology stack

C++20 · Qt 6 · SQLite · CommonLibF4 · zlib · spdlog

## Architecture

```
Plugin Scanner → Asset Scanner → Database Engine → Conflict Engine → Dependency Engine
  → Merge Engine → FormID Translation Engine → Compilation Engine
  → Dummy Plugin Generator → Validation Engine
```

## Workflow

1. Install mods normally
2. Launch Fallout 4 Multiplexer Renewed
3. Scan the installation
4. Review the analysis, conflicts and recommendations
5. Compile and generate the output plugins
6. Disable the original plugins
7. Play using the compiled output

## Documentation

- [Vision, Architecture & Product Specification](docs/VISION_SPECIFICATION.md)

## Related

- [Fallout 4 Transition Layer](https://github.com/LordRichardCL/Fallout_4_Transition_Layer)

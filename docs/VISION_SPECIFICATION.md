# Fallout 4 Multiplexer Renewed
## Vision, Architecture & Product Specification

| | |
|---|---|
| **Author** | Romewolf |
| **Project Name** | Fallout 4 Multiplexer Renewed |
| **Version** | Vision Specification v1.0 |
| **Project Classification** | Fallout 4 Mod Compilation, Conflict Resolution and Load Order Intelligence Platform |

---

## Executive Summary

Fallout 4 Multiplexer Renewed is a next-generation Fallout 4 mod compilation platform designed specifically for large-scale modded installations.

The software is intended to solve some of the largest challenges facing the Fallout 4 modding community:

- Plugin limitations
- Large load-order management
- Record conflicts
- Asset conflicts
- Mod compatibility issues
- Dependency complexity
- Performance degradation from excessive plugin counts
- The difficulty of understanding the final game state

Unlike traditional Fallout 4 tools, Fallout 4 Multiplexer Renewed is not a plugin manager, load order sorter, merge patch generator or runtime virtualisation framework.

Instead, the application functions as a **compiler**.

The software analyses every plugin, every asset and every dependency within a Fallout 4 installation, determines the final winning state of the user's mod collection, and compiles the resulting game state into a configurable set of generated output plugins.

These generated plugins contain fully materialised Bethesda records and can be viewed directly inside:

- xEdit
- FO4Edit
- Creation Kit

The generated plugins become the compiled representation of the user's entire mod setup.

## Mission Statement

The goal of Fallout 4 Multiplexer Renewed is to become the definitive Fallout 4 load order intelligence platform.

The application should provide users with the ability to:

- Understand exactly what their game is loading
- Identify every conflict
- Identify every overriding asset
- Analyse dependencies
- Predict compatibility issues
- Visualise the final game state
- Compile thousands of plugins into a small, manageable output

The software should become the authoritative source of truth for a Fallout 4 installation.

## Core Vision

Current Fallout 4 modding requires users to rely on multiple different tools:

```
Mod Organizer 2
Vortex
xEdit
Wrye Bash
Archive Tools
Conflict Tools
Manual Inspection
```

These tools provide fragmented information about the game. Fallout 4 Multiplexer Renewed consolidates all of this information into a single unified platform.

The software should understand:

```
Plugins
Records
Assets
Dependencies
Overrides
References
Archives
Load Order
Final Winners
```

and present them in a single interface.

## Target Audience

- StoryWealth Users
- Heavy Modders
- Curated Collection Authors
- Wabbajack Users
- Community Maintainers
- Plugin Developers
- Advanced Fallout 4 Users

## Target Platform Requirements

### Primary Runtime

Fallout 4 Multiplexer Renewed is designed primarily for **Fallout 4 Version 1.10.163 (Old Gen)**.

#### Reasoning

The majority of advanced Fallout 4 users continue to operate downgraded installations because of:

- F4SE Compatibility
- Plugin Compatibility
- Framework Stability
- Existing Mod Support
- Long-Term Community Adoption

Examples include StoryWealth, large curated lists, private collections and Wabbajack installations.

### Development Assumption

Unless otherwise specified, validation, testing, record creation, plugin generation and compatibility checks shall target **Fallout 4 1.10.163**.

### Future Runtime Profiles

Architecture should support future expansion to:

- Fallout 4 Next Gen
- Additional Runtime Variants
- Future Bethesda Releases

but these are secondary priorities.

## Product Philosophy

Most existing solutions attempt to work around plugin limitations. Fallout 4 Multiplexer Renewed instead attempts to understand and compile the entire game state.

The software acts similarly to a traditional compiler.

**Input:**

```
Thousands of Mods
Thousands of Plugins
BA2 Archives
Loose Files
Game Assets
```

**Output:**

```
Dummy_Plugin_01.esp
Dummy_Plugin_02.esp
Dummy_Plugin_03.esp
...
Dummy_Plugin_20.esp
```

The generated output represents the final state of the user's game.

## What This Project Is Not

- Not A Mod Manager
- Not A Load Order Sorter
- Not A Bash Patch Replacement
- Not A Runtime Virtualisation Layer
- Not A Merge Patch Generator
- Not An F4SE Runtime Solution

## What This Project Is

- A Load Order Compiler
- A Conflict Resolution Engine
- A Plugin Intelligence Platform
- An Asset Intelligence Platform
- A Fallout 4 Analysis Suite
- A Complete Game State Compiler

## Ultimate Goal

The ideal workflow should be:

1. Install Mods Normally
2. Launch Fallout 4 Multiplexer Renewed
3. Scan Installation
4. Review Analysis
5. Review Conflicts
6. Review Recommendations
7. Compile
8. Generate Output Plugins
9. Disable Original Plugins
10. Play Fallout 4 Using Compiled Output

## What The User Can Do

### Plugin Analysis

- See Every Installed Plugin
- Identify Plugin Types
- Find Master Relationships
- Inspect Record Counts
- Determine Compilation Eligibility

### Conflict Analysis

- View Override Chains
- Understand Winning Records
- Understand Why Records Win
- Identify Problematic Mods
- Detect Hard Conflicts

### Asset Analysis

- View Texture Conflicts
- View Mesh Conflicts
- View Script Conflicts
- View Animation Conflicts
- View Material Conflicts
- View BA2 Archive Conflicts

### Dependency Analysis

- Trace Dependencies
- Visualise Relationships
- Identify Missing Requirements
- Find Potential Breakages

### Compilation

- Compile Entire Mod Lists
- Generate Output Plugins
- Create A Materialised Game State
- Reduce Plugin Complexity

## High-Level Architecture

```
Plugin Scanner
      │
      ▼
Asset Scanner
      │
      ▼
Database Engine
      │
      ▼
Conflict Engine
      │
      ▼
Dependency Engine
      │
      ▼
Merge Engine
      │
      ▼
FormID Translation Engine
      │
      ▼
Compilation Engine
      │
      ▼
Dummy Plugin Generator
      │
      ▼
Validation Engine
```

## Technology Stack

**Preferred Technologies:** C++20, Qt 6, SQLite, CommonLibF4, zlib, spdlog

**Platform:** Windows 10, Windows 11 — 64 Bit Only

**Database:** SQLite

## Mod Manager Integration

| Priority | Target |
|---|---|
| 1 | Mod Organizer 2 |
| 2 | Vortex |
| 3 | Standalone Fallout 4 Installations |

The software must correctly interpret MO2 virtual file systems. **This is mandatory for accurate conflict detection.**

## Unlimited Internal Plugin Processing

The application shall maintain its own plugin indexing system:

```
Plugin #1
Plugin #2
Plugin #3
...
Plugin #5000+
```

The application shall not rely on Fallout 4 runtime limitations. Available memory and storage should be the only practical limit.

## Supported Plugin Formats

- ESM
- ESP
- ESL
- ESL Flagged ESP

Must support:

- Compressed Records
- Injected Records
- Master References
- Light Plugin Mapping
- Override Chains

## Supported Records (Phase One)

```
WEAP  ARMO  AMMO  OMOD  COBJ  KYWD
LVLI  FLST  INGR  ALCH  MISC  BOOK
NOTE  KEYM  FACT  PERK  SPEL  MESG
```

> Note: the original draft listed `AID`; this is assumed to refer to `ALCH` (aid items / consumables), which is the Fallout 4 record signature for aid.

## Future Record Support

```
NPC_  QUST  WRLD  CELL  REFR
ACHR  CONT  DIAL  INFO
```

> Note: the quest record signature in Fallout 4 is `QUST`.

## Internal Database Design

Core data tables:

- Plugins
- Records
- Assets
- Conflicts
- Dependencies
- Masters
- OverrideChains
- FormMappings
- GeneratedRecords
- ValidationResults
- CompilationGroups

Processing should be fully data-driven.

## User Interface Vision

The software should look like a professional engineering application.

**Design Inspiration:** Visual Studio, Power BI, MO2, xEdit, Windows 11 Fluent Design

**Visual Style:**

- Dark Theme
- Fallout Green Highlights
- Professional Dashboards
- Clear Data Visualisation
- Modern Interface Design

### Main Dashboard

Display:

- Mods Installed
- Total Plugins
- ESM Count
- ESP Count
- ESL Count
- Archive Count
- Loose File Count
- Conflict Count
- Estimated Compilation Output

Example:

```
Mods:           2547
Plugins:        3026
ESM:            63
ESP:            742
ESL:            2221
Conflicts:      51882
Output Plugins: 16
```

Actions: **Scan**, **Analyse**, **Compile**, **Validate**, **Reports**

### Navigation Structure

- Dashboard
- Plugins
- Conflicts
- Assets
- Dependencies
- Compilation
- Validation
- Reports
- Settings

### Plugin Explorer

Display: Plugin Name, Type, Record Count, Conflict Count, Master Count, Status, Eligibility

User Filters: ESP, ESM, ESL, Generated, Conflicting, Disabled

### Conflict Centre

The Conflict Centre is a core feature.

Example: **10mm Pistol**

Override Chain:

```
Fallout4.esm
WeaponExpansion.esp
StoryWealthWeapons.esp
UserPatch.esp
```

Display: Winning Plugin, Losing Plugins, Modified Fields, Conflict Severity

Visual Indicators:

- 🟢 Green = Winning Values
- 🔴 Red = Replaced Values
- 🟡 Yellow = Merged Values

## Conflict Types

### Type 1 — Simple Override

Last Plugin Wins.

### Type 2 — Compatible Merge

Different fields modified. For example, Damage, Value and Keywords can be combined.

### Type 3 — Hard Conflict

Multiple plugins modify the same field. A winner must be selected. The user should be informed.

## Asset Intelligence Engine

Must analyse:

- Textures
- Meshes
- Scripts
- Materials
- Audio
- Animations
- Interface Files
- FaceGen
- Precombined Data

Sources: Loose Files, BA2 Archives

### Asset Conflict Viewer

Example: `Textures\Weapons\10mm.dds`

Display: Winning Asset, Winning Mod, Reason, Physical Path, Alternative Versions

### Asset Preview System

Users should be able to preview Textures, Meshes and Materials.

Information Displayed: Resolution, Format, Size, Winning Source, Asset Path

## Dependency Graph Viewer

Users shall be able to visualise relationships:

```
Weapon
  │
  ▼
OMOD
  │
  ▼
Keyword
  │
  ▼
FormList
```

Features: Zoom, Pan, Search, Relationship Highlighting

## Multiplexer Compilation System

This is the core of the entire project.

Compilation stages:

1. Scan
2. Analyse
3. Resolve
4. Merge
5. Rewrite
6. Validate
7. Compile
8. Export

### Generated Output

Default:

```
Dummy_Plugin_01.esp
Dummy_Plugin_02.esp
Dummy_Plugin_03.esp
...
Dummy_Plugin_20.esp
```

Configurable: 10–20 Output Plugins

### Critical Design Requirement

The generated plugins must contain:

- Real Records
- Real Bethesda Data
- Real References
- Real Dependencies
- Real FormIDs

They are **not**:

- Proxy Records
- Virtual Records
- Runtime References
- Placeholder Entries

### xEdit Compatibility Requirement

Opening `Dummy_Plugin_05.esp` in xEdit should display `WEAP`, `ARMO`, `OMOD`, `COBJ`, `KYWD`, `FLST`, `AMMO`, `LVLI` exactly like a normal Fallout 4 plugin.

**This requirement is mandatory.**

### Record Allocation Strategies

- **Strategy A** — By Record Type
- **Strategy B** — Balanced Size Allocation
- **Strategy C** — User Defined Allocation

## FormID Translation Engine

Every record copied into output plugins shall receive new generated identifiers.

```
Original:  01001234
Generated: 0500ABCD
```

All mappings must be recorded.

### Reference Rewriting

Examples:

```
WEAP → OMOD
OMOD → KYWD
KYWD → FLST
```

All references must be updated. No invalid references permitted.

## Validation Engine

Before export, validate:

- Missing References
- Broken FormIDs
- Duplicate IDs
- Missing Masters
- Circular Dependencies
- Orphaned Records

Critical failures stop compilation.

## Compilation Pipeline Visualisation

**Input:** 3026 Plugins, 8.3 Million Records

**Pipeline:**

```
Scanning
   ↓
Analysis
   ↓
Conflict Resolution
   ↓
Merge Processing
   ↓
FormID Rewriting
   ↓
Validation
   ↓
Compilation
```

**Output:** 16 Compiled Plugins

## Validation Centre

Example:

```
1,253,778 Checks
Passed:   1,253,772
Warnings: 6
Errors:   0
```

## Reporting System

Reports:

- Conflict Report
- Asset Report
- Dependency Report
- Compilation Report
- Validation Report

Export Formats: HTML, JSON, PDF

## Future AI Module

Potential future features:

- Conflict Explanations
- Load Order Recommendations
- Compatibility Analysis
- Redundant Mod Detection
- Merge Recommendations
- Troubleshooting Assistance

AI remains advisory only. Users maintain ultimate control.

## Success Criteria

Fallout 4 Multiplexer Renewed shall:

- Process thousands of plugins.
- Ignore traditional plugin analysis limits.
- Understand plugin conflicts.
- Understand asset conflicts.
- Visualise dependencies.
- Determine winning records.
- Determine winning assets.
- Merge compatible modifications.
- Generate fully materialised output plugins.
- Produce xEdit-compatible results.
- Rewrite FormIDs safely.
- Preserve dependency integrity.
- Validate outputs automatically.
- Dramatically reduce load-order complexity.
- Provide complete transparency into the final compiled game state.

## Final Product Definition

Fallout 4 Multiplexer Renewed is a Fallout 4 mod compilation, conflict analysis, dependency intelligence and game-state generation platform developed by Romewolf.

The software analyses an effectively unlimited collection of Fallout 4 plugins, assets and archives, constructs a complete dependency and conflict graph, determines the final winning state of every record and asset, intelligently merges compatible modifications, rewrites references and FormIDs, validates integrity, and compiles the resulting game state into a configurable set of generated dummy plugins.

These generated plugins contain fully materialised Bethesda records, remain fully compatible with xEdit, FO4Edit and the Creation Kit, and provide users with a scalable, transparent and inspectable alternative to traditional Fallout 4 load-order management.

**In simple terms:** Fallout 4 Multiplexer Renewed allows users to see, understand, validate and compile the complete final state of a Fallout 4 modlist into a manageable set of fully inspectable plugins.

---

## Critical Design Clarification: Asset Preservation, Change Detection and Incremental Recompilation

### Asset preservation and runtime asset resolution

Compilation must not duplicate, move, extract or redistribute game assets. The generated plugins contain compiled Fallout 4 records only; the application is not an asset packing, archive rebuilding or asset duplication system.

- Compiled records keep referencing the original asset paths (for example `Meshes\Weapons\10mm\10mm.nif`, `Textures\Weapons\10mm\10mm_d.dds`, `Materials\Weapons\10mm\10mm.bgsm`).
- Asset ownership stays with the original mods: BA2 archives, loose files, Mod Organizer 2 mod folders, Vortex-managed folders.
- At runtime Fallout 4 resolves those paths through its normal asset system (compiled record → mesh/texture → original archive or loose file). The generated plugin is purely a compiled record container.
- No duplicate assets are created unless the user explicitly asks for it.

### Archive awareness

The application understands asset ownership and archive relationships (for example `WeaponExpansion.esp` → `WeaponExpansion - Main.ba2`, `WeaponExpansion - Textures.ba2`) and preserves them in its database.

### Asset traceability

For any generated record a user can see: the generated plugin, the source plugin, the winning override, the referenced meshes, textures and materials, and the original archive sources and file paths of each.

### Design goal

Compile the plugin layer while leaving the asset layer distributed across the original mods. The original mods remain the authoritative source of every game asset.

### Automatic mod space monitoring

After each successful scan or compilation the application saves a snapshot of installed mods, plugins, BA2 archives, loose files, plugin sizes, file hashes, load order and modification dates. That snapshot is the reference state.

On startup or rescan the application detects: new mods, removed mods, added/removed plugins, plugin updates, archive updates, file changes and load order changes, and reports them, e.g.:

```
Mod Space Changes Detected
New Mods: 5
Updated Plugins: 3
Removed Mods: 0
Compilation Status: Out of Date
```

### Incremental recompilation

The software determines what changed, which records changed and which dependencies changed, and rebuilds only the affected work where possible (for example, a changed weapon mod affects WEAP, OMOD and COBJ; armour, NPC, quest and audio are unaffected).

### Compilation status indicator

The dashboard always shows the current build status: **✓ Up To Date**, **⚠ Changes Detected**, **✖ Requires Rebuild**, like a development environment that knows when source changes require a rebuild.

### Long-term design philosophy

Fallout 4 Multiplexer Renewed behaves like a modern build system: users install, remove, update or reorder mods and immediately see what changed, what broke, what requires recompilation and what the next build will include.

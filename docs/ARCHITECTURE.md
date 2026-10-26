# Architecture

This document explains how Fallout 4 Multiplexer Renewed turns a load order into compiled output plugins, and the decisions behind each stage. The code lives in `src/core`; the GUI (`src/gui`) and CLI (`src/cli`) are thin front ends over the same `Pipeline`.

## Pipeline

```
Install resolution ─► Virtual File System ─► Plugin Scanner ─► Asset Scanner
        │                                                        │
        ▼                                                        ▼
   Database Engine ◄── Conflict Engine ◄── Dependency Engine (eligibility)
        ▲
        │        ┌───────────── Compilation Engine ──────────────┐
        └─────── │ Resolve ► Merge ► Rewrite ► Compile (allocate, │ ─► Validation ─► Export ─► Reports
                 │ FormID translation, materialise records)      │
                 └───────────────────────────────────────────────┘
```

| Stage | Module | Output |
|---|---|---|
| Scan | `vfs/GameInstall`, `vfs/VirtualFileSystem`, `engine/PluginScanner`, `assets/AssetScanner` | `Session`: plugins, load order, record index, asset index |
| Analyse | `engine/ConflictEngine`, `engine/DependencyEngine` | `ConflictReport`, record reference graph, eligibility |
| Resolve / Merge / Rewrite / Compile | `engine/CompilationEngine` (+ `MergeEngine`, `FieldModel`, `Normalizer`) | `CompiledOutput`: output plugins with materialised records |
| Validate | `engine/ValidationEngine` | `ValidationReport` (blocks export on errors) |
| Export | `engine/Exporter`, `report/ReportGenerator` | plugin files, FormID map, INI snippet, reports |

Every stage persists its results to SQLite (`db/DatabaseWriter`) so the UI is data driven. The UI queries the database, and results survive restarts.

## Identity: FormKey

A FormID in a plugin file is relative to that file's master list. `01000800` in one plugin and `02000800` in another can be the same record. The engine therefore never compares raw FormIDs. Every FormID is resolved to a **FormKey**:

```
FormKey = (origin plugin, 24-bit object id)
```

The origin is the plugin that owns the namespace: the master the index points at, or the plugin itself. Plugins are numbered in the application's own table (`PluginIndex`, 32-bit), so there is no 254/4096 limit inside the tool. Light plugins need no special case, because their file FormIDs use the same master-index scheme.

The scanner indexes every record of every active plugin as a compact `RecordInstance`: key, plugin, signature, flags, file offset and a content hash. It then sorts the index by `(FormKey, load order)`. Each run of equal keys is an **override chain** (`FormGroup`). Record bodies are loaded on demand from disk through a bounded file-handle cache, so memory scales with record count, not plugin size.

## Record definitions and the FormID decoder

Rewriting references requires knowing exactly which bytes are FormIDs. `esp/RecordDefinitions.cpp` describes the Phase One record types for the 1.10.163 runtime profile:

- simple kinds: `FormId`, `FormIdArray`, `Struct` (FormIDs at offsets), `StructArray`;
- strings: `ZString`, and `LString`/`DLString`/`ILString` (localised);
- structured decoders (`esp/FieldDecoder.cpp`) for **VMAD** script properties (objects, arrays, structs), **CTDA** conditions (global comparison values, parameters of known functions, run-on references), **OBTS** object templates, **OMOD DATA**, **PERK** effect data (context-dependent on the preceding `PRKE`/`EPFT`), and **FACT PLVD**;
- **regions**: runs of subrecords compared and merged as a unit (model data, conditions, effects, object templates, armature lists, perk effects, message buttons).

Every structured decoder checks that the layout consumes exactly the subrecord's size. If it doesn't, the subrecord is reported and treated as **undescribed**: it is copied verbatim and scanned heuristically, never trusted. Undescribed bytes are scanned for 4-byte values that resolve to existing records ("soft references"). Soft references are used for comparison and validation. A soft reference to a compiled record is a validation **error**, because the bytes could not be rewritten safely.

### Self-validation against real data

The Validation Engine counts, per `(record type, subrecord)`, how many decoded FormIDs resolve to an existing record. If fewer than 80 % resolve, the definition is reported as suspect. A wrong offset therefore shows up as a warning against your actual load order instead of silently damaging data.

## Normalisation and the field model

`engine/Normalizer` turns a record instance into a load-order-independent form:

1. decode the layout;
2. replace every described FormID with its FormKey (bytes zeroed, keys kept beside them);
3. resolve localised strings through the plugin's `.STRINGS`/`.DLSTRINGS`/`.ILSTRINGS` (read through the VFS, so string tables inside BA2s work), turning them into inline text. Output plugins are therefore not localised and need no string tables.

`engine/FieldModel` splits the normalised record into **fields**, the unit of comparison and merging:

| Shape | Examples | Merge semantics |
|---|---|---|
| Scalar | `FULL`, `DNAM`, `DATA` | last plugin that changed it wins |
| Region | Model, Conditions, Effects, Object Template | atomic, last changer wins |
| Packed array | `KWDA`, `APPR`, `FVPA`, `CVPA`, `DAMA` | Set or Keyed list merge |
| Repeated list | `LNAM` (FLST), `LVLO`+`COED` (LVLI), `XNAM` (FACT) | Set / Multiset / Keyed list merge |
| Record flags | header flags | scalar |

Count subrecords (`KSIZ`, `LLCT`) are dropped during splitting and regenerated from the merged list, so counts always match.

## Conflict classification

For each override chain, `MergeEngine::merge` compares every plugin's fields with the defining record (the base):

- **Type 1, Simple Override**: at most one plugin changed anything, and it is the winner.
- **Type 2, Compatible Merge**: several plugins changed different fields (or lists), or the winner silently reverts someone else's edit. Last-plugin-wins loses edits here; merging recovers them.
- **Type 3, Hard Conflict**: two plugins changed the same scalar field to different values (or the same keyed list entry).
- **Critical** severity: the winner deletes a record the base did not delete.

Chains whose overrides are all byte-identical to the base are reported as ITMs, not conflicts. Every field carries per-plugin cell status (Base, Same, Winning, Merged, Replaced) and the compiled result. The Conflict Centre renders exactly this.

## Eligibility

A plugin is compiled only if disabling it and loading the outputs reproduces it exactly (`DependencyEngine::evaluateEligibility`):

1. active, readable, no missing masters;
2. not base game / DLC / Creation Club / user-excluded;
3. every record is a compilable (Phase One) type;
4. not referenced by file name from any script string table (`.pex`) or F4SE/MCM config;
5. no FaceGen/voice asset folders named after it;
6. **propagation**: any plugin that stays in the load order keeps all its masters in the load order too (iterated to a fixed point).

## Compilation

### Resolve

For every FormKey with at least one instance in a compiled plugin:

- **Origin compiled** (a new record from a compiled plugin): always emitted with a new FormID. Every record of a compiled plugin must be accounted for, and the validator checks this.
- **Origin retained** (an override of a base-game or retained record): emitted as an override that keeps its FormID. If a retained plugin wins the chain, the record is emitted only when merging produces something different from that winner. Outputs load last, so an emitted override becomes the final state.

### Merge

The chain (all instances, retained and compiled, in load order) goes through `MergeEngine::merge`:

- scalar and region fields: the last plugin that changed the field wins;
- lists: start from the base items, then replay every changer's additions and removals in load order (sets ignore duplicates; multisets keep them for leveled lists; keyed lists merge per key and report contested keys);
- user resolutions (the Conflict Centre "Force" option) replace the merge with one plugin's record verbatim;
- *Merge compatible edits* off: last plugin wins.

### Rewrite: dependency graph and allocation

References from each emitted record to other **new** records form a graph. Tarjan's algorithm groups records into strongly connected components (mutual references must share a plugin) in dependency order. Allocation then assigns each component to an output:

- **Strategy A, by record type**: keywords, messages and factions first; ammunition, misc items and mods; weapons and armour; consumables and books; spells and perks; leveled lists and form lists; constructible objects last. Output slots are shared between types in proportion to their size.
- **Strategy B, balanced**: walk components in dependency order and fill outputs to an even size, with a minimum fill so small load orders use few plugins.
- **Strategy C, user defined**: `PATTERN=N` rules match a signature or plugin name; everything else is balanced.

Then one constraint is enforced in dependency order: **a record may only live in an output at or after every output it references**. Output plugins can only reference their masters, and masters must load first. This makes the output master graph acyclic by construction. When a previous `Multiplexer_FormMap.tsv` exists, each record prefers its previous output and object id. A move forced by new dependencies is reported as a FormID-stability warning.

### FormID translation and materialisation

Each output gets:

- object ids from `0x800` (reusing previous ids where possible; `0x800–0xFFF` when ESL-flagged);
- a master list: the base game ESMs first (keeping master index 00 = `Fallout4.esm`, which also keeps undescribed vanilla references valid), then every retained plugin it references, in load order, then the earlier outputs it references.

`CompiledOutput::translate(FormKey, output)` produces the raw FormID for any key in any output's context. Records are rebuilt from the merged fields (`joinFields`), with every described FormID written through `translate`, and stored uncompressed. If `rewriteHeuristicReferences` is enabled, soft references are rewritten too.

## Validation

Pre-export checks (each counted as a check):

| Check | What it proves |
|---|---|
| Orphaned Records | every record from a compiled plugin was emitted (or intentionally dropped as deleted) |
| Missing Masters | outputs only depend on plugins that stay active, and need ≤ 254 masters |
| Circular Dependencies | outputs only reference earlier, non-empty outputs |
| Duplicate IDs | no FormID is used twice; no override is emitted twice |
| Broken FormIDs | header and reference FormIDs use valid master indices; light ranges respected |
| Missing References | every reference can be expressed and targets an existing record; soft references to compiled records are errors |
| Record Decoding | issues from decoders (unexpected sizes, unparsed script data, missing strings) |
| Definitions | per-field resolution rate against the real load order |

Post-export, every written plugin is re-read: the master list must match the plan, every record must parse, and the record set must match. Errors block export, and the GUI disables it.

## Assets

`AssetScanner` replicates engine priority:

1. archives from the INI lists (`sResourceIndexFileList`, `sResourceStartUpArchiveList`, `SResourceArchiveList`, `SResourceArchiveList2`), defaulting to the 1.10.163 values;
2. per active plugin in load order: `<name> - Main.ba2`, `<name> - Textures.ba2`, `<name> - Voices_<lang>.ba2`;
3. loose files, which override all archives, by mod priority (the MO2 overwrite folder highest).

Every source of every path is recorded, so the Asset Conflict Viewer shows the winner, the reason and every alternative. The scanner also reads the string table of every winning `.pex` and every F4SE/MCM config file to find plugins that are referenced by file name.

## Mod managers

- **Mod Organizer 2**: `ModOrganizer.ini` (`gamePath`, `selected_profile`, `base_directory`, `mod_directory`, `profiles_directory`, `overwrite_directory`, `%BASE_DIR%`), `modlist.txt` (highest priority first; `+` enabled, `-` disabled, `*` unmanaged, separators skipped), `plugins.txt`/`loadorder.txt`, and profile-local INIs when `LocalSettings=true`. The VFS layers Data, then enabled mods by priority, then overwrite, as USVFS presents them to the game.
- **Vortex**: deploys into Data, so the Data folder is the truth; `vortex.deployment.json` attributes files to mods.
- **Standalone**: Data plus `%LOCALAPPDATA%\Fallout4\plugins.txt`.

The implicit plugins (Fallout4.esm, the DLC masters and `Fallout4.ccc` entries) load first, then masters, then everything else, each group in recorded order.

## Technology choices

- **C++20, Qt 6 Widgets, SQLite, zlib, spdlog**, as specified.
- **CommonLibF4 is not used.** It is a library for F4SE runtime plugins (DLLs injected into the game process). The Multiplexer is an offline tool that never runs inside the game, so it reads and writes the file formats directly.
- The core has no Qt dependency and builds on Linux, so the engine is tested in CI on every push. The GUI uses Qt's SQLite driver read-only; all writes go through the core's writer.

## Runtime profiles

`RecordDefinitions::get(RuntimeProfile)` selects the field layouts. Old Gen (1.10.163) is the primary target. The Next Gen profile currently reuses the Old Gen layouts and relies on size validation and the definition self-check to catch differences. Adding a profile means describing the changed subrecords. Nothing else in the engine changes.

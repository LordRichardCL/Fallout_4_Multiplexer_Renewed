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

## Record catalogue

`resources/record_catalogue.csv` lists every Fallout 4 record type: the xEdit Fallout 4 record list plus the eight placed projectile reference types from xEdit's `wbDefinitionsFO4.pas`. It is embedded at build time (`cmake/EmbedText.cmake`) and read by `esp/RecordCatalogue`. Per type it holds:

- **Name** and **Category** (display grouping only);
- **Placement:** header, top-level group, cell, cell child or dialogue topic child;
- **Strategy:**
  - *Specialised*: compiled, fields merged;
  - *Winner*: compiled, the last version of each chain taken whole: `MergeEngine` with that entry forced, never in the patch, and nothing written when a retained plugin wins;
  - *Candidate*: layout described and covered by the definition check, not compiled;
  - *Preserve*: never written; its plugin stays as it is;
- **Group Order:** the top-level group order of a written plugin (xEdit's `AddGroupOrder`), used by `PluginWriter`;
- **Allocation Order:** how the compiler groups types across outputs;
- **Notes.**

A type compiles only when the catalogue says *Specialised* or *Winner* **and** `RecordDefinitions` has a layout for it (`RecordDef::layout`: every subrecord that can hold a FormID is described). Setting *Preserve* turns a type off without touching code. Any other strategy without a layout does nothing and is reported (`strategyWithoutLayout`, checked by a test). The definition check covers every type with a layout and gives each *Candidate* a verdict: confirmed only with no size problems, no FormID-like values outside the described fields, and at least 99% of its FormIDs resolving. Promotion from *Candidate* to *Winner* is a catalogue change made after such a result. Signatures missing from the catalogue are reported as unknown record types: such a plugin is not compiled and its reason says it may be for another game or damaged. Asset path subrecords (`AssetRole`: mesh, texture, material, sound, icon) are part of the field definitions.

## Record definitions and the FormID decoder

Rewriting references requires knowing exactly which bytes are FormIDs. `esp/RecordDefinitions.cpp` describes the supported record types for the 1.10.163 runtime profile (Phase One plus tier 1 base objects, effects, sounds and UI data; see the README for the list):

- simple kinds: `FormId`, `FormIdArray`, `Struct` (FormIDs at offsets), `StructArray`;
- strings: `ZString`, and `LString`/`DLString`/`ILString` (localised);
- structured decoders (`esp/FieldDecoder.cpp`) for **VMAD** script properties (objects, arrays, structs), **CTDA** conditions (global comparison values, parameters of known functions, run-on references), **OBTS** object templates, **OMOD DATA**, **PERK** effect data (context-dependent on the preceding `PRKE`/`EPFT`), and **FACT PLVD**;
- **regions**: runs of subrecords compared and merged as a unit (model data, conditions, effects, object templates, armature lists, perk effects, message buttons).

Every structured decoder checks that the layout consumes exactly the subrecord's size. If it doesn't, the subrecord is reported and treated as **undescribed**: it is copied verbatim and scanned heuristically, never trusted. Undescribed bytes are scanned for 4-byte values that resolve to existing records ("soft references"). Soft references are used for comparison and validation. A soft reference to a compiled record is a validation **error**, because the bytes could not be rewritten safely. A soft reference to a retained record is an error too when the output's master list would give its master index a different plugin, because the copied bytes would silently name the wrong record. Leaving a subrecord undescribed is therefore always safe: the worst case is a blocked export, never a broken plugin.

### Self-validation against real data

The Validation Engine counts, per `(record type, subrecord)`, how many decoded FormIDs resolve to an existing record. If fewer than 80 % resolve, the definition is reported as suspect. A wrong offset therefore shows up as a warning against your actual load order instead of silently damaging data.

`ValidationEngine::checkDefinitions` (Validation → Check Record Definitions, `fo4mx check-definitions`) runs the same measurement over every record of every supported type in the load order, compiled or not, with the base game as the reference. It also reports unexpected sizes and undescribed subrecords that keep looking like FormIDs, which means a field the definitions should describe.

### Eligibility blockers

`DependencyEngine` records, for every unsupported record type, how many plugins contain it, how many it alone blocks, and how many would become eligible if it were supported. That last number re-runs the full eligibility rule, including the rule that plugins staying in the load order keep their masters. The Plugins page shows it as **Blocking Types**, and it decides which record types to support next.

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

For each override chain, `MergeEngine::merge` compares every plugin's fields with the version it was made against, as xEdit does: the last earlier override among the plugin's own (direct or indirect) masters, or the defining record (the base) when there is none (`Session::overrideParents`). So a patch that removes or reverts its master's edit counts as a change, and a plugin that only repeats its master's edit does not. A later plugin made against an earlier one's version supersedes it instead of conflicting with it.

- **Type 1, Simple Override**: at most one plugin changed anything, and it is the winner.
- **Type 2, Compatible Merge**: several plugins changed different fields (or lists), or the winner silently reverts someone else's edit. Last-plugin-wins loses edits here; merging recovers them.
- **Type 3, Hard Conflict**: two plugins, neither made against the other, changed the same scalar field to different values (or the same keyed list entry).
- **Conflict (Not Merged)**, record types without a layout (not compilable): classified record by record, never merged. An override changed the record when its bytes differ from the version it was made against (and, with field comparison on, the generic comparison does not find them identical: master order and compression change the bytes). The chain is a conflict when a changer's version differs from the winner's and the winner was not made against it; otherwise it is a Simple Override. Field differences, if shown, come from `MergeEngine` with merging off, so the last column is the winner's value.
- **Critical** severity: the winner deletes a record the base did not delete.

Chains whose overrides are all byte-identical to the base are reported as ITMs, not conflicts; with field analysis, an override identical to its own masters' version is an ITM. Every field carries per-plugin cell status (Base, Same, Winning, Merged, Replaced) and the compiled result. The Conflict Centre renders exactly this.

## Cleaning report

`engine/CleaningEngine` runs after conflict and dependency analysis and adds findings to the dependency report (marked `cleaning`, stored with `DependencyIssues.cleaning = 1`):

- **ITMs** come from the conflict analysis: overrides identical to the version they were made against (`ConflictReport::itms`). An ITM that is the winner of a chain other plugins changed *undoes changes* (Warning); the rest are summarised per plugin (Info).
- **Deleted overrides** (the master's version is not deleted) are classified by the catalogue as Deleted Reference (Placed References category), Deleted Navmesh (`NAVM`) or Deleted Record.
- **Injected records**: the first version of a record is not in the plugin its FormID names.
- **Circular leveled lists**: strongly connected components (Tarjan) of the `LVLO` references between winning `LVLI`/`LVLN` records (`Session::recordEdges`). `LVSP` is not described yet, so it is not checked.

Findings in official files (base game, DLC, Creation Club) are Info. Likely causes come from `diagnosis::forCleaning`. The app never edits a plugin.

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

## Asset layer and traceability

The compiler never writes assets. Generated plugins are record containers. Every path a compiled record holds (`MODL`, `MOD2`/`MOD4`, `ICON`, ammunition casing models, …) is copied verbatim, so the game resolves it through its normal priority rules at runtime: loose files, then archives in load order.

Disabling a compiled plugin has one runtime consequence: the engine no longer auto-loads archives named after it. `Multiplexer_Archives.ini` registers exactly those archives, in place, through `SResourceArchiveList2`.

`engine/AssetTracer` resolves, for every emitted record:

1. the direct asset paths (meshes under `meshes\`, icons under `textures\`);
2. the materials a mesh names (NIF string table → `.bgsm`/`.bgem`);
3. the textures each material names.

Each path is resolved through the asset index to the winning mod, loose file or archive, the plugin that owns that archive, and the physical file. Assets served by an archive of a compiled plugin are flagged as depending on the archive registration. Direct references that no installed mod provides are validation warnings. Results go to the `RecordAssets` table and the Compilation page's traceability panel.

## Change detection

`engine/ChangeDetector` fingerprints the mod space into an `InstallSnapshot`:

| Item | Fingerprint |
|---|---|
| Mods (MO2 mods, Vortex mods, Data) | enabled, priority, file count, bytes, order-independent hash of every file's path, size and date (plugins excluded: they have their own hash) |
| Plugins | size, date, full-content hash, active flag, load order index, record types |
| Archives | size, date, sampled content hash (first and last 64 KiB) |
| Load order | active plugins in order |
| Build | compiled plugins, participating plugins, output folder, output plugin hashes, settings hash (options, rules, exclusions, user resolutions by name) |

A **scan** snapshot is stored after every scan, and a **build** snapshot after every successful export. `checkForChanges` takes a quick snapshot (install resolution, virtual Data walk, plugin headers, content hashes only for files whose size or date changed) and compares it with the build:

- **Requires Rebuild**: active plugin added, removed, updated, enabled, disabled or moved; archive of a compiled plugin added or removed; settings changed; generated plugin missing or modified.
- **Changes Detected**: only the asset layer changed.
- **Up To Date**: nothing changed.

The report lists every change, the record types the changed plugins contain (affected) and the compiled types they do not touch (unaffected). The GUI runs this check on startup.

## Build cache and incremental recompilation

`engine/BuildCache` is a separate SQLite file next to the results database. Every entry is keyed by content, so invalidation is automatic:

| Cached work | Key | Effect |
|---|---|---|
| Plugin record index | path + size + date + sampled hash | unchanged plugins are not read or parsed |
| Conflict analysis | override-chain signature (each instance's plugin, master list and record bytes hash) + options | unchanged chains are not re-normalised or re-diffed |
| Record references | winning instance signature | dependency graph built without decoding unchanged records |
| Merge result | chain signature + merge options + forced winner | unchanged chains are not re-merged |

Cached values store FormKeys by plugin **name**, so they survive load order changes and new plugin numbering. Conflict cell text stores form references as tokens, rendered against the current load order (so labels show current FormIDs). Allocation, FormID translation and validation always run in full: they are fast, and they guarantee a consistent output. The exporter compares each output image with the file on disk and leaves identical files untouched. It removes generated plugins that the new build no longer produces.

## Technology choices

- **C++20, Qt 6 Widgets, SQLite, zlib, spdlog**, as specified.
- **CommonLibF4 is not used.** It is a library for F4SE runtime plugins (DLLs injected into the game process). The Multiplexer is an offline tool that never runs inside the game, so it reads and writes the file formats directly.
- The core has no Qt dependency and builds on Linux, so the engine is tested in CI on every push. The GUI uses Qt's SQLite driver read-only; all writes go through the core's writer.

## Runtime profiles

`RecordDefinitions::get(RuntimeProfile)` selects the field layouts. Old Gen (1.10.163) is the primary target. The Next Gen profile currently reuses the Old Gen layouts and relies on size validation and the definition self-check to catch differences. Adding a profile means describing the changed subrecords. Nothing else in the engine changes.

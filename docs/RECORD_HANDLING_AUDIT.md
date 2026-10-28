# Fallout 4 Record Handling: Audit and Plan

An audit of how the application recognises, parses, analyses and merges Fallout 4 record types, and a staged plan for widening coverage without risking incorrect merges.

**Sources:**
- **The codebase**, as of commit `f3438fb`.
- **The record list from the current xEdit Fallout 4 definitions** (`resources/fallout4_xedit_record_types.csv`, 138 record types plus the `TES4` header). This is the primary reference for which record types exist.
- **The xEdit documentation** (<https://tes5edit.github.io/docs/>), as a reference for conflict and cleaning concepts. It could not be read during this audit: the host is not in this session's allowed network domains. Statements about xEdit practice below use only long-established, widely known concepts (override, ITM, deleted records and UDRs, injected records, winning and losing overrides). Each is marked *(check against xEdit docs)* so it can be verified once the site is reachable.

## Summary

- **Recognition** is effectively universal already. The plugin reader indexes every record of every signature, and conflict analysis runs on every type. Nothing is ever discarded.
- **Support** is all-or-nothing per type. A type is either *compilable* (fully described: compiled, merged, patched; 61 of the 138) or not. If a plugin contains even one record of a non-compilable type, the whole plugin is kept in the load order untouched.
- **"Preserve / pass-through" is already the behaviour for unsupported types**, at plugin level: such a plugin and all its records stay exactly as they are. No unsupported record is ever written into a generated plugin, and none is merged into the patch.
- **The display overstates.** Field-level conflict analysis runs on types the app does not describe. It can label them **Compatible Merge** and show a **Compiled Result** column, as if the app knew how to merge them. For undescribed types the field comparison is generic: each repeated subrecord is matched by position, which misaligns lists. The classification can be wrong, and it never reflects real merge semantics. **Nothing incorrect is written because of this**, but it misleads.
- **Record-type knowledge is in one place (`RecordDefinitions.cpp`) for layouts**, but six other places hard-code signatures, and they drift: the group writer order, allocation ranking, asset path fields, PERK/VMAD decoder context, the scanner's editor ID extraction, and a list of names for not-yet-supported types.
- **The plugin writer only writes flat top-level groups.** Records that live in nested groups (cells, placed references, navmeshes, dialogue responses) cannot be emitted at all today. This is a structural prerequisite for any world, quest or dialogue support, independent of record layouts.

## 1. Where record recognition lives

| Layer | Location | What it does |
|---|---|---|
| Reading | `esp/PluginFile.cpp` `forEachRecord` | Walks every group and record of a plugin regardless of signature. Group nesting (`topGroup`, `depth`) is computed but the parent group is not kept. |
| Indexing | `engine/PluginScanner.cpp` `indexRecords` | Indexes **every** record: signature, flags, FormID, offset, content hash, editor ID. Counts per type (`PluginInfo::typeCounts`). |
| Definitions | `esp/RecordDefinitions.cpp` | The only catalogue: `records_` (signature → name, *compilable* flag, field layouts, regions) plus `common_` field layouts. Built in code. |
| Eligibility | `engine/DependencyEngine.cpp` `evaluateEligibility` | A plugin is eligible only if every type it contains is compilable (plus script, FaceGen/voice and master rules). |
| Names | `RecordDefinitions::recordName` | The name for display; falls back to the bare signature. |

## 2. Signatures explicitly supported today

**Compilable (61):** described, merged field by field, compiled, included in the patch, and checked by the definition check.

`WEAP ARMO AMMO OMOD COBJ KYWD LVLI FLST INGR ALCH MISC BOOK NOTE KEYM FACT PERK SPEL MESG ARMA OTFT LVLN CONT CMPO GLOB TXST MSWP INNR ENCH MGEF IPDS AORU STAT SCOL MSTT ACTI FURN DOOR LIGH TERM PROJ EXPL IPCT HAZD SNDR SOUN AVIF EQUP LSCR DMGT ZOOM AMDL ARTO EFSH MATO FLOR KSSM LCRT RFCT TRNS DFOB CLFM`

**Named only (37 in the xEdit list):** no layout. They are analysed generically and keep their plugin out.

**Unknown (40 in the xEdit list):** the app has no name for them and shows the bare signature: `AACT AECH ASTP BNDS CAMS CLMT CPTH CSTY DLBR DLVW DOBJ DUAL EYES FSTP FSTS GDRY GRAS HDPT IDLM LAYR LENS LTEX LVSP MATT MOVT MUST NAVI NOCM PLYR RELA REVB RFGP SCSN SMBN SMEN SMQN SPGD STAG TACT TREE`.

Also found:
- `PGRE`/`PHZD` are named in the app but not in the xEdit list.
- `SLGM` (a Skyrim type) is in the writer's group order.

Both are harmless, but the catalogue should be the single authority on such names.

The full per-signature table is at the end of this document.

## 3. Signatures hard-coded outside the catalogue

| Location | Hard-coded knowledge | Risk |
|---|---|---|
| `esp/PluginWriter.cpp` `topLevelOrder` | The order of top-level groups in a written plugin (~130 signatures) | Separate from the catalogue. It has `SLGM` and lacks `DIAL`, `INFO`, `NAVM`, `REFR`, `ACHR`, `LAND`, `DUAL`. |
| `engine/CompilationEngine.cpp` `typeRank` | The grouping order for allocation Strategy A (18 Phase One types) | The 43 newer types all fall into one "other" bucket. |
| `engine/AssetTracer.cpp` `rootFor` | Which subrecords hold asset paths, per record (`ARMO`/`ARMA` `MODL` are FormIDs; `TXST TX00-07`; `MSWP SNAM`; `SNDR ANAM`; `EFSH NAM7-9`; `LIGH NAM0`; `PROJ NAM1`) | Belongs in the field definitions (a field kind "asset path" with its root folder). |
| `esp/FieldDecoder.cpp` | PERK effect context (`PRKE`, `EPFT`, `PRKF`) and VMAD fragment handling by record type | Legitimate structural decoding, but keyed on signatures in code. |
| `engine/PluginScanner.cpp` `quickEditorId` | Editor IDs of **compressed** records are only read for compilable types | Compressed records of other types show no Editor ID in the UI. |
| `engine/DependencyEngine.cpp` | FaceGen and voice folder prefixes (plugin-level eligibility) | Correct as a plugin-level rule; not a record-type rule. |
| `esp/Signature.hpp` | Constants for 22 record types used by name in code | Convenience only. |

## 4. What happens when an unknown or unsupported record appears

1. **Scan:** it is indexed like any record. Nothing is skipped.
2. **Eligibility:** the plugin becomes *Ineligible* with the reason "Contains record types not yet compiled: XXXX (n)". It is listed under **Plugins → Blocking Types**.
3. **Conflict analysis:** field-level by default (`analyseAllRecordTypes = true`). Undescribed subrecords are compared as opaque values matched by position, and each is scanned for values that look like FormIDs. The result is classified Simple Override, Compatible Merge or Hard Conflict *as if* it could be merged (see the Summary).
4. **Compilation:** never written. A chain made only of plugins that stay in the load order is excluded from the patch unless its type is compilable. A plugin that stays in the load order also keeps its masters in the load order.
5. **Dependencies:** references from it are collected generically, so missing-reference findings are best-effort for undescribed types.
6. **Validation and definition check:** not applicable; it is never compiled.

Net effect: **unknown and unsupported records are preserved by preserving their whole plugin.** That is safe, but coarse: one unsupported record keeps a whole mod out (the weapon mod you tested has 33 types, 10 of them unsupported).

## 5. Merge-engine assumptions about record types

- `MergeEngine` itself is **type-agnostic**. It merges a `FieldSet`.
- **All record semantics come from `RecordDefinitions`:**
  - field kinds (FormID, struct, array, string, special decoders);
  - merge modes (Scalar, Set, Multiset, Keyed);
  - count subrecords, companions (`COED` after `LVLO`/`CNTO`) and regions (subrecords merged as a unit).
- **Undescribed subrecords** fall back to *Scalar, matched by occurrence*. For a record type that has never been described, the merge result is therefore only a mechanical guess. That is exactly the "generic merge" the brief warns against, and it is why such types are never written.
- **Special decoders** exist for VMAD (scripts and fragments), CTDA (conditions), OBTS (object templates), OMOD DATA, PERK data and entry points, and FACT vendor locations. They are shared by every type that uses those subrecords.

## 6. Integrating the xEdit-derived record list

The list becomes the **catalogue**: a data file shipped with the app (`resources/fallout4_xedit_record_types.csv`, extended with more columns), loaded once into `RecordDefinitions`.

| Column | Meaning | Who fills it |
|---|---|---|
| Signature, Name | From the xEdit list | Supplied list |
| Category | Grouping for reports (Items, Placeable objects, Magic, Audio, Visual effects, Actors, Quests & dialogue, World, Game settings, …) | Proposed by the app, reviewed by you |
| Placement | Top-level group, cell child, worldspace child, dialogue topic child, header | From the plugin format; drives the reader and writer |
| Parse support | **Raw** (bytes preserved, generic scan) or **Layout** (described fields) | From `RecordDefinitions` |
| Conflict analysis | **Record level** (identical / override / conflict only) or **Field level** (field differences shown) | Derived: Field level only with a layout |
| Merge strategy | **Specialised** (described, list/region semantics), **Winner only** (the last override is taken whole), **Preserve** (never written; its plugin stays), **Investigate** (seen in real data, no layout yet) | Set per type; never inferred |
| Evidence | Definition-check result on real load orders (FormIDs found and resolve rate), tests | Recorded per release |
| Notes | Why a type is preserved, and what blocks it | Free text |

Rules:
- A type is **recognised** when it is in the catalogue.
- A type gets **Field level** analysis only with a layout.
- A type gets a **merge strategy other than Preserve** only with a layout, a passing definition check on a real load order, and tests.
- Unknown signatures (not in the catalogue) are reported as such. They may mean a plugin for another game, or a corrupt file.

## 7. Changes needed to widen coverage safely

1. **One catalogue** loaded from the data file, replacing the names list, the writer's group order, `typeRank` and the asset path table. One source of truth.
2. **Honest conflict analysis per support level.** Types without a layout get record-level classification:
   - Identical to master *(ITM)*;
   - Override (one changer);
   - Conflict (several plugins change it).

   There is no "Compatible Merge" and no "Compiled Result" for them, and the Conflict Centre says *"Not merged: this record type is preserved as is"*.
3. **Cleaning and error checks as reports** *(check against xEdit docs)*:
   - ITMs per plugin (the data exists);
   - deleted references (UDR candidates: deleted `REFR`/`ACHR`/`PGRE`/`PHZD`);
   - deleted navmeshes;
   - records whose FormID claims a master that does not define them (injected records, already counted per plugin).

   These are **reported** with a likely cause; the app does not clean plugins.
4. **Group-structure support** in the reader index and the writer: keep each record's parent (cell, worldspace block, dialogue topic, persistent or temporary children), and write nested groups. This is required before any placed, world or dialogue record could ever be emitted. Until then those types stay **Preserve**.
5. **Editor IDs for all types:** decompress for any type when extracting the Editor ID.
6. **Evidence-gated promotion:** a type moves from Investigate to Specialised only after:
   - its layout is written;
   - the definition check on a real load order resolves at least ~99% of its FormIDs and shows no undescribed fields that look like FormIDs;
   - an end-to-end test compiles and checks its references.

   This is the process already used for tier 1.

## Staged plan

Each stage ships separately, keeps every existing test passing, and changes no merge behaviour for the 61 supported types unless stated.

| Stage | Work | Risk | Outcome |
|---|---|---|---|
| **1. Catalogue** | Load the xEdit list into `RecordDefinitions` with name, category, placement and support columns. Replace the names list, the writer group order, `typeRank` and the asset table with catalogue data. Report unknown signatures separately. Read Editor IDs for compressed records of every type. | Low: data and display only | Every one of the 138 types is recognised and named. Blocking Types and reports show categories. |
| **2. Honest analysis** | Conflict classification by support level (record level without a layout). Conflict Centre and reports say "preserved, not merged" for those types. A **support level** column on the Plugins page and in exports. | Low: display and classification only | No more implied merges for types the app does not understand. |
| **3. Cleaning reports** | ITM, UDR-candidate, deleted-navmesh and injected-record findings per plugin, with likely causes *(terminology to check against xEdit docs)*. | Low: reporting only | Error-checking parity with the usual pre-merge checks. |
| **4. Winner-only strategy** | For types confirmed to be single-value records, a merge strategy that takes the winning override whole: no field merging, still compiled. Candidates must still pass the definition check and have every FormID located. | Medium: only for types with full FormID coverage | More plugins compile without field-level merge risk. |
| **5. Next layouts by evidence** | Describe further types in the order **Plugins → Blocking Types** ranks them across real load orders. Each needs a definition check on your data and tests. Likely early candidates (check against the xEdit Fallout 4 definitions, not assumed): `AACT`, `DOBJ`, `COLL`, `SNCT`, `SOPM`, `ASPC`, `REVB`, `AECH`, `ANIO`, `BPTD`, `DEBR`, `IMGS`, `IMAD`, `LGTM`, `GDRY`, `LENS`, `MOVT`, `STAG`, `NOCM`, `LAYR`, `OVIS`, `PKIN`. | Medium per type, controlled by evidence | Fewer plugins blocked by base-object types. |
| **6. Group structure** | Parent context in the index; nested-group writing; tests that round-trip cells and dialogue topics byte-for-byte. Still no compiling of these types. | Medium: core reader and writer | Foundation for any future world or quest work. |
| **7. Tier 2 (analysis first)** | `NPC_` overrides, `RACE`, `CLAS`, `PACK`, `LVSP`, … Layouts, field-level analysis and definition checks first; compiling only for overrides of existing records (no new FormIDs, no FaceGen impact), each separately justified. | High | Gated individually. |
| **Not planned** | Compiling new NPCs, quests, dialogue, scenes, cells, worldspaces, navmeshes and placed references. See `DEPENDENCY_SCENARIOS.md`: precombines, previs, FaceGen and voice paths, quest alias indexes and script name references tie them to their file. They stay **Preserve**, with full conflict analysis. | — | — |

### What this means for the weapon mod you tested

It contains `CELL`, `REFR`, `ACHR`, `WRLD`, `QUST`, `NPC_`, `RACE`, `LCTN`, `SMQN` and `SMBN` alongside its 23 supported types.
- **Stages 1 to 3** make that visible and accurate: every type named and categorised, and its conflicts classified honestly.
- **It will still not be compilable as a whole.** It places objects in the world and adds a quest and NPCs, so it stays in the load order (Preserve).
- **Its item records are still merged** into the patch wherever other plugins conflict with them, which already works today.

## Per-signature status

Placement for types not in the writer's group order, and categories, are to be confirmed against the xEdit Fallout 4 definitions in stage 1.

| Signature | Name (xEdit list) | Today in the app | Placement |
|---|---|---|---|
| `AACT` | Action | unknown name, analysed generically, keeps its plugin out | top-level group |
| `ACHR` | Placed NPC | named, analysed generically, keeps its plugin out | cell child |
| `ACTI` | Activator | **described, compiled, merged** | top-level group |
| `ADDN` | Addon Node | named, analysed generically, keeps its plugin out | top-level group |
| `AECH` | Audio Effect Chain | unknown name, analysed generically, keeps its plugin out | top-level group |
| `ALCH` | Ingestible | **described, compiled, merged** | top-level group |
| `AMDL` | Aim Model | **described, compiled, merged** | top-level group |
| `AMMO` | Ammunition | **described, compiled, merged** | top-level group |
| `ANIO` | Animated Object | named, analysed generically, keeps its plugin out | top-level group |
| `AORU` | Attraction Rule | **described, compiled, merged** | top-level group |
| `ARMA` | Armor Addon | **described, compiled, merged** | top-level group |
| `ARMO` | Armor | **described, compiled, merged** | top-level group |
| `ARTO` | Art Object | **described, compiled, merged** | top-level group |
| `ASPC` | Acoustic Space | named, analysed generically, keeps its plugin out | top-level group |
| `ASTP` | Association Type | unknown name, analysed generically, keeps its plugin out | top-level group |
| `AVIF` | Actor Value Information | **described, compiled, merged** | top-level group |
| `BNDS` | Bendable Spline | unknown name, analysed generically, keeps its plugin out | top-level group |
| `BOOK` | Book | **described, compiled, merged** | top-level group |
| `BPTD` | Body Part Data | named, analysed generically, keeps its plugin out | top-level group |
| `CAMS` | Camera Shot | unknown name, analysed generically, keeps its plugin out | top-level group |
| `CELL` | Cell | named, analysed generically, keeps its plugin out | interior: block/sub-block groups in the CELL group; exterior: inside WRLD |
| `CLAS` | Class | named, analysed generically, keeps its plugin out | top-level group |
| `CLFM` | Color | **described, compiled, merged** | top-level group |
| `CLMT` | Climate | unknown name, analysed generically, keeps its plugin out | top-level group |
| `CMPO` | Component | **described, compiled, merged** | top-level group |
| `COBJ` | Constructible Object | **described, compiled, merged** | top-level group |
| `COLL` | Collision Layer | named, analysed generically, keeps its plugin out | top-level group |
| `CONT` | Container | **described, compiled, merged** | top-level group |
| `CPTH` | Camera Path | unknown name, analysed generically, keeps its plugin out | top-level group |
| `CSTY` | Combat Style | unknown name, analysed generically, keeps its plugin out | top-level group |
| `DEBR` | Debris | named, analysed generically, keeps its plugin out | top-level group |
| `DFOB` | Default Object | **described, compiled, merged** | top-level group |
| `DIAL` | Dialog Topic | named, analysed generically, keeps its plugin out | top-level group with a child group per topic (INFO); not in the writer's group order |
| `DLBR` | Dialog Branch | unknown name, analysed generically, keeps its plugin out | top-level group |
| `DLVW` | Dialog View | unknown name, analysed generically, keeps its plugin out | top-level group |
| `DMGT` | Damage Type | **described, compiled, merged** | top-level group |
| `DOBJ` | Default Object Manager | unknown name, analysed generically, keeps its plugin out | top-level group |
| `DOOR` | Door | **described, compiled, merged** | top-level group |
| `DUAL` | Dual Cast Data | unknown name, analysed generically, keeps its plugin out | top-level group (not in the writer's group order) |
| `ECZN` | Encounter Zone | named, analysed generically, keeps its plugin out | top-level group |
| `EFSH` | Effect Shader | **described, compiled, merged** | top-level group |
| `ENCH` | Enchantment | **described, compiled, merged** | top-level group |
| `EQUP` | Equip Type | **described, compiled, merged** | top-level group |
| `EXPL` | Explosion | **described, compiled, merged** | top-level group |
| `EYES` | Eyes | unknown name, analysed generically, keeps its plugin out | top-level group |
| `FACT` | Faction | **described, compiled, merged** | top-level group |
| `FLOR` | Flora | **described, compiled, merged** | top-level group |
| `FLST` | FormID List | **described, compiled, merged** | top-level group |
| `FSTP` | Footstep | unknown name, analysed generically, keeps its plugin out | top-level group |
| `FSTS` | Footstep Set | unknown name, analysed generically, keeps its plugin out | top-level group |
| `FURN` | Furniture | **described, compiled, merged** | top-level group |
| `GDRY` | God Rays | unknown name, analysed generically, keeps its plugin out | top-level group |
| `GLOB` | Global | **described, compiled, merged** | top-level group |
| `GMST` | Game Setting | named, analysed generically, keeps its plugin out | top-level group |
| `GRAS` | Grass | unknown name, analysed generically, keeps its plugin out | top-level group |
| `HAZD` | Hazard | **described, compiled, merged** | top-level group |
| `HDPT` | Head Part | unknown name, analysed generically, keeps its plugin out | top-level group |
| `IDLE` | Idle Animation | named, analysed generically, keeps its plugin out | top-level group |
| `IDLM` | Idle Marker | unknown name, analysed generically, keeps its plugin out | top-level group |
| `IMAD` | Image Space Adapter | named, analysed generically, keeps its plugin out | top-level group |
| `IMGS` | Image Space | named, analysed generically, keeps its plugin out | top-level group |
| `INFO` | Dialog response | named, analysed generically, keeps its plugin out | dialogue topic child |
| `INGR` | Ingredient | **described, compiled, merged** | top-level group |
| `INNR` | Instance Naming Rules | **described, compiled, merged** | top-level group |
| `IPCT` | Impact | **described, compiled, merged** | top-level group |
| `IPDS` | Impact Data Set | **described, compiled, merged** | top-level group |
| `KEYM` | Key | **described, compiled, merged** | top-level group |
| `KSSM` | Sound Keyword Mapping | **described, compiled, merged** | top-level group |
| `KYWD` | Keyword | **described, compiled, merged** | top-level group |
| `LAND` | Landscape | named, analysed generically, keeps its plugin out | cell child |
| `LAYR` | Layer | unknown name, analysed generically, keeps its plugin out | top-level group |
| `LCRT` | Location Reference Type | **described, compiled, merged** | top-level group |
| `LCTN` | Location | named, analysed generically, keeps its plugin out | top-level group |
| `LENS` | Lens Flare | unknown name, analysed generically, keeps its plugin out | top-level group |
| `LGTM` | Lighting Template | named, analysed generically, keeps its plugin out | top-level group |
| `LIGH` | Light | **described, compiled, merged** | top-level group |
| `LSCR` | Load Screen | **described, compiled, merged** | top-level group |
| `LTEX` | Landscape Texture | unknown name, analysed generically, keeps its plugin out | top-level group |
| `LVLI` | Leveled Item | **described, compiled, merged** | top-level group |
| `LVLN` | Leveled NPC | **described, compiled, merged** | top-level group |
| `LVSP` | Leveled Spell | unknown name, analysed generically, keeps its plugin out | top-level group |
| `MATO` | Material Object | **described, compiled, merged** | top-level group |
| `MATT` | Material Type | unknown name, analysed generically, keeps its plugin out | top-level group |
| `MESG` | Message | **described, compiled, merged** | top-level group |
| `MGEF` | Magic Effect | **described, compiled, merged** | top-level group |
| `MISC` | Misc. Item | **described, compiled, merged** | top-level group |
| `MOVT` | Movement Type | unknown name, analysed generically, keeps its plugin out | top-level group |
| `MSTT` | Moveable Static | **described, compiled, merged** | top-level group |
| `MSWP` | Material Swap | **described, compiled, merged** | top-level group |
| `MUSC` | Music Type | named, analysed generically, keeps its plugin out | top-level group |
| `MUST` | Music Track | unknown name, analysed generically, keeps its plugin out | top-level group |
| `NAVI` | Navmesh Info Map | unknown name, analysed generically, keeps its plugin out | top-level group |
| `NAVM` | Navmesh | named, analysed generically, keeps its plugin out | cell child |
| `NOCM` | Navmesh Obstacle Manager | unknown name, analysed generically, keeps its plugin out | top-level group |
| `NOTE` | Note | **described, compiled, merged** | top-level group |
| `NPC_` | Non-Player Character | named, analysed generically, keeps its plugin out | top-level group |
| `OMOD` | Object Modification | **described, compiled, merged** | top-level group |
| `OTFT` | Outfit | **described, compiled, merged** | top-level group |
| `OVIS` | Object Visibility Manager | named, analysed generically, keeps its plugin out | top-level group |
| `PACK` | Package | named, analysed generically, keeps its plugin out | top-level group |
| `PERK` | Perk | **described, compiled, merged** | top-level group |
| `PKIN` | Pack-In | named, analysed generically, keeps its plugin out | top-level group |
| `PLYR` | Player Reference | unknown name, analysed generically, keeps its plugin out | top-level group |
| `PROJ` | Projectile | **described, compiled, merged** | top-level group |
| `QUST` | Quest | named, analysed generically, keeps its plugin out | top-level group |
| `RACE` | Race | named, analysed generically, keeps its plugin out | top-level group |
| `REFR` | Placed Object | named, analysed generically, keeps its plugin out | cell child |
| `REGN` | Region | named, analysed generically, keeps its plugin out | top-level group |
| `RELA` | Relationship | unknown name, analysed generically, keeps its plugin out | top-level group |
| `REVB` | Reverb Parameters | unknown name, analysed generically, keeps its plugin out | top-level group |
| `RFCT` | Visual Effect | **described, compiled, merged** | top-level group |
| `RFGP` | Reference Group | unknown name, analysed generically, keeps its plugin out | top-level group |
| `SCCO` | Scene Collection | named, analysed generically, keeps its plugin out | top-level group |
| `SCEN` | Scene | named, analysed generically, keeps its plugin out | top-level group |
| `SCOL` | Static Collection | **described, compiled, merged** | top-level group |
| `SCSN` | Audio Category Snapshot | unknown name, analysed generically, keeps its plugin out | top-level group |
| `SMBN` | Story Manager Branch Node | unknown name, analysed generically, keeps its plugin out | top-level group |
| `SMEN` | Story Manager Event Node | unknown name, analysed generically, keeps its plugin out | top-level group |
| `SMQN` | Story Manager Quest Node | unknown name, analysed generically, keeps its plugin out | top-level group |
| `SNCT` | Sound Category | named, analysed generically, keeps its plugin out | top-level group |
| `SNDR` | Sound Descriptor | **described, compiled, merged** | top-level group |
| `SOPM` | Sound Output Model | named, analysed generically, keeps its plugin out | top-level group |
| `SOUN` | Sound Marker | **described, compiled, merged** | top-level group |
| `SPEL` | Spell | **described, compiled, merged** | top-level group |
| `SPGD` | Shader Particle Geometry | unknown name, analysed generically, keeps its plugin out | top-level group |
| `STAG` | Animation Sound Tag Set | unknown name, analysed generically, keeps its plugin out | top-level group |
| `STAT` | Static | **described, compiled, merged** | top-level group |
| `TACT` | Talking Activator | unknown name, analysed generically, keeps its plugin out | top-level group |
| `TERM` | Terminal | **described, compiled, merged** | top-level group |
| `TES4` | Main File Header | file header (handled by the plugin reader/writer) | file header |
| `TREE` | Tree | unknown name, analysed generically, keeps its plugin out | top-level group |
| `TRNS` | Transform | **described, compiled, merged** | top-level group |
| `TXST` | Texture Set | **described, compiled, merged** | top-level group |
| `VTYP` | Voice Type | named, analysed generically, keeps its plugin out | top-level group |
| `WATR` | Water | named, analysed generically, keeps its plugin out | top-level group |
| `WEAP` | Weapon | **described, compiled, merged** | top-level group |
| `WRLD` | Worldspace | named, analysed generically, keeps its plugin out | top-level group with child groups (cells, blocks) |
| `WTHR` | Weather | named, analysed generically, keeps its plugin out | top-level group |
| `ZOOM` | Zoom | **described, compiled, merged** | top-level group |

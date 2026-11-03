# Fallout 4 Record Handling: Audit and Plan

An audit of how the application recognises, parses, analyses and merges Fallout 4 record types, and a staged plan for widening coverage without risking incorrect merges.

**Sources:**
- **The codebase**, as of commit `f3438fb`.
- **The record list from the current xEdit Fallout 4 definitions** (`resources/fallout4_xedit_record_types.csv`, 138 record types plus the `TES4` header). This is the primary reference for which record types exist.
- **The xEdit documentation** (<https://tes5edit.github.io/docs/>), as a secondary reference for conflict and cleaning concepts. Read from its source (`github.com/TES5Edit/docs`): chapters 5 (Conflict Detection and Resolution), 6 (The Method), 7 (Mod Cleaning and Error Checking), 8 (Managing Mod Files) and 13 (Scripting Functions). See [xEdit documentation check](#xedit-documentation-check).

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
   - Identical to master *(ITM: identical to the version in the plugin's own masters)*;
   - Override (one changer);
   - Conflict (several plugins change it).

   There is no "Compatible Merge" and no "Compiled Result" for them, and the Conflict Centre says *"Not merged: this record type is preserved as is"*.
3. **Cleaning and error checks as reports** (terms as defined in xEdit docs chapter 7):
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

## Update: NPC_ overrides (stage 7a), WTHR and 44 more layouts

**Infrastructure**
- **Compile scope** (catalogue column *Scope*): `overrides` compiles only overrides of a type. A plugin that adds new records of the type keeps its plugin out, with the reason.
- **Nested groups (W1):** the plugin writer now writes cells, worldspaces and dialogue topics with their nested groups, and the definition check rebuilds the load order's own plugins to confirm it (see ARCHITECTURE, *Writing plugins: nested groups*).
- **World overrides (W2):** CELL (layout from xEdit, including the counted XCRI list), WRLD, LAND and the placed reference types compile as overrides only, with their cells and worldspaces. CELL, WRLD, REFR, ACHR, LAND, PHZD, PGRE, PMIS and PARW were confirmed by a real load order (definition check plus the group layout check on 1,366 plugins) and promoted to Winner; PBAR, PBEA, PCON and PFLA (not in that load order) stay Candidates. New references in existing cells compile since W3 (new FormIDs; not when listed in precombine or previs data), new cells, worldspaces and landscape since W4 (not with precombine or previs data, never in an ESL-flagged output). NAVM and dialogue stay out.
- **Placement guard:** only types in their own top-level group can compile; until the world stages land, the guard still applies. Anything stored inside cells, worldspaces or dialogue topics stays analysis-only, whatever the catalogue says. DIAL and WRLD are now `top with children`.
- **Arrays with a header** (`FieldDef::arrayStart`), for WRLD's large-reference list.

**New layouts.** All are transcribed from xEdit's `wbDefinitionsFO4.pas` and its shared definitions, and all start as **Candidate**: described for conflict analysis and the definition check, compiled only after a real load order confirms them. Cell, worldspace and topic children never compile.

| Group | Types | Notes |
|---|---|---|
| Stage 7a | `NPC_` | Scope `overrides`. New NPCs are tied to their plugin by FaceGen data stored under its name. On the reference load order, 154 of the 207 plugins blocked only by NPC_ contain overrides only. |
| Weather | `WTHR` | Includes its 32 cloud texture layers (`00TX`–`O0TX`). |
| Top-level | `RACE`, `LCTN`, `CSTY`, `SCEN`, `RFGP`, `VTYP`, `ECZN`, `IDLE`, `PKIN`, `MUSC`, `MUST`, `WATR`, `DLBR`, `REGN`, `RELA`, `IDLM`, `CLAS`, `CLMT`, `TACT`, `SCCO`, `FSTS`, `CAMS`, `CPTH`, `FSTP`, `MATT`, `SCSN`, `SPGD`, `BNDS`, `SMEN`, `DEBR` | |
| Never compiled (stored inside cells, worldspaces or topics) | `REFR`, `ACHR`, `PGRE`, `PHZD`, `PARW`, `PMIS`, `PBAR`, `PBEA`, `PCON`, `PFLA`, `LAND`, `INFO`, `WRLD`, `DIAL` | Analysed and checked only. |

**Position-dependent subrecords.** A subrecord whose meaning changes with its position is described by everything it can hold:
- RACE `NAM2` is an empty marker or the collar texture set.
- RACE `ZNAM` is a string there, not the common FormID.

Where size cannot tell the cases apart, the subrecord stays undescribed (scanned) and the definition check decides:
- SCEN `HTID`: actor ID array, empty marker or sound.
- Placed reference `PDTO`: topic or 4-character subtype.

**String tables** follow xEdit's localization rules:
- INFO strings come from ILSTRINGS, except `RNAM` (STRINGS).
- **Fix:** `LSCR DESC` is read from STRINGS, not DLSTRINGS. Load-screen text from localized plugins could previously come out wrong in compiled output.

**Not included:**
- `QUST`, `GMST`, `SMQN`, `SMBN`, `PACK`, `NAVM`, `NAVI`, `CELL` and the remaining types stay Preserve.
- The definition check now covers every described type, including millions of placed references, so it takes longer on large load orders. `--types` limits it.

**Reports** are also written as plain text (`.txt`).

## Promotion: 23 candidates confirmed

A definition check on a real load order confirmed all 23 candidates from stages 4 and 5:
- no size problems;
- no FormID-like values outside the described fields;
- 100% of FormIDs resolve (over 55,000 FormIDs in the 14 types that have them; the other 9 have none);
- no other definition warnings for any of the types.

Their strategy is now **Winner**: compiled, every FormID rewritten, the winning version of each chain taken whole.

| Type | Records checked |
|---|---|
| `LAYR` | 15,758 |
| `HDPT` | 5,737 |
| `ADDN` | 1,092 |
| `LTEX` | 587 |
| `IMAD` | 541 |
| `SOPM` | 515 |
| `IMGS` | 487 |
| `GRAS` | 469 |
| `ANIO` | 341 |
| `BPTD` | 294 |
| `LGTM` | 206 |
| `AACT` | 191 |
| `ASPC` | 148 |
| `MOVT` | 131 |
| `STAG` | 113 |
| `GDRY` | 101 |
| `SNCT` | 97 |
| `REVB` | 77 |
| `LENS` | 68 |
| `COLL` | 64 |
| `AECH` | 43 |
| `DOBJ` | 2 |
| `OVIS` | 1 |

On that load order about 29 more plugins become compilable directly, among them grass, god-ray and lighting mods, headlamp patches, hair mods and dismemberment patches.

Note for `HDPT`: as with every compiled record, saves and LooksMenu presets that use a compiled head part refer to its old FormID, so a new game is recommended (as for all compiled records).

## Stage 5 result: layouts by evidence (first batch)

**Evidence.** The `DependencyReport.json` of a real load order (2,302 plugins):
- 1,504 plugins are held back by record types.
- 961 of those have no other reason and at most 6 types.

Plugins those 961 would gain, by type:

| Type | Plugins where it is the only blocker | Extra plugins unlocked (after the stage 4 candidates) |
|---|---|---|
| `NPC_` | 203 | (tier 2) |
| `QUST` | 177 | (not planned) |
| `ADDN` | 8 | 8 |
| `HDPT` | 6 | 6 |
| `GMST` | 6 | 6 |
| `IMAD` | 4 | 4 |
| `WTHR` | 4 | 4 |
| `LTEX` | 2 | 2 |
| `BPTD`, `IMGS` | 1 each | 1 each |

- The stage 4 candidates alone unlock 4 more.
- Every remaining base-object type together adds about 35.
- The large lever is `NPC_` overrides and `QUST`: tier 2 and *Not planned*.

**Batch (Candidate, from xEdit's `wbDefinitionsFO4.pas`):**

| Type | Notes |
|---|---|
| `ADDN` Addon Node | |
| `HDPT` Head Part | |
| `IMAD` Image Space Adapter | Its indexed subrecords have binary signatures (`\x00IAD`–`\x14IAD`, `@IAD`–`TIAD`), confirmed in xEdit's `wbDefinitionsSignatures.pas`. `YNAM` is an interpolator there, not the common FormID. |
| `LTEX` Landscape Texture | |
| `GRAS` Grass | |
| `IMGS` Image Space | |
| `BPTD` Body Part Data | Node data has FormIDs at unaligned offsets 67, 79, 83, 87 and 91 of a 101-byte struct. |

Like stage 4, none compiles before a definition check on a real load order confirms it.

**Left out:**
- `GMST`: string settings hold localized string IDs depending on the Editor ID prefix, which the field model cannot express safely.
- `WTHR`: its layout lives in xEdit's shared multi-game definitions with per-game branches. It will be transcribed separately after this batch is confirmed.

## Stage 4 result: winner-only strategy

Done, in two parts.

**The strategy.** The catalogue's strategies are now *Specialised*, *Winner*, *Candidate* and *Preserve*.
- *Winner* types compile with every FormID rewritten, but the winning version of each chain is taken whole: no field merging, no patch records, and nothing written when a retained plugin wins.
- Their conflicts are classified record by record (Conflict Centre: **Winner only**).
- *Candidate* types have a layout and are covered by the definition check, but are not compiled.

**The evidence gate.** A candidate is confirmed only by a definition check on a real load order with:
- no size problems;
- no FormID-like values outside its described fields;
- at least 99% of its FormIDs resolving.

Promotion to *Winner* is then a one-word catalogue change.

**First candidates** (16), transcribed from xEdit's `wbDefinitionsFO4.pas`:

| Group | Types |
|---|---|
| Animation | `AACT` Action, `STAG` Animation Sound Tag Set, `ANIO` Animated Object |
| Audio | `ASPC` Acoustic Space, `SNCT` Sound Category, `SOPM` Sound Output Model, `REVB` Reverb Parameters, `AECH` Audio Effect Chain |
| Visual effects | `LGTM` Lighting Template, `GDRY` God Rays, `LENS` Lens Flare |
| Game data | `COLL` Collision Layer, `OVIS` Object Visibility Manager, `DOBJ` Default Object Manager |
| Other | `LAYR` Layer, `MOVT` Movement Type |

None compiles yet: each needs a definition check on a real load order first.

**Not included yet:**
- `PKIN`: refers to its own cell, which keeps its plugin anyway.
- `NOCM`: undecoded data.
- `BPTD`, `IMAD`, `IMGS`: large layouts, better done one at a time.
- `DEBR`: model list with per-model data.

## Stage 3 result: cleaning reports

Done. **Dependencies → Cleaning** (also in the dependency report and the CLI) lists:
- identical-to-master records (per-plugin summaries, plus each one that undoes other plugins' changes);
- deleted references, deleted navmeshes and other deleted records;
- injected records;
- circular leveled lists (`LVLI`/`LVLN`).

Each finding has a likely cause and the fix the xEdit documentation gives. Official files are informational only. Nothing is cleaned automatically. Not covered: wild edits (they need the mod's intent, ch. 7.5.5), and `LVSP` loops until `LVSP` has a layout.

## Stage 2 result: honest analysis

Done. Record types without a layout (everything not compiled) are no longer shown as merged:
- **Classification**: they are compared record by record against the version each override was made against: identical to master, Simple Override, or the new **Conflict (Not Merged)** class when the winner drops another plugin's change. They are never Compatible Merge or Hard Conflict.
- **Conflict Centre**:
  - a **Handling** column (Merged / Not merged) and a **Not Merged** filter;
  - for these records the field table's last column is **Used in Game** (the winner's version), not *Compiled Result*;
  - the generic field comparison is labelled as such;
  - the resolution selector is disabled with an explanation.
- **Reports and counts**: the HTML and JSON conflict reports, dashboard bar, CLI summary and analysis message count the new class. The HTML report has a Handling column; the JSON report has `merged` per conflict.
- **Plugins**: a **Support** column on the Plugins page and in **Export…**: *All types merged*, *Partly: N of M records not merged*, *Not merged (all records)* or *Unknown record types*.
- **Setting**: *Show field differences for record types that are not merged* (was *Field-level analysis for every record type*). It changes only what is displayed, never the classification.

Byte comparison can over-report a change (different master order or compression) but never hides one; with field comparison on, such false changes are filtered out.

## Stage 1 result: the catalogue

Done. `resources/record_catalogue.csv` (147 types: the supplied list plus `PARW`, `PBAR`, `PBEA`, `PCON`, `PFLA`, `PGRE`, `PHZD`, `PMIS`, which xEdit's `wbDefinitionsFO4.pas` defines as placed references but the supplied list omits) is now the single source for:

- record names (the xEdit names) everywhere in the app;
- the top-level group order of written plugins, now taken from xEdit's own Fallout 4 group order. The only differences from the old hard-coded list: `SLGM` (not a Fallout 4 group) is gone and `DUAL` is included. Neither is ever written, so outputs do not change;
- the allocation grouping (same 18 ranked types, so outputs do not change);
- the merge strategy, which must agree with the layouts.

Also:
- Asset path subrecords moved from `AssetTracer` into the definitions (`AssetRole`).
- The future-names list is gone.
- Editor IDs are read for compressed records of every type (only the start of the stream is inflated).
- Signatures not in the catalogue are reported as unknown record types.
- **Plugins → Blocking Types**, its export and the CLI dashboard show category and support level.
- `fo4mx record-types` prints the whole catalogue.

Still hard-coded and left for later stages: PERK/VMAD decoding context in `FieldDecoder` (structural, not a list) and the condition function table.

## xEdit documentation check

What the xEdit documentation confirms or changes in this audit.

| Concept | xEdit documentation | Effect here |
|---|---|---|
| **Override vs conflict** | An override changes a master's record and is normal. A *conflict* needs a master and at least two overrides that change different things, so the last one loaded discards the other's change (ch. 5.2). Ten plugins can override one record without a conflict if the last one carries all the changes. | Matches the existing Simple Override / Compatible Merge / Hard Conflict split. |
| **What an override changed** | The conflict algorithm (ch. 5.10) compares each override with the version in **its own masters**: the last of its masters that overrides the record ("CurrentMaster"), not the original record. List entries present only in the override are additions; entries present only in that master version are removals. | **Fixed in this change** (see below). |
| **ITM** | Identical to Master: the plugin repeats its master's version without a change (ch. 7.5.2, glossary). A record identical to its master that is also the conflict winner ("ITM but conflict winner") is not cleaned. ITMs are only meaningful with the plugin's masters loaded and nothing else (ch. 7.4.1). | ITMs are now measured against the plugin's own masters. The stage 3 report lists ITMs per plugin and never suggests removing one that wins a conflict. |
| **Deleted references (UDRs)** | Deleted references are "a significant cause of crashes". xEdit's fix is to undelete, set Initially Disabled and move to Z = -30,000 (ch. 7.5.3). | Stage 3 reports them; the app does not edit plugins, so the report names the xEdit fix. |
| **Deleted navmeshes** | Can cause crashes and cannot be fixed automatically: manual cleaning only (ch. 7.5.4). | Reported as a warning with that explanation. |
| **Wild edits** | Unintended edits; need the mod's intent to detect, so manual only (ch. 7.5.5). | Not detectable automatically. Not planned. |
| **Unresolved references** | "Very serious"; must be fixed by the mod author, not the user (ch. 7.4.13). | Matches the existing **Broken in Source** handling and likely-cause text. |
| **Official DLC errors** | "There are errors in the official DLCs. Do not attempt to correct them." (ch. 7.4.13). | Findings in base-game and DLC masters are shown as informational, never as errors. |
| **Injected records** | Only described through the scripting API (`IsInjected`, ch. 13), without a definition. | Stage 3 uses the established meaning (a new FormID claiming a master that does not define it) and labels it as such. |
| **Circular leveled lists** | Can hang or crash the game; xEdit can detect them (ch. 7.4.14). | Added to stage 3: a cycle check over `LVLI`/`LVLN` (and `LVSP` when described) entries. |
| **Merged patch** | xEdit's own merged patch is obsolete and unsupported; it merged `LVLI`, `LVLN`, `LVSP`, `CONT`, `FACT`, `RACE`, `FLST`, `NPC_` (ch. 5.9). | Our patch covers the described types among these. `RACE`, `NPC_` and `LVSP` stay tier 2. |

### Fixed: overrides are compared with their own masters' version

Until now every override was compared with the **original record** (the first definition). A plugin made with another override as its master was misread:
- A compatibility patch that **removes** an item its master added (a keyword, a leveled list entry, a form list entry) was treated as making no change for that item, so the merge put the item back.
- A plugin that deliberately **reverts** a value its master changed was treated as identical to the base, so the master's change won in the merged result.
- A plugin that only repeats its master's change was counted as a separate changer, which could report a false conflict.

Merging now follows the xEdit algorithm: each override's changes are what differs from the last earlier override among its own (direct or indirect) masters, or the original record when there is none. A later plugin made against an earlier one's version supersedes it rather than conflicting with it. Chains in which no override has another override as master merge exactly as before. The build cache format version was raised so no merge computed the old way is reused.

## Staged plan

Each stage ships separately, keeps every existing test passing, and changes no merge behaviour for the 61 supported types unless stated.

| Stage | Work | Risk | Outcome |
|---|---|---|---|
| **1. Catalogue** *(done)* | Load the xEdit list into `RecordDefinitions` with name, category, placement and support columns. Replace the names list, the writer group order, `typeRank` and the asset table with catalogue data. Report unknown signatures separately. Read Editor IDs for compressed records of every type. | Low: data and display only | Every one of the 138 types is recognised and named. Blocking Types and reports show categories. |
| **2. Honest analysis** *(done)* | Conflict classification by support level (record level without a layout). Conflict Centre and reports say "preserved, not merged" for those types. A **support level** column on the Plugins page and in exports. | Low: display and classification only | No more implied merges for types the app does not understand. |
| **0. Merge against own masters** *(done)* | See [the fix above](#fixed-overrides-are-compared-with-their-own-masters-version). | Corrects merges where patches remove or revert their master's edits | Merge results match what each plugin's author intended. |
| **3. Cleaning reports** *(done)* | ITM, UDR-candidate, deleted-navmesh, injected-record and circular leveled list findings per plugin, with likely causes (terms as in xEdit docs chapter 7). Findings in official master files are informational only. | Low: reporting only | Error-checking parity with the usual pre-merge checks. |
| **4. Winner-only strategy** *(done: strategy and 16 candidates)* | For types confirmed to be single-value records, a merge strategy that takes the winning override whole: no field merging, still compiled. Candidates must still pass the definition check and have every FormID located. | Medium: only for types with full FormID coverage | More plugins compile without field-level merge risk. |
| **5. Next layouts by evidence** *(first batch done)* | Describe further types in the order **Plugins → Blocking Types** ranks them across real load orders. Each needs a definition check on your data and tests. Likely early candidates (check against the xEdit Fallout 4 definitions, not assumed): `AACT`, `DOBJ`, `COLL`, `SNCT`, `SOPM`, `ASPC`, `REVB`, `AECH`, `ANIO`, `BPTD`, `DEBR`, `IMGS`, `IMAD`, `LGTM`, `GDRY`, `LENS`, `MOVT`, `STAG`, `NOCM`, `LAYR`, `OVIS`, `PKIN`. | Medium per type, controlled by evidence | Fewer plugins blocked by base-object types. |
| **6. Group structure** | Parent context in the index; nested-group writing; tests that round-trip cells and dialogue topics byte-for-byte. Still no compiling of these types. | Medium: core reader and writer | Foundation for any future world or quest work. |
| **7. Tier 2 (analysis first)** *(started: NPC_ overrides, RACE and CLAS as Candidates)* | `NPC_` overrides, `RACE`, `CLAS`, `PACK`, `LVSP`, … Layouts, field-level analysis and definition checks first; compiling only for overrides of existing records (no new FormIDs, no FaceGen impact), each separately justified. | High | Gated individually. |
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

## Stage 7a promotion

The definition check on a real load order (build 39) confirmed 27 of the stage 7a Candidates; they are promoted to Winner:
RELA, MUSC, SPGD, RACE, IDLE, CPTH, IDLM, SMEN, REGN, SCSN, ECZN, SCCO, RFGP, FSTP, VTYP, DEBR, WATR, CLAS, BNDS, CAMS, FSTS, TACT, CLMT, MUST, MATT, CSTY, and NPC_ (overrides only: new NPCs keep their plugin, FaceGen is stored under the plugin's name).

Held back: LCTN (confirmed overall, but `RCSR` resolves only 86.4% of its FormIDs, so its layout is not trusted), WTHR (`IMSP` sizes), PKIN (98.9% resolve), SCEN, INFO and DIAL (dialogue never compiles).

## Quest stage (Q1): layouts

- **QUST** has a layout from xEdit. Stages, objectives and aliases repeat the same subrecords (each alias has its own keyword and inventory lists), and the field model merges a record's packed arrays by subrecord type, so everything from DNAM on is one region, kept in order; its FormIDs are found and rewritten by the field definitions.
- **Quest script data** (VMAD) is decoded in full: quest fragments (count, fragment script with its properties, stage fragments) and the alias section (each alias's object, its own version and format, and its scripts). Anything that does not end exactly at the end of the subrecord is scanned and reported.
- **SMQN** and **SMBN** (story manager quest and branch nodes) have layouts.
- All three are **Candidates**: checked by the definition check, not compiled until a real load order confirms them. Quests with dialogue stay with their plugin either way (dialogue is not compiled).

## Quest stage (Q2): SEQ files

- Each output gets `seq\<output>.seq` listing its start-game-enabled quests, as zEdit and xEdit build it: new quests, and overrides that make a quest start with the game when its defining plugin does not (that plugin's own SEQ covers the others). FormIDs as written in the output. An output that no longer needs one loses it at export.
- A plugin's own `seq\<plugin>.seq` is a Safe filename reference: once its quests are compiled it is no longer read.
- QUST, SMQN and SMBN stay Candidates until a definition check on a real load order confirms their layouts; with them promoted, quests without dialogue compile.

## Quest stage (Q3): promotion

The analysis's candidate check on a real load order confirmed QUST (11,474 records, 529,239 FormIDs, 99.9% resolve, script data decoded in full), SMQN (1,284 records, 99.8%) and SMBN (423 records, 100%). They are promoted to Winner. Quests with dialogue stay with their plugin (DIAL, INFO, SCEN and DLBR are not compiled).

## NPC stage (N1)

NPC_ is no longer overrides only: new NPCs compile with new FormIDs. FaceGen is found by the defining plugin's name and the NPC's FormID, so a plugin that ships FaceGen keeps its NPCs through its FaceGen folder (a Compatibility Required filename reference, loose files and archives alike); new NPCs without FaceGen look the same compiled. Relocating FaceGen to the output (N2) comes after confirming how the engine finds the face textures.

## Locations and game settings

- **LCTN** is promoted to Winner. Its overall check passed on a real load order; the 86.4% resolve rate is in `RCSR` (removed persistent references), which xEdit also reads as a plain FormID array: the failures fall at scattered offsets, so they are stale references left by the plugins, not a layout fault. They are rewritten like any other FormID or left as they are when they resolve to nothing.
- **GMST** has a layout and is a **Candidate**. Its value type follows the first letter of its Editor ID (`s` string, `f` float, `i`/`u` integer, `b` bool); a string setting holds a localized string in a localized plugin, which the Normalizer turns into inline text like any other localized field. The next analysis checks it.

## Navmesh stage (M1): layouts

- **NAVM** and **NAVI** have layouts from xEdit and are **Candidates**: checked by every analysis, not compiled.
  - NAVM `NVNM`: version, pathing cell (parent world, then the grid or, in an interior, the parent cell), vertices, triangles (21 bytes), edge links (the linked navmesh), door links (the door reference), cover, cover triangle mappings, waypoints and the navmesh grid (divisor² cells). `MNAM`: references with the triangles cut for them. `ONAM`: base objects.
  - NAVI `NVMI`: the navmesh, its edge and preferred edge links, door links, optional island data and its pathing cell. `NVPP`: precomputed paths and road markers. `NVSI`: deleted navmeshes.
  - Every one must end exactly at the end of its subrecord; anything else is reported and scanned.
- Compiling comes after the check confirms the layouts: every plugin with navmeshes overrides the single NAVI record, so the outputs need its navmesh infos combined per navmesh, not the last plugin's taken.
- The Validation Report lists the definition and group layout check right after errors, so the row limit of the text export never cuts it off.

## Navmesh stage (M1b): first check

The analysis's candidate check on a real load order (build 54):
- **GMST** confirmed (2,225 records): promoted to Winner. Game settings compile; the last plugin's value wins, as in game.
- **NAVI** confirmed (291 records, 99.9% of 2,019,040 FormIDs resolve). It stays a Candidate until navmeshes compile and its navmesh infos can be combined.
- **NAVM** not confirmed: 99.9% of 745,990 FormIDs resolve, but some `NVNM` subrecords did not decode (3,625 values in their scanned bytes look like FormIDs). Older navmeshes are now read as xEdit notes them (cover data from version 13, waypoints from version 12), and the check lists why each one failed, grouped by reason and navmesh version, so the layout can be corrected.

## Navmesh stage (M2): compiling navmeshes

The second check (build 55): NVNM decodes in 28,507 of 28,517 records. The other 10 are version 15 with 4 bytes after the navmesh grid, which xEdit does not describe; they are now kept as they are, and treated as a FormID only if they name an existing record (so the check counts them).

Compiling, ready for when the check confirms NAVM:
- **NAVM** compiles like other cell children: with its cell (copied as a container when no compiled plugin provides it), its pathing cell, edge links, door links and precut references rewritten.
- **NAVI** is not taken from the last plugin. The game combines every plugin's navmesh info map as it loads, one entry per navmesh, so the output carries one NAVI override holding the entries of every compiled plugin (a later plugin's entry for the same navmesh replacing an earlier one) and their deleted navmeshes; the version and precomputed pathing are the last compiled plugin's. Retained plugins keep their own NAVI. Because it references the compiled navmeshes, the dependency rule places it in the output with the last of them.
- Both stay **Candidates** until the analysis confirms NAVM on a real load order.

## Navmesh stage (M3): promotion

The third check (build 56): NVNM decodes in 28,516 of 28,517 records (the 9 with 4 bytes after the grid now decode, and those bytes name no record). The last one, in one plugin, has 6 bytes after its grid. NAVM and NAVI are promoted to Winner:
- navmeshes compile with their cells, and the outputs carry one navmesh info map combining every compiled plugin's entries (M2);
- a plugin with navmesh data that does not decode (NAVM or NAVI) stays in the load order, with the record and the reason in its eligibility: its FormIDs could not be rewritten with certainty, and its entries are part of the combined map.

To watch: the output holding the navmesh info map needs every plugin its entries name as a master. An output past 254 masters is a validation error, never a silent change.

## Navmeshes compiled (build 57) and packages (P1)

Build 57 compiled navmeshes on a real load order: 1,511 plugins compiled (+68), 285,636 new FormIDs, 0 errors; the output holding the navmesh info map has 177 masters. The navmesh info map no longer carries entries for navmeshes that exist in no plugin (8 in one plugin were written as "none"); they are left out.

- **PACK** has a layout from xEdit and is a **Candidate**. Data inputs, the procedure tree and the begin/end/change events repeat the same subrecords, so everything from PKDT on is one region, kept in order. CNAM is the combat style before the first data input (ANAM) and a value after it. PLDT holds a FormID for references, cells, object IDs and keywords (types 0, 1, 4, 6); PTDA for references, object IDs and keywords (types 0, 1, 3, 7); unknown types are scanned. The script data has package fragments (one per OnBegin, OnEnd, OnChange flag), decoded in full. BNAM, IDLB and PFOR are undecoded in xEdit and scanned.

## Packages (P2): promotion, and the next candidates

The analysis's candidate check on a real load order (build 58) confirmed PACK (13,096 records, 99.9% of 62,704 FormIDs resolve): promoted to Winner. Packages compile.

Layout corrections for the remaining candidates:
- **WTHR IMSP** holds 4 image spaces (sunrise, day, sunset, night), or 8 from form version 111 (xEdit): read as a FormID array of either size. The 4 records of 16 bytes no longer fail.
- **INFO TRDA**: the emotion keyword is FFFFFFFF for "none" (xEdit allows it), which was counted as a FormID that resolves to nothing (37% of them). It is no longer read as a FormID.
- A candidate whose FormIDs resolve below 99% is reported with the subrecords that fail, their resolve rate and offsets (PKIN: 98.9% overall).

## Weather, pack-ins, and dialogue script data (D1)

The candidate check on a real load order (build 59):
- **WTHR** confirmed (272 records, 100% of 12,745 FormIDs resolve, IMSP of 4 or 8 image spaces): promoted to Winner.
- **PKIN** at 98.9%: every failure is CNAM (the pack-in's cell), 18 of 1,692, cells missing from their source plugins (read as none in game, written as none). The layout is a single FormID: promoted to Winner.
- **INFO** TRDA now resolves (the no-emotion value). What is left for INFO and SCEN is their script data:
  - INFO and SCEN VMAD fragments are decoded in full (xEdit wbVMADFragmentedINFO / SCEN): flags, the fragment script, one fragment per OnBegin/OnEnd flag and, for scenes, the phase fragments (a uint16 count; each a flag, phase index, a byte, script and function names). They must end exactly.
  - SCEN HTID is the sound in a radio action (action type 6, from the ANAM that starts the action) and alias IDs in a dialogue action.
- Dialogue (DIAL, INFO, SCEN, DLBR) still does not compile: voice files are found by the plugin's name and the response's FormID.

## Archives without a name table

Archive2 can write a BA2 without its name table (`-nostrings`); the game finds files by their hashes, so it loads. These archives were reported unreadable. They are now read: each file is matched by the game's hashes (xEdit TwbHash.FO4: CRC32 of the folder and of the file name without extension, lower case with backslashes, and the extension) to the path another archive or loose file gives it; a file no other source names keeps the name xEdit shows (`<dir hash>\<name hash>.<ext>`). Conflicts and archive priority then cover them like any other archive.

## Dialogue stage (D2): dialogue compiles

The candidate check on a real load order (build 60) confirmed DIAL (144,595 records), INFO (289,355), SCEN (13,166) and DLBR (442): every candidate type is now confirmed. They are promoted to Winner and compile:
- topics (DIAL) in their quest's group, responses (INFO) in their topic's group, scenes (SCEN) and branches (DLBR) in their quest's group, as the nested group writer already writes them;
- a response or topic whose topic or quest no compiled plugin changes is written in a copy of it as the game loads it now (a container, as for cells), so the output changes nothing about it;
- a quest holding compiled dialogue goes to the same output as its dialogue (it joins the world clusters);
- response order comes from each INFO's PNAM (previous INFO), a FormID, rewritten like any other;
- **voice files** are found by the defining plugin's name and the response's FormID (`Sound\Voice\<plugin>\<voice type>\<FormID>_<n>.fuz`): a plugin with such a folder (loose or in an archive) stays in the load order (Compatibility Required). Relocating voice files to the output is the next stage (D3). Overrides of other plugins' responses keep their voice files, which are found under the defining plugin.

## Dialogue stage (D3): voice files

Build 61 compiled dialogue on a real load order: 1,626 plugins (+64), 416,559 new FormIDs, 0 errors.

Voice files move with their responses. The game finds a response's voice as `sound\voice\<defining plugin>\<voice type>\<FormID without its load order index>_<n>.<ext>`. At export, every voice file of a compiled new response (loose or from an archive) is copied into the output folder as `sound\voice\<output>\<voice type>\<new FormID>_<n>.<ext>`; `Multiplexer_Voices.tsv` lists the copies and their sources, and copies a later build no longer needs are removed. A plugin's voice folder is therefore a Safe filename reference instead of Compatibility Required. Overrides of other plugins' responses keep their voice files, found under the defining plugin.

The Validation Report now opens with the number of rows per category, and shows at most a tenth of its row limit per category, so one large category no longer hides the others.

## Archive winners as loose copies (A1)

Build 62: 1,653 plugins compiled (+27, voiced dialogue), 719,923 new FormIDs, 0 errors. 164 plugins were kept out automatically, 146 of them for archive priority, mostly over 1 to 5 files.

On an install without archive loaders (Vortex), a compiled plugin's archives load from the INI list after switching over, before every plugin's own archives, so a file its archive wins today can be won by another archive. Instead of keeping the plugin out, the export now writes each such file loose into the output folder: a loose file wins over every archive, so the game shows the same version. `Multiplexer_ArchiveWinners.tsv` lists the copies with their content hash; a new scan does not take a listed copy for a source (the archive is), and copies a later build no longer needs are removed. A plugin that would need more than 2,000 copies (`archiveWinnerCopyLimit`) is still kept out.

Missing Assets are now Info, summed up with 50 examples: compiling never changes asset paths, so a file no installed mod provides was missing for the source plugin too (6,586 in build 62, such as Fallout4.esm's own `.egt` references).

## NPC stage (N2): FaceGen moves with its NPCs

Build 63: 1,769 plugins compiled (+116, mostly from archive winners kept as 7,060 loose copies), 829,809 new FormIDs, 0 errors, 6,651 warnings (half of build 62). Every record type compiles; 39 plugins were kept out automatically (records the compiler could not translate safely). What keeps plugins out now is filename references: FaceGen folders (90 plugins), precombined meshes and previs (23 and 13), Transfer Settlements blueprints (20), and unknown or unsafe references in scripts and configs.

The game finds an NPC's face as `meshes\actors\character\facegendata\facegeom\<defining plugin>\<FormID>.nif` and its face textures as `textures\actors\character\facecustomization\<defining plugin>\<FormID>_<suffix>.dds`. At export they are copied for every compiled new NPC under the output's name and the NPC's new FormID (`Multiplexer_FaceGen.tsv`), like voice files; FaceGen folders are now Safe filename references. The face mesh itself is copied unchanged: it names its textures by their original path, which stays installed with the original mod (its plugin is disabled, its files are not removed).

## Fields that kept plugins out (F1)

Build 66: 1,804 plugins compiled, 0 errors; 42 kept out automatically. Most of those were values in undescribed bytes that looked like FormIDs of compiled records. The fields are now described from xEdit:
- **PERK EPFD** by EPFT: 5 is a spell; 2 is an actor value and a float when the entry point function is 5, 12, 13 or 14 (two floats otherwise); 8 is an actor value and a float; 7 is localised text; 6 text.
- **MGEF DATA**: the layout is complete (every FormID was already described; the flags at +0 were scanned and looked like FormIDs).
- **PROJ DNAM**: the VATS projectile at +89 (not aligned); the layout is complete.
- **SNDR BNAM**: the base descriptor in an AutoWeapon descriptor (CNAM `ED157AE3`), numbers otherwise; CNAM and LNAM are numbers.
- **EFSH DNAM**: the ambient sound at +108 in the current 157-byte layout; addon models at +241 and the ambient sound at +305 in the old one.

The other keep-outs are by design: new references and cells in precombine or previs data, five plugins that would need more than 2,000 archive winner copies, and three records that reference a record their plugin does not compile.

`Multiplexer_Compilation.json` is no longer written: everything in it is in the Compilation Report.

## Folders of record paths (R1)

Build 67: 1,860 plugins compiled (+56), 975,125 new FormIDs, 0 errors; 25 kept out automatically (as planned: precombine/previs data, very large archive packs and records referencing records their plugin does not compile). Every record type compiles; what keeps plugins out is filename references.

The largest group of Unknown references was a folder named after the plugin, most of them `meshes\scol\<plugin>\` (24). A static collection's mesh is named by its full path in the SCOL record's model, so the game loads it by that path, which compiling keeps unchanged. A folder named after the plugin is now Safe when every file in it is named by path in the plugin's own records (models, textures, sounds); a folder with any file nothing names stays Unknown.

## Asset folders and script diagnostics (R2)

Build 68: 1,864 plugins compiled (+4), 0 errors. The record-path check of R1 freed few plugins: the `meshes\scol\<plugin>\` folders hold files the records do not name (combined meshes left from earlier versions of the collections). Those files are never loaded either way: the game finds files under meshes, textures, materials, sound and music only by the paths records and meshes name, never by a plugin's name, apart from the FaceGen, voice, precombine, previs and LOD folders, which keep their own rules. A folder named after the plugin under those roots is now a Safe reference ("asset folder").

Script references: the most common Unknown use is "other instruction" (one script, `scripts\karm\types\tquest.pex`, accounts for 10 plugins). The reason now names the Papyrus opcode (`array_setelement`, `struct_set`, `array_findelement`, ...), so the next report shows what these scripts do with the name; following the name through arrays and variables to `GetFormFromFile` or `IsPluginInstalled` is the next stage (S1).

## Named constants in scripts (S1)

Build 69: 1,873 plugins compiled (+9), 1,100,415 new FormIDs, 29 outputs, 0 errors. With the opcode now in the reason, 21 of the 22 "other instruction" script references are `return`: the plugin name is the value of a property's read handler, a named constant. `scripts\karm\types\tquest.pex` (uploaded) is such a table: one property per supported mod (`sPluginMod_CWPointLookout` returns `CWPointLookoutFO4.esm`, ...), used by nothing in the script itself.

What the name means is what the scripts reading the property do with it, so every script in the load order now records the properties it reads (propget) and follows the value through copies within the function to its use:
- no script reads the property: the reference is Safe;
- a reader passes it to Game.GetFormFromFile: Compatibility Required, naming the script, the function and the FormID (rewriting a name shared through a constant, for records that may end up in different outputs, is not implemented);
- readers only pass it to Game.IsPluginInstalled: Compatibility Required for now, saying so (the rewrite through the constant is the next step);
- any other use: Unknown, naming the reading script, function and instruction.

## Named constants: presence checks rewritten (S1b)

Build 70: 1,875 plugins compiled, 0 errors. KARM's TQuest constants are read by no script in the load order (Safe); three `CanarySaveFileMonitor.sPluginCanary` constants (RPTN, MUTA, GHUL) are read only by Game.IsPluginInstalled.
- A constant read only by presence checks is now Rewritable: the Advanced Users Auto-Merger rewrites the constant in its copy of the script to the output plugin's name, which is installed, so every check stays true. Its check of the copy accepts the rewritten constant (the decision was made across the load order).
- A constant is no longer taken for unread when a script could read it in a way the scan does not follow: by name at run time (ScriptObject.GetPropertyValue / SetPropertyValue with the property's name), or in a script that does not read back (each of its strings counts as an unknown read). Both make it Unknown.

## Automatic output count; script default values (S1c findings)

Build 71: 1,876 plugins compiled, 0 errors.

**Automatic output count**: the compiler can pick the fewest output plugins whose master lists all fit (254 masters per plugin), trying 1, 2, 4, ... until they fit and then narrowing down between the last count that failed and the first that fit. Only allocation, FormIDs and master lists are repeated per try; merging is done once. `--outputs auto` on the command line.

**Default values in scripts** (uploaded `wanamingo\lobotomitepack\integration.pex`): the plugin name is the default value of `::LAER_PLUGIN_NAME_var`, the variable behind the Auto property `LAER_PLUGIN_NAME`, and also passed directly to GetFormFromFile (rewritable). An Auto property's value can be set in the plugin's own script data (VMAD) in the Creation Kit, which replaces the default at run time, so rewriting the default alone is not reliable: following these needs the plugins' script data checked for the property as well (next stage).

## Plugin names followed through script variables (S2)

Build 72 (Automatic output count): 2 output plugins plus the patch (103,262 records with 115 masters; 1,190,885 records, 218 MB, with 245 masters), 1,876 plugins compiled, 0 errors. Every FormID changed with the output count.

The uploaded `Vlt04ExternalContentScript.pex`, `AR_ExternalContentControlSystemScript.pex` and `Integration.pex` keep plugin names in variables: a script variable's default (`sWattzRifle = "WattzLaserGun.esp"`), a local assigned in one function and passed to another (`sPluginName = "SS2.esm"`, then `ExternalContentCheck(sPluginName, ...)`), or an Auto property's default (`LAER_PLUGIN_NAME`).

Each script's code is now followed: a plugin-name constant that enters a variable (an assignment, a script variable's default, an argument to one of the script's own functions) is traced through copies, casts, text joins and the script's own function parameters (repeated until nothing changes; flow-insensitive, so it over-approximates) to where it ends up:
- GetFormFromFile with a constant FormID, IsPluginInstalled, and log text (Debug.* or a function named Trace): Rewritable; one reference per lookup, all sharing the one constant. The Advanced Users Auto-Merger rewrites the constant once and each FormID; when the lookups' records are in different outputs, one name cannot point at both and the copy is not made (fail closed).
- GetFormFromFile with a FormID computed at run time: Unsafe.
- An Auto property's default: Compatibility Required (the plugin's script data can set the property), or Unknown when the script itself does not use it (other scripts can read it).
- Stored but never used, or used only in log text: Safe.
- Anything else (returned, stored in an array, passed to another script's function): Unknown, naming the instruction or function.

On the uploaded scripts: every name in Vlt04ExternalContentScript is rewritable; in AR_ExternalContentControlSystemScript every name except AmericaRising2.esm (stored in an array for Settlement Menu Manager) is; Integration's are all Auto property defaults.

## Functions returning a plugin name (S2b)

Build 73: 1,877 plugins compiled, 0 errors; the Auto-Merger relinked 532 files with 0 errors and finds 450 relinkable plugins (263 before S2). What keeps plugins out now: precombined meshes (34), Transfer Settlements blueprints (20), `x-cell-exceptions.ini` (7), a script function returning the plugin's name (7: mods that name themselves with `GetPluginName()` in their Program script), F4SE DLLs (9) and GetFormFromFile with FormIDs computed at run time (9).

A plugin name returned as a constant by any function (not only a property's read handler) is now a named constant: a call's result is followed like a property read, matched by the function's name across every script in the load order (a match across unrelated scripts only makes the verdict stricter). Called by nothing: Safe; called only for presence checks: Rewritable; called for GetFormFromFile: Compatibility Required with the caller named; anything else: Unknown.

## FormID pairs in reverse order; Transfer Settlements blueprints (C1)

Build 74: 1,877 plugins compiled, 0 errors (as build 73). Uploaded samples: `x-cell-exceptions.ini`, three Transfer Settlements blueprints, and the file names of a plugin's precombined meshes and previs files.

- **"0x<FormID>:Plugin.esp"** (X-Cell's `[facegen_exception]`, and any config written that way) is a FormID pair like "Plugin.esp|FormID": Rewritable, the name and the FormID rewritten together.
- **Transfer Settlements blueprints** (JSON): each item has `plugin_name`, `FormID` (6 hex digits) and `FormID_dec` (the same number in decimal). An item naming a compiled plugin is Rewritable: its plugin name, hex FormID and decimal FormID are rewritten together. An entry of `header.plugins_required` is Rewritable (it can name the output plugin, which is installed). The settlement itself (`workshop.plugin`, `worldspace_plugin`) stays Unsafe: the workshop's base object is stored as a decimal number without a plugin, so it cannot be rewritten with certainty.
- **Precombined meshes and previs** are named `meshes\precombined\<cell's plugin>\<cell FormID>_<hash>_OC.nif` (and `_Physics.nif`) and `vis\<cell's plugin>\<cell FormID>.uvd`. Renaming them for a compiled new cell is possible only if the files themselves do not hold the references' FormIDs, which compiling changes; that needs one .uvd and one precombined mesh examined (next stage).

## Precombines and previs examined; workbenches; fixes (C2)

Build 75: 1,891 plugins compiled (+14), 2 outputs (3 before), 0 errors; the Auto-Merger finds 471 relinkable plugins (450) and relinked 534 files with 0 errors. The C1 rules worked: Transfer Settlements blueprints keep 3 plugins out (20 before), plugin names in configs 5 (11). Uploaded samples: a precombined mesh and a previs file of one plugin's new cell.

- **What the files hold.** The precombined mesh (`<cell>_<hash>_OC.nif`) holds geometry, material and texture paths, and its own name: no FormIDs. The previs file (`<cell>.uvd`) holds the FormIDs of the cell's references in the plugin's own numbering (load-order index = the plugin's master count: 64 such values in the sample). Moving a compiled cell's previs would mean rewriting values inside a binary format nobody has published; that is not done.
- **The folders no longer keep plugins out.** `meshes\precombined\<plugin>\` and `vis\<plugin>\` are read only for that plugin's own new cells that carry precombine or previs data (PCMB, VISI, XCRI, XPRI, or a previs file named by RVIS), and the compiler already refuses such cells (Precombined Cell), checking the cell as the game loads it. The folder is now a Safe reference ("cell data folder"). A plugin whose cells use the files is kept out automatically with the exact cell named; one whose folder is left over from an earlier version compiles.
- **Transfer Settlements workbench.** `"workshop": {"plugin", "id", "id_dec", "baseid_dec"}` names the settlement's workbench. It is rewritten like an item (C3 below: only when its base object does not move).
- **Fixed: "0x<FormID>:Plugin.esp" rewrites failed their check.** The reference's text ran from the name to the end of the file (the FormID comes before the name), so the copy's check compared the wrong text and kept 3 plugins out (DCGuard_ArmorOverhaul, ellen, llamaCompanionHeatherv2). Nothing was written wrongly; the check failed closed.
- **Fixed: a presence check naming a plugin whose records all went into other records** (merged edits, no new records) had no output to name; it now names the first output, which is installed, so the check answers as before.

## Per-plugin FormID INIs, blueprint pairs, decoding notes (C3)

Build 76: 2,101 plugins compiled (+210 from the precombine/previs rule), 2 outputs, 0 errors; the Auto-Merger finds 517 relinkable plugins (471) and relinked 628 files (534) with 0 errors; 64 plugins kept out automatically (27), most of them new cells with precombine or previs data, now named exactly. Record Decoding warnings rose to 27,615 with the compiled plugins: they are notes about the source records (a FormID past the source's master list, which reads as none and is written as none, also reported as Broken in Source), so they are now Info and name the source plugin, not the output.

Uploaded samples: a Transfer Settlements blueprint of an interior settlement, two `BCR\<plugin>.ini` files and a RobCo Patcher INI.

- **Blueprint pairs.** A blueprint writes a plugin name with its FormID under several keys: `plugin_name` with `FormID`/`FormID_dec` (items), `cell_plugin` with `cell_id`/`cell_id_dec` and `worldspace_plugin` with `worldspace_id`/`worldspace_id_dec` (the settlement's cell and worldspace), `PlotPlanPlugin` with `PlotPlanFormID`/`PlotPlanFormID_dec`, and `PlotPath01Plugin` with only `PlotPath01FormID_dec`. Every such pair is Rewritable: the name, hex and decimal FormID are rewritten together. Hex and decimal must agree, or the entry is Unknown.
- **Workbench, corrected.** The C2 rule took `baseid_dec` below 0x01000000 to be a Fallout4.esm object; the sample's workbench has `baseid_dec` 1919, so the number does not say which plugin the base is from. The Auto-Merger now reads the workbench's record: it is rewritten only when its base object, as the game loads the workbench, is from a plugin that is not compiled (the base then does not move, whatever the number means). Otherwise the blueprint copy fails closed.
- **Per-plugin FormID INIs** (`BCR\<plugin>.ini`: `[Weapons]`, `weaponFormId_<name>=<hex>`, FormIDs relative to the plugin the file is named after). Rewritable when every line is a section, a `key=<hex FormID>` or a comment; the Auto-Merger writes `BCR\<output>.ini` with the same keys and the records' new FormIDs, merging the INIs of all plugins compiled into that output. It fails closed when an entry is not a record of its plugin, when one file's records went into different outputs, or when two INIs share a key; the written file is parsed back and compared. Any other line keeps the file Unknown.
- **RobCo Patcher `filterByModNames=<plugin>`** stays Unknown: it selects every record whose file is that plugin. Naming the output instead would select the records of every plugin compiled into it, so the patch would reach records it never touched.
- **Fixed: light-plugin FormIDs.** A FormID past FFF given for an ESL-flagged plugin (not written `FExxx...`) was masked to its last three digits and could be rewritten to an unrelated record; it now names no record, so the reference is left as it was (the game found nothing before either).

## Failures kept to the plugin that causes them; lookups through own functions (C4)

Build 77: 2,080 plugins compiled (21 fewer than build 76), 0 errors, 859 warnings (decoding notes now Info). The drop came from C3's new checks failing a whole file and so keeping out every compiled plugin it names: the Croup Manor blueprint (17 plugins, because one workbench's base object moved), `BCR` INIs that share a key (3), and a script where one plugin's lookup is no record of it (3).

- **A failure caused by one plugin keeps only that plugin out.** The file is planned again without it in the next round, so the other plugins it names compile and their entries are rewritten. This applies to the settlement workbench and to the shared-name lookup below; any other failed check still keeps every plugin of the file out.
- **Workbench base in its own plugin.** In both samples the workbench's base is a record of the workbench's own plugin, and `baseid_dec` is that record's object ID (CAP_WorkshopWorkbench `[CONT:65007A8E]`, `baseid_dec` 31374 = 0x7A8E). When the base moves with the workbench into the same output, `baseid_dec` is rewritten with its new ID. A base in a plugin that is not compiled stays as it is; any other case keeps the workbench's plugin out.
- **A lookup that finds nothing, sharing its name with a renamed use** (Vlt04ExternalContentScript keeps each plugin's name in one value used by both `GetFormFromFile` and `IsPluginInstalled`): when the FormID is no record of the plugin, renaming the shared name would make the lookup read the output with that FormID, possibly an unrelated record. That plugin stays out; the others in the script are rewritten.
- **`BCR` keys shared by two INIs** (`weaponFormId_Standard` in three plugins): each file's keys are its own labels after `weaponFormId_`, so the tool lists a section's keys; the later plugin's key gets its name appended (`weaponFormId_Standard_CWAuto5`), keeping the original key as its prefix. A key that still clashes keeps the plugins out.
- **Lookups through the script's own function** (PirateCompatUpdater: `OnInit` calls `self.addModItemToFormList("Vault4.esp", 0x9EC6E, list)`, which calls `Game.GetFormFromFile(formId, pluginName)`): when a function passes two of its parameters straight to `GetFormFromFile`, reads the FormID parameter nowhere else and never overwrites either, each call site supplies the lookup's FormID. A constant there is a fixed FormID: Rewritable, patched at the call site. Anything else stays Unsafe (computed at run time).

## A re-planned file is no longer marked failed; constants matched by script type (C5)

Build 78: 2,106 plugins compiled (+26 on build 77), but 2 errors, so the Auto-Merger wrote nothing (Files Relinked: 0). C4 worked as meant: the blueprint, BCR and Vlt04 plugins compile again (OutfitWorkbench, Homemaker, StoryWealth-SRD-PlayerHome, Flashy's Gun For Hire, CWAuto5, CWHuntingShotgun, BoSStory) and only WattzLaserGun stays out of Vlt04ExternalContentScript. Three ESL-flagged plugins' BCR INIs list FormIDs past FFF (made before the plugins were compacted), which name no record: they stay out, as they should.

- **Fixed: the errors.** The file that kept WattzLaserGun out kept its failure on record in the next round, although that round rewrote it for BoSStory and F4NVServiceRifleRedux; the final check (a compiled plugin whose file failed is an Error) then stopped the export. A failure of an earlier round now stays on record only while none of the file's plugins compiles; otherwise it is a note. The final check is unchanged.
- **Fixed: constants matched by function name alone.** Nearly every mod in a family has its own `Program.GetPluginName()` (KARM, MUTA, RPTN, NSPA, KOPopulationControl, SimSettlementsV2), and a call of any of them counted as a read of all of them, so one Sim Settlements script reading its own name ("292 reads") kept six unrelated plugins out. Each read now records the script type it is read from (the receiver's declared type, the script for `self`, the type of a static call), and counts for a constant of script X only when that type is X, extends X, or is extended by X (inheritance across the load order). A read whose type is unknown, or names a script not in the load order, still counts.


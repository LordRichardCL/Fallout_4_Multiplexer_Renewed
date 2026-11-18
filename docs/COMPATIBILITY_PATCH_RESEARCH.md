# Compatibility patch analysis: research report and design (phase 1)

This answers the specification in `docs/specs/COMPATIBILITY_PATCH_ANALYSIS.txt`, section 24 ("first response"),
and covers the deliverables of its section 21. Nothing in the code has changed for it. Written 18 Nov 2026.

Sources are cited as **[S1]**…**[S12]** (section 3). Where a statement rests on this project's own earlier
findings, the audit document is named. Where something could not be verified, it says so.

---

## 1. Summary of the task

The goal is an analyser that follows every version of a record in load order. It establishes:
- what each plugin changed against the version it was made from;
- which of those changes reach the winning version and which are lost, and where;
- which lost changes could coexist and which compete;
- what can be proposed as a patch, with an audit trail for every value.

Detection, compatibility analysis, patch planning and writing stay separate layers. Automatic resolution
happens only where a documented, tested handler exists. Everything else is reported for review. Unknown
structures are detection-only.

The Combat Rifle and laser examples describe behaviour, not the Fallout 4 format. Section 6 shows what a
"laser attachment" really is in Fallout 4, and how far that changes the picture.

---

## 2. What Multiplexer already has (architecture assessment)

| Question | Today | Reusable for this work |
|---|---|---|
| Parsing plugins | `PluginFile`/`Record` read every record and subrecord, including compressed ones and groups. `PluginScanner` indexes them. | Yes, unchanged. |
| Record identity | `FormKey {origin plugin, object ID}`: the defining plugin and the object ID, independent of any plugin's master numbering (`docs/ARCHITECTURE.md`, "Identity: FormKey"). Light (ESL) plugins are handled when FormIDs are resolved (`Session::resolve`). An index past a plugin's master list is read as xEdit reads it (the plugin itself). | Yes: this is the "record identity" of the specification. |
| Masters and load order | `Session` holds the load order, each plugin's masters, and for each record the **override chain** (`Session::instances(group)`), in load order, the defining version first. | Yes: the chain of section 13 stage 2. |
| "Made against" | `Session::overrideParents(chain)` gives, for each version, the latest earlier version from a plugin it depends on, i.e. the version it was authored against. | Yes. This is xEdit's "CurrentMaster" [S2, 5.10] and is central to telling removals from omissions (section 8). |
| Normalising | `Normalizer` turns a record into `NormSub`s. FormIDs become `FormKey`s (raw value kept for evidence). Localized strings become text. Undescribed bytes are kept raw and marked opaque. Absent and empty subrecords stay distinct. | Yes: section 13 stage 3. |
| Field model | `splitFields` cuts a record into `Field`s: scalar, region (a run compared as one), packed array (`KWDA`), or repeated list (`LVLO`+`COED`), each with a stable key. `joinFields` writes them back in order. `RecordDefinitions` gives each subrecord a `FieldKind`, FormID offsets and a `MergeMode` (`Scalar`, `Set`, `Multiset`, `Keyed`). | Partly. These are the element paths and element identities, but only at field and list-item level, not member by member inside a struct (`DATA` of a weapon is one value). |
| Comparing | `MergeEngine::analyse/merge` compare each version with the version it was made against (via the lineage), collect the changers of every field, flag "hard" conflicts (two changers disagree), and merge lists by additions and removals. `ConflictEngine` turns this into the conflict report (Type 1 override, Type 2 compatible merge, Type 3 hard conflict, record conflicts for winner-only types, ITMs). | Yes as the detection core. Missing: per change provenance (who introduced it, who kept it, where it disappeared). |
| Subrecord structure | Kept: fields keep their subrecords. The written order follows the source, and fields a version lacks are placed in xEdit's order (`SubrecordOrder.inc`, generated from xEdit's definitions). | Yes. |
| Writing | `PluginWriter` writes valid plugins with nested groups. The compiler and the existing patch (`Multiplexer_Patch.esp`) write overrides with master lists built from the references they hold. After export a **written-content check** re-reads every output and compares it with the plan. | Yes: section 13 stages 9 and 10 exist in this form. |
| Patch today | `Multiplexer_Patch.esp` (option "Build patch") merges chains made only of plugins that stay, for record types whose fields merge, automatically (last changer wins on a scalar). It is an automated merged patch in the sense of [S2, 5.7/5.9], not a reviewed compatibility patch. | The writing path, yes. Its policy differs from this specification (section 9). |

**Missing for the specification:**
1. A provenance trace per change (introduced, preserved, replaced, disappeared, survives).
2. Element identity inside structs, and per-structure ordering rules taken from xEdit (section 6).
3. A status vocabulary that separates "absent from a plugin that never saw the change" from "removed by a plugin made against it".
4. A conservative classification that withholds automatic choice for competing scalars (today the last changer wins).
5. A reviewable patch plan with user decisions, kept apart from the writer.

---

## 3. Research report

### 3.1 Sources consulted

| # | Source | Type | Topic | Key finding | Effect on the design | Reliability | Remaining uncertainty |
|---|---|---|---|---|---|---|---|
| S1 | xEdit source `Core/wbConflict.pas`, `wbInterface.pas` (dev-4.1.6) | Primary source code | Conflict detection | Each version is compared with the master (first) and with the last version, giving per-version states: Identical to Master, Conflict Winner/Loser, Override, Benign, Ignored, Hidden. A record's overall state is one of Only One, No Conflict, Benign, Override, Conflict, Critical. Each element has a priority (`cpIgnore`, `cpBenignIfAdded`, `cpBenign`, `cpOverride`, `cpTranslate`, `cpNormal`, `cpNormalIgnoreEmpty`, `cpCritical`, `cpFormID`). An element added where the master had none, and not overridden later, is benign (`cpBenignIfAdded`). Injected records raise normal conflicts to critical. Arrays marked sorted are aligned **by sort key**, duplicates numbered (`key<0001>`); other arrays by index, or by key alignment when the "align array elements" option is on. | Element identity for sorted arrays is the sort key; for unsorted ones index plus optional alignment. Field priorities let "Ignored"/"Benign" fields be skipped. The xEdit display is a master-and-winner comparison; it does not trace changes through the middle of the chain, which the specification asks for. | High (it is what xEdit does). | The UI layer (`xeMainForm.pas`) was not read; only the core algorithm. |
| S2 | Tome of xEdit, chapter 5 "Conflict Detection and Resolution" (tes5edit.github.io/docs/5-conflict-detection-and-resolution.html) | Official documentation | Overrides vs conflicts; patch plugins; merged patch; list-merge algorithm | "To get a conflict you need to have the same FormID defined in at least 3 modules, a master, and 2 overrides." "It's perfectly possible for 10 modules to override the same record without a single conflict, as long as the last loaded version of that record includes all the changes." Manual patch: "Copy as override" into a new file that loads last, which adds the source plugin as a master; then drag in each wanted value. "Create Merged Patch": "**This function is obsolete and not supported.**" It covered LVLI, LVLC, LVLN, LVSP, CONT, FACT, RACE, FLST, CREA, NPC_ (plus DIAL in FNV, keywords of some types in Skyrim). Section 5.10 gives the list algorithm: for each override, compare with the version in its own latest master ("CurrentMaster"); entries only in the override are added, entries only in that master are removed; the result is compared with the winner's list. Leveled list entries are compared as strings of level, reference, count, condition, owner and rank, sorted, duplicates numbered (A#0, B#0, B#1). | Confirms: a change is what an override differs from the version it was made against. A removal is an entry present in that version and absent from the override. Duplicates in leveled lists are counted, not collapsed. The automatic merged patch is not the model to copy; manual "copy as override, then carry values" is. | High for definitions; the chapter is written around Fallout 3 screenshots. | Section 5.11 "Records That Merge At Runtime" only links to an afkmods thread about **Skyrim** (S11), unreadable here. |
| S3 | xEdit `wbDefinitionsFO4.pas` / `wbDefinitionsCommon.pas` (dev-4.1.6) | Primary source code | Fallout 4 record layouts | Which arrays xEdit sorts (`wbArrayS`/`wbRArrayS`, sort keys) and which it keeps in order (section 6). OMOD layout (Attach Point keyword, Attach Parent Slots, Includes "should not be sorted", properties with SET/ADD/REM functions). Weapon `APPR` "Attach Parent Slots" is a sorted keyword array. Object templates (`OBTE`, combinations `OBTS` with includes and properties). LVLI entries keyed by level and reference, with COED. `FLST` is **not** sorted ("Should not be sorted according to Arthmoor and JustinOther"). Conditions (`CTDA`) form an unsorted list. | Ordering and identity rules per structure (section 6), taken from the definitions instead of guessed. Multiplexer already generates its layouts and subrecord order from these files. | High for layout; xEdit's sortedness is an editor convention that matches engine behaviour as far as its authors know. | Sortedness says order is irrelevant to the editor, not that the engine ignores duplicates or order in every case. |
| S4 | Mator Smash source (`msAlgorithm.pas`, `msSmash.pas`, github.com/matortheeternal/smash) | Source code of an automated patcher | Element-by-element merging | Three-way merge of (override, master, patch) per element. Arrays: sorted ones keyed by sort key (and recursed into), unsorted ones keyed by the whole value (so a changed entry reads as remove plus add). Deletions are carried only when a "deletions" setting allows it; elements the master had are re-added only with "override". "Single entity" copies a whole element when anything in it changed. For Fallout 4, deletions are turned off for partial-form QUST/LCTN records. README: verify the generated patch in xEdit; it "can in certain rare cases lead to crashes/unintended behavior in game". | Supports per-path settings (a handler registry), "single entity" for structures that must move as a whole, and deletions as an opt-in rather than a default. | Medium: working code, no written specification. | UI meaning of the setting keys is inferred from field names. |
| S5 | Wrye Bash source, Fallout 4 game support and list mergers (github.com/wrye-bash/wrye-bash, `dev` branch, `Mopy/bash/game/fallout4/__init__.py`, `Mopy/bash/patcher/patchers/mergers.py`, `Mopy/bash/brec/common_records.py`) | Source code of the established Bashed Patch | Tag-driven patching for Fallout 4 | Fallout 4 patchers: AliasPluginNames, ImportActors, ImportActorsAIPackages, ImportActorsFactions, ImportActorsPerks, ImportActorsSpells, ImportDestructible, ImportEffectStats, ImportEnchantments, ImportEnchantmentStats, ImportInventory, ImportKeywords, ImportNames, ImportObjectBounds, ImportOutfits, ImportRelations, LeveledLists, TimescaleChecker, TweakActors, TweakSettings. Leveled lists: every plugin's **additions** are merged; entries a plugin lacks are removed **only if the plugin carries the Delev tag** (entries in its masters' versions minus its own); Relev copies the list's level settings. A merged list is written only when it differs from the winner (`mergeOverLast`). | The established default is: additions merge, absences do not remove unless there is explicit evidence (a tag set by the author or user). Field groups are imported from the plugin tagged for them, so the source of each value is chosen per group, not guessed. | High for behaviour (it is the code). | Wrye Bash's written docs were not read (an agent is still reading them; see section 12). |
| S6 | z-edit/zedit repository README | Source repository | zEdit merge/patch | README covers development only; UPF/zMerge/zPatch logic was not found in the folders read. | None. | Low (not reached). | Documentation site was unreachable. |
| S7 | Fallout 4 Creation Kit wiki (falloutck.uesp.net) | Official-adjacent documentation | OMOD, COBJ, LVLI, attach points | **Not read**: the site refuses this environment (DNS, then 403). | Facts about attachments come from S3 instead and are marked as such. | n/a | Workbench and attach-point behaviour in game is described from record layouts only. |
| S8 | `docs/RECORD_HANDLING_AUDIT.md` (this project) | Own verified findings | NAVI, cells | The game combines every plugin's navmesh info map (NAVI), one entry per navmesh, so NAVI is merged per navmesh, not taken from the winner. Cells: precombine/previs lists inherit across versions (PJM's rules), partial cells (flag 0x4000) change nothing of those lists. | NAVI and cell lists are already runtime-combined structures with their own handlers. | Medium-high (checked on the owner's load order). | No general list of Fallout 4 runtime merges was found. |
| S9 | `docs/ZEDIT_REVIEW.md` (this project) | Earlier review | zEdit merging of whole plugins | zMerge combines whole plugins and relinks their FormIDs; it is the plugin-merge case of section 1 of the specification. | Confirms plugin merging is a different problem from compatibility patching. | Medium. | — |
| S10 | `docs/specs/COMPATIBILITY_PATCH_ANALYSIS.txt` | Owner's specification | Requirements | — | The requirements this answers. | — | — |
| S11 | afkmods "TES5Edit – Records that merge at runtime" (linked from S2 5.11) | Community, Skyrim | Runtime merges | **Not read** (blocked). It concerns Skyrim; the specification forbids applying it to Fallout 4 unchecked. | Fallout 4 runtime merges are treated as unknown except where verified (S8). | n/a | Open (section 11). |
| S12 | RobCo Patcher (F4SE, already supported by Multiplexer) | Established tool, known from this project | Runtime list/keyword patching via INI | Edits leveled lists, keywords and other forms at game start from INI files, which the game does not do by itself. | Its existence implies the engine does not merge leveled lists across plugins; treated as supporting evidence, not proof. | Medium. | Inference only. |

### 3.2 What the sources establish, by design question

- **What a change is.** A change is what a version differs from the version its author built it against (S2 5.10, "CurrentMaster"; S5 masters; Multiplexer's `overrideParents`). Comparing only master and winner misses changes made and then lost in the middle of the chain. xEdit's own display (S1) has the same blind spot, so the specification's section 5 goes further than xEdit.
- **Removal vs omission.** No source infers intent from a difference. Two of them settle when an absence may be treated as a removal:
  - S2 5.10: an entry is removed when it is in the plugin's own master's version and absent from the plugin.
  - S5: an absence removes only with an explicit Delev tag.

  Multiplexer can tell, from the master lists, whether a later plugin **could have seen** an earlier change. If ModC does not have ModB as a master, the laser was never in ModC's starting point, so its absence is an *omission*, not a removal. That distinction rests on evidence (section 8).
- **Ordering.** xEdit's sorted arrays (S3, S1) have no meaningful order; reordering them is not a change. Unsorted ones (FLST, conditions, OMOD includes) keep their order, and their identity must not be collapsed.
- **Duplicates.** Leveled list entries may repeat, and repeats count (S2 5.10 numbers them; Multiplexer's `Multiset`).
- **Patch style.** Manual patching is copy the winner as an override, then carry chosen values into it, adding the masters they need (S2 5.8). Automated patchers either merge a few list types (S2 5.9, S5) or apply per-path rules with opt-in deletions (S4). None of them claims safety; all ask for review.

---

## 4. Terminology

These are the terms used from here on. They follow xEdit's vocabulary (S1, S2) and the names already in the code.

| Term | Meaning |
|---|---|
| **Version** | One plugin's copy of a record (xEdit: a record in a module). `RecordInstance` in the code. |
| **Origin (defining) version** | The version in the plugin that created the FormID (`chain[0]`; xEdit "master"). |
| **Override** | Any later version of the record. Not a problem by itself (S2 5.2). |
| **Override chain** | All versions in load order, origin first. |
| **Winning version** | The last version in load order: what the game uses (S2 5.1). |
| **Parent version** | For each override, the latest earlier version in a plugin it masters (`overrideParents`; S2 "CurrentMaster"). The version it was made against. |
| **Change** | A difference between a version and its parent version at one element path. Attributed to that version's plugin. |
| **Conflict** | Two or more overrides with different changes at a record, so that last-wins drops something (S2 5.2: at least three versions). |
| **Lost change** | A change whose value or element is not in the winning version. |
| **Preserved change** | A change a later version still contains. |
| **Competing changes** | Two changes to the same element with different values. |
| **Revert** | A later version holds the origin's value again at a path an earlier version changed. |
| **Removal** | An element present in a version's parent version and absent from the version itself. |
| **Omission** | An element added by a version that a later version's parent chain does not contain, so the later version never had it to remove. |
| **Runtime merge** | Data the engine combines across plugins while loading, so the winner alone does not decide it (NAVI, S8). |
| **Change provenance** | For each change: where it was introduced, kept, replaced, removed or omitted, and whether it survives. |
| **Compatibility patch** | A plugin loading last that carries selected changes into a new winning version. Each value is traceable to its source and approved per rule or by the user. |
| **Merged patch** | An automatic patch that merges chosen record types by fixed rules (xEdit's obsolete one, S2 5.9; Multiplexer_Patch.esp today). |
| **Bashed/smashed patch** | A merged patch driven by tags (S5) or by per-path settings (S4). |
| **Plugin merge** | Combining whole plugins into one file, with their FormIDs relinked (zMerge, S9). Not this task. |
| **Patch plan** | The list of resolution entries (path, chosen value, source, reason, masters, class, decision) the writer receives. |

---

## 5. Parts of the specification that differ from verified behaviour

1. **"Attachments" are not a list on the weapon (section 4 of the specification).** In Fallout 4 an attachment is mostly its own records (S3):
   - an **OMOD** that names the *Attach Point* keyword it fills and the *Attach Parent Slots* it opens;
   - a **COBJ** whose Created Object is the OMOD and whose Workbench Keyword makes it craftable;
   - often a loose-mod MISC record.

   The weapon itself holds, among others:
   - `APPR` Attach Parent Slots (a **sorted keyword set**);
   - `KWDA` keywords (a sorted set);
   - the object template (`OBTE` count, then combinations of `OBTF`/`FULL`/`OBTS`, ended by `STOP`), whose `OBTS` entries list default mods ("Includes") and property changes.

   A "lost laser" in a weapon override therefore shows up as one of these:
   - a missing `APPR` keyword (the slot no longer exists, so nothing attaches there);
   - a missing keyword in `KWDA`;
   - a missing include in an `OBTS` combination.

   The OMOD and COBJ that ModB **adds** are new records and cannot be lost to ModC's override. Their *effect* can be lost, because nothing offers the slot. That needs cross-record analysis (section 7, "dependency").
2. **"Never choose a winner for competing scalars" (section 10)** describes a reviewed compatibility patch. Multiplexer's compiled outputs follow the game: the last plugin wins, and only non-competing changes are merged. That stays, because compiling must not change what the game does. The new analyser will report competition and withhold a choice in the *compatibility patch plan* only. The existing Multiplexer_Patch.esp is a merged patch with fixed rules (section 9). It should be listed in the report as such, and later offered as a reviewed plan.
3. **Test 6 (order change)** has no change at all for xEdit's sorted arrays (KWDA, APPR, FVPA, CNTO, factions, perks, leveled list entries): their order is not data. For FLST, conditions and OMOD includes the order is data and must be reported.
4. **Records that merge at runtime (section 12):** xEdit's documentation has no Fallout 4 list (S2 5.11 only links a Skyrim thread). The one Fallout 4 case verified here is NAVI (S8). Leveled lists do **not** merge at runtime, as far as the evidence goes (S5, S12). The policy (section 9.3) treats everything else as "no runtime merge known" and keeps the analysis conservative.
5. **"RECORD_DISABLED"** applies to placed references (the Initially Disabled flag, or an enable parent), not to base records. It belongs to the placed-record work (plan item 1), which keeps those types as Candidates.

---

## 6. Fallout 4 structures to investigate (from xEdit's definitions, S3)

| Structure | Where | xEdit ordering | Identity | Duplicates | Multiplexer today |
|---|---|---|---|---|---|
| Keywords `KSIZ`/`KWDA` | most items | sorted (`wbArrayS`) | keyword FormID | no meaning | `Set` |
| Attach Parent Slots `APPR` | WEAP, ARMO, NPC_… | sorted | keyword FormID | no | `Set` |
| Object template `OBTE`…`STOP` | WEAP, ARMO, NPC_ | combinations kept in order; each `OBTS` holds an *unsorted* include list and properties | combination: position plus name (`FULL`); include: OMOD FormID + attach point index | possible | one region (Scalar) |
| OMOD `DATA` | OMOD | includes "should not be sorted"; properties counted | include: OMOD FormID; property: property id + function | possible | `OmodData` kind, Scalar |
| OMOD `MNAM`/`FNAM` | OMOD | unsorted arrays | keyword FormID | ? | Scalar |
| Components `FVPA` | COBJ, MISC… | sorted | component FormID | no | `Keyed` (by component) |
| Conditions `CTDA` (+ `CIS1/2`) | COBJ, PERK, INFO, PACK, MGEF… | **unsorted** list; order matters (OR chains) | none stable: position + content | yes | one region, compared whole |
| Leveled entries `LVLO`(+`COED`) | LVLI, LVLN, LVSP | sorted by level, keyed level+reference | (level, reference, count, COED) with duplicate counter | **yes, meaningful** | `Multiset` |
| Leveled list header `LVLD`, `LVLF`, `LVLG` | LVLI… | scalar | — | — | Scalar |
| Container items `CNTO`(+`COED`) | CONT, NPC_ | sorted | item + count (+ COED) | repeats kept | `Multiset` |
| Factions `SNAM` | NPC_ | sorted | faction FormID | no | `Keyed` |
| Perks `PRKR` | NPC_ | sorted | perk FormID | no | `Keyed` |
| FormID list `LNAM` | FLST | **not sorted** (order used by scripts) | position + FormID | yes | `Set` (to be reviewed: order matters) |
| Faction relations `XNAM` | FACT | sorted | faction FormID | no | `Keyed` |
| Scripts `VMAD` | many | scripts sorted by name; properties by name | script name, property name | no | `Vmad` kind, Scalar (opaque to merging) |
| Weapon `DNAM`/`DATA` | WEAP | struct | member offset | — | Scalar (one value for the whole struct) |
| Record flags | all | flags | bit | — | `@flags` Scalar |
| Cells, worldspaces | CELL, WRLD | — | — | — | own handlers (configuration merge, inheritance) |
| Navmesh info `NAVI` | NAVI | runtime-combined per navmesh | navmesh FormID | — | own handler (S8) |

Open points in this table:
- **FLST** is a `Set` today, but xEdit keeps it in order (S3). The provenance layer must treat it as an ordered list. Whether the existing merge should change is a separate decision (section 11).
- **Struct members:** scalar changes inside one struct (damage and weight both sit in weapon `DNAM`) are one Field today. Telling them apart needs the value layer (`ValueMember`, already generated from xEdit for display). The "Damage / Weight / Value" example depends on that.

---

## 7. Proposed provenance data model

The model builds on the existing objects (`FieldSet`, `Field`, `ListItem`, `ChainAnalysis`) and keeps their names.

```
RecordProvenance
  key            FormKey, signature, editor ID
  chain          [VersionRef]          // load order; VersionRef = {plugin, loadOrder, parentIndex, flags}
  winner         index into chain
  elements       [ElementTrace]
  runtimeMerged  bool                  // handled by a runtime-merge handler (NAVI…)

ElementPath      { fieldKey, member?, itemIdentity? }
                 // fieldKey: the existing Field key ("DATA#0", "list:LVLO", "KWDA#0")
                 // member:   value-layer member name inside a struct ("Damage")
                 // itemIdentity: per structure rule (section 6): keyword FormID,
                 //               (level, ref, count)#n, OMOD FormID + index, position for ordered lists

ElementTrace
  path           ElementPath
  originValue    Value | Absent
  winnerValue    Value | Absent
  changes        [ChangeEvent]         // one per version that changed this path, in load order
  handler        HandlerId             // which rule set judged it (Unknown = detection only)

ChangeEvent
  version        chain index
  operation      Set | Add | Remove | Reorder | Unknown     // see section 8
  before, after  Value | Absent         // parent version's and this version's value
  status         Survives | Replaced | Reverted | RemovedLater | OmittedLater | Duplicated | Unknown
  keptBy         [chain index]          // later versions that still hold it
  lostAt         chain index?           // first later version without it
  lostHow        Replaced | Reverted | Removed | Omitted | Unknown
  references     [FormKey]              // records the value names (for masters and validation)

Value             normalized bytes + FormKeys (NormSub / ListItem), never re-encoded text

CompatibilityFinding        (layer 2)
  path, events   the ChangeEvents involved
  kind           Independent | Competing | Dependent | Ambiguous | Complex | Unsupported
  class          Green | Yellow | Red | Unknown
  evidence       [text]                 // rule ids and source references (S1…S12)

PatchPlan                   (layer 3)
  record         FormKey
  base           chain index the patch starts from (normally the winner)
  entries        [Resolution]
Resolution
  path, chosenValue, source (chain index), operation (KeepWinner | CarryForward | UserValue | Exclude)
  reason, class, requiredMasters [plugin], warnings [text], decision (Auto | User | Pending)

ValidationResult            (layer 4, before writing; then the written-content check after)
  per Resolution: references resolve, masters available, record type writable, serialisable
```

The data stays in memory per record and is built lazily for the records a report shows, as the conflict
report does today. Values are the normalized forms, so raw evidence (`rawRefs`, opaque spans) is kept for
debugging.

---

## 8. Classification

### 8.1 Change status (detection layer, observable facts only)

| Status | Rule (all relative to versions and their parent versions) |
|---|---|
| **Survives** | The winner holds the change's value at the path. |
| **Replaced** | A later version whose parent chain includes the change has another non-origin value there. |
| **Reverted** | A later version whose parent chain includes the change holds the origin's value there. |
| **Removed later** | Element added by the change; a later version whose parent chain **includes** it lacks it. The text says "X no longer contains it"; intent is not claimed. |
| **Omitted later** | Element added by the change; a later version whose parent chain **does not include** the changing plugin lacks it. That version never had it: last-wins drops it, nobody removed it. This is the usual "lost" case. |
| **Preserved by** | The later versions that still hold it (listed). |
| **Duplicated** | The same element added again by another version (identity equal), counted only where duplicates are legal (leveled lists). |
| **Unknown** | The path is opaque (undescribed bytes, unknown kind) or the structure has no identity rule. |

"Lost" is the union of Replaced, Reverted, Removed later and Omitted later. The specification's
"ABSENT_FROM_LATER_OVERRIDE" splits into the last two, using master lists as evidence.

### 8.2 Compatibility classes (analysis layer)

| Class | Meaning | Examples |
|---|---|---|
| **Green** | Matches a documented low-risk rule, all validation passed. Not a guarantee. | A lost change at a path no other version changed (different struct members, different scalar fields); an element **omitted** from a sorted set (KWDA, APPR) by a version that never had it; a leveled-list addition omitted later (S2 5.10, S5 default). |
| **Yellow** | Review required. | Two versions change the same path to different values (competing); a **removal** by a version made against the adder; a revert to the origin's value; ordered lists (FLST, conditions, OMOD includes) changed by more than one version; an addition whose values reference a plugin the patch would have to master; template or flag changes. |
| **Red** | Detected, never merged automatically. | VMAD, conditions changed by several versions, object template combinations, OMOD property lists, packages, quests, dialogue, scenes, navmesh, placed references, cell previs/precombine data, deleted records, injected records (S1 raises them to critical), opaque bytes. |
| **Unknown** | No handler for the path. Detection only. | Any path not covered by the handler registry. |

### 8.3 Runtime-merge and whole-record policy

| Category | Policy |
|---|---|
| Runtime-combined by the engine (verified: NAVI, S8) | Not reported as lost; own handler. |
| Cell precombine/previs lists (PJM's inheritance, S8) | Own handler (cell inheritance); never patched by this analyser. |
| Records whose fields merge in Multiplexer (`Specialised`) | Provenance per field, classes as above. |
| Winner-only types (`Winner`) | Provenance per field for the report. Patch entries Red unless a handler is added. |
| Types without a verified layout (Candidates) | Unknown. |
| Behaviour known only for other games (S11) | Not applied. |

### 8.4 Handler registry

Each handler declares:
- signatures and paths;
- identity rule and ordering rule;
- allowed operations;
- class rules;
- validation;
- whether it may merge automatically;
- its sources and its test fixtures.

The first entries map onto existing `MergeMode`s: `Scalar` paths, `Set` (sorted sets), `Multiset` (leveled entries) and `Keyed` (factions, perks, relations). New entries come only with sources and tests:
- `OrderedList` (FLST, conditions);
- `StructMembers` (value-layer members);
- `ObjectTemplate`;
- `AttachSlots` (APPR, with the OMOD/COBJ dependency check).

---

## 9. Process

```
Plugins
  → Load and validate             (PluginScanner, Session: masters, ESL, load order)        exists
  → Build override chains         (Session::instances, overrideParents)                     exists
  → Normalise records             (Normalizer → NormRecord, splitFields → FieldSet)          exists
  → Compare each version with     (MergeEngine::analyse, per field; extended per element)   extend
    its parent version
  → Trace provenance              (new: ElementTrace / ChangeEvent per path)                 new
  → Survives / lost, how          (new: status rules 8.1)                                    new
  → Classify compatibility        (new: handler registry, classes 8.2)                       new
  → Build reviewable patch plan   (new: PatchPlan, user decisions)                           new
  → Validate dependencies         (reuse: reference/master checks of the compiler)           reuse
  → Write approved patch          (reuse: PluginWriter, patch output)                        reuse
  → Reopen and verify             (reuse: written-content check, compared with the plan)     reuse
```

---

## 10. Safety matrix (what exists today)

| Category | Detection | Provenance | Automatic merge | Manual selection | Writing | Validation | Limitations | Sources |
|---|---|---|---|---|---|---|---|---|
| Scalar fields of `Specialised` types | yes (per field) | no (changers only) | yes, last changer wins on competition | forced winner per record only | yes | written-content check | struct members not separated | S1, S2 |
| Sorted sets (KWDA, APPR…) | yes | no | yes (`Set`) | no | yes | yes | — | S3 |
| Leveled entries | yes | no | yes (`Multiset`) | no | yes | yes | — | S2 5.10, S5 |
| Keyed lists (factions, perks, relations) | yes | no | yes (`Keyed`) | no | yes | yes | duplicate-entry bug fixed 17 Nov 2026 | S3 |
| FLST | yes | no | yes (`Set`) | no | yes | yes | order not preserved as data (6) | S3 |
| Conditions, VMAD, object templates, OMOD data | yes (whole field) | no | whole value, last changer | no | yes | yes | not split into elements | S3 |
| Winner-only types | yes | no | no (winner) | forced winner | yes | yes | — | — |
| Cells / worldspaces | yes | partial (configuration merge reports sources) | configuration merge (opt-in) | no | yes | yes + previs checks | own rules | S8 |
| NAVI | yes | no | runtime-style combine | no | yes | yes | — | S8 |
| Candidates / opaque bytes | yes | no | no | no | no | — | — | — |

---

## 11. Major unknowns before patch writing

1. **Runtime merges in Fallout 4.** NAVI is verified. Nothing else is, and the xEdit list (S11) is for Skyrim. Leveled lists, FLST and keywords are taken as not merged at runtime (S5, S12), which still needs an in-game check before any "no false lost-change" rule is encoded.
2. **Attachments in game.**
   - Does a weapon accept an OMOD whose attach point it does not list in `APPR`, through an OMOD that opens the slot (OMOD Attach Parent Slots)?
   - How do OMOD `ADD`/`REM` keyword properties combine with the weapon's own keywords?

   The CK wiki (S7) was unreachable, so this needs the wiki or an in-game check.
3. **Ordered lists.** Whether FLST order matters to the engine in general or only to scripts that index it. xEdit's comment cites authors, not the engine. This affects whether the existing `Set` merge of FLST should change.
4. **Conditions.** How OR/AND grouping across merged conditions behaves. Until verified, condition lists are Red.
5. **Struct members.** The value layer covers named members for most types (generated from xEdit). Whether every member used for a "different member" Green rule is complete and correctly sized is checked by `valuesComplete`; members of incomplete layouts stay Unknown.
6. **Removal intent.** Even a removal by a version made against the adder can be accidental. The plan keeps it Yellow.
7. **Wrye Bash written docs and zEdit docs** were not read (S5 code was; S6 unreachable).

---

## 12. Smallest sensible milestone, tests and files

**Status:** milestone 1 is implemented (18 Nov 2026): `engine/Provenance`, the trace on conflict entries (cached), the Conflict Report table "Changes not in the winning version", and the JSON report's `changes`. Tests are in `tests/test_provenance.cpp` (1–9, 11, 12 and keyed competition) and `conflict_report_traces_changes_not_in_the_winner` (10, with the cached second run).

**Phase 7 (reviewable patch plan)** is implemented (18 Nov 2026): `engine/PatchPlan`. It covers, per record with changes the winner lacks, starting from the winner:
- **Entries:** one per change, with the action (carry forward or keep the winner's value) and how it was decided:
  - Green: carried by default;
  - Yellow: waiting for the user, kept as the winner has it until then;
  - Red/Unknown: never carried, and a saved choice to carry is ignored.
- **Masters** the carried changes need beyond the winner's.
- **Pre-write validation:** two carried changes giving the same field or element different values block the record, as does exceeding the master limit.
- **Status:** Ready, Needs decision, Blocked, or Nothing to carry.

Choices are saved by record (`Plugin.esp:objectID` of the defining plugin) and a stable change id (a hash of field, operation, source plugin and value, independent of load order), in `patch_plan_decisions.json` next to the database. They apply again on later scans.

The plan appears as a section of the Conflict Report and as `patchPlan` in its JSON, and through the CLI (`fo4mx plan`, with `carry|keep|default <record> <change id>`). Test: `patch_plan_defaults_decisions_and_validation`.

Nothing is written from the plan yet. Writing is phase 8/9: carrying changes into a patch record, with the existing writer and the written-content check against the plan.

**Phase 6 (compatibility classes)** is implemented too (18 Nov 2026): `engine/Compatibility` (`classifyChange`), applied to every traced change, counted per class in the Conflict Report and given per row with its rule and the masters a patch would need. Rules, first match wins:
1. **Red:** undescribed data; a record type with spatial, quest, dialogue or package structure (QUST, DIAL, INFO, SCEN, PACK, NAVM, NAVI, CELL, WRLD, placed references…); any version deleted; a field holding scripts, conditions, an object template, OMOD data, perk entries, navmesh, package or dialogue data; a reference to a record not in the load order.
2. **Unknown:** a record type without a field-by-field layout.
3. **Yellow:**
   - another plugin changed it differently (competing);
   - a later plugin made with it as a master differs (removed, replaced, reverted);
   - record flags;
   - ordered lists (FLST);
   - a removal;
   - a reference to a plugin outside the record's chain that the winner does not master (dependent).
4. **Green:** a field no other plugin changed, an element of a set, an entry of a list that keeps repeats, or a keyed element no other plugin changed, omitted by a winner not made against it.

The masters a patch would need beyond the winner's are listed in every case. Tests: `compatibility_classes_for_lost_changes` and `compatibility_dependencies_and_structures`.

**Milestone 1 (phases 2–5 of the specification, detection only):** provenance for the field paths and list
elements Multiplexer already understands, shown in the conflict report. No patch plan, no writing.

- For each record the conflict report lists, build `RecordProvenance` from the chain, parent versions and the existing field model.
- Paths:
  - scalar Fields (whole values);
  - `Set`/`Keyed`/`Multiset` list items, with identity per section 6 (FLST as an ordered list in provenance).
- Statuses from section 8.1, with the omitted/removed distinction from master lists.
- Report text names facts only ("ModC.esp does not contain the keyword ModB.esp added; ModC.esp does not have ModB.esp as a master").

**Tests (unit tests on FieldSet chains, as in `tests/test_merge.cpp`, plus one plugin-level fixture):**
1. Independent scalars (three different fields): three changes; two lost (omitted), one survives.
2. Same field, two values: competing; no choice made.
3. Keyword added by ModB, ModC without ModB as master: omitted later, lost.
4. The same with ModC mastering ModB and keeping it: preserved by ModC, survives.
5. ModC masters ModB and lacks the keyword: removed later (Yellow), wording without intent.
6. Reordered KWDA: no change. Reordered FLST: an order change reported.
7. Leveled list with duplicates: counts kept, duplicate added by two plugins reported as such.
8. Master and winner identical, middle version added an element: lost change still found.
9. Opaque field difference: Unknown, no merge.
10. Plugin-level: three plugins on disk through `Pipeline`, report lines checked.

**Files likely to change:**
- new `src/core/engine/Provenance.hpp/.cpp` (model and tracing);
- `src/core/engine/ConflictEngine.*` (attach provenance to entries);
- `src/core/engine/FieldModel.*` (item identity helpers);
- `src/core/esp/RecordDefinitions.*` (an ordering flag per list, from xEdit: sorted or ordered);
- the report writers (`CompilationResult`/report export, GUI conflict page later);
- `tests/test_provenance.cpp`, `tests/CMakeLists.txt`;
- `docs/ARCHITECTURE.md`.

**Later phases (as in the specification, each with sources and tests):**
- 6: compatibility classes;
- 7: patch plan with user decisions;
- 8: one handler, keyword sets (KWDA/APPR) or leveled lists, which have the strongest sources;
- 9: writing through the existing patch writer and verification against the plan;
- 10: further handlers (struct members, ordered lists, object templates), each after its unknowns in section 11 are settled.

---

## 13. Research still open

- **Fallout 4 CK wiki pages** (Mod/OMOD, Weapon, Constructible Object, Leveled Item): the host refuses this environment.
- **afkmods "records that merge at runtime" thread** (Skyrim; context only).
- **zEdit documentation site.**
- **Wrye Bash written documentation** (the code was read).

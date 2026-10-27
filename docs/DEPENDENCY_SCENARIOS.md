# Dependency and Compilation Scenarios

How the compilation engine should behave when plugins depend on each other and not all of them can be compiled. Each scenario gives the answers, then the behaviour broken down by: what happens, why, risks, architecture, dependency handling, record treatment, final load order and xEdit compatibility. Each one also notes what the engine does today (`DependencyEngine::evaluateEligibility`, `CompilationEngine::compile`) and what should change.

## Ground rules

Every scenario follows from these facts about the Creation Engine.

1. **A FormID belongs to the file that created it.** FormID `0x05001234` means "object `0x001234` of the 6th master". Fallout 4 has no aliasing and no redirection. When a record moves into `Dummy_Plugin_XX.esp` it is a *new record with a new FormID*, and the old FormID stops existing once the original plugin is disabled.
2. **A plugin cannot load without its masters.** A plugin whose master is missing or disabled stops the game at startup. If a plugin stays in the load order, every one of its masters stays too.
3. **Overrides follow the last loaded plugin.** Whatever loads last for a given FormID wins, record by record.
4. **Original mod files are never edited.** The tool reads plugins and writes new files. It never changes, cleans or re-masters someone else's plugin. Uninstalling the output and re-enabling the originals must always restore the original setup exactly.
5. **Compiled FormIDs are a save-game contract.** Saves store FormIDs. Changing a compiled record's FormID between builds breaks saves that reference it. `Multiplexer_FormMap.tsv` exists to keep them stable.

The vocabulary used below:

| Term | Meaning |
|---|---|
| **Compiled** | The plugin's records are emitted into generated plugins and the plugin is disabled. |
| **Retained** | The plugin stays in the load order unchanged. This covers ineligible, excluded, base game and Creation Club plugins. |
| **Pinned** | An eligible plugin that is retained only because a retained plugin uses it as a master. |
| **New record** | The record originates in a compiled plugin. It is re-created in a generated plugin with a new FormID. |
| **Override record** | The record originates in a retained plugin. A generated plugin overrides it, so its FormID does not change. |

---

## Scenario 1: A (framework), B (patch for A), C (expansion for A), all eligible

### Answers

1. **Should all records from A, B and C be compiled?** Yes. Every record of every compiled plugin takes part in compilation: A's new WEAP/OMOD/KYWD, B's overrides, C's new weapons and anything any of them override in other plugins. Some records are emitted in a different form:
   - B's overrides fold into A's records.
   - An override whose result equals what a retained plugin already provides is dropped as an ITM.
   - A deleted new record is dropped.
2. **Should the compiler follow the complete override chain?** Yes, always the whole chain from the origin to the last override, retained plugins included. For a chain such as `A → B → (retained X)`, the result has to account for X even though X is not compiled.
3. **What does the generated record set look like?** One record per FormID group:
   - **A's new records** become new records in the generated plugins. Each one holds the merged content of A's base version, B's patch and any other override in its chain.
   - **C's new records** become new records too. Their references to A's records are rewritten to the new FormIDs.
   - **Overrides of base game or retained records** made by A, B or C become override records. Several plugins changing the same record are merged into one.
   - B contributes **no records of its own**. It only changes the content of A's records.
4. **Does B disappear after compilation?** Yes. It is disabled and listed in `Multiplexer_DisablePlugins.txt`. Its edits live inside the generated records.
5. **Does C disappear after compilation?** Yes, the same way.
6. **Are dependencies removed?** The dependencies *between* A, B and C disappear because they now live in one record set. The generated plugins get their own dependencies:
   - on the base game masters and any retained plugin their records reference;
   - on each other, for example `Dummy_Plugin_04` mastering `Dummy_Plugin_01` because a weapon in 04 uses a keyword in 01.

### Behaviour

- **What happens.** A, B and C are compiled and disabled. Their records are emitted in dependency order: keywords and mods before the weapons that use them, A's records before C's.
- **Why.** All three plugins can be represented completely, so nothing has to stay in the load order and three slots are freed.
- **Risks.**
  - **How the weapon reaches the player matters.** It is usually done with a quest script (`QUST` + VMAD), by overriding the Player `NPC_` (`0x00000007`), or by injecting into a leveled list (`LVLI` override).
    - A leveled list injection compiles fine.
    - A quest or `NPC_` override makes A **ineligible**, and you are in Scenario 3.
  - **Saves.** Saves that contain A's or C's items reference their old FormIDs, so start a new game.
  - **Merging.** Merging B's changes into A's records is only correct if the field definitions for those record types are complete. Unknown subrecords are kept as opaque regions, and conflicting fields are reported in the Conflict Centre.
- **Architecture.** Keep the current flow: group by FormID → merge the chain → emit as new or override → order by strongly connected components → allocate → translate FormIDs → validate.
- **Dependency handling.** References to compiled records are rewritten to generated FormIDs. References to retained records keep their FormIDs, and that plugin becomes a master of the generated plugin.
- **Records.** A and C are **promoted** (re-created as new records). B is **merged** into A's records.
- **Final load order:**
  ```
  Fallout4.esm … DLC*.esm
  (retained plugins, original order)
  Dummy_Plugin_01.esp … Dummy_Plugin_NN.esp
  # A.esp, B.esp, C.esp disabled
  ```
- **xEdit.** Every generated plugin lists only masters that exist and load earlier, ordered by load order. Every FormID resolves. Outputs are ESL-flagged only when their new object IDs fit in `0x800–0xFFF`. The master count stays within the 254 limit (checked today).

**Today:** implemented as described.

---

## Scenario 2: A (framework) ineligible, B depends on A and is eligible

### Answers

1. **Can B still be compiled?** Yes.
2. **Must A remain active?** Yes. A's records stay in A, and the generated plugin that holds B's content uses A as a master.
3. **How should references to A be handled?** Left unchanged. They point at A's real FormIDs, which still exist because A stays loaded. The generated plugin adds A to its master list, and the FormID's master index is renumbered to match that list. The object ID stays the same.
4. **Should B remain separate?** No. Its records are fully representable, so B can be compiled and disabled. That frees one slot without touching A.
5. **Should B be partially compiled?** No, B is compiled in full. Partial compilation only comes up when B itself contains unsupported records (see the Critical Architecture Question).
6. **What if compiled records reference records left in A?** That is fine and expected. The generated plugin carries A as a master. Two things are validated:
   - A loads before the generated plugin, which is always true because generated plugins go at the end;
   - every referenced FormID exists in A (otherwise "Missing Reference").
7. **Safest architecture.** Retained plugins act as fixed masters of the generated plugins. Compiled plugins can depend on retained ones; retained plugins can never depend on compiled ones (Scenarios 3 and 5).

### Behaviour

- **What happens.** B's new records become new records in a generated plugin. B's overrides of A's records become **override records** of A's FormIDs in the generated plugin. A stays.
- **Why.** Override records can live in any plugin that masters the origin, so moving them out of B is lossless.
- **Risks.**
  - **Load order position.** Generated plugins go at the end. B's overrides of A used to win only over plugins before B; now they also win over retained plugins that loaded after B. The merge accounts for this: retained plugins in the chain are part of the merge, and an override equal to the last retained version is dropped. A field that both B and a later retained plugin changed differently is a **hard conflict** and is reported. The winner can be forced in the Conflict Centre.
  - **Updates to A.** If A is updated, the merged copies are stale. Change detection reports this as "Requires Rebuild" (plugin updated).
- **Architecture.** As implemented: retained plugins are masters; the dependency engine only needs to make sure a retained plugin never ends up mastering a compiled one.
- **Dependency handling.** A is a master of every generated plugin that references or overrides it.
- **Records.** B's new records are **promoted**, B's overrides are **copied** as overrides, and A's records are **referenced**, never copied.
- **Final load order:**
  ```
  Fallout4.esm … A.esp … (other retained)
  Dummy_Plugin_01.esp … Dummy_Plugin_NN.esp
  # B.esp disabled
  ```
- **xEdit.** A appears in the generated plugin's master list. B's overrides show up in xEdit as overrides of A's records in the Dummy plugin, the same way any patch would.

**Today:** implemented as described.

---

## Scenario 3: A (framework) eligible, B (quest addon) ineligible, B depends on A

### Answers

1. **Can A be compiled into generated plugins?** **No, not while B stays unchanged.** B lists A as a master, so A must stay in the load order (rule 2). If A's records were compiled as well, there would be two copies of every A record with different FormIDs: B would use the originals and everything compiled would use the copies. That duplicates content and splits references.
2. **Can B remain as a normal plugin?** Yes. That is the point of retaining it.
3. **How should dependencies be redirected?** By default they aren't. A is **pinned**. The only real redirection is to rewrite B so it masters the generated plugins (see the recommended extension below).
4. **Should B reference generated records instead of the originals?** Only by rewriting B, which breaks rule 4 if done in place. As an opt-in feature, the tool could write a *re-mastered copy* of B into the output mod, leaving the original untouched.
5. **Final load order (default):**
   ```
   Fallout4.esm … A.esp  B.esp … (other retained)
   Dummy_Plugin_01.esp … Dummy_Plugin_NN.esp
   ```
   A is pinned, with the reason "Required as a master by B.esp, which stays in the load order". Plugins that override A, such as patches for A, can still be compiled as in Scenario 2.
6. **How can compatibility be preserved?** By pinning: B sees exactly the A it was built for.

### Behaviour

- **What happens.** A is marked Pinned (shown as ineligible with the reason above). Everything else proceeds.
- **Why.** Pinning is the only option that changes no file and cannot break B.
- **Risks.**
  - **Pinning spreads.** A popular framework pinned by one quest mod pins itself and, through the masters rule, everything it masters.
  - **The user can't see the cost.** The UI should show the cost: "Pinning A keeps 1 plugin and 3,200 records out of compilation; the cause is B.esp."
- **Architecture (default).** Master propagation over the plugin graph until nothing changes. This is what `evaluateEligibility` does.
- **Architecture (recommended extension, "Master Redirection").** Opt-in, per retained plugin, allowed only when **all** of these hold:
  1. Every record type in B has complete FormID-field coverage in the runtime definitions. VMAD script properties, conditions and the fields that hold FormIDs all have to be known. A FormID inside data the tool cannot decode makes the plugin non-redirectable.
  2. B is not referenced by file name (`GetFormFromFile`, MCM configs, `sResourceArchiveList`). These references are already detected.
  3. B is not referenced through *A's* file name either. Scripts in B that call `GetFormFromFile(0x800, "A.esp")` would break because A is disabled. These references are also detected.

  If all three hold, write `B.esp` into the output mod with A replaced by the generated plugins in its master list and every A FormID translated. The original stays where it is, and MO2 picks the copy because the output mod has higher priority. Disabling the output mod restores the original exactly.
- **Dependency handling.** Default: B → A, unchanged. With redirection: B → Dummy plugins.
- **Records.** Default: A is **skipped** (left in A). With redirection: A is **promoted**, and B is **copied verbatim apart from FormID translation**.
- **xEdit.** Default: nothing changes. With redirection: the copied B is a normal plugin mastering Dummy plugins, so it opens in xEdit and every reference resolves. The validation engine should run the same reference check on it as on the generated plugins.

**Today:** pinning is implemented. Master Redirection is not. It is the most valuable extension, because frameworks pinned by quest mods are the usual reason large load orders compile poorly.

---

## Scenario 4: A and B eligible, C ineligible, C overrides records from A and B

### Answers

1. **Can A and B still be compiled?** It depends on where the overridden records come from.
   - **Records created in A or B.** If C overrides them, C masters A and B, so they are pinned (Scenario 3). They can't be compiled without Master Redirection of C.
   - **Records C and A/B only share.** If C overrides base game records that A and B also override, C does not need A or B as masters. A and B can be compiled.
   - **Unnecessary masters.** C may list A or B as a master without referencing any of their records (common in patches). This is worth detecting: report it ("C lists A as a master but uses none of its records"), because the user can clean masters in xEdit, or the tool can write a header-only ESL stub named `A.esp` into the output mod. The stub contains no records and so needs no FormIDs, satisfies C's master list and takes no full slot.
2. **How are C's overrides preserved?**
   - **Pinned case:** nothing changes for C.
   - **Shared base records:** the merge includes C's version, because retained plugins are part of the chain.
     - If the merged result equals C's version, no override is written and C still wins.
     - Otherwise the generated override contains C's changes plus A's and B's non-conflicting changes.
     - Fields where C and A/B disagree are hard conflicts. **The default winner should follow the original load order**, so C wins if it loaded after A and B. The user can override this.
3. **What becomes the winning record?** In the game, the generated override, because generated plugins load last. In content, the merge of the full chain resolved in original load order, so the user's original intent is kept unless they force a winner.
4. **Does C need to remain active?** Yes, always. It is ineligible.
5. **Override chains through ineligible plugins.** The generated record is the merged result of the whole chain, with retained plugins treated as read-only contributors. The resulting rules:
   - a chain with **no compiled plugin** is not emitted;
   - a chain whose **last link is retained and equals the merge** is not emitted;
   - a chain whose **origin is compiled** but which is overridden by a retained plugin is impossible after pinning; the engine reports it as an error if it ever occurs.

### Behaviour

- **What happens (C overrides A's and B's own records).** A and B are pinned; C, A and B stay.
- **What happens (C overrides base records shared with A and B).** A and B are compiled. Overrides where C has the final say are not emitted; where A or B add non-conflicting changes, the merged record is emitted.
- **Why.** Same as Scenario 3, and the merge keeps last-plugin-wins semantics for retained plugins.
- **Risks.**
  - Because generated plugins load last, **a retained plugin can lose a hard conflict it used to win**, unless the merge resolves the conflict in its favour.
  - Unresolved hard conflicts must be shown and must default to the original winner.
- **Dependency handling.** C's masters are pinned. Generated plugins master C only if they override or reference C's records.
- **Records.** Overrides of shared records are **copied and merged** into override records. C's records are **referenced**, not copied, unless a merge changes them.
- **Final load order:**
  ```
  Fallout4.esm … [A.esp B.esp if pinned] C.esp … (other retained)
  Dummy_Plugin_01.esp … Dummy_Plugin_NN.esp
  ```
- **xEdit.** In xEdit's conflict view the generated override is the winning version. The `Multiplexer Reports/` conflict report says which fields came from C.

**Today:**
- Pinning and chain merging are implemented.
- Hard conflicts already go to the last plugin in the original load order that changed the field (`MergeEngine`), so a retained C that loaded last keeps its values.
- Not implemented: unnecessary-master detection and the ESL stub.

---

## Scenario 5: A, B, C eligible; D ineligible and depends on records from A, B and C

### Answers

1. **Can A, B and C be removed after compilation?** Not safely without changing D. D masters all three, so they are pinned. With Master Redirection of D they can be compiled and disabled, as long as D passes the redirection checks.
2. **How will D find the records it needs?** By default, in A, B and C, which stay. With redirection, through its rewritten master list and translated FormIDs pointing into the Dummy plugins.
3. **Should original FormIDs be preserved?** Yes, in two places.
   - **`Multiplexer_FormMap.tsv`** (exists): original plugin + object ID → generated plugin + object ID for every new record. It drives stable FormIDs between builds and Master Redirection, and it is the basis of tooling (an xEdit script to re-master a plugin by hand, troubleshooting, save diagnostics).
   - **The database** (`RecordAssets` and related tables) for the GUI's traceability panel.
4. **Is a redirection table required?** Yes, as **build-time data** (the FormMap). It can't be a **runtime** table: the game has no FormID redirection. An F4SE plugin could hook form lookups, but that is fragile, depends on the runtime version and changes nothing for xEdit. It is not recommended.
5. **Should generated plugins keep backwards compatibility for dependent mods?** They can't, natively. A mod built against `A.esp` will never find its records in `Dummy_Plugin_03.esp`. Compatibility comes from three things:
   1. **Pinning** by default;
   2. **Master Redirection** (re-mastered copies) when it can be done safely;
   3. **Stable FormIDs across builds**, so redirected copies and saves keep working when the compilation is rebuilt.

### Behaviour

- **What happens.** By default A, B and C are pinned. With redirection enabled and D eligible for it, A, B and C are compiled and a re-mastered `D.esp` is written to the output mod.
- **Why.** The same master rule as Scenario 3, applied to several masters at once.
- **Risks.**
  - **Partial redirection is not possible.** D has a single master list, so if any one FormID can't be translated, nothing can be redirected.
  - **When D updates, the copy is stale.** Change detection must treat a redirected plugin's source as an input of the build, so a D update requires a rebuild.
  - **A redirected copy is a derived file of someone else's mod.** It is fine for personal use and must never be redistributed. Reports should say so.
- **Dependency handling.** Graph propagation as in Scenario 3, evaluated per retained plugin that depends on compiled plugins.
- **Records.** Default: A, B and C are **skipped** (stay). With redirection: A, B and C are **promoted**, and D is **copied with FormIDs translated**.
- **Final load order (with redirection):**
  ```
  Fallout4.esm … (retained)
  Dummy_Plugin_01.esp … Dummy_Plugin_NN.esp
  D.esp  (re-mastered copy from the output mod, loaded after the Dummy plugins)
  # A.esp, B.esp, C.esp disabled
  ```
  D has to load *after* the generated plugins because it masters them. Its overrides then win again, which matches its original position after A, B and C.
- **xEdit.** The re-mastered D lists the Dummy plugins as masters, and all its references resolve. The validation engine should check it like a generated plugin.

**Today:** pinning is implemented. Master Redirection is not.

---

## Scenario 6: asset dependencies (A weapon records, B textures, C meshes)

### Answers

1. **How should asset ownership be tracked?** Per file, by the virtual file system, never by plugin. An asset belongs to the **mod** and **container** (loose file or BA2 archive) that provides the winning copy of its path. A plugin only *refers* to assets by path:
   - WEAP `MODL` → mesh;
   - the mesh's NIF string table → BGSM/BGEM materials;
   - materials → textures.

   This is what `AssetTracer` follows today.
2. **Should reports keep asset provenance?** Yes. Every generated record should list every asset it uses, with source mod, archive and path. The **Assets Used** report and the Compilation page do this already.
3. **How can users find out which mod provided which asset?** On the Compilation page, under Generated Records, select a record: the traceability panel shows the mesh, materials and textures with the mod and archive each is served from, and the alternatives that lost. The Assets page shows the winner of every path and the reason it won.
4. **What metadata should be stored?** For each asset path:
   - the winning provider: mod, container (`loose` or archive name), path inside the archive, size, hash or timestamp;
   - every losing provider;
   - the reason it wins (loose beats archive, archive order, mod priority);
   - the generated records that use it, and which field uses it (`MODL`, material slot, texture slot);
   - its status (present, missing, provided only by a disabled plugin's archive).

### Behaviour

- **What happens.** Nothing is copied or repacked. Generated records keep the original paths.
- **The important case.** If B or C are **plugins whose only job is to load their BA2 archives** (`B - Main.ba2`, `C - Textures.ba2` load automatically only while `B.esp` / `C.esp` is active), disabling them stops the archives from loading.
- **How that is handled.**
  - An empty asset-loader plugin has nothing to compile, but it can still be "compiled", meaning disabled. When it is, its archives go into `Multiplexer_Archives.ini` for `sResourceArchiveList2`.
  - If the user doesn't merge that INI, the textures and meshes go missing in game. Validation should flag every traced asset that is served only by an archive which will stop loading. Today this is a manual step described in the user guide.
- **Risks.**
  - Archive load order changes once archives are listed in the INI instead of loading with their plugin. INI-listed archives load before plugin archives, which can change which archive wins a path. The asset index should rerun winner resolution for the *post-compilation* archive order and report every path whose winner changes.
  - Very long `sResourceArchiveList2` values should be tested on the target runtime before relying on them for hundreds of archives.
- **Dependency handling.** Asset dependencies never create plugin masters. They only create archive-loading requirements.
- **Records.** Assets are **referenced**, never copied.
- **Final load order.** Unchanged by assets. Only the archive list in the INI changes.
- **xEdit.** Unaffected. xEdit doesn't resolve assets. Paths are byte-identical to the originals.

**Today:**
- Ownership, tracing, provenance and the archives INI are implemented.
- Recommended:
  - post-compilation winner re-resolution;
  - an "archive will stop loading" validation check.

---

## Scenario 7: a new mod after compiling 500 plugins into 20 outputs

### Answers

1. **How should recompilation work?**
   1. On start, change detection compares the installation fingerprint with the last build. It reports `NewWeaponPack.esp` (added plugin) and sets **✖ Requires Rebuild**.
   2. **Rebuild** reruns the pipeline with the build cache. Only `NewWeaponPack.esp` is parsed. Only override chains that contain it are re-merged.
   3. Existing records keep their output and FormID (`Multiplexer_FormMap.tsv`).
   4. Only outputs whose bytes change are rewritten.
2. **Can the previous compilation be updated incrementally?** Yes, and that is what happens today, with the limits below.
3. **Must the entire project be rebuilt?** No, apart from three cases where the work grows (none of them is a full rebuild):
   - **NewWeaponPack is ineligible and masters a compiled plugin.** For example, it masters A from Scenario 1. A must be **pinned back** (un-compiled):
     - A's records leave the generated plugins;
     - their FormIDs go back to A's originals;
     - saves referencing the compiled copies break.

     **This must never happen silently.** Before rebuilding, the tool should present three choices:
     1. **Pin A again**, with a save warning;
     2. **Master-redirect** NewWeaponPack;
     3. **Leave NewWeaponPack disabled.**

     Until it is resolved, NewWeaponPack can't run anyway: its master A is disabled, and the game would not start.
   - **NewWeaponPack overrides compiled records.** Only those chains are re-merged. The changed records stay in their outputs.
   - **An output fills up.** Placing new records in an output with room is fine. *Moving* existing records is not.
4. **What metadata should be stored?**

   | Data | Stored today |
   |---|---|
   | Installation fingerprint (mods, plugins, archives, loose files, sizes, hashes, load order, dates) | ✓ snapshots |
   | Per-plugin parse cache and per-chain merge cache keyed by content signatures | ✓ BuildCache |
   | FormMap: original key → output + object ID | ✓ `Multiplexer_FormMap.tsv` |
   | Compilation settings (outputs, strategy, flags) and the set of compiled plugins | ✓ build snapshot / `Multiplexer_Compilation.json` |
   | Which outputs were written, and their hashes | ✓ |
   | Per-output free capacity and the ESL/ESP decision | partly; should be explicit |
   | Pin and redirection decisions, with their cause | recommended |
   | Inputs of redirected plugins (Scenario 5) | recommended |

### Behaviour

- **Allocation policy for rebuilds.** Recommended: make it **append-only**.
  - Existing records never move.
  - New records go into the lowest-numbered output that satisfies the dependency order (after every output they reference) and has room.
  - Each output is left with headroom, especially ESL-flagged outputs. An ESL output that outgrows `0xFFF` new records has to become a full ESP, which changes every FormID in it (reported today as *FormID Stability*).
  - Today the engine reuses previous outputs and FormIDs where it can, but it may still move a record when its dependencies change. Append-only makes that a last resort that has to be confirmed.
- **Risks.** Uninstalling a compiled mod removes its records, so saves using them lose those items. That is no different from uninstalling a mod normally, but the Changes page should say so.
- **Final load order.** Same Dummy plugins, possibly one more if every output is full. `NewWeaponPack.esp` is disabled once it is compiled.
- **xEdit.** Unchanged plugins are byte-identical to the previous build. Changed ones are validated the same way.

---

## Critical Architecture Question: plugin-level or record-level eligibility?

Example: a plugin that contains WEAP, ARMO, KYWD, QUST and WRLD.

### A) Plugin level: reject the whole plugin

| Advantages | Disadvantages |
|---|---|
| No original file is ever modified (rule 4) | A single unsupported record keeps a whole plugin out |
| The plugin is either fully original or fully compiled, so it is easy to reason about, explain and undo | Large plugins that mix types (most "overhaul" mods) never compile until every type is supported |
| No FormID is ever split between two files | Pinning spreads through the master graph |
| Retained plugins see exactly the masters they were built for | |
| Matches how the game and xEdit see plugins | |

### B) Record level: compile supported records, leave the rest in the original plugin

| Advantages | Disadvantages |
|---|---|
| More records go through conflict resolution and merging | **No slot is saved.** The original plugin must stay active to provide QUST and WRLD, so the main goal fails |
| | **The original plugin has to be edited.** WEAP, ARMO and KYWD must be removed from it, or their duplicates create two versions of each item |
| | **References cross the split.** The QUST left behind references the moved weapons by their old FormIDs (quest aliases, scripts, conditions). Those FormIDs no longer exist unless every one is translated, which needs complete decoding of exactly the types that made the plugin ineligible |
| | **Leftover records reference moved ones.** A WRLD/CELL placing a reference (`REFR`) to a moved weapon breaks the same way. Precombines and previs are tied to the original file |
| | **Updates break the split.** When the mod updates, the split has to be redone from scratch |

The underlying problem with B: **the records left in the original plugin cannot see the moved ones** unless the original plugin is rewritten. Rewriting it safely requires decoding everything it contains, and that is exactly what eligibility measures.

### Recommendation

**Decide at plugin level, analyse at record level.**

1. **The unit of removal is the plugin.** A plugin is either compiled and disabled, or retained untouched. This is what the engine does today, and it is the only model that respects rules 1, 2 and 4.
2. **Record-level analysis is already used for everything except removal:**
   - conflict detection and merging consider every record of every plugin, retained ones included;
   - a retained plugin's overrides take part in merges;
   - eligibility reasons name exact record types and counts.
3. **Make the record-level detail actionable.** For each ineligible plugin, show what blocks it ("2 QUST, 1 WRLD") and what un-blocking it would unlock (its own records, plus every plugin pinned because of it). This tells the project which record types to support next for the biggest gain.
4. **Grow coverage, not splits.** Record-level compilation is only safe when the "unsupported" part can also go into a generated plugin. At that point the type simply *is* supported. So the path forward is to add compilable record types with complete FormID-field coverage, not to split plugins.
5. **Add Master Redirection (Scenarios 3 and 5)** as the opt-in way to compile frameworks that retained plugins depend on. It changes a dependent plugin only through a copy in the output mod, and only when that plugin is fully decodable.

## Summary of recommended changes

Ranked by impact; none of these is implemented yet.

1. **Master Redirection** for fully decodable retained plugins: re-mastered copies in the output mod, loaded after the Dummy plugins.
2. **Pin-back protection.** When a new or updated plugin would force a compiled plugin back into the load order, ask instead of doing it silently.
3. **Append-only allocation** across rebuilds, with per-output headroom.
4. **Pinning cost in the UI.** Show what each ineligible plugin blocks, including transitive pins.
5. **Unnecessary-master detection**, and an optional header-only ESL stub for masters a retained plugin lists but never uses.
6. **Post-compilation archive winner check**, plus validation of assets that are served only by archives which will stop loading.

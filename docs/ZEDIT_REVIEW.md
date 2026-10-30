# zEdit merge review: ideas for this project

[zEdit](https://github.com/z-edit/zedit) (MIT licence) is an xEdit-based modding tool whose **Merge** view combines plugins into one merged plugin. This review covers its merge services (`src/javascripts/Services/merge`), asset handlers (`src/javascripts/Runners/assetHandlers`) and cleaning checks (`src/javascripts/Services/clean`). It looks at what fits a compiler that keeps every plugin's assets in place. Numbers come from a real load order's plugin report (2,302 plugins).

## How zEdit merges

- **Records:** copies every record of the merged plugins into one file, last plugin first, so the winning version is taken (`recordMergingService`). There is no field-level merging. That is our **Winner** strategy, applied to everything.
- **FormIDs**, two methods:
  - **Clean** (default): copy first, keep each record's original object ID, and renumber only IDs that collide between merged plugins.
  - **Clobber**: renumber up front.
  - Both write a FormID map per plugin (`map.json`), like our `Multiplexer_FormMap.tsv`.
- **Masters:** adds every master, then removes the merged plugins from the master list.
- **Rebuild status:** an MD5 per merged plugin and its data folder; "Up to date" / "Ready to be built". We already do this, at record level.
- **Assets:** copied or extracted into the merge folder, or packed into a new archive. We keep them in their original mods and archives (archive loaders / INI), which is lighter and keeps mod-manager priority.

## Ideas that fit, by impact

### 1. Script and config relinker (105 plugins)

**The problem.** Plugins whose file name appears in scripts or config files are kept out today ("Referenced by file name in N script/config files"). On the reference load order **105 plugins are held back by this alone**. Examples:
- `XDI.esm`, `PhotoMode.esp`: MCM `config.json`;
- `CBBE.esp`: a script's string table;
- `Dank_ArmorOverrides.esp`, `Better Vertibird LiTE.esp`: RobCo Patcher INIs.

**zEdit's answer** (`relinker`, `pexService`): load each compiled Papyrus script (`.pex`) that names a merged plugin, then:
- rewrite the plugin name in the script's **string table** to the merged plugin;
- for each `CallStatic Game.GetFormFromFile(id, "Plugin.esp")`, replace `id` with the new FormID from the FormID map;
- save the script to an output folder that overrides the original.

**For us:**
- The same for `.pex` (read from loose files or BA2).
- A text variant for configs:
  - MCM `config.json` and RobCo Patcher INIs use `Plugin.esp|FormID` (rewrite plugin and ID through the form map);
  - other plain mentions just rename the plugin.
- Written into the output mod so they win over the originals.

**Limits:**
- F4SE DLLs (binary) cannot be relinked; such plugins stay out.
- Names a script builds at run time (concatenation) are not literal and cannot be seen; those also stay out.
- Transfer Settlements blueprints are user data: report, do not rewrite.

### 2. Moving plugin-named assets (needed for new NPCs)

**The problem.** The engine finds some files by the **defining plugin's name and the record's FormID**:
- FaceGen: `meshes\actors\character\facegendata\facegeom\<plugin>\<FormID>.nif` and `textures\...\facetint\<plugin>\<FormID>.dds`;
- voice: `sound\voice\<plugin>\<voice type>\<topic>_<FormID>_1.fuz`;
- LOD billboards: `textures\terrain\lodgen\<plugin>\<FormID>.dds`.

Today such a plugin is kept out. 190 plugins on the reference load order have plugin-named assets, always together with other blockers (new NPCs, quests, dialogue).

**zEdit's answer** (`assetHelpers.getNewPath`, the face, voice and billboard handlers):
- copy (or extract) each such file to the merged plugin's folder;
- rename the FormID part of the file name through the FormID map.

**For us:** the missing piece for **compiling new NPCs** (beyond stage 7a's overrides only). Copy each new NPC's FaceGen files to `...\Dummy_Plugin_XX.esp\<new FormID>.nif|.dds` in the output mod. Billboards follow the same pattern if `STAT`s with LOD are compiled. Voice files only matter if `INFO` is ever written, which needs nested groups first.

### 3. Identical To Previous Override (cleaning report)

zEdit's error checks include **ITPO**: a record identical to the previous override even though that override is not one of its masters. It is harmless but usually unintended, and it hides that the plugin was made against another mod's version. Our ITMs compare against the plugin's own masters. ITPO is a cheap, separate addition to the cleaning report.

Two related checks:
- **DR**: a deleted record that still contains data;
- **UES**: unexpected or out-of-order subrecords. Our definition check covers it partly.

### 4. Keep original object IDs where they are free (optional)

zEdit's *Clean* method keeps original object IDs unless they collide. For non-ESL outputs we could do the same, so a record's FormID changes only in its plugin index:
- a script relinker only has to rename the plugin;
- form map lookups are easier to follow by hand.

**Not recommended for now:**
- It does not help saves (the plugin changes anyway).
- It conflicts with ESL-sized outputs (most inputs use 800–FFF).
- Our form map already keeps IDs stable between builds.

### 5. SEQ files (only if quests are ever compiled)

A quest flagged *Start Game Enabled* in an ESP needs its FormID in `seq\<plugin>.seq` (`seqService`). This is not needed while `QUST` is preserved. It becomes required the day it is compiled.

## Not applicable

- **Plugin INI merging** (`<plugin>.ini`) and **MCM translation merging** (`interface\translations`): Skyrim and SkyUI conventions.
- **Archive extraction or a merged archive:** our archive loaders and INI registration keep archives in place and keep mod-manager priority.
- **Field-level merging:** zEdit has none (whole records, last plugin wins). We already go further with Specialised types and the patch.

## Status

- The relinker (idea 1) is built as the separate, opt-in **Advanced Users Auto-Merger** (its own tab and report).
  - Unlike zEdit, it does not rename the plugin name in the script's string table: a string can be used by other code. Every reference is classified first (see the User Guide, *Filename references*), and only uses whose meaning is established (`GetFormFromFile` with a fixed FormID, `IsPluginInstalled`, `Plugin|FormID` pairs, MCM requirements) are pointed at a new string or rewritten in place.
  - **Scripts:** a strict Fallout 4 `.pex` reader. The layout follows Champollion's reader and Caprica's writer, and the whole file must read back exactly, object sizes included.
  - Every copy is checked at compile time and a failure keeps its plugin out; export is a transaction with a manifest.
- FaceGen relocation, ITPO and DR checks: not started.

## Suggested order

1. **Relinker** for scripts and configs: largest gain (about 105 plugins), self-contained, testable with fixtures. Unsupported cases are reported and their plugins stay out.
2. **ITPO and DR** in the cleaning report: small.
3. **FaceGen relocation**, then **new NPCs** as an extension of stage 7a: after the NPC_ layout is confirmed by a definition check.

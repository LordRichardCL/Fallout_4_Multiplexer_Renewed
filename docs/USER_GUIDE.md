# User Guide

## Before you start

- Use a **test profile** for your first compilations (in MO2, copy your profile). Compilation writes new files and only changes your load order when you ask it to.
- Work on a **new game** or a save you can afford to lose. Compiled records get new FormIDs.
- Fallout 4 **1.10.163** (Old Gen) is the supported runtime.

## 1. Settings

Open **Settings** and choose your installation type.

| Type | What to enter |
|---|---|
| Mod Organizer 2 | The MO2 instance folder (the one containing `ModOrganizer.ini`; for global instances it is under `%LOCALAPPDATA%\ModOrganizer\<name>`). Pick a profile or leave "selected in MO2". The game folder is read from MO2. |
| Vortex | The Fallout 4 folder. Vortex deploys into `Data`, and its `vortex.deployment.json` is used to attribute files to mods. |
| Standalone | The Fallout 4 folder. `plugins.txt` is read from `%LOCALAPPDATA%\Fallout4`. |

Optional overrides: a different `plugins.txt`, a different INI folder, and the string table language (for localised plugins).

**Never compile these plugins** lets you keep plugins in the load order. For example, keep a patch you are still editing.

## 2. Scan

**Dashboard → Scan** builds the virtual Data folder and reads every plugin, archive and loose file. The dashboard then shows mods, plugins (ESM/ESP/ESL), archives, loose files, records and eligibility.

## 3. Analyse

**Analyse** walks every override chain and classifies conflicts, builds the record reference graph and evaluates compilation eligibility.

### Plugins

Filter by ESM/ESP/ESL, conflicting, disabled, eligible, ineligible, or **Generated** (the compiled outputs). The detail panel shows masters, dependents, record types, archives and the **exact reason** a plugin can or cannot be compiled. Use **Exclude from compilation** to keep a plugin.

### Conflicts

- **Hard Conflicts** (type 3): two plugins changed the same field differently. Someone loses.
- **Compatible Merges** (type 2): plugins changed different things. Last-plugin-wins would lose edits; compilation merges them.
- **Simple Overrides** (type 1): nothing is lost.
- **Critical**: the winner deletes a record.

Select a record to see the override chain and a grid of every field per plugin:

- green: the value that wins
- red: a value that is replaced
- yellow: a value merged in from a plugin that is not last
- blue column: what the compiler will write

**Winner when compiling** lets you force one plugin's version of the record. The choice is stored in the database and survives rescans.

### Assets

**Conflicting** lists every path provided by more than one source. Select an asset to see the winning mod, the reason (loose beats archive; later archives beat earlier ones; higher mod priority wins) and every alternative. Select an alternative to preview it. Textures show resolution, format, mip levels and an image (BC1–BC5 and uncompressed formats). Meshes show their block structure and referenced materials. Materials show their texture slots.

### Dependencies

- **Plugins**: the master graph, coloured by eligibility. Double-click a plugin to show only its masters and dependents.
- **Records**: search an Editor ID or FormID to see what a record references (green) and what references it (yellow). Double-click to re-centre.
- **Missing Requirements & Breakages**: missing masters and references to records that do not exist.

## 4. Compile

Open **Compilation**:

- **Output plugins**: 10–20. Unused slots are not written.
- **Name prefix**: `Dummy_Plugin_` by default.
- **Strategy**: A (by record type), B (balanced size), or C (user rules like `WEAP=3` or `MyWeapons.esp=5`). Records always come after everything they reference, so rules are preferences, not guarantees.
- **Merge compatible edits**: on by default. Turn it off for strict last-plugin-wins.
- **ESL-flag outputs**: saves load order slots when an output has ≤ 2048 new records.

Press **Compile**. Everything happens in memory and is then validated. The **Validation** page lists every finding. Errors block export.

## 5. Export and switch over

**Export Plugins** writes, to the output folder (by default an MO2 mod folder named `Fallout 4 Multiplexer Output`, otherwise `Data`):

| File | Purpose |
|---|---|
| `Dummy_Plugin_XX.esp` | the compiled game state |
| `Multiplexer_FormMap.tsv` | every old → new FormID; also lets the next compilation keep the same FormIDs |
| `Multiplexer_Archives.ini` | the compiled plugins' BA2s, to merge into `Fallout4Custom.ini` |
| `Multiplexer_DisablePlugins.txt` | plugins that are now compiled |
| `Multiplexer_plugins.txt` | a proposed `plugins.txt` |
| `Multiplexer_Compilation.json` | machine-readable summary |
| `Multiplexer Reports/` | all reports (HTML + JSON) |

Then:

1. In MO2, enable the output mod (or refresh if you exported into an existing mod).
2. Disable the plugins listed in `Multiplexer_DisablePlugins.txt`, or press **Apply to MO2 Profile**. That rewrites the profile's `plugins.txt`/`loadorder.txt` after making timestamped backups.
3. Copy the `[Archive]` line from `Multiplexer_Archives.ini` into `Fallout4Custom.ini` (or the profile's INI when MO2 uses profile-local INIs). Without this, textures and meshes from compiled mods' archives will be missing.
4. Open the outputs in FO4Edit and spot-check a few records.

## Recompiling after changing mods

Rescan, re-analyse and compile again **into the same output folder**. The previous `Multiplexer_FormMap.tsv` is used automatically, so records keep their FormIDs. If a record must move (new dependencies), validation reports a *FormID Stability* warning naming it.

## Reports

**Reports** previews each report and exports it as HTML, JSON or PDF. **Export All** writes every report to a folder.

## Command line

Everything is also available through `fo4mx` (run `fo4mx help`), for example:

```text
fo4mx compile --mo2 "C:\Modding\MO2" --profile "Test" --outputs 16 --strategy type --dry-run
```

`--dry-run` stops after validation.

## Troubleshooting

| Symptom | What to do |
|---|---|
| A mod you expected to compile is ineligible | The Plugins page shows the reason. Most mods that touch cells, NPCs or quests are not compilable in this phase. |
| Validation: "probably refers to compiled record … in a layout this profile does not describe" | A record contains a FormID the definitions do not cover. Exclude the plugin it comes from, or enable *rewrite probable FormIDs* if you have checked the field in FO4Edit. |
| Validation: "Only N% of FormIDs in X Y resolve" | The field layout for this runtime looks wrong. Please report it with the plugin name. |
| Missing textures in game after switching | Merge `Multiplexer_Archives.ini` into `Fallout4Custom.ini`. |
| A localised mod shows empty names | Its `Strings\<plugin>_<lang>.STRINGS` files were not found. Set the right language in Settings. |

Logs are written to `%LOCALAPPDATA%\Fallout4MultiplexerRenewed\logs`. The **Log** button in the status bar shows them live.

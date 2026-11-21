# User Guide

## Before you start

- Use a **test profile** for your first compilations (in MO2, copy your profile). Compilation writes new files and only changes your load order when you ask it to.
- Work on a **new game** or a save you can afford to lose. Compiled records get new FormIDs.
- Fallout 4 **1.10.163** (Old Gen) is the supported runtime.

## 1. Setup

![Setup](images/00-setup.png)

The **Setup** screen opens when the app starts.

1. **How are your mods installed?** Mod Organizer 2, Vortex, or manually.
2. **Folders.** The Fallout 4 folder (**Find** reads it from the registry), the MO2 instance (**Find** looks in `%LOCALAPPDATA%\ModOrganizer`) and profile, and the output folder for the generated plugins. Leave the output folder empty to choose it automatically.
3. **Load order.** Leave `plugins.txt` empty to use the one your mod manager uses, or select another file.

The **Checks** list confirms that the game, the mod manager and the load order were found. The right-hand side shows the exact load order that will be used, with inactive plugins greyed out. **Save and Continue** is available once every check passes. **Skip for Now** opens the app without saving.

Untick **Show this screen at start-up** to go straight to the Dashboard next time. The screen still appears if the saved folders stop working, for example after moving the game. **Settings → Open Setup Screen** opens it at any time.

## 2. Settings

**Finding your way around.** Every page starts with its icon, a title and one line saying what it is for. The longer explanation is behind the **?** next to the title (hover over it); options have their own **?** too. Options are switches with a short description under them. The page's main action is the green button; Save Settings is an ordinary button on the pages that have it, except on Settings itself.

**Shortcuts and help.**
- **Ctrl+K** (or **Go to…** under the navigation): type part of a page, plugin or record name (Editor ID) and press Enter to open it. A plugin opens on the Plugins page; a record with conflicts in the Conflict Centre; any other record in the record graph.
- **F1** (or **Glossary**): what the words mean (plugin, master, override, ESL, ITM, ...).
- **Ctrl+1 … Ctrl+9**: the pages in order.
- Click a column header to sort a table by it. On the Plugins page, right-click the header to show or hide columns.
- In a plugin's details, every plugin named (reasons, masters, required by) is a link that opens it.
- While a task runs, the status bar shows the time left in the current step and a **Stop** button. When it finishes, a card in the corner says what it did in plain words, with buttons to what needs you (closes by itself after 30 seconds).

**Plugin status words.** **Will compile**: goes into the outputs. **Stays in load order**: cannot be compiled, with the reason beside it (it holds something the compiler cannot reproduce, or a plugin that stays needs it as a master). **Kept by you**: excluded, or not among the plugins you chose. **Base game**, **Not enabled**, **Error**. (Reports and exported files keep the technical names: Eligible, Ineligible, Retained, Inactive.)

**Settings** holds the same installation details plus everything else. Choose your installation type:

| Type | What to enter |
|---|---|
| Mod Organizer 2 | The MO2 instance folder (the one containing `ModOrganizer.ini`; for global instances it is under `%LOCALAPPDATA%\ModOrganizer\<name>`). Pick a profile or leave "selected in MO2". The game folder is read from MO2. |
| Vortex | The Fallout 4 folder. Vortex deploys into `Data`, and its `vortex.deployment.json` is used to attribute files to mods. |
| Standalone | The Fallout 4 folder. `plugins.txt` is read from `%LOCALAPPDATA%\Fallout4`. |

Optional overrides: a different `plugins.txt`, a different INI folder, and the string table language (for localised plugins).

**Never compile these plugins** lets you keep plugins in the load order. For example, keep a patch you are still editing.

**Saving settings.** The Settings page is where all settings live, but the Compilation and Auto-Merger pages have options too, and each of these pages has a **Save Settings** button. All of them save the same settings, wherever the changes were made. A change is kept as soon as you make it, while you move between pages, and the next Scan, Analyse or Compile uses it. **Save Settings** keeps it after the app is closed. While something is unsaved the button shows a dot (**Save Settings •**). Closing the app with unsaved changes asks whether to save them. A new results database path is used only once saved. Running a task never saves settings for you. Excluding a plugin on the Plugins page is saved at once, without saving anything else.

## 3. Scan

**Dashboard → Scan** builds the virtual Data folder and reads every plugin, archive and loose file.

**The Dashboard** shows, from the top:
- the build's status in one line, with the one button that matters now (Scan my mods, Compile or Rebuild; Check again and Review Changes once there is a build);
- the steps from Scan to Play & check: done steps are green, the one waiting on you is amber;
- four numbers: plugins (and how many are compiled), output plugins, conflicts (and how many need a choice), validation errors and warnings;
- **Needs your attention**: each item in one sentence with a button to the page that handles it (patch choices waiting, plugins kept out, validation errors, missing masters, worldspaces whose large references were left as they are);
- your load order at a glance (how plugins are handled, kinds of conflicts) and, under **All numbers**, every count: mods, plugins by type, archives, loose files, records, asset conflicts, eligible plugins and outputs.

The navigation groups the pages: **Your mods** (Changes, Plugins, Conflicts, Assets, Dependencies), **Build** (Compilation, Validation, Reports) and **Tools** (Auto-Merger, Diagnostics, Settings). Counts beside a page say something there needs you: changes that affect the build, patch choices waiting, validation errors, plugins the Auto-Merger keeps out. Ctrl+1 to Ctrl+9 open the pages in order.

## 4. Analyse

**Analyse** walks every override chain and classifies conflicts, builds the record reference graph and evaluates compilation eligibility.

### Plugins

Filter by ESM/ESP/ESL, conflicting, disabled, eligible, ineligible, or **Generated** (the compiled outputs).

**The summary above the table** counts every analysed plugin: **Total**, **Eligible** (will be compiled) and **Ineligible** (everything that will not be: plugins holding something the compiler cannot reproduce, retained ones such as the base game, Creation Club or your exclusions, inactive ones and unreadable ones; the card says how many of each). Eligible + Ineligible is always the total. Each is broken down by file type: ESL, ESL-flagged ESP, ESP and ESM (an ESM with the ESL flag counts as an ESM; its tooltip says how many). Click a card to filter the table to it; the **Ineligible** chip shows the same plugins as the card.

**Blocking Types** lists every record type the compiler does not support yet that appears in your load order:

- **Would Unlock:** plugins that become compilable if only this type were supported. This includes plugins currently kept because something depends on them.
- **Only Blocker In:** plugins where it is the only unsupported type.
- **Found In:** how many active plugins contain the type, and how many records.
- **Category** and **Support:** the type's group (Items, World, Quests and Dialogue, …) and how the compiler treats it: *Compiled*, *Preserved* (stays in its plugin, with the reason), or *Unknown record type*. An unknown type is not part of Fallout 4: the plugin may be for another game, or damaged. The plugin's reason says so too.

Select a type to see each plugin it blocks and everything else that plugin needs.

**Export…** (top right) saves a text file with every blocking type and the plugins it holds back, and every plugin's load order position, type, header flags, eligibility and reason, record types with counts, mod, masters and author. It is useful for planning which record types to support next.

![Blocking types](images/13-blocking-types.png)
 The detail panel shows masters, dependents, record types, archives and the **exact reason** a plugin can or cannot be compiled. Use **Exclude from compilation** to keep a plugin.

### Conflicts

- **Hard Conflicts** (type 3): two plugins changed the same field differently. Someone loses.
- **Compatible Merges** (type 2): plugins changed different things. Last-plugin-wins would lose edits; compilation merges them.
- **Simple Overrides** (type 1): nothing is lost.
- **Not Merged**: record types the app does not compile (today only the *Candidate* placed types PBAR, PBEA, PCON and PFLA; see **Plugins → Blocking Types**). They are never merged: the winning plugin's version is used as it is, and the **Handling** column says *Not merged*. They are compared record by record:
  - **Conflict (Not Merged)**: two plugins changed the record independently, so the winner drops the other's change. The summary names whose changes are dropped. Change the load order or make a patch in xEdit.
  - **Simple Override**: one plugin changed it, or each later plugin was made with the earlier one as a master and builds on its version.
  - **Winner only**: types that are compiled but not merged field by field (strategy *Winner*). Their conflicts are classified the same way, and compiling writes the winning plugin's version whole, references rewritten. Choosing another plugin under **Winner when compiling** is not available for them.
  - The field table, when shown, is a generic comparison for information only. Its last column is **Used in Game** (the winner's version), not a compiled result, and **Winner when compiling** is disabled.
- **Critical**: the winner deletes a record.

Every override is compared with the version it was made against (the last earlier version in its own masters, as xEdit does), not with the original record.

**Changes not in the winning version** (Conflict Report, and `changes` in its JSON). This table lists every change a plugin made that the winning version does not hold, with what the load order shows about it:
- **not made with it as a master:** the winning plugin never had the change. This is the usual "lost edit".
- **no longer contains it, holds another value, or holds the earlier value again:** a later plugin built on the change's plugin went on to differ.

Changes are traced through the whole chain, so an edit made and lost in the middle is listed even when the original record and the winner look the same. Elements of a list are matched by what they are, not their position. Reordering a keyword list is no change; reordering a form list is.

Notes on a row:
- another plugin's competing value;
- the same element added by another plugin;
- data the app does not understand.

**Compatibility patch plan** (Conflict Report, below the changes; CLI `fo4mx plan`). For each record with changes the winner lacks, it says what a compatibility patch would do with each change, starting from the winning version:
- Green changes are carried by default.
- Yellow ones wait for your decision; until then the winner's value is kept.
- Red and Unknown ones are never carried.

A record is **Ready**, **Needs decision**, **Blocked** (for example, you chose to carry two different values for the same field) or has **Nothing to carry**. The plan also lists the extra masters a patch would need.

To decide in the app: select a record in the **Conflict Centre** and open its **Compatibility patch plan** tab (beside **Fields**). It lists each change the winner lacks:
- what the change was, which mod made it, and what became of it;
- its class and the rule that decided it;
- an **In the patch** choice: *Default*, *Carry forward* or *Keep the winner's value*. Red and Unknown rows show *Never carried*.

**Form lists (FLST)** keep their order: scripts and workshop menus read them by position, so every change to one is Yellow (your decision). An element a mod added shows where it went ("after 10mm Pistol", "first"). Carried, it goes in the same place: after the nearest element before it in that mod's list that the patched list also has (or before the nearest one after it, or at the end). A record whose list then holds the element somewhere else (for example, the winner already has it elsewhere) is not written from the plan.

A choice is saved at once. The tab shows the record's status, any problem (for example two different values chosen for the same field), and the masters a patch would need. The tab needs Analyse to have run in this session. Compile again to write the result.

To record a choice from the command line: `fo4mx plan <install options> carry|keep|default <Plugin.esp:OBJECTID> <change id>`. The ids are listed by `fo4mx plan` and in the Conflict Report's JSON. Choices are saved in `patch_plan_decisions.json` next to the database and kept across scans. **Apply the compatibility patch plan** (Compilation page, off by default; CLI `--apply-patch-plan`). With this on, compiling writes the plan:
- **What each record becomes:** every record the plan marks Ready is written as its winning version plus the changes the plan carries.
- **Where it goes:** to `Multiplexer_Patch.esp` when only mods that stay in the load order hold the record; to an output when a compiled mod does.
- **Checks:** each record must hold every planned change before it is written. If the mods changed since you made your choices, the record is written as it would be without the plan, and a warning says to analyse again and review.
- **Report:** the Validation Report lists each record written this way, with the changes carried and the mods they came from.
- **Verification:** after export, the written plugins are read back and compared with the plan.

After changing your choices, compile again.

Each row also has a class: whether carrying the change into a patch matches a documented rule. **Green** is low risk, though not guaranteed. **Yellow** needs your review (competing values, a later plugin that removed or replaced it, record flags, form lists, removals, a new master). **Red** is never carried automatically (scripts, conditions, object templates, quests, cells, deleted records, data the app does not understand). **Unknown** means there is no rule for that record type. The report counts each class. The table states facts, not intentions: it never says a plugin removed something on purpose. It is the first step of the compatibility patch work (`docs/COMPATIBILITY_PATCH_RESEARCH.md`); nothing is merged from it yet.

Finding conflicts:

- **Not Merged** shows only the conflicts in record types the app keeps as they are.
- **Click any column header** to sort by it (Winning Plugin, Editor ID, Severity, Written To, …). Click again to reverse.
- **Plugin** + **is involved / wins / loses** shows only the conflicts of one plugin. For example, "MyPatch.esp loses" lists every record where your patch is overridden. The list shows how many conflicts each plugin takes part in and how many it wins.
- **Double-click** a row to filter by its winning plugin.
- **Written To** shows which generated plugin (a `Dummy_Plugin_XX.esp` or `Multiplexer_Patch.esp`) carries the resolved record after compiling. It is empty when last-plugin-wins still applies.
- **Clear Filters** resets the type, search, plugin and sort.

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
- **Details panel** (right of both graphs): click a plugin or record to see it in full, since the boxes in the graph shorten long names. For a record that means its type, FormID, the plugin that created it and the one that wins, and every reference in both directions with the field that holds it. The panel scrolls.

![Record details](images/15-record-details.png)
- **Missing Requirements & Breakages**: missing masters and references to records that do not exist. These are problems in the mods themselves, typically a patch made for a different version of the mod it patches, or a record a mod removed in an update. The game reads such a reference as "none". Compiling keeps that behaviour: the reference is written as none and listed under **Broken in Source** on the Validation page, and the plugin is compiled normally.
- **Cleaning**: the mod-cleaning checks described in the xEdit documentation (chapter 7), for every plugin in your load order. **The app only reports these; it never changes a plugin.** Choose a category at the top right; select a row for the likely cause and the usual fix.
  - **Identical to Master (undoes changes)** (Warning): a record saved unchanged from the plugin's masters that loads after plugins that did change it, so their changes are lost. Clean the plugin's ITMs in xEdit, or load it earlier.
  - **Identical to Master (summary)** (Info): how many unchanged records each plugin has, with examples. Usually harmless.
  - **Deleted Reference** (Warning): a placed object or NPC deleted from a master. A significant cause of crashes; xEdit fixes it with *Undelete and Disable References*.
  - **Deleted Navmesh** (Warning): can crash the game and needs manual cleaning.
  - **Deleted Record** (Info): another record type deleted from a master.
  - **Injected Record** (Info): a new record in another plugin's FormID range. Sometimes deliberate.
  - **Circular Leveled List** (Error): leveled lists that contain each other, directly or through others. The game can loop forever when it resolves them.
  - Findings in the base game, DLC and Creation Club files are always **Info**: the xEdit documentation says not to correct errors in official files.

  Identical-to-master records are measured against the plugin's own masters' version, as xEdit does. For record types that are not merged, the comparison is byte by byte. With field comparison on (Settings), an override whose bytes differ only because of master order or compression is still recognised as identical. Otherwise such a record is not counted as identical.

![Cleaning report](images/16-cleaning.png)

## 5. Compile

Open **Compilation**:

- **New quests, dialogue and NPCs** (Compile more plugins, on by default): plugins that add new quests, dialogue, scenes or NPCs compile like any other; their voice files and faces are copied under the new FormIDs, and start-game-enabled quests are listed in the outputs' `.seq` files. Turn it off (or pick **Safest**) to keep such plugins in the load order, for example in the middle of a playthrough: running quests are tied to their FormIDs, so a save made before compiling loses them. Overrides of quests, dialogue and NPCs still compile. CLI: `--no-new-quests-npcs`.
- **Plugins to compile** (after Scan and Analyse; used by the next Compile): every plugin that could be compiled, with a switch, its status and its record types.
  - **All eligible** (default): every plugin that can be compiled is, unless you switch it off (the same as excluding it on the Plugins page).
  - **Only the ones I choose**: nothing is compiled until you switch it on, so a mod you install later is never compiled by surprise. Use it to compile only some mods, such as weapons or armour. Switching to it starts from what compiles now.
  - **Filters**: search by name, show only plugins that will compile, are switched off, or cannot compile, and **Any record type** narrows the list to plugins with, for example, `WEAP` (weapons); **Switch on shown** / **Switch off shown** then set them all at once.
  - A plugin that stays in the load order keeps its masters there too: switching one off can stop a mod it needs from compiling, and the list shows that at once (hover a status for the full reason). Your choices are saved at once. A plugin an earlier build compiled that you switch off is your choice, so it returns to the load order without the Returning Plugins stop. On the command line: `--only <plugin>` (repeatable) compiles only those.
- **Output plugins**: the most output plugins the compiler may create (1 to 64), or **Automatic**: the fewest output plugins whose master lists all fit (each plugin can have at most 254 masters), tried from 1 upwards; the number used is shown after Compile. Fewer outputs take fewer load order slots, each being larger.
  With a fixed number: It fills them in order and starts a new one only when the current one holds its share of the records (at least 2 MB), so a small load order uses fewer; unused ones are not written. Every output that is not ESL-flagged takes one of Fallout 4's **254 full plugin slots**, shared with the plugins that stay enabled; validation stops the export if the total passes 254 (and warns above 240). Outputs master each other, so the worldspace output has 254 minus this number of masters left for the plugins it needs. There is no benefit in more outputs than the load order needs: raise it when outputs grow very large (100 MB and more) or the worldspace output runs out of masters.
- **Name prefix**: `Dummy_Plugin_` by default (outputs are `<prefix>01.esp`, `<prefix>02.esp`, …). Any other prefix works the same; **Default** puts `Dummy_Plugin_` back. After changing it, the earlier build's outputs keep their old names: remove them from the game once the new build is switched on (they are recognised as outputs, never read as sources).
- **Record allocation** decides how records are spread over the outputs; it does not change what the game sees. In every strategy a record goes into the same or a later output than the new records it references, cells, worldspaces and quests with dialogue go together into the last outputs, and a record keeps the output it had in the previous build when `Multiplexer_FormMap.tsv` is in the output folder (so saves keep working).

  | Strategy | What it does | Good for | Trade-off |
  |---|---|---|---|
  | **Balanced size** (default, recommended) | fills outputs one after another to about the same size, in dependency order | almost every load order | a record's output says nothing about its type |
  | **Grouped by record type** | gives each group of record types (the catalogue's allocation groups: items, actors, quests, world, …) its own outputs, sized by how much of it there is | browsing the result in xEdit | outputs reference each other more (more masters), uneven sizes |
  | **Balanced, with my rules** | balanced size, then rules such as `WEAP=3` (a record type) or `MyWeapons.esp=5` (the defining plugin, or a compiled plugin that changes the record) send records to that output | keeping a mod's records together in a known output | dependencies still win, so a rule is a preference, not a guarantee |

- **Merge compatible edits**: on by default. Turn it off for strict last-plugin-wins.
- **ESL-flag outputs**: an output with 2,048 or fewer new records is ESL-flagged and takes no full slot. Outputs with new cells or worldspaces never are.
- **Room to grow** (Output, under Light outputs; 200 by default): a new output is ESL-flagged only when it leaves this many of the 2,048 new-record places free. An output keeps its light or full state on rebuilds (switching gives its records other FormIDs), so an output made light while nearly full would have to become a full plugin, and break saves, when a mod update adds a few records. An output that fits but without the room stays full, with an Info note in Validation saying why; more outputs spread the records thinner. Outputs that are light already are not affected. CLI: `--light-headroom N`.
- **Undescribed data is never guessed at.** Record data is read field by field from layouts taken from xEdit, so every reference to another record (FormID) is known and moved to its new FormID. Bytes the layouts do not describe (a subrecord the definitions do not list, data past a known layout, a condition function xEdit does not know) are kept exactly as written, never rewritten. They are moved only when they provably mean the same in the output: every 4 bytes at any position are zero, or name the same plugin at the same position in the source's and the output's master lists. Otherwise the record's plugin is kept out (**Kept Out Automatically**, category *Undescribed Data*, naming the plugin, the record, the subrecord, the byte range and the value). There is no option to rewrite values because they look like FormIDs (an earlier "advanced" option was removed).
- **Conflict resolution patch**: also writes `Multiplexer_Patch.esp`, similar to Wrye Bash's Bashed Patch. Plugins that stay in the load order still lose edits to last-plugin-wins: two mods adding to the same leveled list, form list or keyword list, or changing different fields of the same record. The patch carries the merged record instead, and respects winners you forced in the Conflict Centre.
  - It only overrides existing records, so it adds no FormIDs, is always ESL-flagged and takes no full load order slot.
  - It must load last. The proposed `plugins.txt` puts it there.
  - It covers the same record types as compilation. NPCs, quests and world data are not merged.
  - A patch can have at most 254 masters. Larger load orders get `Multiplexer_Patch_2.esp`, `_3`, and so on. Every one is ESL-flagged, so they take no full slots.
  - **Patch only** writes just the patch and compiles nothing, so no FormIDs change and no plugins are disabled.

Press **Compile**. Everything happens in memory and is then validated. The **Validation** page lists every finding. Errors block export.

## 6. Export and switch over

**The Compilation page.** Options on the left, results on the right, and the actions at the bottom (Save Settings, Open Folder, Export, Apply to Game, Compile) with the progress of compile, export and apply. Every option has a one-line description; the **?** beside it explains it in full. **Presets** at the top right set the switches in one click; they never change the number of outputs, their name, the folder or the allocation:
- **Recommended**: combine compatible changes, apply your patch choices, merge cell settings, translate previs files, build Multiplexer_Patch.esp, compile new quests, dialogue and NPCs.
- **Safest**: combine compatible changes only.
- **Custom**: shown when the switches match neither.

Neither preset turns on light outputs, placeholders or Master Redirection: choose those yourself once a first build works. A first run starts on **Recommended**; settings you saved are kept.

The options come in cards, top to bottom, each answering one question:
- **Plugins to compile:** which plugins (all eligible, or only the ones you choose).
- **Output:** how many output plugins, their name, the folder, and **Light outputs**. Its **Room to grow** appears while Light outputs is on.
- **When mods change the same thing:** combining changes, your patch choices, cell settings, previs files.
- **Compile more plugins:** new quests, dialogue and NPCs; placeholders for unused masters; Master Redirection. Under each switch, a line from the last Analyse says what it changes on your load order, for example "3 plugins could be redirected, freeing 7 plugins they hold back". It also says plainly when a switch cannot work with your setup. The two marked **Mod Organizer 2** need the output folder inside MO2's mods folder.

![Compilation options](images/07b-compilation-options.png)

**Patch** (Multiplexer_Patch.esp, patch only), **Advanced** (record allocation and rules, broken references as null) and **How to install** (the steps for your mod manager) open with a click; closed, they show their current settings in one line. The results show the last build (plugins compiled, outputs, records), each output's size, errors, warnings and plugins kept out (each with a link), and the Output Plugins, Generated Records, FormID Mappings and Assets Used tables.

**Export** builds one installable mod, the same for every mod manager, in a folder of its own. Every export gets a new folder; an earlier export is never moved, renamed, deleted or changed:

```
<output folder>\                         Settings / Compilation page: "Output folder" (default <game folder>\Multiplexer Output)
├── Multiplexer Latest.txt               names the latest finished export
├── Multiplexer Output 081126 154512\    an earlier export, left exactly as it was
└── Multiplexer Output 081126 163020\    this export
    ├── Data\                            everything the game loads (see below)
    ├── Multiplexer Output.zip           the Data folder, zipped (Data\ at its root): install this
    ├── Multiplexer_DisablePlugins.txt, Multiplexer_plugins.txt, Multiplexer_Archives.ini, Multiplexer_Instructions.txt
    └── Multiplexer Reports\
```

- **While it is written** the folder is called `Multiplexer Output DDMMYY HHMMSS.tmp`; it gets its final name only when everything is written, and only then does `Multiplexer Latest.txt` name it. An export that fails or is interrupted stays `.tmp` (with `MULTIPLEXER_BUILD_FAILED.txt` saying why when it failed): it is never taken for a finished export, and the latest export stays the latest.
- **Two exports in the same second** get ` (2)`, ` (3)`, …; an existing folder is never reused.
- **You can keep an earlier export open** in File Explorer, xEdit, Mod Organizer 2 or the game while exporting again: nothing in it is touched.
- **The next build** reads its history (the FormID map that keeps FormIDs the same from build to build, the lists of copied files) from the latest finished export, or, before the first export of this version, from the single output folder earlier versions used.
- **Old exports** stay until you remove them: `fo4mx clean-outputs [install options] --keep 5` deletes all but the newest 5 (never the latest), with `--unfinished` also `.tmp` ones. Only Multiplexer's own export folders are touched (named like an export and holding its `Multiplexer_Run.txt`); one in use is skipped and left as it is. Your database, backups made by earlier versions and anything else in the output folder are never touched.
- **A folder inside Mod Organizer 2's mods folder** as the output folder makes every export a mod of its own in the mods folder (`Multiplexer Output DDMMYY HHMMSS`); Apply to Game enables the newest and switches the earlier export mods off (their folders stay).
- **The game's own `Data` folder** as the output folder writes the plugins there directly, as before (no export folders, no zip).
- Multiplexer's own results database and build cache, wherever they are, are never put in a zip.

Install `Multiplexer Output.zip` like any other mod, and make it win over every other mod (install it last):
- **Vortex:** let **Apply to Game** copy it to Vortex's downloads folder (see below), or drag the zip onto the Mods page; install and enable it, and let it win every file conflict (load after all other mods).
- **Mod Organizer 2:** *Install a new mod from an archive*, enable it, and keep it at the bottom of the left pane.
- **Manual:** extract the zip into the game folder (its Data folder merges with the game's).

The package is identical for every mod manager; only these instructions differ. (Advanced: an output folder set to a mod inside Mod Organizer 2's mods folder is used as the mod directly, without a zip; then compiled plugins that have archives get archive loaders instead of loose copies and INI entries. That is the only case where the generated files differ, because only there can the app check that the output mod overrides the original plugins.)

Files the game reads (they mirror the `Data` folder layout because the game finds them by path):

| Files | Why they are there | Needed |
|---|---|---|
| `<prefix>XX.esp`, `Multiplexer_Patch.esp` | the compiled game state and the conflict patch | always |
| `seq\<output>.seq` | start-game-enabled quests of each output; the game reads a plugin's SEQ file by its name | when an output has such quests |
| `sound\voice\<output>\…` | voice files of compiled new dialogue lines, renamed to the line's new FormID (the game finds voice by plugin name and FormID) | when compiled mods have voiced lines |
| `meshes\actors\character\facegendata\facegeom\<output>\…`, `textures\actors\character\facecustomization\<output>\…` | FaceGen of compiled new NPCs, renamed the same way | when compiled mods have NPCs with FaceGen |
| loose copies of meshes, textures and other files | without archive loaders, compiled plugins' archives load earlier (from the INI list); each file such an archive wins today is written loose so it keeps winning | Vortex / plain installs |
| copies of scripts and configs | the Advanced Users Auto-Merger's relinked copies, which must override the originals | when the Auto-Merger is on |
| archive loaders (same file name as the compiled plugin) | empty ESL-flagged plugins that keep a compiled mod's archives loading in place | MO2 with the output in the mods folder |
| master placeholders (same file name as the compiled plugin) | the same empty ESL-flagged plugin, for a compiled master that plugins staying in the load order list but do not use (**Placeholders for unused masters**, Compilation page → Compile more plugins; off by default) | MO2 with the output in the mods folder |

Files for you and for the next build (the game does not read them; nothing needs F4SE or any other runtime component). The build's records (`Multiplexer_FormMap.tsv` and the copy lists) are in `Data`, so they are installed with the package and the next scan finds them even if the `Multiplexer Output` folder was deleted; the notes (lists, INI line, reports) are next to `Data` and are not installed:

| File | Purpose |
|---|---|
| `Multiplexer_FormMap.tsv` | every old → new FormID; read by the next compilation to keep the same FormIDs (save compatibility) |
| `Multiplexer_CompiledPlugins.txt` | the outputs and the plugins this build compiled; the next build reads those plugins as sources while they are switched off |
| `Multiplexer_Archives.ini` | a copy of the archive line **Apply to Game** writes into `Fallout4Custom.ini`; the game does not read this file itself |
| `Multiplexer_RegisteredArchives.txt` | archives Apply to Game added to the INI, so the next build compares with the load order as it was |
| `Multiplexer_ArchiveWinners.tsv`, `Multiplexer_Voices.tsv`, `Multiplexer_FaceGen.tsv`, `Multiplexer_Relinked.tsv`, `Multiplexer_Previs.tsv` | what each copy above is and where it came from; the next export removes copies no longer needed, and the next scan does not mistake them for mod files |

**Translate previs files** (Compilation page, on by default; CLI `--no-previs-translation` turns it off): a cell written to an output wins in game, so its previs file (`vis\<plugin>\<cell>.uvd`) is read in the output's numbering, and the export writes a translated copy loose (`Multiplexer_Previs.tsv`). Turning it off writes no copies and uses the original files. It changes only what the export writes: the previs checks still run, a previs file that cannot be translated still keeps its cell's plugins out, and the same plugins compile. It is there to compare the two in game.

**Merge cell configuration** (Compilation page, off by default; CLI `--merge-cell-config`): when several mods change the same cell, the cell written to an output keeps the winning mod's precombine and previs data, and takes the last real change of each configuration field from the other versions: lighting (XCLL), lighting template, location, encounter zone, music, acoustic space, image space, god rays, sky/weather region, water, water height, exterior LOD, the cell's flags (Has Water, Public Area, Show Sky, Off Limits, Can't Wait...) and its regions (every version's regions together). A version that only repeats the base game's value is not a change. It never touches PCMB, XCRI, XPRI, VISI, RVIS or the No Pre Vis flag, and leaves partial cells as they are. Each field taken is listed in the Validation Report under "Cell Configuration", with the mod it came from. Like FO4CheckPrevisbines' "Fix only Cell Config Conflicts". Worldspaces are merged the same way for their climate, water, LOD water, water environment map, music, map image, max height data, encounter zone, lighting template and location; their parent worldspace settings are never changed. A cell or worldspace whose winning mod stays in the load order (no output would write it) is written, merged, to `Multiplexer_Patch.esp`, which loads last; an exterior cell goes with its worldspace, so the patch also holds that worldspace (the game's current version when nothing changes it), unless an output already holds the worldspace, in which case the cell goes into that output. One whose versions bring nothing forward is not written. When such a cell has its own previs file, the file is translated into the numbering of the plugin now holding the cell, as for compiled cells; a file that cannot be translated leaves the cell as it is. **Large references** (worldspace RNAM) are kept as the game uses them today: when compiling would change which list is in force (the plugin that supplied it is compiled, and the winner has none), the written worldspace carries today's list, translated, and the Validation Report lists it under "Large References". When today's list cannot be told exactly (it depends on whether the game takes the last plugin's list or each cell's last list), the worldspace is left as it is with a warning.

An ID in a previs file that points to no record today (a record its plugin does not contain, or a master index past the plugin's master list) finds nothing in the game now. The translated copy gives it an ID in the output's own range, above every record the output holds, that finds nothing there either, one per distinct ID, so the game behaves as it does today. The "Previs Numbering" line for the file counts them and names the first few. Only IDs of records that exist but cannot be expressed in the output still keep the cell's plugins out.

**Cell inheritance:** as the game reads each version of a cell in load order, a non-partial version's record flags, PCMB and VISI become current (even when blank), its previs (XPRI) and precombine (XCRI) lists only when present and not empty, and a partial version (flag 0x4000) changes none of these (PJM's previsbine guide). Compiling removes the compiled versions and loads the written one last, so the compiler works out the lists the game uses today from the whole chain and writes them into the cell ("Cell Inheritance", Info, names where each came from). A cell is also written when it would otherwise equal the version that stays but a compiled version supplies its lists. Only a partial version written over a compiled version it takes its fields from cannot be expressed: then the plugins compiling that cell stay in the load order ("Cell Inheritance", in the Kept Out list).
| `Multiplexer_ArchivePriority.txt` | only when loose copies are off: files whose winning archive would change |
| `Multiplexer_ArchiveLoaders.txt` | MO2: which compiled plugins have an archive loader |
| `Multiplexer_DisablePlugins.txt`, `Multiplexer_plugins.txt` | plugins to disable (and, under "Enable these", plugins an earlier build compiled that this one keeps), and a proposed `plugins.txt` (for Vortex and manual installs) |
| `Multiplexer Reports/` | all reports (HTML + JSON) |

Then:

1. Install and enable `Multiplexer Output.zip` as above (replace the previous version when you rebuild).
2. On the **Compilation** page, press **Apply to Game…**. It first shows what it will change and what it checked, and changes nothing until you confirm. It refuses (and explains why) when:
   - the build did not finish successfully;
   - more than 254 full plugins (or 4,096 light plugins) would be active after switching over: past 254, plugins take another plugin's load-order index, which crashes the game or loads the wrong records;
   - **manual install:** the output plugins are not in the game's `Data` folder yet, or differ from this build (install the zip first), because disabling the compiled plugins before their replacement is installed would remove their content from the game.
   - the scripts and MCM configs the Advanced Users Auto-Merger rewrote (`MCM\Config\<mod>\config.json` and `keybinds.json`, `Plugin.esp|FormID` pairs, scripts calling `GetFormFromFile`) are not the copies the game loads: another mod's original still wins the file conflict, and it names a compiled plugin, so the MCM menu would look for records that are no longer there (empty menu, settings that do nothing). Under Vortex, resolve the conflicts so the output mod loads after the mods it conflicts with, then deploy; under MO2, give the output mod the higher priority. `Multiplexer_Instructions.txt` lists the files.
   It also warns when compiled plugins are still enabled: the game would then load their records twice. Every change is made together or not at all: each file is backed up first (`<file>.multiplexer-backup-<date>-<time>`, with `-2`, `-3`, … added rather than ever replacing an earlier backup), written whole through a temporary file, and if any step fails the files already changed are restored. Files whose content would not change are left alone. It then:
   - adds the compiled plugins' archives to `SResourceArchiveList2` in `Fallout4Custom.ini`, in the folder the game reads its INIs from (the MO2 profile when it uses profile-local INIs), so their scripts, textures and meshes keep loading. Archives an earlier build added but this one no longer needs are taken out again. Fallout 4 reads at most 1,023 characters of that line (it reads INI values into a 1,024-byte buffer; X-Cell logs it as `"sResourceArchiveList2":"Archive" Buffer overflow`), and every archive past that point silently never loads: the compiled plugins' scripts go missing (`Cannot open store for class ... missing file?` in the Papyrus log) along with their meshes and textures. So the compiler keeps the list within that length: full plugins first, then light ones, each in load order, and a plugin whose archives no longer fit stays in the load order (*Kept Out Automatically: Archive list full*). Apply to Game refuses to write a longer line, unless the build uses the **F4SE archive loader** (Settings → Archives): then there is no such limit, the package ships `MultiplexerArchiveLoader.dll` with the full list, and the game must be started through F4SE ([Archive Loader](ARCHIVE_LOADER.md)). With more than 255 general or 255 texture archives in the list, the game also needs an archive limit raiser that copes with archives from the list: BSA Limit Expander works; Daytripper 4's `ExtendedArchiveLimit` froze the game at the main menu with such a list (turn it off when using BSA Limit Expander; use only one limit raiser). Apply to Game checks this: it refuses to switch over while Daytripper 4's `ExtendedArchiveLimit` is on with such a list, and warns when it finds no limit raiser it knows. Entries in that line whose archive is in no `Data` folder (left by an earlier build, or mods since removed) load nothing, so they are left out and do not use up the space; the game's own archives are always kept. Every other line of the INI is kept as it was.
   - **Mod Organizer 2:** also rewrites the profile's `plugins.txt`/`loadorder.txt` so compiled plugins are disabled and the generated ones enabled, keeping every other plugin's order and state. Only plugins are switched off: the mods stay enabled, because their meshes, textures, sounds and scripts are still used (the summary says which mods the compiled plugins come from). It works with MO2 **closed** (it writes the profile when it exits and would undo the change), and only once the game would see the outputs: in an enabled mod of the profile (checked in `modlist.txt` as it is now, the highest-priority copy deciding), in overwrite or in `Data`, identical to this build. Two ways to get there:
     - your output folder is a folder in MO2's mods folder: Apply to Game offers to enable that mod in the profile (a new entry goes to the top of the left pane, so it wins every conflict; a disabled one is switched on where it is; every other line is kept);
     - otherwise it offers to copy `Multiplexer Output.zip` into MO2's downloads folder (from `ModOrganizer.ini`; allowed while MO2 runs), to install from the Downloads tab.
     Ghosting is never used with MO2. Settings → *Find Instances* lists the global MO2 instances for Fallout 4 (in `%LOCALAPPDATA%\ModOrganizer`) and lets you pick one; a portable instance is the folder you installed MO2 in.
   - **Manual install:** also rewrites `plugins.txt` (and `loadorder.txt` when there is one) the same way.
   - **Vortex:** in two passes.
     1. With your Vortex downloads folder set (on the setup screen, *Mod downloads folder*, or Settings → Installation → *Vortex downloads folder*: Vortex's `downloads\fallout4` folder; *Use Vortex's Default* fills in `%APPDATA%\Vortex\downloads\fallout4` when it exists, *Check* tests it), Apply to Game offers to copy `Multiplexer Output.zip` there, so it appears on Vortex's Downloads page (restart Vortex if it does not). If an archive of that name is already there you choose **Replace** (the old one is backed up first), **Keep Both** (`Multiplexer Output (2).zip`) or **Cancel**. Without the setting, drag the zip onto Vortex's Mods page. Install and enable it in Vortex, let it win every file conflict, and Deploy.
     2. **Close Vortex** and press Apply to Game again. Once the outputs are deployed into `Data` and Vortex is not running, it rewrites `plugins.txt` (compiled plugins disabled, outputs enabled at the end) together with the INI. Vortex keeps its Plugins page in step with `plugins.txt` and reads it back when it starts; check that page afterwards. While Vortex is running, or before the outputs are deployed, the load order is not touched.
   - **Diagnostic Recorder:** when the export prepared it (Settings → Diagnostic Recorder), the dialog also offers *Also copy the Diagnostic Recorder … to the downloads folder* (ticked by default, Vortex and MO2 with a usable downloads folder). `Multiplexer Diagnostic Recorder.zip` then appears beside the package on the Downloads page; install and enable it as its own mod. One copy is kept: an older one of that name is replaced (backed up first). Without a downloads folder the dialog shows where the zip is.
   - writes `Multiplexer_Instructions.txt` beside the package (also written at every export): the steps for your mod manager, numbered in one sequence, a **Load order** section, the checks above, and how to undo everything.
   - records what it changed in `Multiplexer_Applied.json` (in the folder holding the exports), for **Switch back**.

**Switch back** (Compilation page, shown after an Apply to Game; command line: `fo4mx switch-back <install> --yes`) undoes the last Apply to Game in one step: `plugins.txt`, `loadorder.txt`, `modlist.txt` and `Fallout4Custom.ini` return to exactly what they held before, from the backups Apply made, and ghosted plugins are renamed back. It first shows what will change, and names any of those files changed since Apply (by your mod manager or by hand), because switching back replaces those changes too. Like Apply, it is all or nothing and backs up what it replaces, so it can be undone as well. Close your mod manager first. The output mod and its files are left where they are (switched off with the rest), and a copy of the package in a downloads folder stays. Scan again before the next build.
   - **Optional (manual installs, and plugins Vortex did not deploy):** ghosts the compiled plugins (renames `Example.esp` to `Example.esp.ghost`) when you tick the box. Only a plugin this build compiled successfully, directly in the game's `Data` folder, unchanged since the scan, not needed as a master by any plugin that stays, and without an existing `.ghost` file is ghosted; the dialog lists every other plugin with the reason. `.ghost` names are never written into `plugins.txt` or `loadorder.txt`. Undo by renaming the file back. Ghosting is not offered under Mod Organizer 2 (it keeps disabled plugins off) and is never done to plugins Vortex deployed (Vortex would restore them).
   - **Ghosted plugins** (`Example.esp.ghost`, the name some tools give a plugin to hide it from the game) are recognised: the app reads them as `Example.esp`, not loaded by the game. A plugin the last build compiled stays a source of the next build when it is ghosted, as when it is disabled.
   **Where the outputs go in the load order:** after every other plugin, in their own order, with `Multiplexer_Patch.esp` (and any `Multiplexer_Patch_2.esp`, …) last. The build decides which version of each record wins assuming the outputs load last, so a plugin that loads after them wins again and can undo a merge. Leave every other plugin where Vortex, MO2 or LOOT sorted it: Apply to Game moves only the outputs, to the end of `plugins.txt` (and `loadorder.txt`, where there is one; an output an earlier build put elsewhere is listed once, at the end), and keeps everything else in its order. A later sort (Vortex's *Sort Now*, MO2's *Sort*, LOOT) can move them again. The next check then warns ("N enabled plugin(s) load after Dummy_Plugin_01.esp", or "The outputs are not in their own order") in the Apply to Game summary and in `Multiplexer_Instructions.txt`; press Apply to Game again to put them back. Empty archive loaders are ignored by this check: they have no records.
3. **Keep the compiled mods installed.** Disable only their *plugins*, never the mods themselves: their textures, meshes, archives, voice files and FaceGen are still used.
4. Open the outputs in FO4Edit and spot-check a few records.

### Archive priority

With archive loaders (MO2) nothing changes: every archive keeps its position. Without them, archives listed in the INI load before every plugin's own archives. Once compiled plugins' archives move into that list, an archive of a plugin that stays could start winning a file it used to lose. The app does not let that happen: each file a compiled plugin's archive wins today is written loose into the output folder (a loose file wins over every archive), so the same version still wins (`Multiplexer_ArchiveWinners.tsv`). A plugin that would need more than 2,000 such copies, such as a LOD pack, stays in the load order instead (**Kept Out Automatically**, "Archive priority: N files…"), and the compilation runs again without it. With Mod Organizer 2 and the output inside the mods folder, archive loaders keep every archive in place, so nothing is kept out for this reason. With loose copies turned off, any remaining change is listed under **Archive Priority** (up to 50, with the full list in `Multiplexer_ArchivePriority.txt`).

The comparison is always with your load order **as it was before this app changed the INI**: archives an earlier **Apply to Game** added count as their plugin's own. The app knows them from `Multiplexer_RegisteredArchives.txt` in the output folder, from the last build in its database, and from the oldest `Fallout4Custom.ini.multiplexer-backup-*` next to your INI (the INI before the first change). If you delete generated files between builds, keep those backups, or restore the oldest one first.

### What plugins that stay hold back

A plugin that stays in the load order keeps its masters there too, and their masters in turn. **Plugins → Holding Back** lists the plugins that stay and keep others there, most first; **Would Also Compile** is how many would compile if that plugin did (for example once its record types are supported, or if you allowed it). A plugin's details show the list. They also say when another plugin that stays keeps it as a master ("Kept by"), so compiling it would need that one first. Plugins kept by several plugins that stay are counted as **Shared** in the Dependency Report's "What plugins that stay hold back", and in no plugin's own count.

The **Master Redirection** column says whether that switch would free them, before you turn it on. It reads "Would free them" when the plugin can be redirected. "Possible, but needs Mod Organizer 2" means the only obstacle is where the output goes. "Not possible" comes with the reason, for example data the app cannot place, precombines, or another plugin listing it as a master.

### Unused masters

A plugin that stays in the load order keeps its masters there too. Patches often list a master without using any of its records. The **Dependency Report** (and the Dependency Graph's *Missing Requirements & Breakages* tab) lists each one as **Unused Master**: "C.esp lists A.esp as a master but uses none of its records".

A master counts as unused only when none of the plugin's records overrides one of its records, sits in one of its groups, or names it in a FormID the definitions locate. A byte equal to the master's index anywhere in data the definitions do not describe counts as using it, and a plugin that cannot be read uses all its masters.

With **Placeholders for unused masters** on (Compilation page → Compile more plugins, or `--master-placeholders`), such a master compiles anyway, and an empty light plugin with its name keeps the plugins that list it loading. It is written like an archive loader, so it needs the same setup: Mod Organizer 2 with the output folder inside the mods folder, the output mod at the bottom of the left pane, and the master kept enabled. Anywhere else the master stays in the load order, and its reasons on the Plugins page say why. You can also remove the master from the plugin in xEdit (Clean Masters).

### Master Redirection (advanced)

A plugin that stays in the load order keeps its masters there too, even when they could compile. Frameworks kept by one quest mod or patch are the usual case. With **Master Redirection** on (Compilation page → Compile more plugins, or `--redirect-masters`), such a plugin stays as a **copy in the output**, under the same name. The copy lists the output plugins instead of its compiled masters, and every FormID that names a compiled record is translated. Its masters then compile.

- **What the game shows does not change.** The copy loads after the outputs:
  - Its own records keep their FormIDs.
  - Overrides that the outputs or a later plugin also change are left to them, as today.
  - A cell holding its references is written as the game shows it today.
- **Only plugins the app can copy exactly:**
  - ESP files whose records all decode completely. Previs files of cells the copy holds are translated into its numbering and written loose, as for the outputs.
  - No other plugin lists them as a master.
  - Mod Organizer 2 with the output folder inside the mods folder (the copy must win over the original).

  The Plugins page says **Stays, redirected** for a copy. A master that stays because of a plugin that can't be redirected says why in its reasons. **Keep as it is (no redirection)** on the Plugins page leaves a plugin out of it.
- **Checks:** a copy that cannot be built faithfully is not written. Its masters stay and the build runs again. Each copy is read back before anything is written.
- **Undo:** the original file is never changed. Switching the output mod off restores it, and Apply's backups restore the load order. A rescan reads the original in its old place.

The Dependency Report's **Master Redirection** section lists each copy: masters replaced, FormIDs translated, records kept, cells written as today, and overrides left to others. [Master Redirection](MASTER_REDIRECTION_DESIGN.md) has the details.

### When some records cannot be compiled safely

If validation finds records that cannot be written safely, for example a FormID in data the app does not fully understand, the app does **not** block the whole export. It keeps those records' plugins in the load order (or leaves those records out of the patch) and compiles again. The **Validation** page lists every plugin it kept out under **Kept Out Automatically**, with the reason. The **Plugins** page shows them as *Retained*. Only errors that cannot be traced to a plugin or record still block export.

### Broken references

A reference the compiler cannot write faithfully (its master is not installed, or it names a record that its
compiled plugin does not contain, typically a patch made for another version of a mod) is never guessed. By
default the plugin that stores it is **kept out automatically** ("Broken reference: …", with the value the
plugin stores) and stays in your load order unchanged, so the game reads it exactly as before. To compile such
plugins anyway, turn on **Write broken references as null (lossy)** on the Compilation page: the references are
then written as null and listed under *Broken in Source*. How the game itself reads them is not verified, so
the compiled plugin can differ from the original; the better fix is a version of the plugin made for your
version of its masters.

### After restarting the app

The compiled build is held in memory, so after a restart there is nothing to export again; but **Apply to
Game** does not need it. Every export records what switching over needs in `Multiplexer_Build.json`: the
compiled plugins, the outputs with their size and content hash, the archive list, the settings and a snapshot
of the load order at export. After a **Scan**, the app reopens the latest finished export when:

- every output plugin (and the archive loader with its list, when used) is still in the export, byte for byte
  as written;
- the load order and the compilation settings still match the build (the same comparison as the change check;
  changes that do not affect the build, such as an unrelated mod, are allowed and counted). A re-sort (LOOT,
  Vortex sorting on deploy) is checked record by record: plugins moving past plugins they share no record with
  change nothing the build decided, so the build still reopens; if two plugins that edit the same record swap
  places, it does not, and the page names them;
- every plugin it compiled is still found.

The Compilation page then says *Last build reopened: <export>*, and **Apply to Game** and **Open Output
Folder** work on it. **Export** still needs a compilation (the records are not in memory). When the
export does not fit, the page says why (with the first change found) and you compile again. Exports made by
earlier versions of the app have no `Multiplexer_Build.json` and are not reopened.

The app clears its results database and build cache when the app itself is updated (merged records from an
older version must not be reused). The reopened build does not depend on them: it is checked against its own
`Multiplexer_Build.json`.

## Checking the record definitions

Every **Analyse** also checks the **Candidate** record types (described but not compiled yet) against your load order, and the Validation Report (Definitions category) says for each whether your files confirm it. Send that report to the developer: confirmed types are switched on in the next version. The full check below also covers the compiled types and the group layout.

The app needs the exact layout of every record type it compiles. This check confirms those layouts against **your** game files. Run it once after installing a new version of the app, and send the result to the developer if it shows warnings.

1. Start the app. On the **Setup** screen, check the folders and press **Save and Continue**.
2. In the left sidebar, click **Validation**.
3. At the top right, click **Check Record Definitions**.
   - The status bar at the bottom shows progress. A large load order takes a few minutes.
   - You do not need to Scan, Analyse or Compile first. The check scans by itself if needed.
4. When the status bar says **Check Definitions complete**, click the **Definitions** filter above the table.
5. Read the rows:
   - **Info:** one line per record type, for example "ARMA (Armor Addon): 2,310 records decoded, 9,120 FormIDs, 99.9% resolve". Close to 100% is good.
   - **Warning:** a layout that looks wrong (few FormIDs resolve, unexpected sizes) or a field the app does not describe yet. Records are still protected: a wrong layout can block an export, but it cannot produce a broken plugin.
   - **Candidates:** record types whose layout is described (from xEdit's definitions) but which are **not compiled yet**. They are checked too: "LAYR (Layer): candidate confirmed by your load order" means your files match the layout exactly. "candidate not confirmed: …" names what did not match, and the type stays out. Send the result to the developer: confirmed candidates are then switched to compile in the next version.
   - **Group Layout:** the check also rebuilds every plugin in your load order that has cells, worldspaces, quests or dialogue topics with the app's nested group writer and reads it back. Empty groups (the Creation Kit leaves some) are left out and only counted. A plugin that is itself faulty (the same FormID twice) is reported as such and not counted against the writer. Every record must come back with the same content in the same cell, block and sub-block, and the file must have the same groups. Plugins over 64 MB (Fallout4.esm, the DLCs) are checked by placement only: every cell must be in the block the writer would choose. One Info line sums it up; each plugin with a difference gets a Warning. Nothing is compiled from cells yet: this confirms the writer against the Creation Kit's own files first.
6. Click **Save…** to write the rows shown to a text file (default `DefinitionCheck.txt` in Documents), or **Copy** to put them on the clipboard.

![Definition check](images/14-definition-check.png)

The same **Copy** and **Save…** buttons work for any filter. For example, after a compile, click **Errors** and then **Save…** to send the errors that blocked an export.

Command line: `fo4mx check-definitions --mo2 "C:\Modding\MO2"` prints the same results (exit code 4 if there are warnings).

`fo4mx record-types` lists every Fallout 4 record type the app knows: name, category, where it lives in a plugin, its strategy (*Specialised*: compiled with fields merged; *Winner*: compiled, the winning version taken whole; *Candidate*: described and checked, not compiled yet; *Preserve*: stays in its plugin) and support level. The list comes from `resources/record_catalogue.csv`, built from xEdit's Fallout 4 record list.

## Cells, worldspaces and placed references

Mods change the world by **editing existing objects** (moving a vanilla rock, disabling a car, changing a cell's lighting, tweaking landscape), by **placing new objects in existing cells** (a new workbench in Sanctuary, clutter in a vanilla interior), or by **adding new places** (a player home interior, a new worldspace). The compiler includes all three (stages W2, W3 and W4):

- **What is compiled:** overrides of existing placed references (REFR, ACHR and the placed projectile types), landscape (LAND), cells (CELL) and worldspaces (WRLD); and **new placed references in existing cells**. New references get new FormIDs in the output, and every link to them from compiled records (linked references, enable parents and so on) is rewritten. A plugin that stays and refers to them keeps their plugin in the load order too (it is a master).
- **New references in precombines or previs:** a new reference listed in its cell's precombine (XCRI) or previs (XPRI) data keeps its plugin out: those meshes and files were generated for its FormID.
- **How it is written:** each edited reference is written inside its cell, and each exterior cell inside its worldspace, with the same nested groups the Creation Kit and xEdit use. They go into the last output plugin in use, kept together in clusters (an interior cell with everything in it, or a worldspace with all its cells); when a cluster's masters would not fit, the next output takes it.
- **Cells nobody compiled:** when a compiled plugin edits a reference in a cell whose current version comes from a plugin that stays in the load order, the output gets an exact copy of that cell as the game loads it now. The output loads last, so the copy changes nothing. The Compilation Report counts these as **Cells Copied**.
- **Precombined meshes and previs stay exactly as they are.** The game finds them by the plugin that *defines* the cell (`Precombined\<plugin>\<cell>_<mesh>_OC.nif`), and that plugin always stays.
- **New cells, worldspaces and landscape (W4)** get new FormIDs, with everything placed in them. Precombined meshes and previs files are found by the cell's defining plugin and FormID (`Precombined\<plugin>\<cell>_<mesh>_OC.nif`), which compiling changes, so a new cell with precombine or previs data (PCMB, VISI, XCRI, XPRI), or one whose previs file is such a cell, keeps its plugin out. Two new cells at the same grid position of one worldspace would collide in one plugin, so one of their plugins stays. An output holding new cells or worldspaces is never ESL-flagged: cells added by ESL-flagged plugins lose their changes across saves.
- **Precombined mesh and previs folders** (`Meshes\Precombined\<plugin>\`, `Vis\<plugin>\`) are read only for that plugin's own new cells that carry precombine or previs data, and such a cell keeps its plugin out by itself (above). So the folder alone never keeps a plugin out: when the plugin compiles, no cell uses those files, before or after. Moving precombines for a compiled cell is not possible: the previs files (`.uvd`) hold the FormIDs of the cell's references, in the plugin's own numbering, and compiling changes them.
- **What keeps a plugin out:** plugin-named folders the game uses that cannot move (LOD billboards, LooksMenu overlays: see *Filename references*); a new cell with precombine or previs data. The plugin's reason says which.
- **Saves:** new references, cells and worldspaces get new FormIDs, so a game saved with the original plugins loses them. Start a new game.

CELL, WRLD, REFR, ACHR, LAND and the placed hazards, projectiles, missiles and arrows were confirmed by a real load order (1,366 plugins rebuilt by the Group Layout check) and are compiled. PBAR, PBEA, PCON and PFLA did not occur in it (not even in the base game or DLC) and stay Candidates: a plugin that contains one stays in the load order, and the Definition Check says "candidate not present in your load order" for each that no plugin has. Why they are not promoted on xEdit's layout alone: [PLACED_RECORDS_PBAR_PBEA_PCON_PFLA.md](PLACED_RECORDS_PBAR_PBEA_PCON_PFLA.md). A Group Layout difference in a later definition check is reported, and keeps any world type that is still a Candidate out.

## Rebuilding after switching over

You do not need to switch back to the original load order to rebuild. Scans recognise the app's own files: Dummy plugins and patches are never read as sources, an archive loader is looked through to the original plugin underneath it, and plugins an earlier build compiled count as active sources in their original position even though they are disabled now. **Changes** and **Rebuild** therefore work straight from the switched-over load order.

What an earlier build compiled is not only kept in the results database (which a new version of the app clears, and **Reset to Fresh State** deletes). Every output of an earlier build in the load order, enabled or not and under whatever name prefix it was built, is matched with a record of the plugins it compiled:

1. `Multiplexer_CompiledPlugins.txt`, installed with the outputs (it is in the package's `Data` folder);
2. the export folders in the Output folder (also exports written before that file existed: their `Multiplexer_DisablePlugins.txt` and the plugins in their `Data` folder);
3. the results database.

The plugins it compiled that are switched off are read as sources, and the Validation Report lists them under **Earlier Build**. When an earlier output is in the load order but nothing records what it compiled, while installed plugins are switched off, compiling stops with an **Earlier Build** error instead of leaving those plugins out: switch them back on (they are in that build's `Multiplexer_DisablePlugins.txt`), or remove the old output from the game if it is no longer used, then scan again.

A plugin an earlier build compiled that this build keeps in the load order must be switched on again: **Apply to Game** does it, `Multiplexer_DisablePlugins.txt` lists it under "Enable these", and the switch-over check warns while it is still off.

**FormIDs stay the same from build to build.** Saves refer to compiled records by the outputs' FormIDs, so a rebuild keeps every record where the build in use put it:
- records keep their output and object ID (from the previous build's `Multiplexer_FormMap.tsv`);
- a new record that a record of the previous build now refers to goes into the same output or an earlier one, so the older record does not have to move to a later output (a light output takes it only while it stays within the light range);
- each output stays light (ESL) or full as it was in the previous build, whatever **Light outputs** says (that setting, and its room to grow, apply to new outputs), because switching changes the FormID of every record in it. It switches only when it must (more new records than a light plugin holds, or new cells or worldspaces).

When a record of the build in use would still get another FormID (it had to move, the output count went down, an output had to switch), compiling stops with a **FormID Stability** error while that build is in use. The warnings above it say which records and why. The Compilation page asks whether to compile anyway (for this build only), or use `--accept-formid-changes`. Do that only if you will start a new game or accept that saves made with the current build lose those records.

**Compiled plugins never return unasked.** Saves made with a build hold a compiled plugin's records under the outputs' FormIDs. If the next build cannot compile that plugin (most often because a new or updated plugin that stays in the load order needs it as a master, or the mod was updated with record types not supported yet), it would return to the load order, and those saves would lose what they used of it. So compiling stops with a **Returning Plugins** error that names each such plugin and why. The Compilation page then asks which may return: tick them and compile again (they are kept in the settings; save them with Save Settings). On the command line: `--allow-return <plugin>` (repeatable). Or remove or switch off what keeps it out and compile again. A plugin you exclude yourself (Plugins page) is your choice and is not stopped; neither is a patch-only build.

## Starting fresh

**Settings → Reset to Fresh State…** removes everything the app has stored: settings and folders, exclusions, conflict resolutions, the results database, the build cache and the logs. The Setup screen then opens as on a first start. Your mods, your load order and the generated plugins in the output folder are not touched.

When you start a **new build** of the app for the first time, it clears the previous build's results and cache automatically (your settings and conflict resolutions are kept), so nothing stale carries over.

## Keeping the build up to date

When the app starts it compares your mod space with the last exported build. The Dashboard's status line shows:

- **Your game is up to date** (Up To Date): nothing that matters changed.
- **Some files changed** (Changes Detected): only assets changed (new textures, an updated loose-file mod). The compiled plugins are still correct.
- **Your mods changed: rebuild needed** (Requires Rebuild): plugins were added, removed, updated or reordered, compilation settings changed, or a generated plugin was deleted or edited.

**Changes** lists every added, removed and updated mod, plugin and archive, load order moves, and whether each one requires a rebuild. It also shows which record types the changed plugins contain and which compiled types are unaffected. **Check Now** re-runs the comparison at any time.

**Rebuild** runs Scan → Analyse → Compile → Export in one step and reuses everything that did not change:

- unchanged plugins are not re-parsed;
- unchanged override chains are not re-analysed or re-merged;
- output plugins whose content is identical are not rewritten;
- outputs the new build no longer needs are removed;
- records keep their FormIDs (`Multiplexer_FormMap.tsv`); a record that must move is reported as a *FormID Stability* warning.

Compile into the same output folder each time.

## Assets

Compilation never copies, extracts or repacks assets. Compiled records keep their original mesh, texture and material paths, and the game loads those files from the mods that provide them. The only thing to do is merge `Multiplexer_Archives.ini`, so archives that belonged to compiled plugins keep loading from where they are.

On the Compilation page, select a record under **Generated Records** to see its generated plugin, source plugin, winning override, and every mesh, material and texture it uses with the mod, archive and path each one is served from. **Assets Used** lists all of them. Missing assets are reported in the Validation Centre.

## Likely causes

Every finding on the **Validation** page and under **Dependencies → Missing Requirements & Breakages** has a **Likely Cause** column. Select a row to read the full message and cause in the panel underneath. For missing references the cause is worked out from the data:

| What the app sees | Likely cause it reports |
|---|---|
| Many missing references from one plugin to the same mod, or from a patch | The patch was made for a different version of that mod; install versions that match |
| A plugin referring to its own record that is not there | The record was removed or renumbered in an update (or by ESL compaction) |
| An object ID above FFF in an ESL-flagged plugin | The reference predates that plugin being compacted and ESL-flagged |
| A record missing from Fallout4.esm or a DLC | Made for a different game version (the Next-Gen update moved records) |
| A FormID with an FE/FF prefix | A load-order FormID saved into the file by a script or tool |
| A FormID from a plugin that is not a master | Added in xEdit without adding the master, or a master removed afterwards |

**Missing References (summary)** rows group all references from one plugin to another (for example *ImprovedAREnclaveNWBBEPatch.esp -> BlackBirdElitePA.esp: 173 references*). Other categories (missing assets, archive priority, kept out automatically, missing masters and so on) carry a general explanation. Likely causes are included in **Copy**, **Save…** and the HTML and JSON reports.

## NPCs

NPC records compile: overrides of existing NPCs, and **new NPCs** with new FormIDs (start a new game). The game finds an NPC's face (FaceGen) as `facegeom\<plugin>\<FormID>.nif`, so the export copies the FaceGen meshes and face textures of every compiled new NPC into the output folder under the output's name and the NPC's new FormID (`Multiplexer_FaceGen.tsv` lists them). The face mesh still names its textures by their original path, so keep the original mod installed (its plugin disabled). New NPCs without FaceGen (creatures, robots, NPCs that take their face from a template) look the same compiled.

## Quests

Quests (QUST) and the story manager nodes that start them (SMQN, SMBN) are compiled (confirmed by a real load order: 11,474 quests, 99.9% of their FormIDs resolve):
- **Quests without dialogue compile** (setup, tracking and script quests, MCM quests). Stages, objectives and aliases are kept exactly in order, and every FormID in them, including the quest's scripts, fragments and alias scripts, is rewritten.
- **Start-game-enabled quests** are listed in `seq\<output>.seq` for each output, so the game starts them as before.
- **Dialogue compiles** (DIAL, INFO, SCEN, DLBR, confirmed by a real load order) in its quest and topic groups, with the quest in the same output. Voice files move with their lines: the game finds them by the plugin's name and the line's FormID (`Sound\Voice\<plugin>\<voice type>\<FormID>_1.fuz`), so the export copies the voice files of every compiled new line into the output folder under the output's name and the line's new FormID (`Multiplexer_Voices.tsv` lists them).
- **Saves:** quests get new FormIDs; start a new game.

## Locations and game settings

Locations (LCTN) and game settings (GMST) compile (both confirmed by a real load order); a setting changed by two plugins keeps the last one's value, as in game.

## Navmeshes

Navmeshes (NAVM) compile with their cells (confirmed by a real load order: 28,516 of 28,517 decode). The navmesh info map (NAVI) is not taken from the last plugin: the game combines every plugin's map as it loads, so the outputs carry one map combining the entries of every compiled plugin, a later plugin's entry for the same navmesh replacing an earlier one. Retained plugins keep their own map. A plugin whose navmesh data does not decode stays in the load order; its Dependency Report entry names the record. Start a new game after compiling navmeshes.

## AI packages

AI packages (PACK) compile (confirmed by a real load order: 13,096 packages, 99.9% of their FormIDs resolve), including their data inputs, procedure tree and script fragments. New packages get new FormIDs: start a new game.

## Filename references

Some files find a plugin by its **file name** rather than by its records: a script calling `Game.GetFormFromFile(0x812, "MyMod.esp")`, an MCM menu that requires `MyMod.esp`, a RobCo Patcher line `MyMod.esp|812`, FaceGen meshes in a folder called `MyMod.esp`. If that plugin were compiled away, those files would point at a plugin that is no longer loaded.

Every place an active plugin's name appears is found and **classified by what it is and how it is used**. The most restrictive class decides; **the number of references never does** (forty comment lines are still safe, one DLL is not).

| Class | Meaning | Examples | Plugin |
|---|---|---|---|
| **Safe** | Unaffected by compiling | precombined mesh and previs folders (the cells themselves decide), FaceGen and voice folders (moved with their records), comment lines in INI/TOML/TXT, Papyrus sources (`.psc`), a name in a script's string table that its code never uses, RobCo Patcher INI files whose name only contains a plugin name (not exactly `<plugin>.ini`) | eligible |
| **Rewritable** | Can be changed exactly and checked | `GetFormFromFile` with a fixed FormID, `IsPluginInstalled`, `Plugin.esp\|FormID` and `0xFormID:Plugin.esp` pairs, MCM `pluginRequirements`, Transfer Settlements blueprint entries (a plugin name with its FormID; the settlement's workbench when its base object stays), per-plugin FormID INIs (`BCR\<plugin>.ini`, written again as `BCR\<output>.ini`), RobCo Patcher configs named exactly `<plugin>.ini` (RobCo applies them only while that plugin is loaded, so the copy is written as `Multiplexer - <plugin>\<output>.ini` beside the original, which RobCo then skips; when that path would pass Windows' 259 characters, the folder is `Multiplexer - <8 hex digits>` instead, and a copy whose path still does not fit keeps its plugin in the load order) | eligible only with the Advanced Users Auto-Merger on |
| **Compatibility Required** | A game convention that would need the plugin's files moved (not implemented) | LOD billboard folders named after the plugin; LooksMenu overlay folders (`F4SE\Plugins\F4EE\Overlays\<plugin>`); `seq\<plugin>.seq`; `<plugin> - Geometry.csg` and `<plugin>.cdx` in the Data folder ("clean" precombines: the precombined meshes take their geometry from that file, which the game loads only while the plugin is active, and they store a hash of its name, so a compiled, disabled plugin's precombined meshes would lose their geometry); a script property in a plugin's record holding a plugin's file name (Sim Settlements 2 add-on configs check `GetFormFromFile(their own FormID, that name)` and reject the add-on when it does not match), except the case below | kept |
| **Safe** (compiler) | `Data\<plugin>.ini` (the game reads a mod's own INI only while its plugin is active) | its settings are carried, in load order, into the last output's INI (`Data\<output>.ini`), read while the outputs are loaded. Outputs load after every plugin that stays, so a plugin whose setting would end with another value (a plugin that stays sets the same setting later today) stays in the load order (Kept Out Automatically: Plugin INI) | compiled |
| **Safe** (compiler) | A Sim Settlements 2 add-on config's `sAddonFilename` naming the plugin that adds the config | the compiler writes the output plugin's name there, where the config then is, so SS2's check passes; it is SS2's only use of the name besides its log (Validation: "Script Property", Info). Configs without `sAddonFilename` are not tied to their plugin at all | compiled |
| **Unsafe** | Cannot be preserved | F4SE DLLs (compiled code), `GetFormFromFile` with a FormID computed at run time, blueprint data written without a FormID | kept |
| **Unknown** | Its meaning cannot be established | the name assigned, compared or joined in a script; a config naming the plugin without a FormID; other files or folders named after it; a script that does not read back exactly | kept |

Where they are found: script properties (VMAD) in the active plugins' own records, compiled scripts and Papyrus sources anywhere, configuration files under `F4SE\Plugins`, `MCM\Config` and `MCM\Settings`, F4SE DLLs (ASCII and UTF-16 strings), and every folder and file name, in loose files and archives. Names are matched whatever their letter case, as whole names (`MyModS.esp` is not `ModS.esp`); when a longer active plugin name ends at the same place (`Power Armor.esp`), it wins.

**Where to see them.** The **Dependency Report** (HTML, TXT, JSON) has a block per plugin: the number of references, each reference (up to 200 per plugin in HTML and TXT; JSON has all) with its type, use, location, classification and reason, then the overall classification, the reason and the recommended action. The **Asset Report** lists every reference, and the **Plugins** page shows them in the plugin's details.

## Advanced Users Auto-Merger

![Auto-Merger](images/18-auto-merger.png)

An optional, separate mode for experienced users (left sidebar, under Tools: **Auto-Merger**). It is off by default; the normal compiler never needs it.

**What it does.** With the Auto-Merger on, a plugin whose filename references are all **Safe** or **Rewritable** is compiled, and exactly its Rewritable references are changed, in copies written into the output folder:

- **Scripts (.pex):** `GetFormFromFile` gets the output plugin and the record's new FormID; `IsPluginInstalled` gets the output that holds most of the plugin's records. Each changed use points at its own string, so a string the script uses elsewhere is never renamed. A FormID the plugin does not define is left as it was (the game found nothing before either).
- **Configs:** `Plugin.esp|FormID` pairs get the output plugin and the new FormID, with the same number of digits; MCM `pluginRequirements` entries get the output plugin.

Nothing else in a file changes, and compiled files are never patched by blind byte replacement: every change is made at the exact place the scanner recorded.

**Plugin name and FormID pairs (report only).** Sim Settlements 2 keeps many plugin names in structs with `sPluginName` and `iFormID`; those still keep their plugins out. The Auto-Merger Report has a section that shows how every script uses those two members, a verdict (Clean or Not clean), and which plugins nothing but these pairs keeps out. It does not change what is compiled; it is the evidence needed before such pairs could be rewritten.

**Checks (fail closed).** During **Compile**, before anything is written, every copy is:
1. scanned again;
2. checked to contain exactly the expected changes;
3. checked that no reference to a compiled plugin remains that depends on its file name;
4. checked that every reference to other plugins, and every safe one, is unchanged;
5. compared byte for byte with the original outside the changed places (scripts must also read back exactly);
6. checked to name only outputs of this build.

A copy that fails any check keeps its plugins in the load order (**Kept Out Automatically**, with the reason) and the compilation runs again without them.

**Export is a transaction.** All copies are written beside their targets and read back; only then are they moved into place. If any step fails, nothing changes: earlier copies are restored and the error is shown. `Multiplexer_Relinked.tsv` in the output folder lists every change: file, written and original checksums, plugin, location, use, old value and new value.
- **Your mods are never changed.** A file already in the output folder that this app did not write is never replaced: its plugin is kept out instead (with Vortex or a manual install, where the output folder is Data, choose a separate output folder).
- Copies must override the originals: in Mod Organizer 2 keep the output mod below (after) the mods it relinks.
- **To undo**, turn the Auto-Merger off and export again, or delete the files listed in the manifest. Copies changed since they were written are left alone.
- The output folder's own copies are never read as originals when scanning.

The tab lists every plugin with filename references (**Kept Out**, **Kept Out by a Failed Check**, **Relinkable**, **Compiled and Relinked**), every file with its classification and result, and every change. The **HTML / JSON / TXT** buttons save its own report, also written by **Export All** as `AutoMergerReport`. On the command line: `--auto-merge`.

Start a new game when using it, and keep backups of your mod setup.

## Reports

**Reports** previews each report and exports it as HTML, JSON, TXT or PDF. **Export All** writes every report to a folder in all three file formats (HTML, JSON and TXT). The TXT reports are plain text with aligned columns, for reading in any text editor. The command line's `--reports DIR` writes them too.

## Diagnostics (optional Diagnostic Recorder)

![Diagnostics](images/17-diagnostics.png)

`MultiplexerDiagnosticRecorder.dll` is an optional F4SE plugin that records a small, fixed set of events while you play: the game, F4SE and recorder versions, F4SE's start-up, game data, new game, save loading and saving messages, the active plugins as the game reads them, which F4SE plugins loaded (with F4SE's own reasons for those that did not), and whether the game exited normally. It changes no plugin, save or game data and installs no hooks; it is not a crash logger (keep Buffout 4 for crash logs).

1. **Settings → Diagnostics:** turn on *Prepare the Diagnostic Recorder with each export*.
2. **Export.** `Multiplexer Output\Diagnostic Recorder\` holds the DLL, *Multiplexer Diagnostic Recorder.zip* and install notes, beside the package, never inside it.
3. Install the zip as a mod (Vortex or MO2), or copy the DLL into `<game>\Data\F4SE\Plugins` (F4SE loads plugins from that folder only, never from the game folder itself). Start the game through F4SE.
   The **Diagnostics** page shows whether it loaded (green *loaded*), read from F4SE's `f4se.log` of the last game start; **Check Installation** gives the details: the DLL found in a folder F4SE does not read, F4SE's own reason for refusing it, or the game not started through F4SE. The recorder also writes `Documents\My Games\Fallout4\F4SE\MultiplexerDiagnosticRecorder.log`, a short plain-text log beside the other F4SE plugin logs: loaded, recording to which file, main menu reached, game exited normally. `fo4mx diagnose --install` does the same check from the command line.
4. **Diagnostics → Open Newest Log.** The **Archive compatibility** section compares the archive list in your INI with what the game received when it read it and with the BA2 files it actually opened: requested, verified and missing archives, and the first missing one with its position in the list (a list the game cut short shows exactly where). Each finding gives a likely cause, its evidence and what to check: the game stopping while loading a save, while saving or while loading the plugins; F4SE plugins that did not load and why; compiled plugins still enabled, output plugins not enabled, or a load order that differs from the one Multiplexer built for.

Logs are in `Documents\My Games\Fallout4\F4SE\MultiplexerDiagnosticRecorder` (the newest 10 sessions). Paths in your user folder are written as `%USERPROFILE%`, and saves by name only, so a log can be shared. `fo4mx diagnose` does the same from the command line. [Diagnostic Recorder](DIAGNOSTIC_RECORDER.md) has the details.

## Command line

Everything is also available through `fo4mx` (run `fo4mx help`), for example:

```text
fo4mx compile --mo2 "C:\Modding\MO2" --profile "Test" --outputs 16 --strategy type --dry-run
```

`--dry-run` stops after validation. `fo4mx status …` prints the build status and every change since the last build (exit code 3 when a rebuild is required, handy for scripts). `--no-cache` forces a full rebuild.

## Troubleshooting

| Symptom | What to do |
|---|---|
| A mod you expected to compile is ineligible | The Plugins page shows the reason, and **Blocking Types** shows which unsupported types hold back the most plugins. Most often a script or config names it by file name, or a plugin that stays needs it as a master: **Holding Back** shows which plugins that stay keep the most others out. |
| You want to be sure the record layouts match your game | **Validation → Check Record Definitions** decodes every supported record in your load order and lists any layout that looks wrong under **Definitions**. Findings never block export, and compiled records are always protected by validation. |
| Validation: "Undescribed Data … bytes the record definitions do not describe" | The record holds bytes no layout describes, and they cannot be shown to mean the same in the output. The bytes are preserved, but not safe to move, so the plugin stays in the load order. Describing that subrecord (from xEdit) lets it compile; please report the record type and subrecord. |
| `MULTIPLEXER_BUILD_FAILED.txt` in the package folder | The last export failed (a plugin could not be written, the written files did not match what was compiled, or a relink failed). Its plugins and zip were removed; do not install the folder. Fix what the note names and build again. |
| Validation: "Only N% of FormIDs in X Y resolve" | The field layout for this runtime looks wrong. Please report it with the plugin name. |
| Missing textures in game after switching | Merge `Multiplexer_Archives.ini` into `Fallout4Custom.ini`. |
| A localised mod shows empty names | Its `Strings\<plugin>_<lang>.STRINGS` files were not found. Set the right language in Settings. |

Logs are written to `%LOCALAPPDATA%\Fallout4MultiplexerRenewed\logs`. The **Log** button in the status bar shows them live.

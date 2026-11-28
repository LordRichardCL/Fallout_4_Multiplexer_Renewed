# Changelog

Versions are tagged `vMAJOR.MINOR.PATCH-alpha` while the project is in alpha. Each entry says what was **tested** (and how), not only what was added; [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) keeps the current state. Pushing a version tag makes CI publish a GitHub pre-release with the Windows package and this version's section as its notes; a release created by hand on GitHub (which creates the tag) gets the package attached instead.

Hotfixes: a `v<version>-hotfix<N>` tag publishes a zip holding only the files that changed since that release (listed in docs/release/HOTFIX_FILES.txt), at the same paths as in the release. Extract it over the release's folder and replace the files.

Release policy (alpha):
- A version is tagged only after a build on a real load order has 0 validation errors and its export passes the written-content check.
- Each entry has a **Tested** line: build verified, confirmed in game, or not yet.
- Anything not yet confirmed in game stays off by default (Advanced preset).

## Unreleased

Added
- **Fallout 4 Multiplexer Load Monitor** (`MultiplexerLoadMonitor.dll`, v0.1), an optional F4SE plugin that only observes. It shows and logs which stage of start-up or a load the game is in, and the file it was last seen opening or reading. Its sources are F4SE's messages, F4SE's Papyrus and Scaleform callbacks, and observing wrappers of the game's own file imports; it changes nothing. A small overlay in the game window's corner shows the stage, the current and previous item, and how long it has been on the current item. A watchdog notes silences in the log. The last observed operation is not a confirmed cause of a hang. See [docs/LOAD_MONITOR.md](docs/LOAD_MONITOR.md).

**Tested:** unit tests for its core; the DLL compiles without warnings. Not yet run in game.

## 0.2.0-alpha-hotfix3 (27 Nov 2026) — Hotfix 3 for 0.2.0-alpha (includes Hotfixes 1 and 2)

**Install:** close the app, extract `Fallout4MultiplexerRenewed-v0.2.0-alpha-hotfix3-windows-x64.zip` over the 0.2.0-alpha folder and replace the files. It holds only `Fallout4MultiplexerRenewed.exe` and `fo4mx.exe`. Hotfix 2's licence files are not in it: install Hotfix 2 first if you have not (or skip them; the programs are the same either way). The app shows "v0.2.0 alpha · Hotfix 3". Then scan, compile and export again.

**Tested:** automated tests, and the owner's own files run through the app's code: every OMOD record of CombatRifle Receivers.esp decodes, the one with extra bytes keeps only those 7 zero bytes undescribed; Respawnable Legendary Bosses.esp's 43 placed NPCs decode completely; ghoulification.esm with its Program and HUDFramework scripts gives exactly one group (GHUL_Program and GHUL_HUDFramework, the two quests the 24 Nov build put into different outputs). Not yet a new build on the real load order.

Fixed
- **OMOD DATA with bytes past its own layout** (CombatRifle Receivers.esp: `mod_CombatRifle_Receiver_10mm` has 7 zero bytes after its last property). The whole subrecord used to stay undescribed, and its property count, read as a FormID, kept the plugin in the load order. Now the described part keeps its references and only the extra bytes stay undescribed; zero bytes cannot name a record, so the plugin compiles. Extra bytes that could name a record still keep it out.
- **Worldspace cell offset tables (OFST)** were copied into the output plugin unchanged. The table lists where each cell sits in the plugin file it was saved in, so in an output it points at the wrong place. It is now written as zeros (no offsets), as Bethesda's DLC and Creation Club plugins write it in their own worldspace overrides; the game then finds the cells itself. The overrides of the Commonwealth and Diamond City in the owner's build already had zeros; this covers mods that add a worldspace with its own table (Penns Woods has none).

## 0.2.0-alpha-hotfix2 (24 Nov 2026) — Hotfix 2 for 0.2.0-alpha (includes Hotfix 1)

**Install:** close the app, extract `Fallout4MultiplexerRenewed-v0.2.0-alpha-hotfix2-windows-x64.zip` over the 0.2.0-alpha folder and replace the files. It holds only what changed: `Fallout4MultiplexerRenewed.exe`, `fo4mx.exe`, `LICENSE` and `LICENSE.txt`. It includes everything in Hotfix 1, so it is the only hotfix to install. The app shows "v0.2.0 alpha · Hotfix 2". Then scan, compile and export again, install the new output package as before, and check the MCM menus in game.

**Tested:** automated tests; the fixes come from the owner's build of 24 Nov 2026, where these plugins stayed in the load order. Not yet a new build on the real load order.

Fixed
- **Plugins whose scripts find their records through one name** (a function such as `GetPluginName()`, or one name kept in a variable) stayed in the load order when the build put those records into two outputs: the rewritten name can point at only one (ghoulification.esm, MUTATION.esm and REPUTATION.esm on the owner's load order). With the Auto-Merger on, the compiler now keeps every record one such name finds in the same output plugin.
- **ACHR/REFR XHLP** (a health value, a float) was undescribed: Respawnable Legendary Bosses.esp's 3500.0 looked like a FormID and kept the plugin out. xEdit's Fallout 4 definitions do not list it either; it is now read as a number.
- When a subrecord's bytes are undescribed, the finding now gives the decoder's reason (for example why an OMOD DATA layout did not fit), so the layout can be completed from the report alone.

Package
- The download now includes `LICENSE` (the project's licence) and `LICENSE.txt` (the Mozilla Public License 2.0 notice for xEdit).
- Hotfix zips hold only the files that changed, at the release's own paths (Hotfix 1's zip had them in a subfolder, so extracting it over the release folder did not replace the programs).

## 0.2.0-alpha-hotfix1 (24 Nov 2026) — Hotfix 1 for 0.2.0-alpha

**Install:** extract `Fallout4MultiplexerRenewed-v0.2.0-alpha-hotfix1-windows-x64.zip` over the 0.2.0-alpha folder and replace the files (only `Fallout4MultiplexerRenewed.exe` and `fo4mx.exe` change). The app shows "v0.2.0 alpha · Hotfix 1". Then scan, compile and export again, install the new output package as before, and start a new game or load a save made with the build.

**Tested:** automated tests (a Vortex install whose Data folder holds only the earlier build's rewritten MCM config, its original only in Vortex's staging folder) and against the owner's build of 24 Nov 2026, whose report showed the problem; not yet a new build on the real load order.

Fixed
- **MCM menus showing "Missing plugins" after a rebuild (Vortex).** Vortex puts only the winning copy of each file in the Data folder. When the previous build's rewritten copies of scripts and configs were correctly deployed, the next scan found no original to read beside them and left those files out without a word: the plugins they name compiled without their references, the new package had no rewritten copies, and once it replaced the old one the mods' original configs named plugins that were switched off. On the owner's load order 212 files were affected (75 MCM configs, 96 F4SE configs such as RobCo Patcher and LooksMenu presets, 29 scripts and 12 INIs). The original is now read from Vortex's staging folder (`stagingPath` in vortex.deployment.json), so the rebuild rewrites them again.
- When no original can be read at all, the plugins the earlier copy was made for stay in the load order with the reason given (fail closed), instead of compiling with the file unseen.

## 0.2.0-alpha (24 Nov 2026)

**Tested:** build verified on 22 Nov 2026 (Vortex, Fallout 4 1.10.163, 2,270 plugins: 1,401 compiled into 7 outputs and a patch, 310,224 new FormIDs, 0 validation errors, written plugins verified, Apply to Game switched the load order over). In game: first session (about 9 minutes from a save, normal quit, all 421 archives loaded, no Papyrus failures tied to compiling); a second build with every setting on (1,668 compiled) ran a new game for about 30 minutes without a crash, all 689 archives loaded. A third build the same night (1,668 compiled into 5 outputs and a patch, 791,783 new FormIDs, 0 validation errors, 153 full and 623 light plugin slots) started a new game and played through character creation and Vault 111 into the Commonwealth (about 21 minutes, six saves, normal quit): all 689 archives loaded, no script property pointed at a missing record in the outputs, and the script errors on output records are ones the source mods also have. A baseline comparison without the build is still to follow. On 24 Nov 2026 Switch Back restored the original plugins on the same real install. The changes added on 24 Nov (Check a Mod, the relinked MCM form check, merged and carried changes in Check a Mod, the searchable plugin filter, Winner for Listed, and the Check Startup fixes) are covered by the automated tests and were run against that session's logs, but not yet by a new build on the real load order.

Safety and testing
- Presets from least to most change: **Safest** (the first-run default), **Recommended**, **Advanced**; an alpha notice on the Compilation page.
- **Ready to Test?** before Apply to Game: validation, export, plugin slots, your newest save, F4SE, a crash logger.
- **Switch Back** undoes the last Apply to Game in one click.
- **Check a Save** says whether a save would lose anything with the load order or the next build.
- FormID Stability and Returning Plugins stop a rebuild that would cost saves their records.

Diagnostics
- Crash logs (Buffout 4) and Papyrus logs name the original mod behind compiled records; readings warn when a log is older than the build.
- Support bundle: one zip with the reports, logs, readings and load order, personal details removed.
- Build comparison: what a rebuild changes against the previous build.
- Check Startup (Diagnostics): which start-up quests of the build ran or could not attach, the message boxes they hold, popups that found no message, and the MCM menus whose required plugin was relinked, from the Papyrus log, MCM.log and f4se.log's load order. `fo4mx startup-check`.
- Check a Mod (Diagnostics): for a mod that doesn't seem to work, the later plugins that overwrite its changes (and its patches' changes), with the records, the patch that held them until then, and what to do. `fo4mx overwritten`.
- Auto-Merger: every rewritten `Plugin|FormID` pair (MCM configs included) is checked to name the same record as before, by type and Editor ID, against the build's map and the records the outputs hold; a mismatch keeps the plugin out. The report says how many pairs were checked.
- Check a Mod counts apart the changes a build's merge keeps (lists and other merged fields), which are lost only without a build.
- Check a Mod also counts apart the changes the last build carried with the Compatibility Patch option.
- Conflicts: **Winner for Listed** makes the plugin chosen in the Plugin filter win every listed record it takes part in when compiling (the per-record "Winner when compiling" choice for a whole list, after a confirmation), or puts them all back to automatic.
- Conflicts: the Plugin filter is a search. Type any part of a plugin's name to see the plugins that match. The Check a Mod picker searches the same way.
- Check Startup reads the plugin list f4se.log has after loading a save (not only after saving one). Without a load order it no longer counts every quest as holding a message box.
- The User Guide explains the Faster Loose Files warning that appears while the Diagnostic Recorder is installed (harmless: the recorder passes every file open on).

Faster
- An app update keeps the build cache's parts whose code did not change (plugin index, references, conflicts, merges, each versioned by the code that produces it), instead of deleting the whole cache; the results database keeps the last build's record. Damaged entries are recomputed; an unreadable cache file is set aside and recreated.
- Compile anyway (FormID changes accepted) keeps the compilation that just stopped instead of compiling again, when nothing else changed since; any other change compiles again.

Scanning
- Scan asks what to read, with the counts: what the game loads (always what the build uses) or also a preview of inactive plugins (would it compile, what would it conflict with). Settings → Scan; `--scan-scope`.
- Inactive plugins are hidden on the Plugins page until *Show inactive plugins* is on.

Conflict Centre
- Accept Automatic for one record or for every waiting one; a Review column and filters for records waiting for you and records with your choices; manual choices confirmed as saved; a roomier list.
- Applying a build to the game is no longer reported as a change that needs a rebuild.

Compiling
- More plugins compile: plugin INIs, unused masters (placeholders), LOD billboards, constants and cross-script calls in scripts, Master Redirection (experimental).
- Precombined Cell Check (read only) for new cells with precombine or previs data.
- About ten times faster analysis on large load orders; readers refuse damaged files instead of crashing.

Fewer files
- Each export writes its reports as three files in `Multiplexer Reports\` instead of eighteen: `Multiplexer Report.html` (every report, with a contents bar), `.txt` (the same as plain text) and `.json` (every row). The Reports page still exports a single report in any format.
- A smaller download: the Windows package leaves out libraries the app never uses (OpenGL and Direct3D shader compilers, unused database drivers) and carries the Visual C++ runtime as three DLLs instead of its 25 MB installer. CI starts the packaged app on its own before publishing, so a missing DLL stops the release.

Interface
- Dashboard steps are the actions; plugin slots; the status bar shows step n of N and the time left; the build status updates while the app is open.
- Diagnostics organised by situation; Validation explained in the User Guide.

## 0.1.0

First public code: scan, analyse, compile and export for Mod Organizer 2, Vortex and manual installs; reports; Diagnostic Recorder; Auto-Merger. **Tested:** builds on real load orders (see docs/RECORD_HANDLING_AUDIT.md); in game: early results only.

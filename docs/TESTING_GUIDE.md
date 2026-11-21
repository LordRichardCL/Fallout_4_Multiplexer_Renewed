# In-Game Testing Guide

A checklist for trying a build in the game, and what to send back when something is wrong. Work on a **test profile** (a copy of your Mod Organizer 2 profile, or a backup of your setup) and start a **new game** for every build.

## Before you start

1. **Settings:** keep a crash logger installed (Buffout 4). Optionally turn on *Prepare the Diagnostic Recorder* (Settings → Diagnostics) and install its package.
2. **Build:** Scan, Analyse, Compile (Recommended preset), Export.
3. **Read Validation:** 0 errors before going on. Note the warnings.
4. **Apply to Game.** Read the preview first. If anything goes wrong later, **Switch Back** (Compilation page) restores your setup in one click.

## 1. The game starts

| Check | Expected | If not, send |
|---|---|---|
| Reaches the main menu | Yes | the crash log (Diagnostics → Read Crash Log → Save…), `Multiplexer_Instructions.txt` |
| New game starts and the intro plays | Yes | as above, plus the Diagnostic Recorder log if installed |
| Papyrus log has no `Cannot open store for class` lines for compiled mods | None | `Papyrus.0.log` and its reading (Diagnostics → Read Papyrus Log), Validation report |

## 2. Compiled content works

Pick one mod of each kind that the Plugins page says **Will compile**.

| Kind | What to check |
|---|---|
| Weapon / armour mod | It appears in its crafting bench or vendor list; its model and textures load (not purple, not invisible) |
| Leveled lists | Items from two mods that both add to a vendor or loot list both appear (merged) |
| Quest mod | Its quest starts as described; dialogue plays with voice |
| NPC mod | The NPC's face matches the original (no dark face or wrong head) |
| World edit (new or changed objects in a cell) | The objects are there and placed as before |
| MCM menu of a compiled mod | The menu opens and its settings work (if the Auto-Merger rewrote it) |

If something is missing or broken, note the mod, the item or NPC, and where. Send the **Dependency Report** and **Compilation Report** (Reports page) along with it.

## 3. Things this app changes on purpose

| Feature (if on) | What to check |
|---|---|
| **Merge cell settings** | Lighting and water in a cell two mods both edit look like a mix of both, not just the last mod's |
| **Translate previs files** | Cells with precombines render without flicker or missing walls; walk through a few |
| **Light outputs** | The game shows the outputs as light (ESL) plugins; nothing else changes |
| **Placeholders** | Plugins that listed the compiled master still load (no "missing master") |
| **Master Redirection** | The redirected plugin's content still works exactly as before (its quests, items, placed objects). In xEdit, the copy (in the output mod) lists the Dummy plugins as masters, not the framework |
| **Plugin INIs** | Settings a compiled mod's INI made (grass density, camera...) still apply |

## 4. Saving and rebuilding

1. Save, quit, load: everything is still there.
2. Update or add one small mod, then **Scan → Analyse → Compile → Export → Apply** again. Compiling must not stop with *FormID Stability* or *Returning Plugins* unless the build says why.
3. Before loading, Diagnostics → **Check a Save…** on the save from step 1 should say nothing is lost.
4. Load the save from step 1: compiled items, NPCs and quests from before are still there.

## 5. When something goes wrong

1. **Switch Back** first, if you want your game back at once.
2. **Easiest:** Diagnostics → **Support Bundle…** puts everything below (crash log and its reading, reports, instructions, recorder log, load order) in one zip. Send it with a note of what happened and where.
3. **Crash:** Diagnostics → Read Crash Log names the original mod behind each output FormID. Save the reading and send it with the crash log.
4. **Not a crash, but wrong:** note what and where, then send (or the support bundle):
   - the Reports folder of the export (`Multiplexer Reports`);
   - `Multiplexer_Instructions.txt`;
   - the Diagnostic Recorder log, if installed.
5. **To narrow it down:** exclude the suspected mod (Plugins page), rebuild, and test again. Or compile only some mods ("Only the ones I choose" on the Compilation page) and widen the selection step by step.

## What results help most

- The game start (section 1) with the Recommended preset.
- One mod of each kind in section 2.
- Master Redirection on a quest mod that keeps a framework in the load order (the Holding Back view shows candidates).
- A rebuild after one mod update (section 4).
- Any **Kept Out Automatically** warnings in Validation, with the plugin names.

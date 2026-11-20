# What's New

Recent changes in plain words, newest first. The [User Guide](USER_GUIDE.md) has the details of each.

## 20 November 2026

### Master Redirection (advanced, new)
- A framework that could compile, but stays because a plugin that stays uses its records, can now compile. That plugin stays as a copy in the output, with the same name, that points at the output plugins instead. The copy loads after the outputs, and the game shows the same thing as before.
- It is off by default (Compilation page → Advanced) and works with Mod Organizer 2 only. It covers only plugins the app can copy exactly. Every copy is checked and read back, and switching the output mod off restores the original.

### Your saves are safer across rebuilds
- **FormIDs stay put.** A rebuild keeps every FormID the build you play with gave. New records that older ones now use go beside them rather than forcing the old ones to move. If a FormID would still change, compiling stops with a **FormID Stability** error and asks first.
- **Compiled plugins never come back unasked.** If a new or updated mod would put a compiled plugin back into the load order, which would cost saves what they used of it, compiling stops with **Returning Plugins** and lets you choose.
- **Light outputs leave room to grow.** A new output is ESL-flagged only if 200 of its 2,048 new-record places stay free (adjustable). An output never switches between light and full on its own, so this stops a small mod update from forcing that switch, and the FormID changes it would bring, later.
- **Form lists keep their order.** Entries several mods add to the same form list are kept in place, which matters for lists that scripts read by position.

### More plugins compile
- **Unused masters.** A plugin that stays but lists a master without using any of its records is reported. With **Placeholders for unused masters** on (Mod Organizer 2), that master compiles anyway.
- **Plugin INIs** (`<plugin>.ini`) of compiled plugins are carried into the output's INI. Their settings end up with the same values, and a plugin is kept only if its INI would otherwise change a final value, or sets archive lists.
- **Scripts read more precisely.** Plugin names are now followed through named constants, joined text, functions that return them, Auto property defaults, and function calls into other scripts. More plugins are cleared as safe, and anything that cannot be followed still keeps its plugin out.
- **New quests, dialogue and NPCs** have their own switch (Compilation page → Advanced). Turn it off in the middle of a playthrough to keep such plugins as they are.

### You choose what compiles
- **Plugins to compile** (Compilation page): compile everything eligible with a switch to leave some out, or **only the ones you choose**, such as just your weapon mods. A plugin left out keeps its masters in the load order too, and the list shows that straight away.
- **Holding Back** (Plugins page) shows which plugins that stay keep the most other plugins out, and why.

### Easier to use
- **Plain status words** everywhere: Will compile, Stays in load order, Kept by you, Base game, Not enabled.
- **Sort any table** by clicking a column header, and right-click a header to show or hide columns.
- **Go to (Ctrl+K)** jumps to any page, plugin or record by typing part of its name. Plugin names in reasons are links.
- **Glossary (F1)** explains the words the app cannot avoid, such as master, override and ESL.
- **Stop** cancels a running task, and the status bar shows the time left.
- **A summary card** after each task says what happened and offers buttons to what needs you next.
- **The setup screen** explains what will and will not happen to your game before you start.

### Fixes
- Scripts calling a native, missing or untyped function with a plugin name are no longer treated as safe.
- Quick search no longer opens the previous result when Enter is pressed straight after typing.
- The Holding Back view no longer hides its key column.
- Windows builds survive temporary download outages (cached packages and retries).


## 18–19 November 2026

- **Compatibility patch:** changes inside structs (for example weapon stats) are traced and carried member by member, and patch choices are made per change in the Conflict Centre.
- **Script pair evidence:** reports show how scripts use plugin name and FormID pairs (Sim Settlements 2 add-ons, for example). This is evidence only and changes nothing.
- **Tool configs:** `0xID~Plugin` entries are read correctly.
- **Worldspace large references** are kept as the game uses them today.
- **New layout** for the Dashboard, the Compilation page, the navigation and every other page.

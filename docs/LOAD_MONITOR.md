# Fallout 4 Multiplexer Load Monitor

`MultiplexerLoadMonitor.dll` is an F4SE plugin that **only watches**. It logs and shows which part of the start-up or a load the game is in, and which file it was last seen working on. It is for narrowing down a long load or a hang. It changes nothing: not the load order, records, archives, file loading, engine limits or saves. Every call it observes is passed on with the same arguments, and the game gets back the same result and error code.

> **LAST OBSERVED OPERATION != CONFIRMED CAUSE OF HANG.**
> The monitor sees F4SE events and the game's file activity. Most of the game's work happens in memory: building forms, linking references, loading cells and running scripts. While that work goes on, no file is opened or read, so nothing here can see it. When the game goes quiet, the last item shown is where activity **stopped being visible**. It is not proof of what the game is stuck on. Use it to choose what to test next, for example by removing that plugin and loading again.

## Install

1. Copy `MultiplexerLoadMonitor.dll` into `Data\F4SE\Plugins\`. It is in the release package.
2. Optionally, put `MultiplexerLoadMonitor.ini` next to it (see Settings).
3. Start the game through F4SE.

The log is written to `Data\F4SE\Plugins\MultiplexerLoadMonitor.log` and is replaced at each start. If that folder can't be written to, the log goes to `Documents\My Games\Fallout4\F4SE\` instead. Under MO2, files the game writes into `Data` land in the `overwrite` folder.

To remove it, delete the DLL. It leaves nothing behind except its log.

## What can be observed (research for v0.1)

The mechanisms were checked against the F4SE source (`PluginAPI.h`, `Hooks_Papyrus.cpp` and `Hooks_Scaleform.cpp`, in F4SE 0.6.23 and the current 0.7.x). Timings come from the Multiplexer Archive Loader's start-up trace on a real load order of 2,587 plugin files.

| Event | Observable? | Method | Requires hook? | Earliest point | Risk / limitation |
|---|---|---|---|---|---|
| ESP/ESM/ESL loading | **Partly.** Opening a plugin file and reading from it are seen. Parsing the records in memory is not. | Wrap Fallout4.exe's `CreateFileW`/`CreateFileA` import, plus `ReadFile` and `CloseHandle` to attribute reads. | IAT wrapper of kernel32 imports. No game code is patched. | Plugin files start opening about 14 s after F4SE load. F4SE plugins load before that. | Plugin files were opened within about 5 s, then the game spent about 170 s with no new opens. Whether that time shows up as reads is what v0.1's read counts will tell. If the game reads by memory mapping or by calls that bypass the import, the counts stay at zero and the log says so. |
| Individual plugin names | **Yes**, when the file is opened or read. | The file path of `CreateFile`. | As above. | Each plugin's first open. | The name is the file being opened or read. That isn't necessarily the plugin whose records the engine is processing at that moment. |
| BA2 loading | **Partly.** Opening and reads are seen. Registration in the engine's archive list is not. | As for plugins. | As above. | The first archive opens about 6 s after F4SE load, on the main thread. | Reading a texture or mesh from an archive later in the game is also a "read". Read summaries are only logged during start-up and loads. |
| Individual BA2 names | **Yes.** | As above. | As above. | Each archive's first open. | Reopens share one set of counters per file. |
| Main menu initialisation | **Yes**, as F4SE milestones. | F4SE messages: `InputLoaded` ("right before the main menu initializes"), then `GameLoaded`. Also the first Scaleform menu movie. | F4SE messaging. F4SE's Scaleform callback, called once for each menu movie. | `InputLoaded` / the first menu movie. | Which menu a movie belongs to isn't read: that would need GFx structures. The monitor counts the movies. Registering the callback adds an empty `f4se.plugins.MultiplexerLoadMonitor` object to each menu, as every Scaleform-using F4SE plugin does. `MenuMovies=0` turns this off. |
| Papyrus initialisation | **The VM's creation, yes. The scripts' own initialisation, no.** | F4SE Papyrus interface `Register`. F4SE calls it from its hook on the game's native-function registration, after the VM exists. | F4SE's own hook. The monitor adds none. | When the VM is created. | Individual script `OnInit` events are only visible in `Papyrus.0.log` (with logging on) or through engine event sinks. Those need CommonLibF4 and the Address Library, which v0.1 doesn't use. |
| Save loading | **Start and end, yes.** | F4SE `PreLoadGame` (with the save path) and `PostLoadGame` (with success or failure). The `.ess` file's open and reads. | F4SE messaging, plus the IAT wrapper. | `PreLoadGame`. | What happens between the two messages (reading change forms, loading the cell) is inside the engine. |
| New game | **Start only.** | F4SE `NewGame`. | F4SE messaging. | `NewGame`. | F4SE sends no message when a new game finishes loading the world. |
| Cell/world loading | **Not in v0.1.** | Needs the engine's cell events (for example `TESCellFullyLoadedEvent` / `BGSActorCellEvent` sinks) through CommonLibF4 and the Address Library. | Engine event sinks, which need game addresses. | — | Planned for a later version once verified against CommonLibF4. Meanwhile, reads from archives during a load show that the game is still fetching assets. |

## Design

- **Event sources**
  - F4SE messages.
  - The F4SE Papyrus and Scaleform callbacks.
  - Wrappers around four of Fallout4.exe's own kernel32 imports: `CreateFileW`, `CreateFileA`, `ReadFile` and `CloseHandle`.

  This is the same technique the Multiplexer Archive Loader uses: it doesn't depend on the game version, and it works alongside other DLLs that wrap the same imports, because each wrapper calls the one it replaced. The monitor uses no game addresses, no game structures and no code patches.
- **Shared state** (`MonitorCore.hpp`, unit-tested on every platform). One lock-protected record holds:
  - the current stage and when it started;
  - the current item and when it became current;
  - the previous item;
  - the last completed event;
  - the last plugin and the last archive;
  - the time of the last observation.

  **Stages change only when an event is observed, never because time has passed.**
- **Files.** Plugins, archives and saves are tracked. Loose files under `Data` are only counted unless `LooseFiles=1`, because there can be many thousands. Each tracked file keeps atomic counters for opens, reads and bytes. Reads are matched to files by handle under a shared lock, so the game's reading threads don't wait on each other. Each file's first open is logged once. Individual reads are never logged: a summary is written every 5 s during loads, listing the files read most.
- **Watchdog thread.** While the game is starting or loading, it logs a note after 15 s with no observed activity, then at longer and longer intervals. The note gives the stage, the last item, the previous item, the last plugin, the last archive and the "not a cause" warning. When activity resumes, it logs how long the silence lasted.
- **Overlay**
  - It is its own click-through window that never takes focus, in the top-left corner of the game window.
  - Its own thread draws it with GDI, so it keeps counting when the game stops responding. It stays visible when Windows replaces the hung game window with its "not responding" ghost.
  - It appears once the game window exists, which is the sign that rendering has started. Until then, everything is in the log.
  - It hides when another program is in front, and 10 s after the main menu is reached or a save has loaded. After `NewGame` it stays, because no event says the world has loaded.
  - It isn't shown when `Fallout4Prefs.ini` asks for exclusive full screen, because a window over the game can make it leave full screen. Use borderless windowed, or set `Overlay=1`.
- **Log.** Lines are written without forcing each to disk: Windows keeps written data when the game hangs or is ended. Milestones and watchdog notes are flushed. Times are computed in integer milliseconds, because the game changes the floating-point settings of its threads.
- **Shutdown.** When the process exits, a last line is written and the log is closed. If the DLL were ever unloaded before the process ends, the game's imports are put back first.

The overlay shows:

```
Multiplexer Load Monitor
Stage: Opening plugin files
Current: Reading Dummy_Plugin_05.esp (120.4 MB of 189.7 MB read so far)
Previous: Opened plugin PennsWoods.esp
On current: 12.3 s   No new activity: 6.0 s
Last observed is not a confirmed cause
```

"Read so far" is what the game asked for, not progress: a file can be read more than once, or out of order.

## Settings

All settings are optional. They go in `MultiplexerLoadMonitor.ini`, next to the DLL:

```ini
[Monitor]
Overlay=auto        ; auto (not over exclusive full screen), 1 = always, 0 = never
LooseFiles=0        ; 1 = also track and log loose files under Data (many lines)
TrackReads=1        ; 0 = only opens, no read observation
MenuMovies=1        ; 0 = do not register the Scaleform callback
QuietSeconds=15     ; no observed activity for this long while loading: a log note
SummarySeconds=5    ; interval of the read summaries during loads
```

## Reading the log

- `Stage: …` lines are milestones, each with the event that caused it.
- `Observing the game's imports: …` and `F4SE interfaces: …` show which sources are active. A `NO` means that source sees nothing in this game version or setup.
- `At game data ready: …` gives the totals: archives and plugin files opened, the reads seen on them, loose-file opens and failed opens. These read counts show how much of plugin loading is visible as reads.
- `Reads in the last 5 s: …` lists the files the game was reading from.
- `No observed activity for …` lines are the watchdog notes. Read them together with the warning at their end.

## Version 0.1 scope and next steps

v0.1 does the following:

- loads as an F4SE plugin;
- writes the log;
- reports real milestones from F4SE events and callbacks;
- records the real file names the game opens and reads;
- shows the overlay once the game window exists;
- shuts down cleanly.

Not yet:

- Cell and world loading, script `OnInit` events, and the end of a new-game load. These need engine event sinks through CommonLibF4 and the Address Library, to be verified for the game version before use.
- Naming the menu behind each menu movie.

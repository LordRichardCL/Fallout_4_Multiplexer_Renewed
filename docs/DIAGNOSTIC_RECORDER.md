# Multiplexer Diagnostic Recorder

`MultiplexerDiagnosticRecorder.dll` is an optional F4SE plugin that records a small, fixed set of
runtime events while Fallout 4 runs. Multiplexer reads the log, recognises known problem patterns
and shows the likely causes, the evidence and what to check. It exists to help diagnose problems with a
Multiplexer build (and to give users a log worth sharing with the developer); it is not a crash
logger and does not try to explain every crash.

## What it records

| Event | When | Content |
|---|---|---|
| `session_start` | F4SE loads the plugin | recorder, game (runtime) and F4SE versions, F4SE release index, game folder, `Fallout4.exe` file version, process ID |
| `f4se_message` | each message F4SE sends to plugins | `post_load`, `post_post_load`, `input_loaded`, `game_data_ready` (`ready`: false before the plugins load, true after), `game_loaded`, `new_game`, `pre_load_game` / `delete_game` (`save`: the save's file name only), `post_load_game` (`success`), `pre_save_game`, `post_save_game` |
| `load_order` | after F4SE's `post_load` | `plugins.txt` as the game process reads it (under Mod Organizer 2 that is the profile's file, through its virtual file system): every entry with `active` |
| `creation_club` | after `post_load` | `Fallout4.ccc`: each Creation Club plugin and whether it is in `Data` |
| `modules` | after `post_load` | DLLs loaded from the game folder or from `Data\F4SE\Plugins`: name, path, size, file version; other modules (Windows, drivers) are only counted |
| `f4se_plugins` | after `post_load` | each DLL in `Data\F4SE\Plugins` and whether it is loaded; whether the Address Library file for this game version (`version-<game version>-0.bin`) is present |
| `f4se_log` | after `post_load` | F4SE's own plugin load lines from `f4se.log` (`plugin <dll> (...) loaded correctly` or the reason it was not loaded), at most 400 |
| `archive_ini` | F4SE loads the plugin | each `[Archive]` key starting with `sResource` in `Fallout4.ini` and `Fallout4Custom.ini` (My Games), read as plain text, so its full value: file, key, length, value |
| `archive_presence` | F4SE loads the plugin | every archive the INI lists that is not in `Data` at all (`checked`, `absent`, `missing`): it cannot load whatever the list does |
| `archive_observers` | F4SE loads the plugin | whether the game's import-table entries below could be observed (`ini_reads`, `open_a`, `open_w`) |
| `archive_ini_read` | each time the game reads a `sResource*` INI key | section, key, file name, the game's `buffer` size, the length `returned`, `truncated` (the documented sign of a value cut to fit: returned = buffer - 1), and the value the game received |
| `archive_open` | each time the game opens a `.ba2` file | path, `ok`, `write` (opened for writing: not a load), `error` when it failed |
| `archive_handles` | `game_data_ready` (true) and `game_loaded` (main menu), on a thread of its own | the `.ba2` files the process holds open (`method`: Windows' handle snapshot, or a scan of handle values where it is missing, as under Wine and Proton) |
| `recorder_error` / `recorder_warning` | when something the recorder needs fails | where, and the message |
| `session_end` | normal process exit | `reason: process_exit`. A crash, or the process being terminated, never writes it |

Out of scope by design: game memory addresses or structures, FormID lookups, Papyrus, other plugins'
internals, process memory, saves and game state. The recorder uses the F4SE plugin interface and Windows
APIs only. For the archive events (1.2.0) it replaces three entries of `Fallout4.exe`'s import table
(`GetPrivateProfileStringA`, `CreateFileA`, `CreateFileW`) with wrappers that call the previous entry
(Windows, or another plugin's wrapper) with the same arguments, return its result unchanged and restore
its error code: they observe and change nothing. A plugin that replaces the same entry later and calls
Windows directly (X-Cell's Profile patch) hides the reads from the recorder; `archive_handles` still shows
what loaded. Every entry point catches every exception, every entry point catches every exception, so a failure is
written to the log instead of reaching the game.

## Compatibility

One DLL serves both game versions:

- **Old Gen** (1.10.163, F4SE 0.6.23): F4SE calls `F4SEPlugin_Query`, then `F4SEPlugin_Load`.
- **Next Gen** (F4SE 0.7.x): F4SE reads the exported `F4SEPlugin_Version`. The recorder declares
  address independence (no hard-coded addresses) and structure independence (no game structures), so it
  is version independent and needs no Address Library.

The plugin interface is declared in `src/recorder/F4SEPluginAbi.hpp` (checked against F4SE 0.6.23 and
0.7.9); no F4SE source is part of this project.

## The log

`Documents\My Games\Fallout4\F4SE\MultiplexerDiagnosticRecorder\session-<UTC date>-<time>-<process ID>.jsonl`
(the My Games folder F4SE 0.7.1+ reports, otherwise `Fallout4`). One file per game session; the newest 10
are kept. JSON Lines: one object per line, each written and flushed when it happens, so a crash leaves
everything before it on disk.

Every line has `schema` (1), `seq` (0, 1, 2, ...), `time` (UTC, ISO 8601; not on `session_end`), `ms`
(milliseconds since the recorder started) and `event`, plus the event's fields above. Readers ignore
unknown events and fields.

Beside it, `Documents\My Games\Fallout4\F4SE\MultiplexerDiagnosticRecorder.log` (recorder 1.1.0 and later)
is a short plain-text status log, rewritten at each start, so it is easy to see that the plugin loaded:
its version and F4SE's, the session file it records to (or why it could not create one), all F4SE plugins
loaded, game data ready, main menu reached, new game or save loaded, game exited normally.

Privacy: paths inside the user's profile folder are written as `%USERPROFILE%\...` (no account name), and
saves appear by file name only. The log does list the plugins and F4SE plugin DLLs.

## What Multiplexer recognises

`src/core/diagnostics/RecorderLog.cpp`; each finding has a severity, a likely cause (worded as likely),
a confidence, the evidence from the log and the checks to make.

| Pattern | Evidence | Severity, confidence |
|---|---|---|
| The game stopped while loading a save | `pre_load_game` without a later `post_load_game`, no `session_end` | Error, High |
| The game stopped while saving | `pre_save_game` without `post_save_game`, no `session_end` | Error, High |
| The game stopped while loading the plugins | `game_data_ready` false without true, no `session_end` | Error, Medium |
| The game stopped before F4SE finished loading plugins | no `post_load`, no `session_end` | Error, Medium |
| The game was quit after its exit save | no `session_end`, and the last F4SE messages are a completed save followed only by the deletion of an `Exitsave` (quitting to the desktop ends the game without the shutdown the recorder sees) | Info, High |
| The session ended without a normal exit | no `session_end` otherwise (a crash, or a quit path that skips the normal shutdown) | Warning, Low |
| A save failed to load | `post_load_game` with `success: false` | Error, High |
| F4SE did not load a plugin | F4SE's own line; the cause from its wording (built for another game version, Address Library missing, Old Gen plugin under Next Gen F4SE, Windows error 126/193, declined during query or load) | Warning, High |
| A DLL in `Data\F4SE\Plugins` was not loaded | listed, not loaded, no F4SE line explaining it | Warning, Medium |
| No Address Library data for this game version | `address_library_present: false` | Info |
| Plugins compiled into the output are still enabled | the build's `Multiplexer_DisablePlugins.txt` against the session's active plugins | Error, High |
| An output plugin is not enabled | the package's output plugins against the session's active plugins | Error, High |
| The game's load order differs from the one Multiplexer built for | active plugins in `Multiplexer_plugins.txt` that are not active | Warning, High |
| The plugins load in a different order | the first position where the shared active plugins differ in order | Warning, High |
| The game version differs from Multiplexer's setting | `runtime_version` against Settings (Old Gen / Next Gen) | Warning, High |
| The game read only part of the archive list | `archive_ini_read` of `SResourceArchiveList2` with `truncated`, shorter than the value in the INI: the buffer, the characters received, and the first archive past the cut with its entry number and character position | Error, High |
| Requested archives did not load | the build's `Multiplexer_Archives.ini` list (or the INI's) against the BA2 files opened or held open; only when the game's own `Fallout4 - *.ba2` were seen loading, so the method is known to work there | Error, High when the list was cut, else Medium |
| The game stopped loading the archive list part way | every listed archive in `Data` before a character position loaded and none after it (works when another plugin hid the INI read) | Error, High |
| Listed archives are not installed | `archive_presence`; for older logs, archives named after plugins that are not in `plugins.txt` at all | Error, High (Medium when inferred) |
| Archive loading could not be verified | none of the game's own archives seen opening | Info |
| Close to the archive limit | more than 230 BA2 files opened and no Addictol or X-Cell loaded (Old and Next Gen load at most 256 general and 256 texture archives) | Warning, Medium |
| The recorder could not record everything | `recorder_error` | Warning |

The **Archive compatibility** section of the report (Diagnostics page, `fo4mx diagnose`) puts the layers side
by side: entries and characters in the INI, characters the game received and its buffer, requested,
runtime verified and missing archives, and the first missing archive with its entry number and character
position in the INI value. Run the same build with and without X-Cell or Addictol to see which layer
stops the list: the buffer size is the game's own in every run.

`fo4mx diagnose --install` and **Check Installation** also say whether an archive-limit plugin is
installed: Addictol (`Data\F4SE\Plugins\Addictol.dll`; its Archive Limits module is on unless
`Addictol.toml` sets `bArchiveLimits = false`) or X-Cell.

The build comparison uses the export's notes (the folder holding `Multiplexer_plugins.txt`). Given the
folder that holds the exports (`<game>\Multiplexer Output` by default), the latest finished export in it
is used.

## Using it

1. Settings → Diagnostics: **Prepare the Diagnostic Recorder with each export**.
2. Export. Multiplexer writes `Multiplexer Output\Diagnostic Recorder\` beside the package (never inside
   its `Data` folder, so installing the package never installs the recorder): the DLL under
   `Data\F4SE\Plugins`, `Multiplexer Diagnostic Recorder.zip` and the install notes. Turning the setting
   off removes them at the next export.
3. Install the zip as a mod (Vortex, MO2), or copy the DLL into `<game>\Data\F4SE\Plugins`. F4SE reads
   plugins from that folder only: a copy in the game folder (next to `Fallout4.exe`) is never loaded.
4. Start the game through F4SE and play.
5. Not sure it loaded? The Diagnostics page says so (read from `f4se.log`); **Check Installation** (or
   `fo4mx diagnose --install --game "<game>"`) names the problem: the DLL in a folder F4SE does not read,
   F4SE's own reason for refusing it, or the game not started through F4SE.
6. Diagnostics page → **Open Newest Log** (or `fo4mx diagnose [file] --package "<game>\Multiplexer Output"`).

## Building

`src/recorder` is built on Windows only (`FO4MX_BUILD_RECORDER`, on by default), with the C runtime
linked statically: the DLL needs only Windows' own libraries. CI puts it next to
`Fallout4MultiplexerRenewed.exe` and `fo4mx.exe`, where export looks for it.

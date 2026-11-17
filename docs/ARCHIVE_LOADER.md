# Archive Loader

`MultiplexerArchiveLoader.dll` is an F4SE plugin that gives Fallout 4 its archive list in full. Turn it on
in **Settings → Archives** (*Load compiled plugins' archives with the F4SE archive loader*), or with
`fo4mx --archive-loader`.

## Why

A compiled plugin is disabled, so the game no longer loads its BA2 archives with it. Multiplexer lists
them in `SResourceArchiveList2` (Fallout4Custom.ini) instead. The game reads that line into a 1,024-byte
buffer: past 1,023 characters every archive is silently never loaded. Without the loader, Multiplexer keeps
the list within that length, and plugins whose archives do not fit stay in the load order (*Archive list
full*). With it, they are compiled too.

## What changes when it is on

- **Compile:** no *Archive list full*. The report says how many archives load through the loader.
- **Export:** the package's `Data\F4SE\Plugins` holds `MultiplexerArchiveLoader.dll` and
  `MultiplexerArchives.txt` (the full list, in the game's order). Install the package as usual.
- **Apply to Game:** writes the whole list into `Fallout4Custom.ini` (the game reads the first 1,023
  characters itself, the loader the rest), and waits until this build's loader and list are in the game's
  Data folder.
- **Playing:** always start the game through F4SE (`f4se_loader.exe`, or your mod manager's F4SE entry).
  Without F4SE only the start of the list loads.
- **Diagnostics:** with the Diagnostic Recorder installed, the Archive compatibility section shows the read
  as `CUT, the rest supplied` and every archive verified; if the loader did not supply the list, the finding
  *The archive loader did not supply the archive list* says what to check.

Only on Fallout 4 1.10.163, where it was tested in game; on another version it logs that and changes
nothing, and Multiplexer keeps the list within the limit.

## How it works

1. Reads the list to supply: `Data\F4SE\Plugins\MultiplexerArchives.txt`, or, without that file,
   `SResourceArchiveList2` from `Fallout4Custom.ini` as plain text.
2. Observes the game's INI reads of `sResourceArchiveList2` (a pass-through wrapper of the game's
   `GetPrivateProfileStringA` import), so it knows the exact value the game received.
3. Changes nothing unless that value is the start of the list to supply: a list from another build, or an
   INI changed since, is left alone.
4. When the game opens its first BA2 archive (every INI read is over by then), it looks in the executable's
   own data for a pointer to that exact text with a pointer to the setting's name
   (`sResourceArchiveList2`, any case, with or without `:Archive`) within 24 bytes; only if none is found
   there does it search the rest of the game's memory the same way. At most four such settings are
   changed; anything else and nothing is changed. No game addresses, no Address Library.
5. Points them at the full list. The game's own code then opens and registers every archive, in list
   order, as it does for a short list.
6. At game data ready (every archive is open by then) it puts the original values back.

Everything is written to `Documents\My Games\Fallout4\F4SE\MultiplexerArchiveLoader.log`.

### Startup trace (1.1.0)

The log also records when things happen, so a loading problem can be placed in the startup sequence:

- every line has the wall-clock time and the seconds since `F4SEPlugin_Load` (`[14:37:30.168 +   0.153s]`);
- `List checked: …`: entries, distinct names, names listed twice and archives not found in `Data`, with up
  to 20 of each as warnings. Only a note: the list is supplied as it is (the game skips a missing archive);
- `Archive registration starts: the game opens its first archive, …`: the moment the list is supplied;
- one `BA2 #n  <archive>  thread <id>  [phase]` line for each archive the game opens, the first time it
  opens it. Phase `list`: named in `MultiplexerArchives.txt`, opened by the game's own list loading;
  `list, NOT supplied`: named there, but the list was not supplied; `not in the list`: the game's own
  lists, DLC, Creation Club and plugins' archives. Further opens of an archive are only counted, and so
  are archives the game looks for and does not find (it looks for `<plugin> - Main.ba2` and
  `- Textures.ba2` for every plugin; most do not exist): numbering those would bury the ones that load;
- `First plugin file opened by the game: …`: archive registration is over and plugin data loading starts;
  then a count every 250 plugin files;
- every F4SE message (PostLoad, PostPostLoad, InputLoaded, both GameDataReady messages, GameLoaded, …) with
  the counts at that moment;
- a watchdog thread: when the game opens no file for 20 seconds before its data is ready, it writes the last
  archive opened (number, name, thread), the last file of any kind, and whether any plugin file has been
  opened yet (`still in archive registration` or `reading plugins`); then once a minute while it lasts. A
  very large plugin can take that long to read, so one note is not a hang; notes that keep coming after
  the last archive, with no plugin file opened, are.

Only the game's own file opens are seen (Fallout4.exe's imports), not those of other F4SE plugins. Trace
lines are not flushed one by one (that would slow loading); Windows keeps what was written even if the
game hangs or is ended.

### Startup sequence (traced, 1.10.163)

From the code and the Diagnostic Recorder's sessions of build `161126 223443` (times from one launch):

1. F4SE loads plugin DLLs in file order: Daytripper 4 logs `Installed ExtendedArchiveLimit patch`
   (13:51:25.659), then this loader's `F4SEPlugin_Load` (13:51:26.243): it reads the list file and wraps
   the three imports. It registers no archive itself, here or later.
2. F4SE PostLoad / PostPostLoad (+0.8 s).
3. The game reads its INIs (`sResourceArchiveList2` 14 times, 1,023 characters kept).
4. The game opens its first archive (`Fallout4 - Startup.ba2`, +4.2 s); inside that `CreateFileW`, before
   the real open, the loader points the setting at the full list.
5. The game registers its startup and base archives, then `sResourceArchiveList2` in list order with its
   own code, one after another on one thread (+7.6 to +10.7 s for 699 archives).
6. The game registers DLC, Creation Club and each active plugin's archives (from +10.8 s).
7. Plugin data loading, then GameDataReady; the loader restores the setting.

So the list is registered before every plugin's archives, through the game's INI-list loading, not the
plugin-archive step. Load order of the DLLs does not matter: the loader acts when the game opens its first
archive, long after every DLL has loaded.

### Hang with a long list and Daytripper 4's ExtendedArchiveLimit (resolved, 17 Nov 2026)

**Symptom.** Build `161126 223443` and later: the game froze at the main menu (no GameDataReady) with the full
list of 681 archives, with the compiled plugins switched over or not, and in the original load order too as
soon as the loader supplied that list. Every such launch stopped at the 1,104th archive opened, whichever
archive that was (Beast Hunter's, then KT_Framework's once Beast Hunter was off).

**What the tests showed** (archives registered from the list, including the game's own base ones; only
archives that opened are counted):

| Launch | General | Textures | Result |
|---|---|---|---|
| half list | 198 | 161 | loads |
| build of 16 Nov | 231 | 191 | loads |
| 240 + 240 test list | 249 | 249 | loads |
| full list | 382 | 317 | hangs |

The total does not matter (the original load order opens 893 general and 767 texture archives, most of them
through plugins); more than 256 of one type arriving through the list does.

**Where it hung.** A ProcDump mini dump of the frozen game: the busy thread was at `Fallout4.exe+1B773F7`, in
the archive index's name-table insert (`BSTSmallIndexScatterTable` insert, `+1B772F0`), called from
`BSResource::Archive2::Index::AddDataFile` (`+1B767F0`, return address `+1B768AC`). Offsets for 1.10.163 as
named by [BSALimitExpander](https://github.com/WirelessLan/BSALimitExpander); the index layout in
[CommonLibF4](https://github.com/Ryan-rsm-McKenzie/commonlibf4) has fixed arrays of 256 archives and a one-byte
archive number per file, one index for general archives and one for textures: the engine's limit. A limit
raiser replaces that storage (BSALimitExpander and X-Cell's ArchiveLimits module hook `AddDataFile`).

**Cause and fix.** Daytripper 4 (1.4.9) with `ExtendedArchiveLimit` on raises the limit for the archives
plugins load, but its handling hung when more than 256 of a type came through the INI list. With
`ExtendedArchiveLimit` turned off in Daytripper 4's INI and **BSA Limit Expander** (0.3.4) installed instead,
the same build with the full list (382 + 317 through the list, 1,660 archives in all) reached the main menu
and started a new game. Nothing in the loader or the build had to change.

**Use one limit raiser only**: BSA Limit Expander, Daytripper 4's `ExtendedArchiveLimit`, Archive Limit
Breaker and X-Cell's ArchiveLimits all replace the same function. With a list of more than 255 general or
texture archives, use BSA Limit Expander (or another raiser shown to work with this list) and turn the others
off.

**What the app checks.** With the archive loader in use, the switch-over check (Apply to Game and
`Multiplexer_Instructions.txt`) counts the list's general and texture archives, plus the game's own 9 of each
from `Fallout4.ini` (a name with ` - Textures` is a texture archive), and looks in `Data\F4SE\Plugins` (under
Mod Organizer 2, its virtual Data) for the limit raisers when either count is over 256:

| Found | Result |
|---|---|
| BSA Limit Expander, Daytripper 4's ExtendedArchiveLimit off or no Daytripper 4 | nothing to say |
| Daytripper 4 with ExtendedArchiveLimit on, no BSA Limit Expander | **blocked**: it froze the game with such a list |
| both | **blocked**: they replace the same game function; turn ExtendedArchiveLimit off |
| neither | warning: install BSA Limit Expander (another raiser the app does not know may already work) |

Daytripper 4's ExtendedArchiveLimit counts as on (its default) unless `Daytripper4.ini` (or `.toml`) has a
line whose key contains `ExtendedArchiveLimit` set to `0`, `false`, `off` or `no`; the line found is quoted
in the message. Lists within the limit are not checked.

## Checked so far

In the game (1.10.163): with a 25,345-character list the setting was found in the executable's data and
supplied; archives listed past the cut whose plugins were disabled loaded (TrueStormsFO4, SSTFungalForest,
SS2WastelandReconstructionKit); the original value was restored and a save loaded.

In the game (1.10.163, 0.1.0): the setting name was found in the executable but no setting object with
the assumed layout, so nothing was changed (the recorder's report then showed archives past the cut
loading only because their plugins were active again; it now says so instead of crediting the list).


Under Wine, with a host program that keeps its settings like the game, reads the INI with a 1,024-byte
buffer and opens the archives from its setting: without the loader 36 of 81 archives opened, with it 81 of
81, and the original value was restored; a list from another build was left alone.
